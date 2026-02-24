#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialise the high-level GUI (LVGL widgets).
 *        Pre-requisite: bsp_init() (LVGL and hardware must be ready).
 */
esp_err_t gui_init(void);

/** @brief Update the top status bar state text. */
esp_err_t gui_set_state(const char *state_text);

/** @brief Update the top-right icons (Wi-Fi and Battery). */
esp_err_t gui_set_status_icons(bool wifi_ok, int batt_percent);

/** @brief Set the transcript text (usually displayed during/after recording).
 */
esp_err_t gui_set_transcript(const char *text);

/** @brief Set the main AI response text. */
esp_err_t gui_set_response(const char *text);

/** @brief Switch response panel between full-screen and compact mode. */
esp_err_t gui_set_response_compact(bool compact);

/** @brief Show or hide the response text panel. */
esp_err_t gui_set_response_panel_visible(bool visible);

/** @brief Update the bottom footer hint text. */
esp_err_t gui_set_footer(const char *text);

/** @brief Scroll the response text up/down. */
esp_err_t gui_scroll_response(int16_t delta_pixels);

/** @brief Update the recording progress bar (0-100). */
esp_err_t gui_set_recording_progress(uint8_t percent);

/**
 * @brief Returns true if the Profile ('M') touchscreen button is currently
 * being pressed.
 */
bool gui_is_profile_pressed(void);

/* Events ------------------------------------------------------------------ */

typedef enum {
  GUI_EVENT_PROFILE,
  GUI_EVENT_SCROLL_UP,
  GUI_EVENT_SCROLL_DOWN
} gui_event_type_t;

typedef void (*gui_event_callback_t)(gui_event_type_t event);

/**
 * @brief Register a callback for GUI events (button presses, etc.).
 */
void gui_set_event_callback(gui_event_callback_t cb);
