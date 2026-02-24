#include "app_storage.h"

#include "bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *TAG = "app_storage";

// Track SD card mount state
static bool s_sd_mounted = false;

// PSRAM buffer for queued JPEG images (to avoid SDMMC conflicts during network
// I/O)
#define MAX_QUEUED_IMAGES 2        // Reduced to 2 for easier testing
#define MAX_JPEG_SIZE (256 * 1024) // 256KB max per image
#define INACTIVITY_TIMEOUT_MS                                                  \
  (10 * 1000) // 10 seconds of inactivity before saving
#define MIN_QUEUE_FOR_IMMEDIATE_SAVE                                           \
  1 // Save immediately if queue is almost full (1 of 2)

typedef struct {
  uint8_t *data; // Allocated in PSRAM
  size_t len;
  bool valid;
  time_t timestamp; // When image was queued
} queued_image_t;

typedef struct {
  uint8_t *data; // Allocated in PSRAM
  size_t len;
  uint32_t sample_rate_hz;
  bool valid;
  time_t timestamp;
} queued_audio_t;

static queued_image_t s_image_queue[MAX_QUEUED_IMAGES];
static int s_queue_head = 0;
static int s_queue_tail = 0;
static int s_queue_count = 0;

static queued_audio_t s_audio_queue[MAX_QUEUED_IMAGES];
static int s_audio_queue_head = 0;
static int s_audio_queue_tail = 0;
static int s_audio_queue_count = 0;

// Task and synchronization for opportunistic saving
static TaskHandle_t s_save_task_handle = NULL;
static TimerHandle_t s_inactivity_timer = NULL;
static bool s_save_task_running = false;
static SemaphoreHandle_t s_sd_mount_mutex =
    NULL; // Protect SD mount/unmount operations
static SemaphoreHandle_t s_queue_mutex =
    NULL; // Protect queued image ring buffer
static volatile bool s_storage_busy = false;
static bool s_mount_after_camera_attempted = false;

// Directory paths
#define SD_BASE_PATH "/sdcard"
#define SD_MEDIA_PATH SD_BASE_PATH "/media"
#define SD_IMAGES_PATH SD_MEDIA_PATH "/images"

/**
 * @brief Create directory if it doesn't exist
 */
static esp_err_t create_directory_if_not_exists(const char *path) {
  struct stat st = {0};
  if (stat(path, &st) == 0) {
    if (S_ISDIR(st.st_mode)) {
      return ESP_OK;
    } else {
      ESP_LOGE(TAG, "Path exists but is not a directory: %s", path);
      return ESP_FAIL;
    }
  }

  // Directory doesn't exist, create it
  ESP_LOGI(TAG, "Attempting to create directory: %s", path);
  if (mkdir(path, 0755) != 0) {
    if (errno == EEXIST) {
      return ESP_OK; /* Race condition? Still fine */
    }
    ESP_LOGE(TAG, "Failed to create directory '%s' (errno: %d, %s)", path,
             errno, strerror(errno));
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Created directory: %s", path);
  return ESP_OK;
}

/**
 * @brief Ensure all required directories exist
 */
static esp_err_t ensure_directory_structure(void) {
  esp_err_t ret = ESP_OK;

  ret = create_directory_if_not_exists(SD_MEDIA_PATH);
  if (ret != ESP_OK)
    return ret;

  ret = create_directory_if_not_exists(SD_IMAGES_PATH);
  if (ret != ESP_OK)
    return ret;

  ret = create_directory_if_not_exists(SD_MEDIA_PATH "/audio");
  if (ret != ESP_OK)
    return ret;

  ret = create_directory_if_not_exists(SD_BASE_PATH "/logs");
  if (ret != ESP_OK)
    return ret;

  ret = create_directory_if_not_exists(SD_BASE_PATH "/logs/chat");
  if (ret != ESP_OK)
    return ret;

  ret = create_directory_if_not_exists(SD_BASE_PATH "/data");
  if (ret != ESP_OK)
    return ret;

  ESP_LOGI(TAG, "Directory structure verified");
  return ESP_OK;
}

/**
 * @brief Validate JPEG data integrity
 */
static bool validate_jpeg(const uint8_t *data, size_t len) {
  if (len < 4) {
    return false;
  }

  // Check JPEG start marker (FF D8)
  if (data[0] != 0xFF || data[1] != 0xD8) {
    return false;
  }

  // Check JPEG end marker (FF D9) - best effort
  if (len >= 2 && data[len - 2] == 0xFF && data[len - 1] == 0xD9) {
    return true;
  }

  // If no end marker, still accept if it starts correctly
  // (some JPEGs might be truncated in transmission)
  return true;
}

// Forward declaration
static void app_storage_process_queue_internal(void);

static int app_queue_count_locked(void) {
  return s_queue_count + s_audio_queue_count;
}

static int app_queue_get_count(void) {
  if (s_queue_mutex == NULL) {
    return 0;
  }
  if (xSemaphoreTake(s_queue_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    return 0;
  }
  int count = app_queue_count_locked();
  xSemaphoreGive(s_queue_mutex);
  return count;
}

static void app_queue_clear(void) {
  if (s_queue_mutex == NULL) {
    return;
  }
  if (xSemaphoreTake(s_queue_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return;
  }
  while (s_queue_count > 0) {
    if (s_image_queue[s_queue_head].valid && s_image_queue[s_queue_head].data) {
      free(s_image_queue[s_queue_head].data);
      s_image_queue[s_queue_head].data = NULL;
    }
    s_image_queue[s_queue_head].len = 0;
    s_image_queue[s_queue_head].timestamp = 0;
    s_image_queue[s_queue_head].valid = false;
    s_queue_head = (s_queue_head + 1) % MAX_QUEUED_IMAGES;
    s_queue_count--;
  }
  s_queue_head = 0;
  s_queue_tail = 0;

  while (s_audio_queue_count > 0) {
    if (s_audio_queue[s_audio_queue_head].valid &&
        s_audio_queue[s_audio_queue_head].data) {
      free(s_audio_queue[s_audio_queue_head].data);
      s_audio_queue[s_audio_queue_head].data = NULL;
    }
    s_audio_queue[s_audio_queue_head].len = 0;
    s_audio_queue[s_audio_queue_head].timestamp = 0;
    s_audio_queue[s_audio_queue_head].valid = false;
    s_audio_queue_head = (s_audio_queue_head + 1) % MAX_QUEUED_IMAGES;
    s_audio_queue_count--;
  }
  s_audio_queue_head = 0;
  s_audio_queue_tail = 0;

  xSemaphoreGive(s_queue_mutex);
}

static bool app_queue_pop(queued_image_t *out_item) {
  if (!out_item || s_queue_mutex == NULL) {
    return false;
  }
  if (xSemaphoreTake(s_queue_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }
  if (s_queue_count == 0) {
    xSemaphoreGive(s_queue_mutex);
    return false;
  }

  *out_item = s_image_queue[s_queue_head];
  s_image_queue[s_queue_head].data = NULL;
  s_image_queue[s_queue_head].len = 0;
  s_image_queue[s_queue_head].timestamp = 0;
  s_image_queue[s_queue_head].valid = false;
  s_queue_head = (s_queue_head + 1) % MAX_QUEUED_IMAGES;
  s_queue_count--;

  xSemaphoreGive(s_queue_mutex);
  return true;
}

static bool app_queue_pop_audio(queued_audio_t *out_item) {
  if (!out_item || s_queue_mutex == NULL) {
    return false;
  }
  if (xSemaphoreTake(s_queue_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }
  if (s_audio_queue_count == 0) {
    xSemaphoreGive(s_queue_mutex);
    return false;
  }

  *out_item = s_audio_queue[s_audio_queue_head];
  s_audio_queue[s_audio_queue_head].data = NULL;
  s_audio_queue[s_audio_queue_head].len = 0;
  s_audio_queue[s_audio_queue_head].timestamp = 0;
  s_audio_queue[s_audio_queue_head].valid = false;
  s_audio_queue_head = (s_audio_queue_head + 1) % MAX_QUEUED_IMAGES;
  s_audio_queue_count--;

  xSemaphoreGive(s_queue_mutex);
  return true;
}

/**
 * @brief Timer callback for inactivity detection
 */
static void inactivity_timer_callback(TimerHandle_t xTimer) {
  (void)xTimer;
  // Notify save task that inactivity period has elapsed
  if (s_save_task_handle != NULL) {
    xTaskNotifyGive(s_save_task_handle);
  }
}

/**
 * @brief Task for opportunistic SD card saving
 *
 * This task monitors inactivity and saves queued images when:
 * 1. User has been inactive for INACTIVITY_TIMEOUT_MS
 * 2. Queue is almost full (MIN_QUEUE_FOR_IMMEDIATE_SAVE)
 *
 * This ensures SD card is only accessed when system is idle,
 * avoiding DMA conflicts with network/camera operations.
 */
static void opportunistic_save_task(void *pvParameters) {
  ESP_LOGI(TAG, "Opportunistic save task started");

  while (1) {
    // Wait for notification (inactivity timer or queue full)
    uint32_t notification = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (notification == 0) {
      continue;
    }

    // Check if we have images to save
    const int queued_now = app_queue_get_count();
    if (queued_now <= 0) {
      continue;
    }

    // Check if queue is almost full - save immediately
    bool immediate_save = (queued_now >= MIN_QUEUE_FOR_IMMEDIATE_SAVE);

    if (immediate_save) {
      ESP_LOGI(TAG, "Queue almost full (%d/%d), saving immediately", queued_now,
               MAX_QUEUED_IMAGES);
    } else {
      ESP_LOGI(TAG, "Inactivity detected, saving %d queued images", queued_now);
    }

    // Process queue (mount, save all, unmount)
    app_storage_process_queue_internal();
  }
}

/**
 * @brief Internal function to process queue (called by task)
 */
static void app_storage_process_queue_internal(void) {
  s_storage_busy = true;

  const int pending = app_queue_get_count();
  if (pending <= 0) {
    s_storage_busy = false;
    return; // Nothing to process
  }

  if (!bsp_sdcard_is_present()) {
    ESP_LOGW(TAG, "SD card not present, clearing %d queued images", pending);
    app_queue_clear();
    s_storage_busy = false;
    return;
  }

  ESP_LOGI(TAG, "Processing %d queued images (system idle, optimal timing)",
           pending);

  if (s_sd_mount_mutex == NULL) {
    ESP_LOGW(TAG, "SD mount mutex not initialized, skipping save attempt");
    s_storage_busy = false;
    return;
  }

  // Tenta adquirir mutex com timeout curto para nao bloquear LVGL task.
  // Se nao conseguir, reagenda via timer e retorna imediatamente.
  if (xSemaphoreTake(s_sd_mount_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    ESP_LOGW(TAG, "Failed to acquire SD mount mutex, rescheduling");
    if (s_inactivity_timer != NULL) {
      xTimerStart(s_inactivity_timer, 0);
    }
    s_storage_busy = false;
    return;
  }

  // Verifica memoria DMA disponivel ANTES de tentar montar.
  // Nao faz vTaskDelay aqui - isso bloquearia o sistema inteiro.
  size_t free_heap = esp_get_free_heap_size();
  size_t free_internal = esp_get_free_internal_heap_size();
  size_t largest_block =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t largest_dma = heap_caps_get_largest_free_block(
      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);

  ESP_LOGI(TAG,
           "Memory before SD mount: Total=%u, Internal=%u, Largest=%u, "
           "LargestDMA=%u",
           (unsigned)free_heap, (unsigned)free_internal,
           (unsigned)largest_block, (unsigned)largest_dma);

  // Requer pelo menos 24KB contiguos DMA para o FATFS/SDMMC.
  const size_t min_dma_required = 24 * 1024;

  ESP_LOGI(TAG,
           "DMA memory diagnostic: LargestDMA=%u bytes, Internal=%u bytes, "
           "LargestBlock=%u bytes (non-DMA)",
           (unsigned)largest_dma, (unsigned)free_internal,
           (unsigned)largest_block);

  if (largest_dma < min_dma_required) {
    ESP_LOGW(TAG,
             "Insufficient DMA memory for SD mount: %u bytes (need %u). "
             "Will retry later. Queue: %d images",
             (unsigned)largest_dma, (unsigned)min_dma_required,
             app_queue_get_count());
    xSemaphoreGive(s_sd_mount_mutex);
    // Reagenda com timer para tentar novamente mais tarde
    if (s_inactivity_timer != NULL) {
      xTimerStart(s_inactivity_timer, 0);
    }
    s_storage_busy = false;
    return;
  }

  ESP_LOGI(TAG, "Sufficient DMA memory available: %u bytes (need %u)",
           (unsigned)largest_dma, (unsigned)min_dma_required);

  // Monta SD somente se ainda nao estiver montado.
  // NUNCA desmonta/remonta: ciclos de mount/unmount fragmentam a memoria
  // interna DMA e levam ao ESP_ERR_NO_MEM na proxima tentativa.
  if (!s_sd_mounted) {
    ESP_LOGI(TAG, "Mounting SD card (1-bit, DEFAULT freq)");
    esp_err_t mount_ret = bsp_sdcard_mount();

    if (mount_ret != ESP_OK) {
      ESP_LOGW(TAG, "Failed to mount SD: %s. Queue: %d images remain",
               esp_err_to_name(mount_ret), app_queue_get_count());
      xSemaphoreGive(s_sd_mount_mutex);
      // Reagenda para tentar novamente
      if (s_inactivity_timer != NULL) {
        xTimerStart(s_inactivity_timer, 0);
      }
      s_storage_busy = false;
      return;
    }

    s_sd_mounted = true;
    bsp_lvgl_lock(-1);
    ensure_directory_structure();
    bsp_lvgl_unlock();
    ESP_LOGI(TAG, "SD card mounted successfully (will remain mounted)");
  } else {
    ESP_LOGI(TAG, "SD card already mounted, proceeding to save");
    bsp_lvgl_lock(-1);
    ensure_directory_structure();
    bsp_lvgl_unlock();
  }

  // Processa fila: salva todas as imagens e audios
  int saved_count = 0;
  int failed_count = 0;

  queued_image_t image_item = {0};
  while (app_queue_pop(&image_item)) {
    if (!image_item.valid || !image_item.data || image_item.len == 0) {
      continue;
    }

    esp_err_t save_ret =
        app_storage_save_image(image_item.data, image_item.len);
    if (save_ret == ESP_OK) {
      saved_count++;
    } else {
      failed_count++;
      ESP_LOGW(TAG, "Failed to save queued image: %s",
               esp_err_to_name(save_ret));
    }

    // Libera buffer PSRAM
    free(image_item.data);
    image_item.data = NULL;
    image_item.valid = false;
    image_item.len = 0;
  }

  queued_audio_t audio_item = {0};
  while (app_queue_pop_audio(&audio_item)) {
    if (!audio_item.valid || !audio_item.data || audio_item.len == 0) {
      continue;
    }

    esp_err_t save_ret = app_storage_save_audio(audio_item.data, audio_item.len,
                                                audio_item.sample_rate_hz);
    if (save_ret == ESP_OK) {
      saved_count++;
    } else {
      failed_count++;
      ESP_LOGW(TAG, "Failed to save queued audio: %s",
               esp_err_to_name(save_ret));
    }

    // Libera buffer PSRAM
    free(audio_item.data);
    audio_item.data = NULL;
    audio_item.valid = false;
    audio_item.len = 0;
  }

  ESP_LOGI(TAG, "Batch save complete (SD kept mounted): %d saved, %d failed",
           saved_count, failed_count);

  xSemaphoreGive(s_sd_mount_mutex);

  ESP_LOGI(TAG, "Storage queue flush finished");
  s_storage_busy = false;
}

esp_err_t app_storage_init(void) {
  ESP_LOGI(TAG, "Initializing storage subsystem with opportunistic saving");

  // Initialize SD card detection
  esp_err_t ret = bsp_sdcard_detect_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize SD card detection: %s",
             esp_err_to_name(ret));
    return ret;
  }

  // Initialize SD card detection only
  // Mount will happen opportunistically when system is idle
  if (bsp_sdcard_is_present()) {
    ESP_LOGI(TAG, "SD card detected (will save opportunistically when idle)");
  } else {
    ESP_LOGI(TAG, "SD card not present");
  }

  // Create mutex to protect SD mount/unmount operations
  s_sd_mount_mutex = xSemaphoreCreateMutex();
  if (s_sd_mount_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create SD mount mutex");
    return ESP_ERR_NO_MEM;
  }

  // Create opportunistic save task (low priority, runs when idle)
  if (xTaskCreate(opportunistic_save_task, "save_task",
                  8192, // Stack size (SD/FATFS path needs more stack)
                  NULL,
                  2, // Low priority (below camera/network tasks)
                  &s_save_task_handle) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create opportunistic save task");
    return ESP_ERR_NO_MEM;
  }

  // Create inactivity timer
  s_inactivity_timer =
      xTimerCreate("inactivity_timer", pdMS_TO_TICKS(INACTIVITY_TIMEOUT_MS),
                   pdFALSE, // One-shot (will be reset on interaction)
                   NULL, inactivity_timer_callback);

  if (s_inactivity_timer == NULL) {
    ESP_LOGE(TAG, "Failed to create inactivity timer");
    return ESP_ERR_NO_MEM;
  }

  s_queue_mutex = xSemaphoreCreateMutex();
  if (s_queue_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create queue mutex");
    return ESP_ERR_NO_MEM;
  }

  // Start the timer (will be reset on each interaction)
  xTimerStart(s_inactivity_timer, 0);

  s_save_task_running = true;
  ESP_LOGI(
      TAG,
      "Opportunistic save system initialized (saves after %d ms inactivity)",
      INACTIVITY_TIMEOUT_MS);

  return ESP_OK;
}

void app_storage_mount_after_camera_init(void) {
  // Mount once after camera path is alive. This avoids boot-time conflict
  // with esp_video device registration while still reducing runtime mount
  // failures caused by fragmented heap.
  if (s_mount_after_camera_attempted || s_sd_mounted) {
    return;
  }
  s_mount_after_camera_attempted = true;

  if (!bsp_sdcard_is_present()) {
    ESP_LOGD(TAG, "SD not present after camera init");
    return;
  }
  if (s_sd_mount_mutex == NULL) {
    ESP_LOGW(TAG, "SD mount mutex unavailable after camera init");
    return;
  }
  if (xSemaphoreTake(s_sd_mount_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    ESP_LOGW(TAG, "Could not acquire SD mutex after camera init");
    return;
  }

  if (!s_sd_mounted) {
    esp_err_t ret = bsp_sdcard_mount();
    if (ret == ESP_OK) {
      s_sd_mounted = true;
      (void)ensure_directory_structure();
      ESP_LOGI(TAG, "SD mounted after camera init (kept mounted)");
    } else {
      ESP_LOGW(TAG, "SD mount after camera init failed: %s",
               esp_err_to_name(ret));
    }
  }
  xSemaphoreGive(s_sd_mount_mutex);
}

esp_err_t app_storage_save_image(const uint8_t *jpeg_data, size_t jpeg_len) {
  if (!jpeg_data || jpeg_len == 0) {
    ESP_LOGE(TAG, "Invalid parameters");
    return ESP_ERR_INVALID_ARG;
  }

  // Validate JPEG
  if (!validate_jpeg(jpeg_data, jpeg_len)) {
    ESP_LOGE(TAG, "Invalid JPEG data (missing FF D8 marker)");
    return ESP_ERR_INVALID_ARG;
  }

  // Check if SD card is present
  if (!bsp_sdcard_is_present()) {
    ESP_LOGD(TAG, "SD card not present, skipping save");
    return ESP_ERR_NOT_FOUND;
  }

  // Check available memory before attempting mount
  // FATFS may need contiguous internal memory (not PSRAM) for buffers
  size_t free_heap = esp_get_free_heap_size();
  size_t free_internal = esp_get_free_internal_heap_size();
  size_t largest_free_block =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t min_free_heap = 80 * 1024; // Need at least 80KB free for SD mount
  size_t min_free_internal =
      32 * 1024;                     // FATFS needs contiguous internal memory
  size_t min_contiguous = 64 * 1024; // CRITICAL: Need at least 64KB contiguous
                                     // block for FATFS DMA buffers

  ESP_LOGI(TAG,
           "Memory check - Total: %u bytes, Internal: %u bytes, Largest "
           "contiguous: %u bytes",
           (unsigned)free_heap, (unsigned)free_internal,
           (unsigned)largest_free_block);

  if (free_heap < min_free_heap) {
    ESP_LOGW(TAG,
             "Insufficient total memory for SD mount: %u bytes free (need %u). "
             "Image not saved.",
             (unsigned)free_heap, (unsigned)min_free_heap);
    return ESP_ERR_NO_MEM;
  }

  if (free_internal < min_free_internal ||
      largest_free_block < min_contiguous) {
    ESP_LOGW(TAG,
             "Insufficient internal/contiguous memory: free=%u (need %u), "
             "largest_block=%u (need %u). FATFS requires 64KB contiguous "
             "internal memory for DMA buffers.",
             (unsigned)free_internal, (unsigned)min_free_internal,
             (unsigned)largest_free_block, (unsigned)min_contiguous);
    ESP_LOGW(
        TAG,
        "SD mount will fail. Memory fragmented by network/audio processing. "
        "SD should have been mounted after camera init.");
    return ESP_ERR_NO_MEM;
  }

  // Lazy mount: Mount SD card only when needed (not at boot to avoid ISP
  // conflict) The SDMMC host is already initialized by ESP-Hosted (C6
  // communication), but mounting the SD card filesystem conflicts with ISP
  // initialization. Mount only when actually saving a file.
  if (!s_sd_mounted) {
    ESP_LOGI(TAG, "Mounting SD card for image save...");

    // Add delay to ensure camera/ISP is fully initialized and stable
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_err_t ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
      ESP_LOGW(TAG,
               "Failed to mount SD card: %s (heap: %u bytes). "
               "Will retry on next save attempt.",
               esp_err_to_name(ret), (unsigned)free_heap);
      return ESP_FAIL;
    }
    s_sd_mounted = true;
    ensure_directory_structure();
    ESP_LOGI(TAG, "SD card mounted successfully for save operation");
  }

  // Ensure directories exist
  esp_err_t ret = ensure_directory_structure();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to ensure directories");
    // Continue anyway, might already exist
  }

  // Generate filename with timestamp
  char filename[128];
  time_t now = time(NULL);
  struct tm timeinfo;

  if (localtime_r(&now, &timeinfo) == NULL) {
    ESP_LOGE(TAG, "Failed to get local time");
    return ESP_FAIL;
  }

  snprintf(filename, sizeof(filename), "%s/I%02d%02d%02d.JPG", SD_IMAGES_PATH,
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

  /* Protect SPI bus shared with LCD */
  bsp_lvgl_lock(-1);
  FILE *f = fopen(filename, "wb");
  if (!f) {
    bsp_lvgl_unlock();
    ESP_LOGE(TAG, "Failed to open file for writing: %s (errno: %d, %s)",
             filename, errno, strerror(errno));
    return ESP_FAIL;
  }

  // Write JPEG data
  size_t written = fwrite(jpeg_data, 1, jpeg_len, f);
  fclose(f);
  bsp_lvgl_unlock();

  if (written != jpeg_len) {
    ESP_LOGE(TAG, "Failed to write complete file: %s (written: %u/%u bytes)",
             filename, (unsigned)written, (unsigned)jpeg_len);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Image saved successfully: %s (%u bytes)", filename,
           (unsigned)jpeg_len);
  return ESP_OK;
}

bool app_storage_is_ready(void) { return bsp_sdcard_is_present(); }

esp_err_t app_storage_queue_image(const uint8_t *jpeg_data, size_t jpeg_len) {
  if (!jpeg_data || jpeg_len == 0) {
    ESP_LOGE(TAG, "Invalid parameters for queue");
    return ESP_ERR_INVALID_ARG;
  }

  // Validate JPEG
  if (!validate_jpeg(jpeg_data, jpeg_len)) {
    ESP_LOGE(TAG, "Invalid JPEG data (missing FF D8 marker)");
    return ESP_ERR_INVALID_ARG;
  }

  // Check queue capacity
  if (s_queue_mutex == NULL ||
      xSemaphoreTake(s_queue_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGW(TAG, "Queue mutex unavailable, dropping frame");
    return ESP_FAIL;
  }

  if (s_queue_count >= MAX_QUEUED_IMAGES) {
    ESP_LOGW(TAG, "Image queue full (%d images), dropping oldest",
             s_queue_count);
    // Free oldest image
    if (s_image_queue[s_queue_head].valid && s_image_queue[s_queue_head].data) {
      free(s_image_queue[s_queue_head].data);
      s_image_queue[s_queue_head].data = NULL;
      s_image_queue[s_queue_head].valid = false;
      s_image_queue[s_queue_head].len = 0;
      s_image_queue[s_queue_head].timestamp = 0;
    }
    s_queue_head = (s_queue_head + 1) % MAX_QUEUED_IMAGES;
    s_queue_count--;
  }

  // Check JPEG size limit
  if (jpeg_len > MAX_JPEG_SIZE) {
    ESP_LOGW(TAG, "JPEG too large (%u bytes, max %u), truncating",
             (unsigned)jpeg_len, (unsigned)MAX_JPEG_SIZE);
    jpeg_len = MAX_JPEG_SIZE;
  }

  // Allocate buffer in PSRAM for JPEG data
  uint8_t *buffer =
      heap_caps_malloc(jpeg_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buffer) {
    ESP_LOGE(TAG, "Failed to allocate PSRAM buffer for JPEG (%u bytes)",
             (unsigned)jpeg_len);
    xSemaphoreGive(s_queue_mutex);
    return ESP_ERR_NO_MEM;
  }

  // Copy JPEG data to PSRAM buffer
  memcpy(buffer, jpeg_data, jpeg_len);

  // Add to queue with timestamp
  s_image_queue[s_queue_tail].data = buffer;
  s_image_queue[s_queue_tail].len = jpeg_len;
  s_image_queue[s_queue_tail].valid = true;
  s_image_queue[s_queue_tail].timestamp = time(NULL);
  s_queue_tail = (s_queue_tail + 1) % MAX_QUEUED_IMAGES;
  s_queue_count++;

  ESP_LOGI(TAG, "JPEG queued in PSRAM (%u bytes, queue: %d/%d)",
           (unsigned)jpeg_len, s_queue_count, MAX_QUEUED_IMAGES);

  const bool trigger_immediate =
      (s_queue_count >= MIN_QUEUE_FOR_IMMEDIATE_SAVE);
  xSemaphoreGive(s_queue_mutex);

  // Check if queue is almost full - trigger immediate save
  if (trigger_immediate && s_save_task_handle != NULL) {
    ESP_LOGW(TAG, "Queue almost full, triggering immediate save");
    xTaskNotifyGive(s_save_task_handle);
  }

  return ESP_OK;
}

esp_err_t app_storage_queue_audio(const uint8_t *pcm_data, size_t pcm_bytes,
                                  uint32_t sample_rate_hz) {
  if (!pcm_data || pcm_bytes == 0) {
    ESP_LOGE(TAG, "Invalid parameters for audio queue");
    return ESP_ERR_INVALID_ARG;
  }

  // Check queue capacity
  if (s_queue_mutex == NULL ||
      xSemaphoreTake(s_queue_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    ESP_LOGW(TAG, "Queue mutex unavailable, dropping audio");
    return ESP_FAIL;
  }

  if (s_audio_queue_count >= MAX_QUEUED_IMAGES) {
    ESP_LOGW(TAG, "Audio queue full (%d items), dropping oldest",
             s_audio_queue_count);
    // Free oldest audio
    if (s_audio_queue[s_audio_queue_head].valid &&
        s_audio_queue[s_audio_queue_head].data) {
      free(s_audio_queue[s_audio_queue_head].data);
      s_audio_queue[s_audio_queue_head].data = NULL;
      s_audio_queue[s_audio_queue_head].valid = false;
      s_audio_queue[s_audio_queue_head].len = 0;
      s_audio_queue[s_audio_queue_head].timestamp = 0;
    }
    s_audio_queue_head = (s_audio_queue_head + 1) % MAX_QUEUED_IMAGES;
    s_audio_queue_count--;
  }

  // Check Audio size limit
  if (pcm_bytes > MAX_JPEG_SIZE) {
    ESP_LOGW(TAG, "Audio too large (%u bytes, max %u), truncating",
             (unsigned)pcm_bytes, (unsigned)MAX_JPEG_SIZE);
    pcm_bytes = MAX_JPEG_SIZE;
  }

  // Allocate buffer in PSRAM for Audio data
  uint8_t *buffer =
      heap_caps_malloc(pcm_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buffer) {
    ESP_LOGE(TAG, "Failed to allocate PSRAM buffer for audio (%u bytes)",
             (unsigned)pcm_bytes);
    xSemaphoreGive(s_queue_mutex);
    return ESP_ERR_NO_MEM;
  }

  // Copy audio data to PSRAM buffer
  memcpy(buffer, pcm_data, pcm_bytes);

  // Add to queue with timestamp
  s_audio_queue[s_audio_queue_tail].data = buffer;
  s_audio_queue[s_audio_queue_tail].len = pcm_bytes;
  s_audio_queue[s_audio_queue_tail].sample_rate_hz = sample_rate_hz;
  s_audio_queue[s_audio_queue_tail].valid = true;
  s_audio_queue[s_audio_queue_tail].timestamp = time(NULL);
  s_audio_queue_tail = (s_audio_queue_tail + 1) % MAX_QUEUED_IMAGES;
  s_audio_queue_count++;

  ESP_LOGI(TAG, "Audio queued in PSRAM (%u bytes, queue: %d/%d)",
           (unsigned)pcm_bytes, s_audio_queue_count, MAX_QUEUED_IMAGES);

  const bool trigger_immediate =
      (s_audio_queue_count >= MIN_QUEUE_FOR_IMMEDIATE_SAVE);
  xSemaphoreGive(s_queue_mutex);

  // Check if queue is almost full - trigger immediate save
  if (trigger_immediate && s_save_task_handle != NULL) {
    ESP_LOGW(TAG, "Audio queue almost full, triggering immediate save");
    xTaskNotifyGive(s_save_task_handle);
  }

  return ESP_OK;
}

// Legacy function - now just notifies interaction
// Actual saving happens opportunistically via task
void app_storage_process_queue(void) {
  // Just notify that interaction occurred
  // The opportunistic task will handle saving when idle
  app_storage_notify_interaction();
}

void app_storage_notify_interaction(void) {
  // Reset inactivity timer when user interacts
  // This prevents saving during active use
  if (s_inactivity_timer != NULL &&
      xTimerIsTimerActive(s_inactivity_timer) != pdFALSE) {
    BaseType_t timer_ret = xTimerReset(s_inactivity_timer, 0);
    if (timer_ret != pdPASS) {
      ESP_LOGW(TAG, "Failed to reset inactivity timer on interaction");
    }
  }
}

int app_storage_get_queue_count(void) { return app_queue_get_count(); }

bool app_storage_is_busy(void) { return s_storage_busy; }

/* -----------------------------------------------------------------------
 * WAV helper – build a minimal 44-byte RIFF/PCM header in-place.
 * 16-bit, mono, PCM (format tag 1).
 * ----------------------------------------------------------------------- */
static void build_wav_header(uint8_t *hdr, uint32_t pcm_bytes,
                             uint32_t sample_rate_hz) {
  const uint16_t num_channels = 1;
  const uint16_t bits_per_sample = 16;
  const uint32_t byte_rate =
      sample_rate_hz * num_channels * (bits_per_sample / 8);
  const uint16_t block_align = num_channels * (bits_per_sample / 8);
  const uint32_t data_chunk_size = pcm_bytes;
  const uint32_t riff_size =
      36 + pcm_bytes; /* 36 = rest of header after RIFF */

  /* RIFF chunk */
  memcpy(hdr, "RIFF", 4);
  hdr[4] = (uint8_t)(riff_size);
  hdr[5] = (uint8_t)(riff_size >> 8);
  hdr[6] = (uint8_t)(riff_size >> 16);
  hdr[7] = (uint8_t)(riff_size >> 24);
  memcpy(hdr + 8, "WAVE", 4);

  /* fmt sub-chunk */
  memcpy(hdr + 12, "fmt ", 4);
  hdr[16] = 16;
  hdr[17] = 0;
  hdr[18] = 0;
  hdr[19] = 0; /* sub-chunk size = 16 */
  hdr[20] = 1;
  hdr[21] = 0; /* PCM = 1 */
  hdr[22] = (uint8_t)(num_channels);
  hdr[23] = (uint8_t)(num_channels >> 8);
  hdr[24] = (uint8_t)(sample_rate_hz);
  hdr[25] = (uint8_t)(sample_rate_hz >> 8);
  hdr[26] = (uint8_t)(sample_rate_hz >> 16);
  hdr[27] = (uint8_t)(sample_rate_hz >> 24);
  hdr[28] = (uint8_t)(byte_rate);
  hdr[29] = (uint8_t)(byte_rate >> 8);
  hdr[30] = (uint8_t)(byte_rate >> 16);
  hdr[31] = (uint8_t)(byte_rate >> 24);
  hdr[32] = (uint8_t)(block_align);
  hdr[33] = (uint8_t)(block_align >> 8);
  hdr[34] = (uint8_t)(bits_per_sample);
  hdr[35] = (uint8_t)(bits_per_sample >> 8);

  /* data sub-chunk */
  memcpy(hdr + 36, "data", 4);
  hdr[40] = (uint8_t)(data_chunk_size);
  hdr[41] = (uint8_t)(data_chunk_size >> 8);
  hdr[42] = (uint8_t)(data_chunk_size >> 16);
  hdr[43] = (uint8_t)(data_chunk_size >> 24);
}

/* -----------------------------------------------------------------------
 * Ensure SD is mounted (reuses the already-mounted state to avoid DMA
 * fragmentation).  Returns ESP_OK only when mount is confirmed.
 * ----------------------------------------------------------------------- */
static esp_err_t storage_ensure_mounted(void) {
  if (s_sd_mounted) {
    return ESP_OK;
  }
  if (!bsp_sdcard_is_present()) {
    return ESP_ERR_NOT_FOUND;
  }
  if (s_sd_mount_mutex == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  if (xSemaphoreTake(s_sd_mount_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  esp_err_t ret = ESP_OK;
  if (!s_sd_mounted) {
    ret = bsp_sdcard_mount();
    if (ret == ESP_OK) {
      s_sd_mounted = true;
      (void)ensure_directory_structure();
      ESP_LOGI(TAG, "SD mounted on-demand");
    } else {
      ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(ret));
    }
  }
  xSemaphoreGive(s_sd_mount_mutex);
  return ret;
}

/* -----------------------------------------------------------------------
 * app_storage_save_audio
 * ----------------------------------------------------------------------- */
esp_err_t app_storage_save_audio(const uint8_t *pcm_data, size_t pcm_bytes,
                                 uint32_t sample_rate_hz) {
  if (!pcm_data || pcm_bytes == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t ret = storage_ensure_mounted();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "save_audio: SD not available (%s)", esp_err_to_name(ret));
    return ret;
  }

  /* Build timestamp filename */
  char filename[128];
  time_t now = time(NULL);
  struct tm ti;
  if (localtime_r(&now, &ti) == NULL) {
    ESP_LOGE(TAG, "save_audio: failed to get local time");
    return ESP_FAIL;
  }
  snprintf(filename, sizeof(filename), "%s/R%02d%02d%02d.WAV",
           SD_MEDIA_PATH "/audio", ti.tm_hour, ti.tm_min, ti.tm_sec);

  /* Protect SPI bus shared with LCD */
  bsp_lvgl_lock(-1);
  FILE *f = fopen(filename, "wb");
  if (!f) {
    bsp_lvgl_unlock();
    ESP_LOGE(TAG, "save_audio: fopen failed '%s' (errno %d: %s)", filename,
             errno, strerror(errno));
    return ESP_FAIL;
  }

  /* Write WAV header */
  uint8_t wav_hdr[44];
  build_wav_header(wav_hdr, (uint32_t)pcm_bytes, sample_rate_hz);
  size_t hdr_written = fwrite(wav_hdr, 1, sizeof(wav_hdr), f);

  /* Write PCM payload */
  size_t pcm_written = fwrite(pcm_data, 1, pcm_bytes, f);
  fclose(f);
  bsp_lvgl_unlock();

  if (hdr_written != sizeof(wav_hdr) || pcm_written != pcm_bytes) {
    ESP_LOGE(TAG, "save_audio: incomplete write to '%s' (hdr=%u/%u, pcm=%u/%u)",
             filename, (unsigned)hdr_written, (unsigned)sizeof(wav_hdr),
             (unsigned)pcm_written, (unsigned)pcm_bytes);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Audio saved: %s (%u bytes PCM -> %u bytes WAV)", filename,
           (unsigned)pcm_bytes, (unsigned)(sizeof(wav_hdr) + pcm_bytes));
  return ESP_OK;
}

/* -----------------------------------------------------------------------
 * app_storage_save_chat_log
 * ----------------------------------------------------------------------- */
esp_err_t app_storage_save_chat_log(const char *mode_label,
                                    const char *ai_response) {
  if (!mode_label || !ai_response) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t ret = storage_ensure_mounted();
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "save_chat_log: SD not available (%s)", esp_err_to_name(ret));
    return ret;
  }

  /* Daily log file: CHAT_YYYYMMDD.txt */
  char filename[128];
  time_t now = time(NULL);
  struct tm ti;
  if (localtime_r(&now, &ti) == NULL) {
    ESP_LOGE(TAG, "save_chat_log: failed to get local time");
    return ESP_FAIL;
  }
  snprintf(filename, sizeof(filename), "%s/C%02d%02d.TXT",
           SD_BASE_PATH "/logs/chat", ti.tm_mon + 1, ti.tm_mday);

  /* Protect SPI bus shared with LCD */
  bsp_lvgl_lock(-1);
  /* Append mode: cria o arquivo se não existir, senão acrescenta */
  FILE *f = fopen(filename, "a");
  if (!f) {
    bsp_lvgl_unlock();
    ESP_LOGE(TAG, "save_chat_log: fopen failed '%s' (errno %d: %s)", filename,
             errno, strerror(errno));
    return ESP_FAIL;
  }

  /* Format: [HH:MM:SS] [MODE] AI: <response>\n */
  char timestamp[24];
  snprintf(timestamp, sizeof(timestamp), "%02d:%02d:%02d", ti.tm_hour,
           ti.tm_min, ti.tm_sec);

  int written =
      fprintf(f, "[%s] [%s] AI: %s\n", timestamp, mode_label, ai_response);
  fclose(f);
  bsp_lvgl_unlock();

  if (written < 0) {
    ESP_LOGE(TAG, "save_chat_log: fprintf failed for '%s'", filename);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Chat log appended: %s (%d bytes)", filename, written);
  return ESP_OK;
}

esp_err_t app_storage_ensure_mounted(void) {
  if (s_sd_mounted) {
    /* Mesmo se já montado, garante a estrutura de diretórios */
    bsp_lvgl_lock(-1);
    ensure_directory_structure();
    bsp_lvgl_unlock();
    return ESP_OK;
  }

  if (xSemaphoreTake(s_sd_mount_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "ensure_mounted: failed to take mount mutex");
    return ESP_ERR_TIMEOUT;
  }

  if (s_sd_mounted) {
    xSemaphoreGive(s_sd_mount_mutex);
    return ESP_OK;
  }

  /* Requer pelo menos 24KB contiguos DMA para o FATFS/SDMMC */
  size_t largest_dma = heap_caps_get_largest_free_block(
      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  if (largest_dma < (24 * 1024)) {
    ESP_LOGE(TAG, "ensure_mounted: Insufficient DMA memory (%u bytes)",
             (unsigned)largest_dma);
    xSemaphoreGive(s_sd_mount_mutex);
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "ensure_mounted: Mounting SD card (first time)...");
  esp_err_t ret = bsp_sdcard_mount();
  if (ret == ESP_OK) {
    s_sd_mounted = true;
    bsp_lvgl_lock(-1);
    ensure_directory_structure();
    bsp_lvgl_unlock();
    ESP_LOGI(TAG, "ensure_mounted: SD card mounted successfully");
  } else {
    ESP_LOGE(TAG, "ensure_mounted: mount failed: %s", esp_err_to_name(ret));
  }

  xSemaphoreGive(s_sd_mount_mutex);
  return ret;
}
