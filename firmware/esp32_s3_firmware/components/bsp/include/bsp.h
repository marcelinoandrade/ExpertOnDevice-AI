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

esp_err_t bsp_audio_capture_blocking(
    const bsp_audio_capture_cfg_t *cfg,
    uint8_t *buffer,
    size_t buffer_len,
    size_t *captured_bytes
);
