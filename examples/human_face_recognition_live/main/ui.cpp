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
static lv_obj_t *s_stats_label = nullptr;
static lv_obj_t *s_warn_label = nullptr;  // glare / spoof warning banner (top-center)
static lv_obj_t *s_range_lbl = nullptr;   // caption of the RANGE toggle button
static lv_obj_t *s_reco_lbl = nullptr;    // caption of the RECO toggle button
static lv_obj_t *s_spoof_lbl = nullptr;   // caption of the SPOOF toggle button

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

static void range_btn_cb(lv_event_t *e)
{
    (void)e;
    face_processor_cycle_range();
    char b[24];
    snprintf(b, sizeof(b), "RANGE:%s", face_processor_range_name());
    if (s_range_lbl) {
        lv_label_set_text(s_range_lbl, b);
    }
}

static void reco_btn_cb(lv_event_t *e)
{
    (void)e;
    int on = face_processor_toggle_reco();
    if (s_reco_lbl) {
        lv_label_set_text(s_reco_lbl, on ? "RECO:On" : "RECO:Off");
    }
}

static void spoof_btn_cb(lv_event_t *e)
{
    (void)e;
    int on = face_processor_toggle_spoof();
    if (s_spoof_lbl) {
        lv_label_set_text(s_spoof_lbl, on ? "SPOOF:On" : "SPOOF:Off");
    }
}

// Create a bottom-row touch button; returns the inner LABEL so toggle captions can be updated.
static lv_obj_t *make_button(lv_obj_t *parent, const char *caption, int x_ofs, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 184, 56);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, x_ofs, -10);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, caption);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);
    return lbl;
}

/* Periodically refresh the live stats panel. Runs in the LVGL task with the port
 * lock held (lv_timer callbacks always do), so touching widgets here is safe. */
static void stats_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_stats_label) {
        return;
    }
    pipeline_stats_t s;
    face_processor_get_stats(&s);

    const char *rec = (s.rec_state == 1) ? "run" : (s.rec_state == 2) ? "wait" : "off";
    const char *live = (s.spoof_state == 2) ? " SPOOF?" : (s.spoof_state == 1) ? " live" : "";
    unsigned int_free = s.free_internal / 1024;
    unsigned int_used = (s.total_internal - s.free_internal) / 1024;
    unsigned int_tot = s.total_internal / 1024;
    unsigned int_lg = s.largest_internal / 1024;
    unsigned int_min = s.min_free_internal / 1024;
    unsigned ps_free = s.free_psram / 1024;
    unsigned ps_used = (s.total_psram - s.free_psram) / 1024;
    unsigned ps_tot = s.total_psram / 1024;
    unsigned st_used = (s.store_total - s.store_free) / 1024;
    unsigned st_tot = s.store_total / 1024;

    char buf[768];
    snprintf(buf, sizeof(buf),
             "FPS %2.0f    RNG %s\n"
             "Cap%4.1f Cpy%4.1f Drw%4.1f\n"
             "Det%4.1f%s Dsp%4.1f ms\n"
             "Rec%5.1f ms [%s]\n"
             "Faces %d  DB %d%s\n"
             "- MODELS -\n"
             "D %s %dx%d\n"
             "R %s  feat%d\n"
             "- MEM KB free/used/tot -\n"
             "Int %u/%u/%u\n"
             " lgst %u  min %u\n"
             "PSR %u/%u/%u\n"
             "- STORE (face DB) -\n"
             "%d faces, cap ~%d\n"
             "%u/%u KB used\n"
             "- LIGHT -\n"
             "luma %3.0f  sat %2.0f%%%s\n"
             "RECO:%s  SPOOF:%s",
             s.fps, face_processor_range_name(), s.cap_ms, s.copy_ms, s.draw_ms, s.det_ms,
             s.det_busy ? "*" : " ", s.disp_ms, s.rec_ms, rec, s.faces, s.db_count, live, s.det_model,
             s.model_in_w, s.model_in_h, s.reco_model, s.feat_len, int_free, int_used, int_tot, int_lg,
             int_min, ps_free, ps_used, ps_tot, s.db_count, s.db_capacity, st_used, st_tot, s.mean_luma,
             s.sat_frac * 100.0f, s.glare ? " GLARE" : (s.bright ? " BRIGHT" : ""), s.reco_on ? "On" : "Off",
             s.spoof_on ? "On" : "Off");
    lv_label_set_text(s_stats_label, buf);

    // Warning banner: strongest condition wins (spoof > glare > bright).
    if (s_warn_label) {
        char w[96];
        if (s.spoof_state == 2) {
            snprintf(w, sizeof(w), LV_SYMBOL_WARNING " SPOOF SUSPECTED  (liveness %d)", s.live_score);
        } else if (s.glare) {
            snprintf(w, sizeof(w), LV_SYMBOL_WARNING " STRONG GLARE - reduce light / reposition");
        } else if (s.bright) {
            snprintf(w, sizeof(w), LV_SYMBOL_WARNING " TOO BRIGHT - reduce lighting");
        } else {
            w[0] = '\0';
        }
        if (w[0]) {
            lv_label_set_text(s_warn_label, w);
            lv_obj_clear_flag(s_warn_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_warn_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
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
    lv_obj_align(s_enroll_label, LV_ALIGN_BOTTOM_MID, 0, -76);

    // Warning banner (top-center): glare / over-exposure / suspected spoof. Hidden when clear.
    s_warn_label = lv_label_create(scr);
    lv_obj_set_style_bg_color(s_warn_label, lv_color_hex(0xC02020), 0);
    lv_obj_set_style_bg_opa(s_warn_label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_warn_label, lv_color_white(), 0);
    lv_obj_set_style_pad_all(s_warn_label, 8, 0);
    lv_obj_set_style_radius(s_warn_label, 6, 0);
    lv_obj_set_style_text_font(s_warn_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_warn_label, "");
    lv_obj_align(s_warn_label, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_add_flag(s_warn_label, LV_OBJ_FLAG_HIDDEN);

    // Live stats panel (top-right): per-stage timing, memory, model/ROI info.
    s_stats_label = lv_label_create(scr);
    style_text_panel(s_stats_label);
    lv_obj_set_style_text_font(s_stats_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_stats_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(s_stats_label, "stats...");
    lv_obj_align(s_stats_label, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_timer_create(stats_timer_cb, 300, nullptr); // refresh ~3 Hz

    // Touch buttons: 5 across the bottom (pitch 198 px, centred).
    char rb[24];
    snprintf(rb, sizeof(rb), "RANGE:%s", face_processor_range_name());
    make_button(scr, "ENROLL", -396, enroll_btn_cb);
    make_button(scr, "CLEAR DB", -198, clear_btn_cb);
    s_range_lbl = make_button(scr, rb, 0, range_btn_cb);
    s_reco_lbl = make_button(scr, "RECO:On", 198, reco_btn_cb);
    s_spoof_lbl = make_button(scr, "SPOOF:Off", 396, spoof_btn_cb);

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
