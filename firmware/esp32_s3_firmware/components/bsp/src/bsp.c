#include "bsp.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "secret.h"

static const char *TAG = "bsp";

/* Board pin map from docs/ESP-IDF examples (ESP32-S3-Touch-LCD-2). */
#define BSP_SPI_HOST SPI2_HOST
#define BSP_PIN_NUM_LCD_SCLK 39
#define BSP_PIN_NUM_LCD_MOSI 38
#define BSP_PIN_NUM_LCD_MISO 40
#define BSP_PIN_NUM_LCD_DC 42
#define BSP_PIN_NUM_LCD_CS 45
#define BSP_PIN_NUM_LCD_RST -1
#define BSP_LCD_PIXEL_CLOCK_HZ (80 * 1000 * 1000)
#define BSP_LCD_H_RES 240
#define BSP_LCD_V_RES 320

#define BSP_I2C_NUM I2C_NUM_0
#define BSP_PIN_NUM_I2C_SDA 48
#define BSP_PIN_NUM_I2C_SCL 47

#define BSP_LCD_BK_LIGHT_GPIO 1
#define BSP_BUTTON_GPIO 18
#define BSP_BUTTON_ACTIVE_LEVEL 1

#define BSP_I2S_PORT I2S_NUM_0
/* INMP441 wiring on accessible header pins (UART kept free). */
#define BSP_MIC_BCLK_GPIO 16
#define BSP_MIC_WS_GPIO 17
#define BSP_MIC_SD_GPIO 21
#define BSP_WIFI_MAXIMUM_RETRY 8
#define BSP_WIFI_WAIT_TIMEOUT_MS 20000
#define BSP_LVGL_TICK_PERIOD_MS 2
#define BSP_LVGL_TASK_MIN_DELAY_MS 1
#define BSP_LVGL_TASK_MAX_DELAY_MS 500
#define BSP_LVGL_TASK_STACK (6 * 1024)
#define BSP_LVGL_TASK_PRIORITY 4

static SemaphoreHandle_t s_lvgl_mutex;
static lv_disp_drv_t s_disp_drv;
static esp_lcd_panel_handle_t s_panel_handle;
static esp_lcd_touch_handle_t s_touch_handle;
static i2s_chan_handle_t s_i2s_rx_handle;
static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_num;

#define BSP_WIFI_CONNECTED_BIT BIT0
#define BSP_WIFI_FAIL_BIT BIT1

static bool bsp_notify_lvgl_flush_ready(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *edata,
    void *user_ctx
)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    lv_disp_flush_ready(&s_disp_drv);
    return false;
}

static void bsp_increase_lvgl_tick(void *arg)
{
    (void)arg;
    lv_tick_inc(BSP_LVGL_TICK_PERIOD_MS);
}

static void bsp_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    (void)drv;
    esp_lcd_panel_draw_bitmap(
        s_panel_handle,
        area->x1,
        area->y1,
        area->x2 + 1,
        area->y2 + 1,
        color_map
    );
}

static void bsp_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    uint16_t x = 0;
    uint16_t y = 0;
    uint8_t touch_cnt = 0;

    esp_lcd_touch_read_data(s_touch_handle);
    bool pressed = esp_lcd_touch_get_coordinates(
        s_touch_handle,
        &x,
        &y,
        NULL,
        &touch_cnt,
        1
    );

    if (pressed && touch_cnt > 0) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void bsp_lvgl_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t task_delay_ms = BSP_LVGL_TASK_MAX_DELAY_MS;

        if (bsp_lvgl_lock(-1)) {
            task_delay_ms = lv_timer_handler();
            bsp_lvgl_unlock();
        }

        if (task_delay_ms > BSP_LVGL_TASK_MAX_DELAY_MS) {
            task_delay_ms = BSP_LVGL_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < BSP_LVGL_TASK_MIN_DELAY_MS) {
            task_delay_ms = BSP_LVGL_TASK_MIN_DELAY_MS;
        }

        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

static esp_err_t bsp_lcd_init(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num = BSP_PIN_NUM_LCD_SCLK,
        .mosi_io_num = BSP_PIN_NUM_LCD_MOSI,
        .miso_io_num = BSP_PIN_NUM_LCD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BSP_LCD_H_RES * 80 * sizeof(lv_color_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi init");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BSP_PIN_NUM_LCD_DC,
        .cs_gpio_num = BSP_PIN_NUM_LCD_CS,
        .pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = bsp_notify_lvgl_flush_ready,
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_SPI_HOST, &io_config, &io_handle),
        TAG,
        "panel io init"
    );

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io_handle, &panel_config, &s_panel_handle), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel_handle), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel_handle), TAG, "panel init2");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel_handle, false, false), TAG, "panel mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel_handle, false), TAG, "panel swap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel_handle, true), TAG, "panel on");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel_handle, true), TAG, "panel invert");

    gpio_set_direction(BSP_LCD_BK_LIGHT_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BSP_LCD_BK_LIGHT_GPIO, 1);
    return ESP_OK;
}

static esp_err_t bsp_touch_init(void)
{
    const i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BSP_PIN_NUM_I2C_SDA,
        .scl_io_num = BSP_PIN_NUM_I2C_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };

    ESP_RETURN_ON_ERROR(i2c_param_config(BSP_I2C_NUM, &i2c_conf), TAG, "i2c param");
    ESP_RETURN_ON_ERROR(i2c_driver_install(BSP_I2C_NUM, i2c_conf.mode, 0, 0, 0), TAG, "i2c install");

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    /* Using legacy i2c driver (v1 API): scl_speed_hz must be zero. */
    tp_io_config.scl_speed_hz = 0;
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)BSP_I2C_NUM, &tp_io_config, &tp_io_handle),
        TAG,
        "touch io"
    );

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_V_RES,
        .y_max = BSP_LCD_H_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &s_touch_handle), TAG, "touch init");
    return ESP_OK;
}

static esp_err_t bsp_lvgl_init(void)
{
    lv_init();
    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_lvgl_mutex) {
        return ESP_ERR_NO_MEM;
    }

    static lv_disp_draw_buf_t draw_buf;
    const size_t buf_pixels = BSP_LCD_H_RES * 40;
    lv_color_t *buf1 = heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_color_t *buf2 = heap_caps_malloc(buf_pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf1 || !buf2) {
        return ESP_ERR_NO_MEM;
    }
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_pixels);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = BSP_LCD_H_RES;
    s_disp_drv.ver_res = BSP_LCD_V_RES;
    s_disp_drv.flush_cb = bsp_lvgl_flush_cb;
    s_disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&s_disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = bsp_lvgl_touch_cb;
    lv_indev_drv_register(&indev_drv);

    const esp_timer_create_args_t tick_timer_args = {
        .callback = bsp_increase_lvgl_tick,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_timer_args, &lvgl_tick_timer), TAG, "tick timer create");
    ESP_RETURN_ON_ERROR(
        esp_timer_start_periodic(lvgl_tick_timer, BSP_LVGL_TICK_PERIOD_MS * 1000),
        TAG,
        "tick timer start"
    );

    BaseType_t task_ok = xTaskCreatePinnedToCore(
        bsp_lvgl_task,
        "lvgl_task",
        BSP_LVGL_TASK_STACK,
        NULL,
        BSP_LVGL_TASK_PRIORITY,
        NULL,
        1
    );
    if (task_ok != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t bsp_button_init(void)
{
    const gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BSP_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io_conf);
}

static esp_err_t bsp_audio_init(void)
{
    const i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BSP_I2S_PORT, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_i2s_rx_handle), TAG, "i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BSP_MIC_BCLK_GPIO,
            .ws = BSP_MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = BSP_MIC_SD_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_i2s_rx_handle, &std_cfg), TAG, "i2s std init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_i2s_rx_handle), TAG, "i2s enable");
    ESP_LOGI(TAG, "I2S mic init ok (BCLK=%d WS=%d SD=%d)", BSP_MIC_BCLK_GPIO, BSP_MIC_WS_GPIO, BSP_MIC_SD_GPIO);
    return ESP_OK;
}

static void bsp_wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry_num < BSP_WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_wifi_retry_num++;
            ESP_LOGW(TAG, "Wi-Fi reconnect attempt %d/%d", s_wifi_retry_num, BSP_WIFI_MAXIMUM_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, BSP_WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_retry_num = 0;
        ESP_LOGI(TAG, "Wi-Fi connected, got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, BSP_WIFI_CONNECTED_BIT);
    }
}

static esp_err_t bsp_wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init");
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        return loop_err;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init");

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &bsp_wifi_event_handler, NULL, &instance_any_id),
        TAG,
        "wifi event register"
    );
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &bsp_wifi_event_handler, NULL, &instance_got_ip),
        TAG,
        "ip event register"
    );

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, SECRET_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, SECRET_WIFI_PASS, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "esp_wifi_set_config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start");

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        BSP_WIFI_CONNECTED_BIT | BSP_WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(BSP_WIFI_WAIT_TIMEOUT_MS)
    );

    if (bits & BSP_WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi ready");
        return ESP_OK;
    }
    if (bits & BSP_WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Wi-Fi failed after retries");
        return ESP_FAIL;
    }

    ESP_LOGE(TAG, "Wi-Fi connection timeout");
    return ESP_ERR_TIMEOUT;
}

esp_err_t bsp_init(void)
{
    ESP_LOGI(TAG, "Init BSP");
    ESP_RETURN_ON_ERROR(bsp_lcd_init(), TAG, "lcd init failed");
    ESP_RETURN_ON_ERROR(bsp_touch_init(), TAG, "touch init failed");
    ESP_RETURN_ON_ERROR(bsp_lvgl_init(), TAG, "lvgl init failed");
    ESP_RETURN_ON_ERROR(bsp_button_init(), TAG, "button init failed");
    ESP_RETURN_ON_ERROR(bsp_audio_init(), TAG, "audio i2s init failed");
    ESP_RETURN_ON_ERROR(bsp_wifi_init(), TAG, "wifi init failed");
    return ESP_OK;
}

bool bsp_lvgl_lock(int timeout_ms)
{
    if (!s_lvgl_mutex) {
        return false;
    }
    const TickType_t timeout_ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lvgl_mutex, timeout_ticks) == pdTRUE;
}

void bsp_lvgl_unlock(void)
{
    if (s_lvgl_mutex) {
        xSemaphoreGiveRecursive(s_lvgl_mutex);
    }
}

esp_err_t bsp_display_show_status(const char *status_text)
{
    ESP_LOGI(TAG, "[DISPLAY][STATUS] %s", status_text ? status_text : "(null)");
    return ESP_OK;
}

esp_err_t bsp_display_show_text(const char *body_text)
{
    ESP_LOGI(TAG, "[DISPLAY][TEXT] %s", body_text ? body_text : "(null)");
    return ESP_OK;
}

bool bsp_button_is_pressed(void)
{
    return gpio_get_level(BSP_BUTTON_GPIO) == BSP_BUTTON_ACTIVE_LEVEL;
}

esp_err_t bsp_audio_capture_blocking(
    const bsp_audio_capture_cfg_t *cfg,
    uint8_t *buffer,
    size_t buffer_len,
    size_t *captured_bytes
)
{
    if (!cfg || !buffer || !captured_bytes || buffer_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_i2s_rx_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    const uint32_t sample_rate = cfg->sample_rate_hz ? cfg->sample_rate_hz : 16000;
    const size_t total_samples_target = (sample_rate * cfg->capture_ms) / 1000;
    size_t total_out_samples = total_samples_target;
    if (total_out_samples * sizeof(int16_t) > buffer_len) {
        total_out_samples = buffer_len / sizeof(int16_t);
    }

    int16_t *out = (int16_t *)buffer;
    size_t out_samples_written = 0;
    int32_t raw_chunk[256];

    uint32_t timeout_count = 0;
    while (out_samples_written < total_out_samples) {
        size_t bytes_read = 0;
        esp_err_t read_err = i2s_channel_read(
            s_i2s_rx_handle,
            raw_chunk,
            sizeof(raw_chunk),
            &bytes_read,
            pdMS_TO_TICKS(350)
        );
        if (read_err == ESP_ERR_TIMEOUT) {
            timeout_count++;
            if (timeout_count == 1) {
                ESP_LOGW(TAG, "I2S timeout; trying channel recovery");
                i2s_channel_disable(s_i2s_rx_handle);
                i2s_channel_enable(s_i2s_rx_handle);
                continue;
            }

            /* If there is still no data, return silence instead of failing hard. */
            while (out_samples_written < total_out_samples) {
                out[out_samples_written++] = 0;
            }
            ESP_LOGW(TAG, "I2S no data from mic; returning silence. Check SD/BCLK/WS wiring and L/R pin.");
            break;
        }
        if (read_err != ESP_OK) {
            return read_err;
        }

        size_t raw_samples = bytes_read / sizeof(int32_t);
        for (size_t i = 0; (i + 1) < raw_samples && out_samples_written < total_out_samples; i += 2) {
            /* Interleaved stereo frames: pick the stronger channel (L/R pin agnostic). */
            int32_t sample_l = raw_chunk[i] >> 12;
            int32_t sample_r = raw_chunk[i + 1] >> 12;
            int32_t sample = (abs(sample_l) >= abs(sample_r)) ? sample_l : sample_r;
            if (sample > INT16_MAX) {
                sample = INT16_MAX;
            } else if (sample < INT16_MIN) {
                sample = INT16_MIN;
            }
            out[out_samples_written++] = (int16_t)sample;
        }
    }

    *captured_bytes = out_samples_written * sizeof(int16_t);
    ESP_LOGI(
        TAG,
        "Audio captured: %u Hz, %u-bit, %u ch, %u ms, %u bytes",
        (unsigned)cfg->sample_rate_hz,
        (unsigned)cfg->bits_per_sample,
        (unsigned)cfg->channels,
        (unsigned)cfg->capture_ms,
        (unsigned)*captured_bytes
    );

    return ESP_OK;
}
