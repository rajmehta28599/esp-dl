#include "ui.hpp"
#include "face_processor.hpp"
#include "bsp_display.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

static const char *TAG = "ui";

static lv_obj_t *s_canvas = nullptr;
static lv_obj_t *s_status_label = nullptr;
static lv_obj_t *s_enroll_label = nullptr;

static void style_text_panel(lv_obj_t *label)
{
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(label, 6, 0);
    lv_obj_set_style_radius(label, 4, 0);
}

static void enroll_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_set_enroll_status("Enrolling current face...");
    face_processor_request_enroll();
}

static void clear_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_set_enroll_status("Clearing database...");
    face_processor_clear_db();
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *caption, lv_align_t align, int x_ofs,
                             int y_ofs, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 200, 70);
    lv_obj_align(btn, align, x_ofs, y_ofs);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, caption);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(lbl);
    return btn;
}

void ui_init(void)
{
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // Full-screen canvas that displays the camera frames (created first => bottom layer).
    s_canvas = lv_canvas_create(scr);
    lv_obj_set_size(s_canvas, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 0);

    // Status bar (top-left).
    s_status_label = lv_label_create(scr);
    style_text_panel(s_status_label);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_status_label, "Starting camera...");
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 8, 8);

    // Enrollment status line (just above the buttons).
    s_enroll_label = lv_label_create(scr);
    style_text_panel(s_enroll_label);
    lv_obj_set_style_text_font(s_enroll_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_enroll_label, "Tap ENROLL to register the centered face");
    lv_obj_align(s_enroll_label, LV_ALIGN_BOTTOM_MID, 0, -90);

    // Touch buttons.
    make_button(scr, "ENROLL", LV_ALIGN_BOTTOM_MID, -120, -10, enroll_btn_cb);
    make_button(scr, "CLEAR DB", LV_ALIGN_BOTTOM_MID, 120, -10, clear_btn_cb);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "UI initialized");
}

void ui_update_camera_canvas(uint8_t *buf, uint32_t w, uint32_t h)
{
    if (!s_canvas) {
        return;
    }
    if (lvgl_port_lock(100)) {
        lv_canvas_set_buffer(s_canvas, buf, (lv_coord_t)w, (lv_coord_t)h, LV_IMG_CF_TRUE_COLOR);
        lv_refr_now(NULL);
        lvgl_port_unlock();
    }
}

void ui_set_status(const char *text)
{
    if (!s_status_label) {
        return;
    }
    if (lvgl_port_lock(50)) {
        lv_label_set_text(s_status_label, text);
        lvgl_port_unlock();
    }
}

void ui_set_enroll_status(const char *text)
{
    if (!s_enroll_label) {
        return;
    }
    if (lvgl_port_lock(50)) {
        lv_label_set_text(s_enroll_label, text);
        lvgl_port_unlock();
    }
}
