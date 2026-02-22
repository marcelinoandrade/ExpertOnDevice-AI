#include "gui.h"

#include <stdio.h>
#include "bsp.h"
#include "lvgl.h"

static lv_obj_t *s_label_state;
static lv_obj_t *s_label_response;
static char s_state_text[32] = "Pronto";

static esp_err_t gui_set_label_text(lv_obj_t *label, const char *text)
{
    if (!label) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!bsp_lvgl_lock(200)) {
        return ESP_ERR_TIMEOUT;
    }
    lv_label_set_text(label, text ? text : "");
    bsp_lvgl_unlock();
    return ESP_OK;
}

esp_err_t gui_init(void)
{
    if (!bsp_lvgl_lock(-1)) {
        return ESP_FAIL;
    }

    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_text_color(screen, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

    s_label_state = lv_label_create(screen);
    lv_obj_set_width(s_label_state, 230);
    lv_obj_align(s_label_state, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_label_set_text(s_label_state, "Pronto");

    s_label_response = lv_label_create(screen);
    lv_obj_set_width(s_label_response, 230);
    lv_obj_align(s_label_response, LV_ALIGN_TOP_LEFT, 8, 48);
    lv_label_set_long_mode(s_label_response, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_label_response, "Segure o botao para falar");

    bsp_lvgl_unlock();
    return ESP_OK;
}

esp_err_t gui_set_state(const char *state_text)
{
    snprintf(s_state_text, sizeof(s_state_text), "%s", state_text ? state_text : "");
    return gui_set_label_text(s_label_state, s_state_text);
}

esp_err_t gui_set_transcript(const char *text)
{
    (void)text;
    return ESP_OK;
}

esp_err_t gui_set_response(const char *text)
{
    return gui_set_label_text(s_label_response, text ? text : "");
}

esp_err_t gui_set_recording_progress(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    char bar[11];
    const uint8_t filled = (uint8_t)((percent + 9) / 10); // 0..10
    for (uint8_t i = 0; i < 10; i++) {
        bar[i] = (i < filled) ? '#' : '.';
    }
    bar[10] = '\0';

    char line[48];
    snprintf(line, sizeof(line), "Gravando [%s] %u%%", bar, (unsigned)percent);
    return gui_set_label_text(s_label_state, line);
}
