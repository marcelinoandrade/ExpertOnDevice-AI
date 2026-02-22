#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

esp_err_t gui_init(void);
esp_err_t gui_set_state(const char *state_text);
esp_err_t gui_set_transcript(const char *text);
esp_err_t gui_set_response(const char *text);
esp_err_t gui_set_response_compact(bool compact);
esp_err_t gui_set_response_panel_visible(bool visible);
esp_err_t gui_set_footer(const char *text);
esp_err_t gui_scroll_response(int16_t delta_pixels);
esp_err_t gui_set_recording_progress(uint8_t percent);
esp_err_t gui_hide_camera_preview(void);
esp_err_t gui_show_camera_preview_rgb565(const uint8_t *rgb565_data, uint16_t width, uint16_t height);
