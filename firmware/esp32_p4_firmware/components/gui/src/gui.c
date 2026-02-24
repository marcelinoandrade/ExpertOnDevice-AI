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
static lv_obj_t *s_canvas_preview; /* Camera live-view canvas     */
static lv_color_t *s_preview_buf;
static lv_coord_t s_response_scroll_y;

/* ==================================================================
 *  Layout constants  (240x240 ST7789 display)
 * ================================================================== */
#define SCR_W 240
#define SCR_H 240
#define MARGIN 6
#define PAD 5
#define STATUSBAR_H 24
#define FOOTER_H 22
#define PROGRESS_H 16
#define CONTENT_Y (STATUSBAR_H + 2)
#define CONTENT_W (SCR_W - 2 * MARGIN)
#define CONTENT_H (SCR_H - CONTENT_Y - FOOTER_H - 2)

/* ==================================================================
 *  Colour palette
 *  Display uses inversion-ON: displayed colour is approx. bitwise NOT.
 *    send 0xFF -> display 0x00 (black)
 *    send 0x00 -> display 0xFF (white)
 *
 *  IMPORTANT – Anti-aliasing rule for RGB565 panels:
 *  Every widget that renders text MUST have bg_opa = LV_OPA_COVER
 *  (fully opaque, 255).  Semi-transparent backgrounds cause LVGL
 *  to blend anti-aliased font pixels against an intermediate colour,
 *  producing "dirty" multi-coloured edges.  If a translucent overlay
 *  is needed, place a parent panel with the desired opa and put the
 *  label inside it with bg_opa = 0 (inherit the panel bg).
 * ================================================================== */
#define C_BG 0xFFFFFF          /* screen background -> black      */
#define C_TEXT 0x000000        /* all text          -> white      */
#define C_STRIP_BG 0xE0E0E0    /* bar/footer strip  -> dark gray  */
#define C_STRIP_TEXT 0x000000  /* text on strips    -> white      */
#define C_BAR_TRACK 0xC0C0C0   /* progress track    -> dark gray  */
#define C_BAR_FILL 0x303030    /* progress filled   -> bright     */
#define C_RESPONSE_BG 0xF0F0F0 /* response card bg  -> near-black */

/* ==================================================================
 *  Font selection - two-level hierarchy
 * ================================================================== */
#if LV_FONT_MONTSERRAT_14
#define FONT_BODY (&lv_font_montserrat_14)
#else
#define FONT_BODY LV_FONT_DEFAULT
#endif

#if LV_FONT_MONTSERRAT_12
#define FONT_SMALL (&lv_font_montserrat_12)
#else
#define FONT_SMALL FONT_BODY
#endif

/* Bitmap fonts: no anti-aliasing → sharp on RGB565 over any background. */
#if LV_FONT_UNSCII_8
#define FONT_STRIP (&lv_font_unscii_8) /* small – status/footer bars  */
#else
#define FONT_STRIP FONT_BODY
#endif

/* Response text over camera: use unscii_8 for compact readable overlay. */
#if LV_FONT_UNSCII_8
#define FONT_OVERLAY (&lv_font_unscii_8)
#elif LV_FONT_UNSCII_16
#define FONT_OVERLAY (&lv_font_unscii_16)
#else
#define FONT_OVERLAY FONT_BODY
#endif

/* ==================================================================
 *  Helpers
 * ================================================================== */
static void gui_swap_rgb565_bytes(uint16_t *buffer, size_t pixel_count) {
  if (!buffer)
    return;
  for (size_t i = 0; i < pixel_count; i++) {
    uint16_t v = buffer[i];
    buffer[i] = (uint16_t)((v >> 8) | (v << 8));
  }
}

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

/** Bring all HUD widgets to foreground (over camera canvas). */
static void gui_raise_hud(void) {
  if (s_panel_status)
    lv_obj_move_foreground(s_panel_status);
  if (s_panel_response)
    lv_obj_move_foreground(s_panel_response);
  if (s_bar_progress)
    lv_obj_move_foreground(s_bar_progress);
  if (s_label_progress)
    lv_obj_move_foreground(s_label_progress);
  if (s_panel_footer)
    lv_obj_move_foreground(s_panel_footer);
}

/** Create edge-to-edge strip using factory_demo-like pattern:
 *  opaque parent panel + transparent child label. */
static lv_obj_t *gui_create_strip(lv_obj_t *parent, lv_obj_t **out_label,
                                  lv_align_t align, lv_coord_t y_ofs,
                                  const char *text) {
  lv_obj_t *panel = lv_obj_create(parent);
  /* Remove theme/default styles to avoid hidden gradients/transitions
   * that can dirty text edges on RGB565. */
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
  lv_obj_set_style_bg_opa(lbl, LV_OPA_TRANSP,
                          LV_PART_MAIN); /* Transparent label bg */
  lv_obj_set_style_text_color(lbl, lv_color_hex(C_STRIP_TEXT), LV_PART_MAIN);
  lv_obj_set_style_opa(lbl, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl, FONT_STRIP, LV_PART_MAIN);
  lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
  lv_label_set_text(lbl, text ? text : "");

  if (out_label)
    *out_label = lbl;
  return panel;
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

  /* -- Camera preview canvas (full-screen, behind everything) -- */
  s_canvas_preview = lv_canvas_create(scr);
  lv_obj_set_size(s_canvas_preview, SCR_W, SCR_H);
  lv_obj_align(s_canvas_preview, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(s_canvas_preview, LV_OBJ_FLAG_HIDDEN);

  s_preview_buf = heap_caps_malloc(SCR_W * SCR_H * sizeof(lv_color_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_preview_buf) {
    s_preview_buf = heap_caps_malloc(SCR_W * SCR_H * sizeof(lv_color_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (s_preview_buf) {
    lv_canvas_set_buffer(s_canvas_preview, s_preview_buf, SCR_W, SCR_H,
                         LV_IMG_CF_TRUE_COLOR);
  }

  /* -- Status bar (top, edge-to-edge) -- */
  s_panel_status =
      gui_create_strip(scr, &s_label_status, LV_ALIGN_TOP_LEFT, 0, "Pronto");
  // Restringe a largura máxima para não sobrepor o canto direito (ícones)
  lv_obj_set_width(s_label_status, lv_pct(65));
  lv_label_set_long_mode(s_label_status, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_align(s_label_status, LV_ALIGN_LEFT_MID, PAD, 0);

  /* Build the right-side icons */
  s_label_icons = lv_label_create(s_panel_status);
  lv_obj_remove_style_all(s_label_icons);
  lv_obj_set_style_text_color(s_label_icons, lv_color_hex(C_STRIP_TEXT),
                              LV_PART_MAIN);
  lv_obj_set_style_text_font(s_label_icons, FONT_STRIP, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_label_icons, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_opa(s_label_icons, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(s_label_icons, LV_ALIGN_RIGHT_MID, -PAD, 0);
  lv_label_set_text(s_label_icons, "");

  /* -- Main response / content area (viewport + scrollable label) -- */
  s_panel_response = lv_obj_create(scr);
  lv_obj_set_size(s_panel_response, CONTENT_W, CONTENT_H);
  lv_obj_align(s_panel_response, LV_ALIGN_TOP_LEFT, MARGIN, CONTENT_Y);
  lv_obj_clear_flag(s_panel_response, LV_OBJ_FLAG_SCROLLABLE);
  /* Opaque background — camera is hidden before showing AI response. */
  lv_obj_set_style_bg_color(s_panel_response, lv_color_hex(C_RESPONSE_BG),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_panel_response, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(s_panel_response, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_panel_response, PAD + 1, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_panel_response, 0, LV_PART_MAIN);
  lv_obj_set_style_clip_corner(s_panel_response, true, LV_PART_MAIN);

  s_label_response = lv_label_create(s_panel_response);
  /* Avoid runtime 0-width before first layout pass. */
  lv_obj_set_width(s_label_response, CONTENT_W - 2 * (PAD + 1));
  lv_obj_align(s_label_response, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_label_set_long_mode(s_label_response, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_color(s_label_response, lv_color_hex(C_TEXT),
                              LV_PART_MAIN);
  lv_obj_set_style_text_font(s_label_response, FONT_BODY, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_label_response, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_label_response, 0, LV_PART_MAIN);
  lv_label_set_text(s_label_response, "Segure encoder e fale.");
  s_response_scroll_y = 0;
  gui_apply_response_scroll();

  /* -- Recording progress bar (hidden until recording) -- */
  s_bar_progress = lv_bar_create(scr);
  lv_obj_set_size(s_bar_progress, CONTENT_W, PROGRESS_H);
  lv_obj_align(s_bar_progress, LV_ALIGN_BOTTOM_MID, 0, -(FOOTER_H + 4));
  lv_bar_set_range(s_bar_progress, 0, 100);
  lv_bar_set_value(s_bar_progress, 0, LV_ANIM_OFF);
  /* track (unfilled) */
  lv_obj_set_style_bg_color(s_bar_progress, lv_color_hex(C_BAR_TRACK),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_bar_progress, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(s_bar_progress, 4, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_bar_progress, 0, LV_PART_MAIN);
  /* indicator (filled) */
  lv_obj_set_style_bg_color(s_bar_progress, lv_color_hex(C_BAR_FILL),
                            LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s_bar_progress, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(s_bar_progress, 4, LV_PART_INDICATOR);
  lv_obj_add_flag(s_bar_progress, LV_OBJ_FLAG_HIDDEN);

  /* progress percentage label (centred on bar) */
  /* No bg (transparent) – the bar underneath already has opaque bg.
   * Text is fully opaque; LVGL will anti-alias against the bar. */
  s_label_progress = lv_label_create(scr);
  lv_obj_set_style_text_color(s_label_progress, lv_color_hex(C_TEXT),
                              LV_PART_MAIN);
  lv_obj_set_style_text_opa(s_label_progress, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_label_progress, FONT_SMALL, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_label_progress, 0, LV_PART_MAIN);
  lv_obj_align_to(s_label_progress, s_bar_progress, LV_ALIGN_CENTER, 0, 0);
  lv_label_set_text(s_label_progress, "");
  lv_obj_add_flag(s_label_progress, LV_OBJ_FLAG_HIDDEN);

  /* -- Footer (bottom, edge-to-edge) -- */
  s_panel_footer = gui_create_strip(scr, &s_label_footer, LV_ALIGN_BOTTOM_LEFT,
                                    0, "Enc:Falar Btn1:Foto Knob:Config");
  lv_obj_set_height(s_panel_footer, FOOTER_H);

  bsp_lvgl_unlock();
  return ESP_OK;
}

/* ==================================================================
 *  Public API
 * ================================================================== */

esp_err_t gui_set_state(const char *state_text) {
  if (!s_label_status)
    return ESP_ERR_INVALID_STATE;
  if (!bsp_lvgl_lock(200))
    return ESP_ERR_TIMEOUT;

  lv_label_set_text(s_label_status, state_text ? state_text : "");
  lv_obj_align(s_label_status, LV_ALIGN_LEFT_MID, PAD, 0);

  /* Hide progress bar - re-shown by gui_set_recording_progress
   * if still in recording state. */
  if (s_bar_progress)
    lv_obj_add_flag(s_bar_progress, LV_OBJ_FLAG_HIDDEN);
  if (s_label_progress)
    lv_obj_add_flag(s_label_progress, LV_OBJ_FLAG_HIDDEN);

  gui_raise_hud();
  bsp_lvgl_unlock();
  return ESP_OK;
}

esp_err_t gui_set_status_icons(bool wifi_ok, int batt_percent) {
  if (!s_label_icons)
    return ESP_ERR_INVALID_STATE;
  if (!bsp_lvgl_lock(200))
    return ESP_ERR_TIMEOUT;

  const char *wifi_sym = wifi_ok ? LV_SYMBOL_WIFI : "";
  const char *batt_sym;

  if (batt_percent < 0) {
    batt_sym = "";
  } else if (batt_percent > 80) {
    batt_sym = LV_SYMBOL_BATTERY_FULL;
  } else if (batt_percent > 40) {
    batt_sym = LV_SYMBOL_BATTERY_3;
  } else if (batt_percent > 15) {
    batt_sym = LV_SYMBOL_BATTERY_1;
  } else {
    batt_sym = LV_SYMBOL_BATTERY_EMPTY;
  }

  char txt[32];
  if (batt_percent < 0) {
    snprintf(txt, sizeof(txt), "%s", wifi_sym);
  } else {
    snprintf(txt, sizeof(txt), "%s %s%d%%", wifi_sym, batt_sym, batt_percent);
  }
  lv_label_set_text(s_label_icons, txt);

  if (!wifi_ok || (batt_percent >= 0 && batt_percent <= 15)) {
    /* Display is inverted, so red (0xFF0000) becomes 0x00FFFF */
    lv_obj_set_style_text_color(s_label_icons, lv_color_hex(0x00FFFF),
                                LV_PART_MAIN);
  } else {
    lv_obj_set_style_text_color(s_label_icons, lv_color_hex(C_STRIP_TEXT),
                                LV_PART_MAIN);
  }

  gui_raise_hud();
  bsp_lvgl_unlock();
  return ESP_OK;
}

esp_err_t gui_set_transcript(const char *text) {
  (void)text;
  return ESP_OK;
}

esp_err_t gui_set_response(const char *text) {
  if (!s_label_response || !s_panel_response)
    return ESP_ERR_INVALID_STATE;
  if (!bsp_lvgl_lock(200))
    return ESP_ERR_TIMEOUT;

  /* Ensure panel is visible whenever new text is set. */
  lv_obj_clear_flag(s_panel_response, LV_OBJ_FLAG_HIDDEN);

  /* Keep deterministic width even if parent layout is deferred. */
  lv_obj_set_width(s_label_response, CONTENT_W - 2 * (PAD + 1));
  lv_label_set_text(s_label_response, text ? text : "");
  s_response_scroll_y = 0;
  gui_apply_response_scroll();
  gui_raise_hud();
  bsp_lvgl_unlock();
  return ESP_OK;
}

esp_err_t gui_set_response_compact(bool compact) {
  if (!s_panel_response || !s_label_response)
    return ESP_ERR_INVALID_STATE;
  if (!bsp_lvgl_lock(200))
    return ESP_ERR_TIMEOUT;

  if (compact) {
    /* Shrink panel to just fit the menu text; camera stays visible around it.
     */
    lv_obj_clear_flag(s_panel_response, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_panel_response, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(s_panel_response, 10, LV_PART_MAIN);
    lv_obj_align(s_panel_response, LV_ALIGN_CENTER, 0, 0);
    /* Opaque dark background — no semi-transparency → no pixelation. */
    lv_obj_set_style_bg_color(s_panel_response, lv_color_hex(0x202020),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_panel_response, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_panel_response, 8, LV_PART_MAIN);
    /* White text, bitmap font for sharp rendering. */
    lv_obj_set_style_text_color(s_label_response, lv_color_hex(0xFFFFFF),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_label_response, FONT_STRIP, LV_PART_MAIN);
    lv_obj_set_width(s_label_response, LV_SIZE_CONTENT);
  } else {
    /* Restore full-size opaque panel for AI responses. */
    lv_obj_clear_flag(s_panel_response, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(s_panel_response, CONTENT_W, CONTENT_H);
    lv_obj_set_style_pad_all(s_panel_response, PAD + 1, LV_PART_MAIN);
    lv_obj_align(s_panel_response, LV_ALIGN_TOP_LEFT, MARGIN, CONTENT_Y);
    lv_obj_set_style_bg_color(s_panel_response, lv_color_hex(C_RESPONSE_BG),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_panel_response, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_panel_response, 6, LV_PART_MAIN);
    /* Dark text for contrast on light background. */
    lv_obj_set_style_text_color(s_label_response, lv_color_hex(C_TEXT),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(s_label_response, FONT_BODY, LV_PART_MAIN);
    lv_obj_set_width(s_label_response, CONTENT_W - 2 * (PAD + 1));
  }

  gui_raise_hud();
  bsp_lvgl_unlock();
  return ESP_OK;
}

esp_err_t gui_set_response_panel_visible(bool visible) {
  if (!s_panel_response)
    return ESP_ERR_INVALID_STATE;
  if (!bsp_lvgl_lock(200))
    return ESP_ERR_TIMEOUT;

  if (visible) {
    lv_obj_clear_flag(s_panel_response, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_panel_response, LV_OBJ_FLAG_HIDDEN);
  }
  gui_raise_hud();
  bsp_lvgl_unlock();
  return ESP_OK;
}

esp_err_t gui_scroll_response(int16_t delta_pixels) {
  if (!s_label_response || !s_panel_response)
    return ESP_ERR_INVALID_STATE;
  if (delta_pixels == 0)
    return ESP_OK;
  if (!bsp_lvgl_lock(200))
    return ESP_ERR_TIMEOUT;

  s_response_scroll_y += delta_pixels;
  gui_apply_response_scroll();
  gui_raise_hud();
  bsp_lvgl_unlock();
  return ESP_OK;
}

esp_err_t gui_set_footer(const char *text) {
  if (!s_label_footer)
    return ESP_ERR_INVALID_STATE;
  if (!bsp_lvgl_lock(200))
    return ESP_ERR_TIMEOUT;

  lv_label_set_text(s_label_footer, text ? text : "");
  gui_raise_hud();
  bsp_lvgl_unlock();
  return ESP_OK;
}

esp_err_t gui_set_recording_progress(uint8_t percent) {
  if (percent > 100)
    percent = 100;
  if (!bsp_lvgl_lock(200))
    return ESP_ERR_TIMEOUT;

  /* Show and update bar */
  if (s_bar_progress) {
    lv_obj_clear_flag(s_bar_progress, LV_OBJ_FLAG_HIDDEN);
    lv_bar_set_value(s_bar_progress, (int32_t)percent, LV_ANIM_ON);
  }
  /* Show and update percentage text */
  if (s_label_progress) {
    char txt[16];
    snprintf(txt, sizeof(txt), "REC %u%%", (unsigned)percent);
    lv_obj_clear_flag(s_label_progress, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_label_progress, txt);
    lv_obj_align_to(s_label_progress, s_bar_progress, LV_ALIGN_CENTER, 0, 0);
  }

  gui_raise_hud();
  bsp_lvgl_unlock();
  return ESP_OK;
}

esp_err_t gui_hide_camera_preview(void) {
  if (!s_canvas_preview)
    return ESP_ERR_INVALID_STATE;
  if (!bsp_lvgl_lock(200))
    return ESP_ERR_TIMEOUT;

  lv_obj_add_flag(s_canvas_preview, LV_OBJ_FLAG_HIDDEN);
  gui_raise_hud();
  bsp_lvgl_unlock();
  return ESP_OK;
}

esp_err_t gui_show_camera_preview_rgb565(const uint8_t *rgb565_data,
                                         uint16_t width, uint16_t height) {
  if (!rgb565_data || width == 0 || height == 0 || !s_canvas_preview ||
      !s_preview_buf) {
    if (s_canvas_preview) {
      if (!bsp_lvgl_lock(200))
        return ESP_ERR_TIMEOUT;
      lv_obj_add_flag(s_canvas_preview, LV_OBJ_FLAG_HIDDEN);
      bsp_lvgl_unlock();
    }
    return ESP_ERR_INVALID_ARG;
  }
  if (width != 240 || height != 240) {
    return ESP_ERR_INVALID_SIZE;
  }

  if (!bsp_lvgl_lock(200))
    return ESP_ERR_TIMEOUT;

  memcpy(s_preview_buf, rgb565_data, SCR_W * SCR_H * sizeof(lv_color_t));
  gui_swap_rgb565_bytes((uint16_t *)s_preview_buf, SCR_W * SCR_H);

  lv_obj_clear_flag(s_canvas_preview, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_canvas_preview);
  gui_raise_hud();
  /* NOTE: lv_refr_now() removido intencionalmente.
   * Chamar lv_refr_now() dentro do lock LVGL força um flush SPI síncrono
   * que pode bloquear a taskLVGL por tempo indefinido quando a fila SPI
   * está cheia, impedindo o IDLE0 de rodar e disparando o WDT.
   * O refresh ocorre naturalmente no próximo tick da taskLVGL. */
  lv_obj_invalidate(s_canvas_preview);

  bsp_lvgl_unlock();
  return ESP_OK;
}
