#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
  uint32_t sample_rate_hz;
  uint8_t bits_per_sample;
  uint8_t channels;
  uint16_t capture_ms;
} bsp_audio_capture_cfg_t;

esp_err_t bsp_init(void);

bool bsp_lvgl_lock(int timeout_ms);
void bsp_lvgl_unlock(void);

esp_err_t bsp_display_show_status(const char *status_text);
esp_err_t bsp_display_show_text(const char *body_text);
bool bsp_button_is_pressed(void);
bool bsp_wifi_is_ready(void);
void bsp_enter_deep_sleep(void);

esp_err_t bsp_audio_capture_blocking(const bsp_audio_capture_cfg_t *cfg,
                                     uint8_t *buffer, size_t buffer_len,
                                     size_t *captured_bytes);

/* SD Card ----------------------------------------------------------------- */
/* SD SPI pins on the Waveshare ESP32-S3-Touch-LCD-2 */
#define BSP_SD_MISO_GPIO 40
#define BSP_SD_MOSI_GPIO 38
#define BSP_SD_CLK_GPIO 39
#define BSP_SD_CS_GPIO 41

/**
 * @brief Initialise the SD-card detect GPIO (no external CD pin on Waveshare,
 *        so this just prepares the SPI bus config; always reports "present").
 */
esp_err_t bsp_sdcard_detect_init(void);

/** @brief Returns true if an SD card is physically present (best-effort). */
bool bsp_sdcard_is_present(void);

/**
 * @brief Mount the SD card via SDSPI and register FATFS at "/sdcard".
 *        Safe to call multiple times — returns ESP_OK if already mounted.
 */
esp_err_t bsp_sdcard_mount(void);

/** @brief Unmount the SD card and free the SPI slot. */
esp_err_t bsp_sdcard_unmount(void);
