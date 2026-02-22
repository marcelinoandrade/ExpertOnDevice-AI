#include "bsp.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

#include "driver/gpio.h"
#include "driver/jpeg_encode.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_wifi.h"
#include "esp_wifi_remote.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "linux/videodev2.h"
#include "lwip/dns.h"

#include "bsp/esp-bsp.h"
#include "secret.h"

static const char *TAG = "bsp";

#define BSP_BUTTON_GPIO BSP_BUTTON_ENCODER
#define BSP_BUTTON_ACTIVE_LEVEL 0
#define BSP_AUDIO_READ_TIMEOUT_MS 350
#define BSP_WIFI_MAXIMUM_RETRY 8
#define BSP_WIFI_WAIT_TIMEOUT_MS 20000
#define BSP_CAMERA_MMAP_BUFFERS 2
#define BSP_CAMERA_MAX_JPEG_BYTES (512 * 1024)
#define BSP_CAMERA_PREVIEW_SIZE 240
#define BSP_CAMERA_PREVIEW_SKIP_FRAMES                                         \
  8 // More frames for ISP stabilization (AWB, AGC, etc.)
#define BSP_CAMERA_CAPTURE_SKIP_FRAMES 6
#define BSP_CAMERA_JPEG_MAX_WIDTH                                              \
  320 // Reduced for AI (smaller file, faster processing)
#define BSP_CAMERA_JPEG_MAX_HEIGHT 240

#define BSP_WIFI_CONNECTED_BIT BIT0
#define BSP_WIFI_FAIL_BIT BIT1

static bool s_display_ready;
static bool s_audio_ready;
static bool s_wifi_ready;
static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_num;
static volatile int s_knob_delta;
static portMUX_TYPE s_knob_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_video_ready;
static jpeg_encoder_handle_t s_jpeg_encoder_handle = NULL;

static esp_err_t bsp_button_init(void);
static esp_err_t bsp_camera_ensure_ready(void);
static int bsp_camera_open_capture_fd(const char **opened_dev_name);
static bool bsp_camera_try_set_capture_format(int fd, uint32_t pixfmt,
                                              uint32_t *out_w, uint32_t *out_h,
                                              uint32_t *out_bytesperline);
static void bsp_knob_left_cb(void *arg, void *data);
static void bsp_knob_right_cb(void *arg, void *data);

static void bsp_knob_left_cb(void *arg, void *data) {
  (void)arg;
  (void)data;
  portENTER_CRITICAL(&s_knob_lock);
  s_knob_delta--;
  portEXIT_CRITICAL(&s_knob_lock);
}

static void bsp_knob_right_cb(void *arg, void *data) {
  (void)arg;
  (void)data;
  portENTER_CRITICAL(&s_knob_lock);
  s_knob_delta++;
  portEXIT_CRITICAL(&s_knob_lock);
}

static esp_err_t bsp_camera_ensure_ready(void) {
  if (s_video_ready) {
    return ESP_OK;
  }

  i2c_master_bus_handle_t i2c_handle = NULL;
  ESP_RETURN_ON_ERROR(bsp_get_i2c_bus_handle(&i2c_handle), TAG,
                      "get i2c handle failed");

  esp_video_init_csi_config_t csi_cfg = {
      .sccb_config =
          {
              .init_sccb = false,
              .i2c_handle = i2c_handle,
              .freq = 100000,
          },
      .reset_pin = -1,
      .pwdn_pin = -1,
  };

  const esp_video_init_config_t cfg = {
      .csi = &csi_cfg,
  };

  esp_err_t err = esp_video_init(&cfg);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return err;
  }

#if CONFIG_ESP_VIDEO_ENABLE_ISP &&                                             \
    CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER
  ESP_LOGI(TAG, "ISP pipeline enabled (AWB/AGC/AE via IPA when configured)");
#else
  ESP_LOGW(
      TAG,
      "ISP pipeline disabled; image auto-adjustment quality may be reduced");
#endif

#if CONFIG_ESP_IPA_AWB_ALGORITHM && CONFIG_ESP_IPA_AGC_ALGORITHM
  ESP_LOGI(TAG, "IPA auto algorithms active: AWB, AGC");
#endif
#if CONFIG_ESP_IPA_AEN_ALGORITHM || CONFIG_ESP_IPA_ATC_ALGORITHM
  ESP_LOGI(TAG, "IPA auto enhancement/exposure tuning active");
#endif

  s_video_ready = true;
  return ESP_OK;
}

static int bsp_camera_open_capture_fd(const char **opened_dev_name) {
  // For MIPI CSI (OV2710), ISP processes RAW10->RGB565 through /dev/video0
  // ISP DVP device (/dev/video1) only exists when ISP is configured for DVP
  // input
  const char *video_dev_candidates[] = {
      ESP_VIDEO_MIPI_CSI_DEVICE_NAME, // /dev/video0 - MIPI CSI with ISP
                                      // processing
      ESP_VIDEO_ISP_DVP_DEVICE_NAME, // /dev/video1 - ISP DVP (may not exist for
                                     // CSI)
      ESP_VIDEO_DVP_DEVICE_NAME,     // /dev/video2 - DVP
  };
  const size_t candidates =
      sizeof(video_dev_candidates) / sizeof(video_dev_candidates[0]);

  for (size_t i = 0; i < candidates; i++) {
    int test_fd = open(video_dev_candidates[i], O_RDONLY);
    if (test_fd < 0) {
      test_fd = open(video_dev_candidates[i], O_RDWR);
    }
    if (test_fd < 0) {
      ESP_LOGW(TAG, "open %s failed (errno=%d)", video_dev_candidates[i],
               errno);
      continue;
    }

    struct v4l2_capability cap = {0};
    if (ioctl(test_fd, VIDIOC_QUERYCAP, &cap) != 0) {
      ESP_LOGW(TAG, "VIDIOC_QUERYCAP failed on %s", video_dev_candidates[i]);
      close(test_fd);
      continue;
    }

    const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS)
                              ? cap.device_caps
                              : cap.capabilities;
    if ((caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE)) ==
        0) {
      ESP_LOGW(TAG, "skip non-capture %s caps=0x%08" PRIx32,
               video_dev_candidates[i], caps);
      close(test_fd);
      continue;
    }

    if (opened_dev_name) {
      *opened_dev_name = video_dev_candidates[i];
    }
    return test_fd;
  }

  return -1;
}

static bool bsp_camera_try_set_capture_format(int fd, uint32_t pixfmt,
                                              uint32_t *out_w, uint32_t *out_h,
                                              uint32_t *out_bytesperline) {
  // ISP-supported resolutions (OV2710 outputs 1920x1080 RAW, ISP converts to
  // RGB) Try current format first, then common ISP output resolutions
  const uint32_t sizes[][2] = {
      {640, 480},   // VGA - commonly supported by ISP
      {1280, 720},  // HD - commonly supported by ISP
      {1920, 1080}, // Full HD - matches sensor output
      {800, 600},   // SVGA
  };

  // First, try to use current format (ISP may already have a good format set)
  struct v4l2_format cur = {0};
  cur.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(fd, VIDIOC_G_FMT, &cur) == 0 && cur.fmt.pix.width > 0 &&
      cur.fmt.pix.height > 0) {
    // Only change pixel format, keep resolution
    struct v4l2_format req = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix.width = cur.fmt.pix.width,
        .fmt.pix.height = cur.fmt.pix.height,
        .fmt.pix.pixelformat = pixfmt,
    };
    if (ioctl(fd, VIDIOC_S_FMT, &req) == 0) {
      if (out_w)
        *out_w = req.fmt.pix.width;
      if (out_h)
        *out_h = req.fmt.pix.height;
      if (out_bytesperline)
        *out_bytesperline = req.fmt.pix.bytesperline;
      ESP_LOGD(TAG, "Using current format: %" PRIu32 "x%" PRIu32, *out_w,
               *out_h);
      return true;
    }
  }

  // If current format doesn't work, try ISP-supported resolutions
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    struct v4l2_format req = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix.width = sizes[i][0],
        .fmt.pix.height = sizes[i][1],
        .fmt.pix.pixelformat = pixfmt,
    };
    if (ioctl(fd, VIDIOC_S_FMT, &req) == 0) {
      if (out_w)
        *out_w = req.fmt.pix.width;
      if (out_h)
        *out_h = req.fmt.pix.height;
      if (out_bytesperline)
        *out_bytesperline = req.fmt.pix.bytesperline;
      ESP_LOGD(TAG, "Set format: %" PRIu32 "x%" PRIu32, *out_w, *out_h);
      return true;
    }
  }

  return false;
}

static void bsp_wifi_remote_event_handler(void *arg,
                                          esp_event_base_t event_base,
                                          int32_t event_id, void *event_data) {
  (void)arg;

  if (event_base == WIFI_REMOTE_EVENT && event_id == WIFI_EVENT_STA_START) {
    (void)esp_wifi_remote_connect();
    return;
  }

  if (event_base == WIFI_REMOTE_EVENT &&
      event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_wifi_retry_num < BSP_WIFI_MAXIMUM_RETRY) {
      (void)esp_wifi_remote_connect();
      s_wifi_retry_num++;
      ESP_LOGW(TAG, "Wi-Fi remote reconnect attempt %d/%d", s_wifi_retry_num,
               BSP_WIFI_MAXIMUM_RETRY);
    } else if (s_wifi_event_group) {
      xEventGroupSetBits(s_wifi_event_group, BSP_WIFI_FAIL_BIT);
    }
    return;
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
    s_wifi_retry_num = 0;
    s_wifi_ready = true;
    ESP_LOGI(TAG, "Wi-Fi remote connected, got IP: " IPSTR,
             IP2STR(&event->ip_info.ip));
    if (s_wifi_event_group) {
      xEventGroupSetBits(s_wifi_event_group, BSP_WIFI_CONNECTED_BIT);
    }
  }
}

static void bsp_configure_dns_fallback(void) {
  /* C6-hosted link may come up without DHCP-provided DNS on first boot.
   * Configure public resolvers as fallback to avoid getaddrinfo(rc=202).
   */
  const ip_addr_t dns0 = IPADDR4_INIT_BYTES(1, 1, 1, 1); /* Cloudflare */
  const ip_addr_t dns1 = IPADDR4_INIT_BYTES(8, 8, 8, 8); /* Google */
  dns_setserver(0, &dns0);
  dns_setserver(1, &dns1);

  const ip_addr_t *cur0 = dns_getserver(0);
  const ip_addr_t *cur1 = dns_getserver(1);
  ESP_LOGI(TAG, "DNS fallback set: dns0=" IPSTR " dns1=" IPSTR,
           IP2STR(ip_2_ip4(cur0)), IP2STR(ip_2_ip4(cur1)));
}

static esp_err_t bsp_network_stack_init(void) {
  /* Even with C6-managed connectivity, LWIP/esp-netif must be initialized
   * before esp_http_client calls, otherwise tcpip asserts with Invalid mbox.
   */
  esp_err_t err = esp_netif_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return err;
  }

  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return err;
  }

  bsp_configure_dns_fallback();

  return ESP_OK;
}

static esp_err_t bsp_wifi_remote_init(void) {
#if CONFIG_ESP_WIFI_REMOTE_ENABLED
  if (!s_wifi_event_group) {
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
      return ESP_ERR_NO_MEM;
    }
  }

  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!sta) {
    sta = esp_netif_create_default_wifi_sta();
    if (!sta) {
      return ESP_FAIL;
    }
  }

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t err = esp_wifi_remote_init(&cfg);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return err;
  }

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  err = esp_event_handler_instance_register(WIFI_REMOTE_EVENT, ESP_EVENT_ANY_ID,
                                            &bsp_wifi_remote_event_handler,
                                            NULL, &instance_any_id);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return err;
  }

  err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            &bsp_wifi_remote_event_handler,
                                            NULL, &instance_got_ip);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return err;
  }

  wifi_config_t wifi_config = {0};
  strlcpy((char *)wifi_config.sta.ssid, SECRET_WIFI_SSID,
          sizeof(wifi_config.sta.ssid));
  strlcpy((char *)wifi_config.sta.password, SECRET_WIFI_PASS,
          sizeof(wifi_config.sta.password));
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;

  err = esp_wifi_remote_set_mode(WIFI_MODE_STA);
  if (err != ESP_OK) {
    return err;
  }

  err = esp_wifi_remote_set_config(WIFI_IF_STA, &wifi_config);
  if (err != ESP_OK) {
    return err;
  }

  err = esp_wifi_remote_start();
  if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
    return err;
  }

  EventBits_t bits = xEventGroupWaitBits(
      s_wifi_event_group, BSP_WIFI_CONNECTED_BIT | BSP_WIFI_FAIL_BIT, pdFALSE,
      pdFALSE, pdMS_TO_TICKS(BSP_WIFI_WAIT_TIMEOUT_MS));

  if (bits & BSP_WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "Wi-Fi remote ready");
    return ESP_OK;
  }
  if (bits & BSP_WIFI_FAIL_BIT) {
    ESP_LOGW(TAG, "Wi-Fi remote failed after retries");
    return ESP_FAIL;
  }

  ESP_LOGW(TAG, "Wi-Fi remote connection timeout");
  return ESP_ERR_TIMEOUT;

#else
  ESP_LOGW(TAG, "Wi-Fi remote is disabled; C6 hosted netif won't be created");
  return ESP_ERR_NOT_SUPPORTED;
#endif
}

static void bsp_log_connectivity_status(void) {
  if (s_wifi_ready) {
    ESP_LOGI(TAG, "Connectivity delegated to ESP32-C6 co-processor (STA up)");
  } else {
    ESP_LOGI(TAG,
             "Connectivity delegated to ESP32-C6 co-processor (STA not ready)");
  }
}

esp_err_t bsp_init(void) {
  ESP_LOGI(TAG, "Init BSP for ESP32-P4-EYE");

  /* Ensure board-level power/RTC GPIO state is configured on every cold boot.
   */
  ESP_RETURN_ON_ERROR(bsp_p4_eye_init(), TAG, "board init failed");

  // 1. Inicie o I2C e a Tela primeiro (Recursos Leves)
  ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "i2c init failed");
  if (!bsp_display_start()) {
    return ESP_FAIL;
  }
  s_display_ready = true;

  // 2. MONTE O SD CARD (Agora com mais tempo para o sistema respirar)
  ESP_LOGI(TAG, "Mounting SD Card...");
  bsp_sdcard_mount();
  vTaskDelay(pdMS_TO_TICKS(500)); // Aumente para 500ms para o VFS estabilizar!

  // 3. INICIE A CÂMERA APENAS UMA VEZ
  ESP_LOGI(TAG, "Starting Camera subsystem...");
  esp_err_t cam_err = bsp_camera_ensure_ready();
  if (cam_err != ESP_OK) {
    ESP_LOGE(TAG, "Camera hardware failed: %s", esp_err_to_name(cam_err));
  }

  ESP_RETURN_ON_ERROR(bsp_display_backlight_on(), TAG, "backlight init failed");
  ESP_RETURN_ON_ERROR(bsp_button_init(), TAG, "button init failed");
  esp_err_t knob_err = bsp_knob_init();
  if (knob_err == ESP_OK) {
    ESP_RETURN_ON_ERROR(bsp_knob_register_cb(KNOB_LEFT, bsp_knob_left_cb, NULL),
                        TAG, "knob left cb failed");
    ESP_RETURN_ON_ERROR(
        bsp_knob_register_cb(KNOB_RIGHT, bsp_knob_right_cb, NULL), TAG,
        "knob right cb failed");
  } else {
    ESP_LOGW(TAG, "knob init failed: %s", esp_err_to_name(knob_err));
  }
  ESP_RETURN_ON_ERROR(bsp_extra_pdm_codec_init(), TAG, "pdm mic init failed");
  ESP_RETURN_ON_ERROR(bsp_network_stack_init(), TAG, "netif init failed");
  esp_err_t wifi_err = bsp_wifi_remote_init();
  if (wifi_err != ESP_OK) {
    ESP_LOGW(TAG, "C6 hosted Wi-Fi init not ready: %s",
             esp_err_to_name(wifi_err));
  } else {
    // Configura o SNTP para sincronizar o horário global real via internet
    esp_sntp_config_t sntp_config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp_config);

    // Configura Timezone (Ex: Brasil <-03>3)
    setenv("TZ", "<-03>3", 1);
    tzset();
    ESP_LOGI(TAG, "SNTP time synchronization initialized (Timezone: BRT)");
  }
  s_audio_ready = true;

  bsp_log_connectivity_status();
  return ESP_OK;
}

static esp_err_t bsp_button_init(void) {
  const gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << BSP_BUTTON_GPIO) | (1ULL << BSP_BUTTON_NUM1) |
                      (1ULL << BSP_BUTTON_NUM2) | (1ULL << BSP_BUTTON_NUM3),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  return gpio_config(&io_conf);
}

bool bsp_lvgl_lock(int timeout_ms) {
  if (!s_display_ready) {
    return false;
  }

  /* P4 BSP uses 0 for an infinite wait. */
  const uint32_t timeout_for_bsp = (timeout_ms < 0) ? 0u : (uint32_t)timeout_ms;
  return bsp_display_lock(timeout_for_bsp);
}

void bsp_lvgl_unlock(void) {
  if (s_display_ready) {
    bsp_display_unlock();
  }
}

esp_err_t bsp_display_show_status(const char *status_text) {
  ESP_LOGI(TAG, "[DISPLAY][STATUS] %s", status_text ? status_text : "(null)");
  return ESP_OK;
}

esp_err_t bsp_display_show_text(const char *body_text) {
  ESP_LOGI(TAG, "[DISPLAY][TEXT] %s", body_text ? body_text : "(null)");
  return ESP_OK;
}

bool bsp_button_is_pressed(void) {
  return gpio_get_level(BSP_BUTTON_GPIO) == BSP_BUTTON_ACTIVE_LEVEL;
}

bool bsp_photo_button_is_pressed(void) {
  return gpio_get_level(BSP_BUTTON_NUM1) == BSP_BUTTON_ACTIVE_LEVEL;
}

bool bsp_button2_is_pressed(void) {
  return gpio_get_level(BSP_BUTTON_NUM2) == BSP_BUTTON_ACTIVE_LEVEL;
}

bool bsp_button3_is_pressed(void) {
  return gpio_get_level(BSP_BUTTON_NUM3) == BSP_BUTTON_ACTIVE_LEVEL;
}

int bsp_knob_consume_delta(void) {
  int delta = 0;
  portENTER_CRITICAL(&s_knob_lock);
  delta = s_knob_delta;
  s_knob_delta = 0;
  portEXIT_CRITICAL(&s_knob_lock);
  return delta;
}

esp_err_t bsp_camera_capture_jpeg(uint8_t **jpeg_data, size_t *jpeg_len) {
  if (!jpeg_data || !jpeg_len) {
    return ESP_ERR_INVALID_ARG;
  }
  *jpeg_data = NULL;
  *jpeg_len = 0;

  ESP_RETURN_ON_ERROR(bsp_camera_ensure_ready(), TAG, "camera init failed");

  // Initialize JPEG encoder if not already done
  if (s_jpeg_encoder_handle == NULL) {
    jpeg_encode_engine_cfg_t encode_eng_cfg = {
        .timeout_ms = 500, // Increased timeout for encoding, but with smaller
                           // resolution should be fast
    };
    esp_err_t err =
        jpeg_new_encoder_engine(&encode_eng_cfg, &s_jpeg_encoder_handle);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to create JPEG encoder: %s", esp_err_to_name(err));
      return err;
    }
  }
  esp_err_t ret = ESP_FAIL;
  uint8_t *preview_rgb565 = NULL;
  uint8_t *rgb565_buf = NULL;
  uint8_t *jpeg_out_buf = NULL;
  size_t jpeg_out_size = 0;
  size_t jpeg_alloced_size = 0;
  uint16_t preview_w = 0;
  uint16_t preview_h = 0;

  /* Use the same frame source as the on-screen preview to avoid mismatch
   * between what user sees and what is sent to AI.
   */
  ret = bsp_camera_capture_preview_rgb565(&preview_rgb565, &preview_w,
                                          &preview_h);
  if (ret != ESP_OK || !preview_rgb565 || preview_w == 0 || preview_h == 0) {
    ESP_LOGE(TAG, "preview frame capture for JPEG failed");
    goto cleanup;
  }

  const size_t rgb565_size = (size_t)preview_w * (size_t)preview_h * 2;
  rgb565_buf =
      heap_caps_malloc(rgb565_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!rgb565_buf) {
    rgb565_buf =
        heap_caps_malloc(rgb565_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!rgb565_buf) {
    ret = ESP_ERR_NO_MEM;
    goto cleanup;
  }
  memcpy(rgb565_buf, preview_rgb565, rgb565_size);

  /* Send preview pixels to JPEG encoder without extra byte swapping.
   * Display path may apply its own swap, but AI payload must keep camera order.
   */

  /* For some scenes, compressed JPEG can be larger than expected.
   * Allocate with generous headroom to avoid encoder overflow.
   */
  jpeg_out_size = rgb565_size;
  if (jpeg_out_size < 4096) {
    jpeg_out_size = 4096;
  }
  if (jpeg_out_size > BSP_CAMERA_MAX_JPEG_BYTES) {
    jpeg_out_size = BSP_CAMERA_MAX_JPEG_BYTES;
  }

  jpeg_encode_memory_alloc_cfg_t jpeg_mem_cfg = {
      .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
  };
  jpeg_out_buf = (uint8_t *)jpeg_alloc_encoder_mem(jpeg_out_size, &jpeg_mem_cfg,
                                                   &jpeg_alloced_size);
  if (!jpeg_out_buf) {
    ret = ESP_ERR_NO_MEM;
    goto cleanup;
  }
  jpeg_out_size = jpeg_alloced_size;

  jpeg_encode_cfg_t enc_config = {
      .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
      .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
      .image_quality = 80,
      .width = preview_w,
      .height = preview_h,
  };

  uint32_t actual_jpeg_size = 0;
  ret = jpeg_encoder_process(s_jpeg_encoder_handle, &enc_config, rgb565_buf,
                             rgb565_size, jpeg_out_buf, jpeg_out_size,
                             &actual_jpeg_size);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "JPEG encoding failed from preview: %s",
             esp_err_to_name(ret));
    goto cleanup;
  }
  if (actual_jpeg_size < 4 || actual_jpeg_size > BSP_CAMERA_MAX_JPEG_BYTES) {
    ret = ESP_FAIL;
    goto cleanup;
  }
  if (jpeg_out_buf[0] != 0xFF || jpeg_out_buf[1] != 0xD8) {
    ret = ESP_FAIL;
    goto cleanup;
  }

  *jpeg_data =
      heap_caps_malloc(actual_jpeg_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!*jpeg_data) {
    *jpeg_data = heap_caps_malloc(actual_jpeg_size,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!*jpeg_data) {
    ret = ESP_ERR_NO_MEM;
    goto cleanup;
  }
  memcpy(*jpeg_data, jpeg_out_buf, actual_jpeg_size);
  *jpeg_len = actual_jpeg_size;
  ESP_LOGI(TAG, "captured JPEG from preview: %u bytes (%ux%u)",
           (unsigned)actual_jpeg_size, (unsigned)preview_w,
           (unsigned)preview_h);
  ret = ESP_OK;

cleanup:
  free(preview_rgb565);
  free(rgb565_buf);
  if (jpeg_out_buf) {
    free(jpeg_out_buf);
  }
  if (ret != ESP_OK) {
    free(*jpeg_data);
    *jpeg_data = NULL;
    *jpeg_len = 0;
  }
  return ret;
}

esp_err_t bsp_camera_capture_preview_rgb565(uint8_t **rgb565_data,
                                            uint16_t *width, uint16_t *height) {
  if (!rgb565_data || !width || !height) {
    return ESP_ERR_INVALID_ARG;
  }
  *rgb565_data = NULL;
  *width = 0;
  *height = 0;

  ESP_RETURN_ON_ERROR(bsp_camera_ensure_ready(), TAG, "camera init failed");

  int fd = -1;
  const char *opened_dev = NULL;
  fd = bsp_camera_open_capture_fd(&opened_dev);
  if (fd < 0) {
    ESP_LOGE(TAG, "preview camera open failed");
    return ESP_FAIL;
  }
  ESP_LOGD(TAG, "preview camera device opened: %s", opened_dev);

  esp_err_t ret = ESP_FAIL;
  struct v4l2_requestbuffers req = {0};
  struct v4l2_buffer buf = {0};
  const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  void *mmap_ptrs[BSP_CAMERA_MMAP_BUFFERS] = {0};
  size_t mmap_sizes[BSP_CAMERA_MMAP_BUFFERS] = {0};
  bool stream_on = false;

  uint32_t frame_w = 0;
  uint32_t frame_h = 0;
  uint32_t bytesperline = 0;
  uint32_t pixel_format = 0;

  // OV2710 outputs RAW10 at 1920x1080, ISP converts to RGB565
  // Don't try to set unsupported resolutions directly - let ISP handle it
  // Try RGB565 first (ISP output), then RGB24
  if (bsp_camera_try_set_capture_format(fd, V4L2_PIX_FMT_RGB565, &frame_w,
                                        &frame_h, &bytesperline)) {
    pixel_format = V4L2_PIX_FMT_RGB565;
    ESP_LOGD(TAG,
             "preview format RGB565: %" PRIu32 "x%" PRIu32 " (ISP processed)",
             frame_w, frame_h);
  } else if (bsp_camera_try_set_capture_format(fd, V4L2_PIX_FMT_RGB24, &frame_w,
                                               &frame_h, &bytesperline)) {
    pixel_format = V4L2_PIX_FMT_RGB24;
    ESP_LOGD(TAG,
             "preview format RGB24: %" PRIu32 "x%" PRIu32 " (ISP processed)",
             frame_w, frame_h);
  } else {
    ESP_LOGE(TAG, "Failed to set any supported capture format for preview");
    goto cleanup;
  }

  req.count = BSP_CAMERA_MMAP_BUFFERS;
  req.type = type;
  req.memory = V4L2_MEMORY_MMAP;
  if (ioctl(fd, VIDIOC_REQBUFS, &req) != 0 || req.count < 1) {
    ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
    goto cleanup;
  }

  for (uint32_t i = 0; i < req.count && i < BSP_CAMERA_MMAP_BUFFERS; i++) {
    memset(&buf, 0, sizeof(buf));
    buf.type = type;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = i;
    if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) {
      ESP_LOGE(TAG, "VIDIOC_QUERYBUF failed idx=%" PRIu32, i);
      goto cleanup;
    }
    mmap_ptrs[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                        fd, buf.m.offset);
    if (mmap_ptrs[i] == MAP_FAILED) {
      mmap_ptrs[i] = NULL;
      ESP_LOGE(TAG, "mmap failed idx=%" PRIu32, i);
      goto cleanup;
    }
    mmap_sizes[i] = buf.length;
    if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
      ESP_LOGE(TAG, "VIDIOC_QBUF failed idx=%" PRIu32, i);
      goto cleanup;
    }
  }

  if (ioctl(fd, VIDIOC_STREAMON, (void *)&type) != 0) {
    ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
    goto cleanup;
  }
  stream_on = true;

  for (int i = 0; i < BSP_CAMERA_PREVIEW_SKIP_FRAMES; i++) {
    memset(&buf, 0, sizeof(buf));
    buf.type = type;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_DQBUF, &buf) != 0) {
      ESP_LOGE(TAG, "VIDIOC_DQBUF warmup failed");
      goto cleanup;
    }
    if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
      ESP_LOGE(TAG, "VIDIOC_QBUF warmup failed");
      goto cleanup;
    }
  }

  memset(&buf, 0, sizeof(buf));
  buf.type = type;
  buf.memory = V4L2_MEMORY_MMAP;
  if (ioctl(fd, VIDIOC_DQBUF, &buf) != 0) {
    ESP_LOGE(TAG, "VIDIOC_DQBUF failed");
    goto cleanup;
  }

  if (buf.index >= BSP_CAMERA_MMAP_BUFFERS || !mmap_ptrs[buf.index] ||
      buf.bytesused == 0) {
    ESP_LOGE(TAG, "invalid preview frame");
    goto cleanup;
  }

  if (frame_w < BSP_CAMERA_PREVIEW_SIZE || frame_h < BSP_CAMERA_PREVIEW_SIZE) {
    ESP_LOGE(TAG, "preview frame too small: %" PRIu32 "x%" PRIu32, frame_w,
             frame_h);
    goto cleanup;
  }
  if (bytesperline == 0) {
    bytesperline =
        frame_w * (pixel_format == V4L2_PIX_FMT_RGB565
                       ? 2
                       : (pixel_format == V4L2_PIX_FMT_RGB24 ? 3 : 1));
  }

  *rgb565_data =
      heap_caps_malloc(BSP_CAMERA_PREVIEW_SIZE * BSP_CAMERA_PREVIEW_SIZE * 2,
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!*rgb565_data) {
    *rgb565_data =
        heap_caps_malloc(BSP_CAMERA_PREVIEW_SIZE * BSP_CAMERA_PREVIEW_SIZE * 2,
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!*rgb565_data) {
    ret = ESP_ERR_NO_MEM;
    goto cleanup;
  }

  const uint8_t *src = (const uint8_t *)mmap_ptrs[buf.index];

  // Return buffer to camera immediately after reading to avoid frame
  // accumulation
  if (ioctl(fd, VIDIOC_QBUF, &buf) != 0) {
    ESP_LOGW(TAG, "VIDIOC_QBUF failed after capture");
  }

  // Center-square crop from source, then sample to 240x240.
  // This keeps coordinates in-bounds and avoids quadrant/noise artifacts.
  const uint32_t src_side = (frame_w < frame_h) ? frame_w : frame_h;
  const uint32_t crop_x = (frame_w - src_side) / 2;
  const uint32_t crop_y = (frame_h - src_side) / 2;

  for (uint32_t y = 0; y < BSP_CAMERA_PREVIEW_SIZE; y++) {
    uint8_t *dst_line = *rgb565_data + y * BSP_CAMERA_PREVIEW_SIZE * 2;
    const uint32_t src_y =
        crop_y + (((uint64_t)y * src_side) / BSP_CAMERA_PREVIEW_SIZE);
    const uint8_t *src_line = src + src_y * bytesperline;

    if (pixel_format == V4L2_PIX_FMT_RGB565) {
      for (uint32_t x = 0; x < BSP_CAMERA_PREVIEW_SIZE; x++) {
        const uint32_t src_x =
            crop_x + (((uint64_t)x * src_side) / BSP_CAMERA_PREVIEW_SIZE);
        const uint8_t *src_pixel = src_line + src_x * 2;
        // Keep byte order as delivered by the camera/ISP path.
        dst_line[x * 2] = src_pixel[0];
        dst_line[x * 2 + 1] = src_pixel[1];
      }
    } else if (pixel_format == V4L2_PIX_FMT_RGB24) {
      // Convert RGB24 to RGB565 with center-square sampling
      for (uint32_t x = 0; x < BSP_CAMERA_PREVIEW_SIZE; x++) {
        const uint32_t src_x =
            crop_x + (((uint64_t)x * src_side) / BSP_CAMERA_PREVIEW_SIZE);
        const uint8_t *src_pixel = src_line + src_x * 3;
        uint8_t r = src_pixel[0];
        uint8_t g = src_pixel[1];
        uint8_t b = src_pixel[2];
        uint16_t rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        dst_line[x * 2] = (uint8_t)(rgb565 & 0xFF);
        dst_line[x * 2 + 1] = (uint8_t)(rgb565 >> 8);
      }
    }
  }
  *width = BSP_CAMERA_PREVIEW_SIZE;
  *height = BSP_CAMERA_PREVIEW_SIZE;
  ret = ESP_OK;

cleanup:
  if (stream_on) {
    (void)ioctl(fd, VIDIOC_STREAMOFF, (void *)&type);
  }
  for (int i = 0; i < BSP_CAMERA_MMAP_BUFFERS; i++) {
    if (mmap_ptrs[i]) {
      (void)munmap(mmap_ptrs[i], mmap_sizes[i]);
    }
  }
  close(fd);

  if (ret != ESP_OK) {
    free(*rgb565_data);
    *rgb565_data = NULL;
    *width = 0;
    *height = 0;
  }
  return ret;
}

esp_err_t bsp_audio_capture_blocking(const bsp_audio_capture_cfg_t *cfg,
                                     uint8_t *buffer, size_t buffer_len,
                                     size_t *captured_bytes) {
  if (!cfg || !buffer || !captured_bytes || buffer_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!s_audio_ready) {
    return ESP_ERR_INVALID_STATE;
  }

  const uint32_t sample_rate =
      cfg->sample_rate_hz ? cfg->sample_rate_hz : 16000;
  if (sample_rate != 16000) {
    ESP_LOGW(TAG,
             "P4 microphone path currently captures at 16 kHz (requested=%u)",
             (unsigned)sample_rate);
  }

  size_t total_out_samples = (16000u * cfg->capture_ms) / 1000u;
  if ((total_out_samples * sizeof(int16_t)) > buffer_len) {
    total_out_samples = buffer_len / sizeof(int16_t);
  }

  int16_t *out = (int16_t *)buffer;
  size_t out_samples_written = 0;
  int16_t raw_buf[512];

  while (out_samples_written < total_out_samples) {
    size_t remaining_out_samples = total_out_samples - out_samples_written;
    size_t request_bytes = remaining_out_samples * sizeof(int16_t) *
                           2; /* Prefer stereo read then downmix. */
    if (request_bytes > sizeof(raw_buf)) {
      request_bytes = sizeof(raw_buf);
    }

    size_t bytes_read = 0;
    esp_err_t read_err = bsp_extra_pdm_i2s_read(
        raw_buf, request_bytes, &bytes_read, BSP_AUDIO_READ_TIMEOUT_MS);
    if (read_err == ESP_ERR_TIMEOUT) {
      memset(out + out_samples_written, 0,
             (total_out_samples - out_samples_written) * sizeof(int16_t));
      out_samples_written = total_out_samples;
      break;
    }
    if (read_err != ESP_OK) {
      return read_err;
    }
    if (bytes_read == 0) {
      memset(out + out_samples_written, 0,
             (total_out_samples - out_samples_written) * sizeof(int16_t));
      out_samples_written = total_out_samples;
      break;
    }

    const size_t raw_samples = bytes_read / sizeof(int16_t);
    if (raw_samples >= 2 && (raw_samples % 2) == 0) {
      /* Stereo interleaved -> keep left channel for mono app pipeline. */
      for (size_t i = 0;
           i + 1 < raw_samples && out_samples_written < total_out_samples;
           i += 2) {
        out[out_samples_written++] = raw_buf[i];
      }
    } else {
      /* Fallback if stream arrives as mono PCM16. */
      for (size_t i = 0;
           i < raw_samples && out_samples_written < total_out_samples; i++) {
        out[out_samples_written++] = raw_buf[i];
      }
    }
  }

  *captured_bytes = out_samples_written * sizeof(int16_t);
  ESP_LOGI(TAG, "Audio captured: 16000 Hz, 16-bit, 1 ch, %u ms, %u bytes",
           (unsigned)cfg->capture_ms, (unsigned)*captured_bytes);
  return ESP_OK;
}
