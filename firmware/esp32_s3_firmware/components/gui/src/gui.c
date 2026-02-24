#include "gui.h"

#include "bsp.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

/* ==================================================================
 *  Widget handles
 * ================================================================== */
static lv_obj_t *s_panel_status;   /* Top strip background panel  */
static lv_obj_t *s_label_status;   /* Top status text             */
static lv_obj_t *s_label_icons;    /* Top-right icons (Wi-Fi, Bat)*/
static lv_obj_t *s_panel_response; /* Main response viewport      */
static lv_obj_t *s_label_response; /* Main content / AI response  */
static lv_obj_t *s_bar_progress;   /* Recording progress bar      */
static lv_obj_t *s_label_progress; /* "REC 65%" on the bar        */
static lv_obj_t *s_panel_footer;   /* Bottom strip background panel */
static lv_obj_t *s_label_footer;   /* Bottom contextual hints     */
static gui_event_callback_t s_event_cb = NULL;
static lv_coord_t s_response_scroll_y;
static bool s_profile_btn_held = false;

/* ==================================================================
 *  Layout constants (Waveshare 240x320 ST7789 display)
 * ================================================================== */
#define SCR_W 240
#define SCR_H 320
#define MARGIN 6
#define PAD 5
#define STATUSBAR_H 26
#define FOOTER_H 24
#define PROGRESS_H 18
#define CONTENT_Y (STATUSBAR_H + 2)
#define CONTENT_W (SCR_W - 2 * MARGIN)
#define CONTENT_H                                                              \
  (SCR_H - CONTENT_Y - FOOTER_H - 52) /* Stop the text area ABOVE the buttons  \
                                       */

/* ==================================================================
 *  Colour palette (Inversion-ON display)
 * ================================================================== */
#define C_BG 0xFFFFFF          /* Black */
#define C_TEXT 0x000000        /* White */
#define C_STRIP_BG 0xE0E0E0    /* Dark gray for bars */
#define C_STRIP_TEXT 0x000000  /* White text on bars */
#define C_BAR_TRACK 0xC0C0C0   /* Dark gray track */
#define C_BAR_FILL 0x303030    /* Bright green-ish on screen */
#define C_RESPONSE_BG 0xF0F0F0 /* Near-black for response card */

/* ==================================================================
 *  Font selection
 * ================================================================== */
#define FONT_BODY LV_FONT_DEFAULT
#if LV_FONT_MONTSERRAT_14
#undef FONT_BODY
#define FONT_BODY (&lv_font_montserrat_14)
#endif

#define FONT_SMALL FONT_BODY
#if LV_FONT_MONTSERRAT_12
#undef FONT_SMALL
#define FONT_SMALL (&lv_font_montserrat_12)
#endif

/* Unscii for status bars (looks more "embedded" and Sharp) */
#define FONT_STRIP FONT_BODY
#if LV_FONT_UNSCII_8
#undef FONT_STRIP
#define FONT_STRIP (&lv_font_unscii_8)
#endif

/* ==================================================================
 *  Helpers
 * ================================================================== */
static lv_coord_t gui_response_max_scroll_y(void) {
  if (!s_panel_response || !s_label_response) {
    return 0;
  }
  lv_obj_update_layout(s_panel_response);
  lv_obj_update_layout(s_label_response);
  const lv_coord_t label_h = lv_obj_get_height(s_label_response);
  const lv_coord_t view_h = lv_obj_get_content_height(s_panel_response);
  if (label_h <= view_h) {
    return 0;
  }
  return label_h - view_h;
}

static void gui_apply_response_scroll(void) {
  if (!s_label_response) {
    return;
  }
  const lv_coord_t max_scroll = gui_response_max_scroll_y();
  if (s_response_scroll_y < 0) {
    s_response_scroll_y = 0;
  }
  if (s_response_scroll_y > max_scroll) {
    s_response_scroll_y = max_scroll;
  }
  lv_obj_set_y(s_label_response, -s_response_scroll_y);
}

static lv_obj_t *gui_create_strip(lv_obj_t *parent, lv_obj_t **out_label,
                                  lv_align_t align, lv_coord_t y_ofs,
                                  const char *text) {
  lv_obj_t *panel = lv_obj_create(parent);
  lv_obj_remove_style_all(panel);
  lv_obj_set_size(panel, SCR_W, STATUSBAR_H);
  lv_obj_align(panel, align, 0, y_ofs);
  lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(panel, lv_color_hex(C_STRIP_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_opa(panel, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(panel, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(panel, 0, LV_PART_MAIN);

  lv_obj_t *lbl = lv_label_create(panel);
  lv_obj_remove_style_all(lbl);
  lv_obj_set_width(lbl, SCR_W - 2 * PAD);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
  /* Force solid background for label to improve anti-aliasing sharpness on
   * RGB565 */
  lv_obj_set_style_bg_color(lbl, lv_color_hex(C_STRIP_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_opa(lbl, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_text_color(lbl, lv_color_hex(C_STRIP_TEXT), LV_PART_MAIN);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl, FONT_STRIP, LV_PART_MAIN);
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
  lv_label_set_text(lbl, text ? text : "");

  if (out_label)
    *out_label = lbl;
  return panel;
}

static void gui_btn_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  gui_event_type_t etype =
      (gui_event_type_t)(uintptr_t)lv_event_get_user_data(e);

  /* Track 'M' (Profile) button hold state for zero-touch portal trigger */
  if (etype == GUI_EVENT_PROFILE) {
    if (code == LV_EVENT_PRESSED) {
      s_profile_btn_held = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
      s_profile_btn_held = false;
    }
  }

  if (code == LV_EVENT_CLICKED && s_event_cb) {
    s_event_cb(etype);
  }
}

static lv_obj_t *gui_create_btn(lv_obj_t *parent, const char *text,
                                lv_coord_t x, lv_coord_t y,
                                gui_event_type_t type) {
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_size(btn, 50, 40);
  lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, x, y);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x404040), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(btn, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_add_event_cb(btn, gui_btn_event_cb, LV_EVENT_ALL,
                      (void *)(uintptr_t)type);

  lv_obj_t *label = lv_label_create(btn);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  /* Make label background solid to match button for cleaner text edges */
  lv_obj_set_style_bg_color(label, lv_color_hex(0x404040), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(label, LV_OPA_COVER, LV_PART_MAIN);

  return btn;
}

/* ==================================================================
 *  Initialisation
 * ================================================================== */
esp_err_t gui_init(void) {
  if (!bsp_lvgl_lock(-1))
    return ESP_FAIL;

  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

  /* -- Status bar (top) -- */
  s_panel_status =
      gui_create_strip(scr, &s_label_status, LV_ALIGN_TOP_LEFT, 0, "Pronto");
  lv_obj_set_width(s_label_status, lv_pct(65));
  lv_label_set_long_mode(s_label_status, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_align(s_label_status, LV_ALIGN_LEFT_MID, PAD, 0);

  s_label_icons = lv_label_create(s_panel_status);
  lv_obj_remove_style_all(s_label_icons);
  lv_obj_set_style_bg_color(s_label_icons, lv_color_hex(C_STRIP_BG),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_label_icons, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_opa(s_label_icons, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_label_icons, lv_color_hex(C_STRIP_TEXT),
                              LV_PART_MAIN);
  lv_obj_set_style_text_font(s_label_icons, FONT_STRIP, LV_PART_MAIN);
  lv_obj_align(s_label_icons, LV_ALIGN_RIGHT_MID, -PAD, 0);
  lv_label_set_text(s_label_icons, "");

  /* -- Main response area -- */
  s_panel_response = lv_obj_create(scr);
  lv_obj_set_size(s_panel_response, CONTENT_W, CONTENT_H);
  lv_obj_align(s_panel_response, LV_ALIGN_TOP_LEFT, MARGIN, CONTENT_Y);
  lv_obj_clear_flag(s_panel_response, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(s_panel_response, lv_color_hex(C_RESPONSE_BG),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_panel_response, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(s_panel_response, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_panel_response, PAD + 1, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_panel_response, 0, LV_PART_MAIN);
  lv_obj_set_style_clip_corner(s_panel_response, true, LV_PART_MAIN);

  s_label_response = lv_label_create(s_panel_response);
  lv_obj_set_width(s_label_response, CONTENT_W - 2 * (PAD + 1));
  lv_obj_align(s_label_response, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_label_set_long_mode(s_label_response, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(s_label_response, lv_color_hex(C_TEXT),
                              LV_PART_MAIN);
  lv_obj_set_style_text_font(s_label_response, FONT_BODY, LV_PART_MAIN);
  lv_label_set_text(s_label_response, "Segure para falar.");
  s_response_scroll_y = 0;
  gui_apply_response_scroll();

  /* -- Progress bar -- */
  s_bar_progress = lv_bar_create(scr);
  lv_obj_set_size(s_bar_progress, CONTENT_W, PROGRESS_H);
  /* Overlay the progress bar precisely onto the bottom footer strip when
   * recording */
  lv_obj_align(s_bar_progress, LV_ALIGN_BOTTOM_MID, 0, -3);
  lv_bar_set_range(s_bar_progress, 0, 100);
  lv_obj_set_style_bg_color(s_bar_progress, lv_color_hex(C_BAR_TRACK),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_bar_progress, lv_color_hex(C_BAR_FILL),
                            LV_PART_INDICATOR);
  lv_obj_add_flag(s_bar_progress, LV_OBJ_FLAG_HIDDEN);

  s_label_progress = lv_label_create(scr);
  lv_obj_set_style_text_font(s_label_progress, FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_label_progress, lv_color_hex(C_TEXT),
                              LV_PART_MAIN);
  /* The bar underneath is C_BAR_TRACK or C_BAR_FILL.
   * We'll use a transparent label here but ensure text opa is 255.
   * Since the bar is solid, it should be fine, but we can try to improve it if
   * needed. */
  lv_obj_set_style_text_opa(s_label_progress, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align_to(s_label_progress, s_bar_progress, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(s_label_progress, LV_OBJ_FLAG_HIDDEN);

  /* -- Footer -- */
  s_panel_footer = gui_create_strip(scr, &s_label_footer, LV_ALIGN_BOTTOM_LEFT,
                                    0, "Dica: Use o Touch");
  lv_obj_set_height(s_panel_footer, FOOTER_H);

  /* -- Touch Buttons (above footer) -- */
  /* Position: Footer is at bottom. SCR_H=320, FOOTER_H=24. Footer ends at 320.
   * Buttons at y ~ -30 from bottom. */
  gui_create_btn(scr, "M", 10, -(FOOTER_H + 10), GUI_EVENT_PROFILE);
  gui_create_btn(scr, LV_SYMBOL_UP, 120, -(FOOTER_H + 10), GUI_EVENT_SCROLL_UP);
  gui_create_btn(scr, LV_SYMBOL_DOWN, 180, -(FOOTER_H + 10),
                 GUI_EVENT_SCROLL_DOWN);

  bsp_lvgl_unlock();
  return ESP_OK;
}

void gui_set_event_callback(gui_event_callback_t cb) { s_event_cb = cb; }

/* ==================================================================
 *  Public API
 * ================================================================== */
esp_err_t gui_set_state(const char *state_text) {
  if (!s_label_status)
    return ESP_ERR_INVALID_STATE;
  if (!bsp_lvgl_lock(200))
    return ESP_ERR_TIMEOUT;
  lv_label_set_text(s_label_status, state_text ? state_text : "");
  if (s_bar_progress)
    lv_obj_add_flag(s_bar_progress, LV_OBJ_FLAG_HIDDEN);
  if (s_label_progress)
    lv_obj_add_flag(s_label_progress, LV_OBJ_FLAG_HIDDEN);
  bsp_lvgl_unlock();
  return ESP_OK;
}

esp_err_t gui_set_status_icons(bool wifi_ok, int batt_percent) {
  if (!s_label_icons)
    return ESP_ERR_INVALID_STATE;
  if (!bsp_lvgl_lock(200))
    return ESP_ERR_TIMEOUT;

  const char *wifi_sym = wifi_ok ? LV_SYMBOL_WIFI : LV_SYMBOL_CLOSE;
  char txt[32];
  if (batt_percent < 0) {
    snprintf(txt, sizeof(txt), "%s", wifi_sym);
  } else {
    snprintf(txt, sizeof(txt), "%s %d%%", wifi_sym, batt_percent);
  }
  lv_label_set_text(s_label_icons, txt);
  bsp_lvgl_unlock();
  return ESP_OK;
}

esp_err_t gui_set_transcript(const char *text) {
  (void)text;
  return ESP_OK;
}

esp_err_t gui_set_response(const char *text) {
  if (!s_label_response)
    return ESP_ERR_INVALID_STATE;
  if (!bsp_lvgl_lock(200))
    return ESP_ERR_TIMEOUT;
  lv_label_set_text(s_label_response, text ? text : "");
  s_response_scroll_y = 0;
  gui_apply_response_scroll();
  bsp_lvgl_unlock();
  return ESP_OK;
}

esp_err_t gui_set_response_compact(bool compact) {
  if (!s_panel_response)
    return ESP_ERR_INVALID_STATE;
  if (!bsp_lvgl_lock(200))
    return ESP_ERR_TIMEOUT;
  if (compact) {
    lv_obj_set_size(s_panel_response, CONTENT_W, 60);
    lv_obj_align(s_panel_response, LV_ALIGN_CENTER, 0, 0);
  } else {
    lv_obj_set_size(s_panel_response, CONTENT_W, CONTENT_H);
    lv_obj_align(s_panel_response, LV_ALIGN_TOP_LEFT, MARGIN, CONTENT_Y);
  }
  bsp_lvgl_unlock();
  return ESP_OK;
}

esp_err_t gui_set_response_panel_visible(bool visible) {
  if (!s_panel_response)
    return ESP_ERR_INVALID_STATE;
  if (!bsp_lvgl_lock(200))
    return ESP_ERR_TIMEOUT;
  if (visible)
    lv_obj_clear_flag(s_panel_response, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(s_panel_response, LV_OBJ_FLAG_HIDDEN);
  bsp_lvgl_unlock();
  return ESP_OK;
}

esp_err_t gui_set_footer(const char *text) {
  if (!s_label_footer)
    return ESP_ERR_INVALID_STATE;
  if (!bsp_lvgl_lock(200))
    return ESP_ERR_TIMEOUT;
  lv_label_set_text(s_label_footer, text ? text : "");
  bsp_lvgl_unlock();
  return ESP_OK;
}

esp_err_t gui_scroll_response(int16_t delta_pixels) {
  if (!s_label_response)
    return ESP_ERR_INVALID_STATE;
  if (delta_pixels == 0)
    return ESP_OK;
  if (!bsp_lvgl_lock(200))
    return ESP_ERR_TIMEOUT;
  s_response_scroll_y += delta_pixels;
  gui_apply_response_scroll();
  bsp_lvgl_unlock();
  return ESP_OK;
}

esp_err_t gui_set_recording_progress(uint8_t percent) {
  if (percent > 100)
    percent = 100;
  if (!bsp_lvgl_lock(200))
    return ESP_ERR_TIMEOUT;
  if (s_bar_progress) {
    lv_obj_clear_flag(s_bar_progress, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(s_bar_progress, (int32_t)percent, LV_ANIM_ON);
  }
  if (s_label_progress) {
    char txt[16];
    snprintf(txt, sizeof(txt), "REC %u%%", (unsigned)percent);
    lv_obj_clear_flag(s_label_progress, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_label_progress, txt);
  }
  bsp_lvgl_unlock();
  return ESP_OK;
}
bool gui_is_profile_pressed(void) { return s_profile_btn_held; }
