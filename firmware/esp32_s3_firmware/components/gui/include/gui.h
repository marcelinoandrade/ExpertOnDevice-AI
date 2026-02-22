#pragma once

#include "esp_err.h"
#include <stdint.h>

esp_err_t gui_init(void);
esp_err_t gui_set_state(const char *state_text);
esp_err_t gui_set_transcript(const char *text);
esp_err_t gui_set_response(const char *text);
esp_err_t gui_set_recording_progress(uint8_t percent);
