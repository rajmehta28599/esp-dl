#include "ui.hpp"
#include "face_processor.hpp"
#include "ppa_display.hpp"
#include "bsp_display.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <time.h>
#include <cstring>

static const char *TAG = "ui";

static lv_obj_t *s_canvas = nullptr;
static lv_obj_t *s_status_label = nullptr; // headline result (top-left)
static lv_obj_t *s_enroll_label = nullptr; // action feedback (above buttons)
static lv_obj_t *s_stats_label = nullptr;  // R&D dashboard (right)
static lv_obj_t *s_warn_label = nullptr;   // glare / spoof banner (top-center)
static lv_obj_t *s_range_lbl = nullptr;    // RANGE button caption
static lv_obj_t *s_det_lbl = nullptr;      // DET (detector model) button caption
static lv_obj_t *s_rec_lbl = nullptr;      // REC (recognizer model) button caption
static lv_obj_t *s_spoof_lbl = nullptr;    // SPOOF (mode) button caption

// Punch (attendance) card: profile snapshot + id + UTC time, shown briefly on a fresh match.
static lv_obj_t *s_punch_card = nullptr;
static lv_obj_t *s_punch_img = nullptr;
static lv_obj_t *s_punch_id = nullptr;
static lv_obj_t *s_punch_time = nullptr;
static lv_obj_t *s_punch_dist = nullptr;
static lv_img_dsc_t s_punch_dsc;       // points at the punch thumbnail buffer
static bool s_card_showing = false;
static uint32_t s_card_since = 0;
#define PUNCH_CARD_MS 4000

static void style_text_panel(lv_obj_t *label, lv_opa_t bg_opa)
{
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(label, bg_opa, 0);
    lv_obj_set_style_pad_all(label, 6, 0);
    lv_obj_set_style_radius(label, 4, 0);
}

/* -------- button callbacks (run in the LVGL task) -------- */
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
    snprintf(b, sizeof(b), "RNG:%s", face_processor_range_name());
    if (s_range_lbl) {
        lv_label_set_text(s_range_lbl, b);
    }
}

static void det_btn_cb(lv_event_t *e)
{
    (void)e;
    const char *n = face_processor_cycle_det_model(); // applied by AI task shortly
    char b[24];
    snprintf(b, sizeof(b), "DET:%s", n);
    if (s_det_lbl) {
        lv_label_set_text(s_det_lbl, b);
    }
}

static void rec_btn_cb(lv_event_t *e)
{
    (void)e;
    const char *n = face_processor_cycle_feat_model();
    char b[24];
    snprintf(b, sizeof(b), "REC:%s", n);
    if (s_rec_lbl) {
        lv_label_set_text(s_rec_lbl, b);
    }
}

static void spoof_btn_cb(lv_event_t *e)
{
    (void)e;
    const char *n = face_processor_cycle_spoof();
    char b[24];
    snprintf(b, sizeof(b), "SPF:%s", n);
    if (s_spoof_lbl) {
        lv_label_set_text(s_spoof_lbl, b);
    }
}

// Create a touch button; returns the inner LABEL so dynamic captions can be updated.
static lv_obj_t *make_button(lv_obj_t *parent, const char *caption, lv_align_t align, int x, int y,
                             lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 200, 54);
    lv_obj_align(btn, align, x, y);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, caption);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);
    return lbl;
}

/* Refresh the R&D dashboard + warning banner (LVGL task, port lock held). */
static void stats_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_stats_label) {
        return;
    }
    // Static: this callback runs only on the LVGL task and is non-reentrant. Keeps ~1.5 KB
    // of buffers off that task's stack (a stack overflow there would boot-loop on first refresh).
    static pipeline_stats_t s;
    face_processor_get_stats(&s);

    float cap_hz = s.cap_ms > 0.1f ? 1000.0f / s.cap_ms : 0.0f;
    float det_hz = s.det_ms > 0.1f ? 1000.0f / s.det_ms : 0.0f;
    float rec_hz = s.rec_ms > 0.1f ? 1000.0f / s.rec_ms : 0.0f;
    unsigned int_free = s.free_internal / 1024, int_tot = s.total_internal / 1024;
    int int_pct = s.total_internal ? (int)(100ULL * (s.total_internal - s.free_internal) / s.total_internal) : 0;
    float ps_free = s.free_psram / 1048576.0f, ps_tot = s.total_psram / 1048576.0f;
    int ps_pct = s.total_psram ? (int)(100ULL * (s.total_psram - s.free_psram) / s.total_psram) : 0;
    unsigned st_used = (s.store_total - s.store_free) / 1024, st_tot = s.store_total / 1024;

    // Colour-coded fragments (label has recolor enabled): green ok, yellow caution, red alert.
    static char c0[28], c1[28], recln[72], livln[48], lightln[40], posln[56];
    snprintf(c0, sizeof(c0), "#%s %.0f%%#", s.load_core0 > 90 ? "ff5555" : "66ff66", s.load_core0);
    snprintf(c1, sizeof(c1), "#%s %.0f%%#", s.load_core1 > 90 ? "ff5555" : "66ff66", s.load_core1);
    if (s.rec_state == 3) {
        snprintf(recln, sizeof(recln), "#ffcc33 N/A - ESPDet (no landmarks)#");
    } else {
        snprintf(recln, sizeof(recln), "%.0f ms (%.1f/s) [%s]", s.rec_ms, rec_hz,
                 s.rec_state == 1 ? "run" : s.rec_state == 2 ? "wait" : "idle");
    }
    if (s.spoof_mode == 0) {
        snprintf(livln, sizeof(livln), "off");
    } else if (s.spoof_state == 2) {
        snprintf(livln, sizeof(livln), "#ff5555 SPOOF? (%d)#", s.live_score);
    } else {
        snprintf(livln, sizeof(livln), "#66ff66 live (%d)#", s.live_score);
    }
    snprintf(lightln, sizeof(lightln), "%s",
             s.glare ? "#ff5555 GLARE#" : (s.bright ? "#ffcc33 BRIGHT#" : "#66ff66 OK#"));
    if (s.dist_guide == 0) {
        snprintf(posln, sizeof(posln), "no face");
    } else if (s.dist_guide == 1) {
        snprintf(posln, sizeof(posln), "%d mm  #66ff66 OK hold#", s.face_dist_mm);
    } else if (s.dist_guide == 2) {
        snprintf(posln, sizeof(posln), "%d mm  #ffcc33 BACK %d mm#", s.face_dist_mm, s.dist_delta_mm);
    } else {
        snprintf(posln, sizeof(posln), "%d mm  #ffcc33 CLOSER %d mm#", s.face_dist_mm, s.dist_delta_mm);
    }

    static char buf[1100];
    snprintf(buf, sizeof(buf),
             "#22ddff PERFORMANCE#\n"
             "Pipeline  %.0f FPS\n"
             "Capture   %.0f ms (%.0f/s)\n"
             "Copy ROI  %.1f ms\n"
             "Detect    %.0f ms (%.0f/s)\n"
             "Recognize %s\n"
             "Overlay   %.1f ms\n"
             "Display   %.0f ms\n"
             "Load  C0 %s  C1 %s\n"
             "#22ddff MODELS#  (%s)\n"
             "Detect %s  %dx%d\n"
             "Recog  %s  f%d\n"
             "  %.1fM  %.2fG  TAR %.1f%%\n"
             "Spoof  %s   %s\n"
             "#22ddff MEMORY#\n"
             "Int   %u/%u KB (%d%%)\n"
             "PSRAM %.1f/%.1f MB (%d%%)\n"
             "#22ddff STORAGE (face DB)#\n"
             "Faces %d / ~%d max\n"
             "%u/%u KB used\n"
             "#22ddff LIGHT#\n"
             "luma %.0f  sat %.0f%%  %s\n"
             "#22ddff POSITION#  ipd %d px\n"
             "%s",
             s.fps, s.cap_ms, cap_hz, s.copy_ms, s.det_ms, det_hz, recln, s.draw_ms, s.disp_ms, c0, c1,
             s.model_loc, s.det_model, s.model_in_w, s.model_in_h, s.reco_model, s.feat_len, s.feat_params,
             s.feat_gflops, s.feat_tar, face_processor_spoof_name(), livln, int_free, int_tot, int_pct,
             ps_free, ps_tot, ps_pct, s.db_count, s.db_capacity, st_used, st_tot, s.mean_luma,
             s.sat_frac * 100.0f, lightln, s.ipd_px, posln);
    lv_label_set_text(s_stats_label, buf);

    // Warning banner: strongest condition wins (spoof > glare > bright).
    if (s_warn_label) {
        char w[96];
        if (s.spoof_mode != 0 && s.spoof_state == 2) {
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

    // Punch card: rising-edge show, auto-hide after PUNCH_CARD_MS. The thumbnail buffer stays
    // owned by the punch (face_processor won't refill it) until we call punch_consumed() on hide.
    if (s_punch_card) {
        if (!s_card_showing) {
            punch_event_t p;
            const uint16_t *thumb = nullptr;
            if (face_processor_get_punch(&p, &thumb) && thumb) {
                s_punch_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
                s_punch_dsc.header.always_zero = 0;
                s_punch_dsc.header.w = p.thumb_w;
                s_punch_dsc.header.h = p.thumb_h;
                s_punch_dsc.data_size = (uint32_t)p.thumb_w * p.thumb_h * 2;
                s_punch_dsc.data = (const uint8_t *)thumb;
                lv_img_set_src(s_punch_img, &s_punch_dsc);

                char b[48];
                snprintf(b, sizeof(b), "ID %d    sim %.2f", p.id, p.sim);
                lv_label_set_text(s_punch_id, b);
                time_t e = (time_t)p.epoch;
                struct tm tmv;
                gmtime_r(&e, &tmv);
                char ts[40];
                strftime(ts, sizeof(ts), "%Y-%m-%d  %H:%M:%S UTC", &tmv);
                lv_label_set_text(s_punch_time, ts);
                snprintf(b, sizeof(b), "Distance %d mm", p.dist_mm);
                lv_label_set_text(s_punch_dist, b);

                lv_obj_clear_flag(s_punch_card, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(s_punch_card);
                s_card_showing = true;
                s_card_since = lv_tick_get();
            }
        } else if (lv_tick_elaps(s_card_since) > PUNCH_CARD_MS) {
            lv_obj_add_flag(s_punch_card, LV_OBJ_FLAG_HIDDEN);
            s_card_showing = false;
            face_processor_punch_consumed(); // release the thumbnail only now the card is done
        }
    }
}

#if USE_PPA_DISPLAY
// ---- PPA full UI (productionization increment A) ----
// PPA owns the camera rect (top PPA_CAM_H rows); LVGL owns the bottom band (buttons + status), which
// never overlaps it, so PPA writes the camera uncontested. The punch card is a MODAL overlay shown via
// ppa_display_pause() (which drains the in-flight blit so LVGL can own the whole FB) and hidden via
// ppa_display_resume(). The R&D dashboard / thumbnail are deferred to a later increment.
static lv_obj_t *s_ppa_status = nullptr;
static lv_obj_t *s_ppa_fps = nullptr; // prominent live FPS readout in the band

static void ppa_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    static pipeline_stats_t s;
    face_processor_get_stats(&s);
    if (s_ppa_fps) {
        static char fb[24];
        snprintf(fb, sizeof(fb), "%.0f FPS", s.fps);
        lv_label_set_text(s_ppa_fps, fb);
    }
    if (s_ppa_status) {
        static char b[88];
        snprintf(b, sizeof(b), "%s %dx%d   rec %.0f ms   DB %d", s.det_model, s.model_in_w,
                 s.model_in_h, s.rec_ms, s.db_count);
        lv_label_set_text(s_ppa_status, b);
    }

    if (!s_punch_card) {
        return;
    }
    if (!s_card_showing) {
        punch_event_t p;
        const uint16_t *thumb = nullptr;
        if (face_processor_get_punch(&p, &thumb)) {
            char b[48];
            snprintf(b, sizeof(b), "ID %d    sim %.2f", p.id, p.sim);
            lv_label_set_text(s_punch_id, b);
            time_t e = (time_t)p.epoch;
            struct tm tmv;
            gmtime_r(&e, &tmv);
            char ts[40];
            strftime(ts, sizeof(ts), "%Y-%m-%d  %H:%M:%S UTC", &tmv);
            lv_label_set_text(s_punch_time, ts);
            snprintf(b, sizeof(b), "Distance %d mm", p.dist_mm);
            lv_label_set_text(s_punch_dist, b);
            ppa_display_pause(); // drain in-flight blit -> LVGL safely owns the whole FB for the card
            lv_obj_clear_flag(s_punch_card, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(s_punch_card);
            s_card_showing = true;
            s_card_since = lv_tick_get();
        }
    } else if (lv_tick_elaps(s_card_since) > PUNCH_CARD_MS) {
        lv_obj_add_flag(s_punch_card, LV_OBJ_FLAG_HIDDEN);
        s_card_showing = false;
        ppa_display_resume(); // PPA resumes writing the camera region
        face_processor_punch_consumed();
    }
}

static void ui_init_ppa(void)
{
    lvgl_port_lock(0);
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // Bottom chrome band (rows >= PPA_CAM_H): 6 control buttons (2x3) + status. Non-overlapping with
    // the PPA camera rect above.
    char b[24];
    make_button(scr, "ENROLL", LV_ALIGN_BOTTOM_LEFT, 12, -72, enroll_btn_cb);
    make_button(scr, "CLEAR DB", LV_ALIGN_BOTTOM_LEFT, 224, -72, clear_btn_cb);
    snprintf(b, sizeof(b), "RNG:%s", face_processor_range_name());
    s_range_lbl = make_button(scr, b, LV_ALIGN_BOTTOM_LEFT, 436, -72, range_btn_cb);
    snprintf(b, sizeof(b), "DET:%s", face_processor_det_model_name());
    s_det_lbl = make_button(scr, b, LV_ALIGN_BOTTOM_LEFT, 12, -10, det_btn_cb);
    snprintf(b, sizeof(b), "REC:%s", face_processor_feat_model_name());
    s_rec_lbl = make_button(scr, b, LV_ALIGN_BOTTOM_LEFT, 224, -10, rec_btn_cb);
    snprintf(b, sizeof(b), "SPF:%s", face_processor_spoof_name());
    s_spoof_lbl = make_button(scr, b, LV_ALIGN_BOTTOM_LEFT, 436, -10, spoof_btn_cb);

    // Prominent live FPS (band, right side) + a small detail line beneath it.
    s_ppa_fps = lv_label_create(scr);
    lv_obj_set_style_text_color(s_ppa_fps, lv_color_hex(0x66ff66), 0);
    lv_obj_set_style_text_font(s_ppa_fps, &lv_font_montserrat_24, 0);
    lv_label_set_text(s_ppa_fps, "-- FPS");
    lv_obj_align(s_ppa_fps, LV_ALIGN_BOTTOM_RIGHT, -8, -66);

    s_ppa_status = lv_label_create(scr);
    lv_obj_set_style_text_color(s_ppa_status, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_ppa_status, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_ppa_status, "starting...");
    lv_obj_align(s_ppa_status, LV_ALIGN_BOTTOM_RIGHT, -8, -14);

    // Punch card (text-only; centred over the camera). Shown via the pause handoff on a fresh match.
    s_punch_card = lv_obj_create(scr);
    lv_obj_set_size(s_punch_card, 320, 150);
    lv_obj_align(s_punch_card, LV_ALIGN_CENTER, 0, -40);
    lv_obj_set_style_bg_color(s_punch_card, lv_color_hex(0x0e2a16), 0);
    lv_obj_set_style_bg_opa(s_punch_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_punch_card, lv_color_hex(0x33ff88), 0);
    lv_obj_set_style_border_width(s_punch_card, 3, 0);
    lv_obj_set_style_radius(s_punch_card, 10, 0);
    lv_obj_clear_flag(s_punch_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *phdr = lv_label_create(s_punch_card);
    lv_label_set_text(phdr, LV_SYMBOL_OK " PUNCH");
    lv_obj_set_style_text_color(phdr, lv_color_hex(0x66ff99), 0);
    lv_obj_set_style_text_font(phdr, &lv_font_montserrat_24, 0);
    lv_obj_align(phdr, LV_ALIGN_TOP_MID, 0, 4);
    s_punch_id = lv_label_create(s_punch_card);
    lv_obj_set_style_text_color(s_punch_id, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_punch_id, &lv_font_montserrat_20, 0);
    lv_obj_align(s_punch_id, LV_ALIGN_TOP_MID, 0, 44);
    lv_label_set_text(s_punch_id, "");
    s_punch_time = lv_label_create(s_punch_card);
    lv_obj_set_style_text_color(s_punch_time, lv_color_hex(0xcfe8ff), 0);
    lv_obj_set_style_text_font(s_punch_time, &lv_font_montserrat_16, 0);
    lv_obj_align(s_punch_time, LV_ALIGN_TOP_MID, 0, 80);
    lv_label_set_text(s_punch_time, "");
    s_punch_dist = lv_label_create(s_punch_card);
    lv_obj_set_style_text_color(s_punch_dist, lv_color_hex(0xcfe8ff), 0);
    lv_obj_set_style_text_font(s_punch_dist, &lv_font_montserrat_16, 0);
    lv_obj_align(s_punch_dist, LV_ALIGN_TOP_MID, 0, 108);
    lv_label_set_text(s_punch_dist, "");
    lv_obj_add_flag(s_punch_card, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(ppa_timer_cb, 200, nullptr); // ~5 Hz: status line + punch-card poll
    lvgl_port_unlock();
}
#endif // USE_PPA_DISPLAY

void ui_init(void)
{
#if USE_PPA_DISPLAY
    ui_init_ppa();
    return;
#endif
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // Full-screen camera canvas (created first => bottom layer).
    s_canvas = lv_canvas_create(scr);
    lv_obj_set_size(s_canvas, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 0);

    // Headline result (top-left).
    s_status_label = lv_label_create(scr);
    style_text_panel(s_status_label, LV_OPA_50);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_24, 0);
    lv_label_set_text(s_status_label, "Starting camera...");
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 8, 8);

    // Action feedback line (just above the buttons, bottom-left).
    s_enroll_label = lv_label_create(scr);
    style_text_panel(s_enroll_label, LV_OPA_50);
    lv_obj_set_style_text_font(s_enroll_label, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_enroll_label, "Tap ENROLL to register the centered face");
    lv_obj_align(s_enroll_label, LV_ALIGN_BOTTOM_LEFT, 12, -134);

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

    // R&D dashboard (top-right): per-stage perf, models+metrics, memory, storage, light.
    s_stats_label = lv_label_create(scr);
    style_text_panel(s_stats_label, LV_OPA_70);
    lv_label_set_recolor(s_stats_label, true);
    lv_obj_set_style_text_font(s_stats_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(s_stats_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(s_stats_label, "dashboard...");
    lv_obj_align(s_stats_label, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_timer_create(stats_timer_cb, 300, nullptr); // ~3 Hz

    // Buttons: 2 rows x 3, bottom-left (clear of the right-hand dashboard).
    char b[24];
    make_button(scr, "ENROLL", LV_ALIGN_BOTTOM_LEFT, 12, -72, enroll_btn_cb);
    make_button(scr, "CLEAR DB", LV_ALIGN_BOTTOM_LEFT, 224, -72, clear_btn_cb);
    snprintf(b, sizeof(b), "RNG:%s", face_processor_range_name());
    s_range_lbl = make_button(scr, b, LV_ALIGN_BOTTOM_LEFT, 436, -72, range_btn_cb);
    snprintf(b, sizeof(b), "DET:%s", face_processor_det_model_name());
    s_det_lbl = make_button(scr, b, LV_ALIGN_BOTTOM_LEFT, 12, -10, det_btn_cb);
    snprintf(b, sizeof(b), "REC:%s", face_processor_feat_model_name());
    s_rec_lbl = make_button(scr, b, LV_ALIGN_BOTTOM_LEFT, 224, -10, rec_btn_cb);
    snprintf(b, sizeof(b), "SPF:%s", face_processor_spoof_name());
    s_spoof_lbl = make_button(scr, b, LV_ALIGN_BOTTOM_LEFT, 436, -10, spoof_btn_cb);

    // Punch (attendance) card - centred, hidden until a fresh match. Snapshot + id + UTC time.
    s_punch_card = lv_obj_create(scr);
    lv_obj_set_size(s_punch_card, 300, 296);
    lv_obj_align(s_punch_card, LV_ALIGN_CENTER, 0, -16);
    lv_obj_set_style_bg_color(s_punch_card, lv_color_hex(0x0e2a16), 0);
    lv_obj_set_style_bg_opa(s_punch_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_punch_card, lv_color_hex(0x33ff88), 0);
    lv_obj_set_style_border_width(s_punch_card, 3, 0);
    lv_obj_set_style_radius(s_punch_card, 10, 0);
    lv_obj_clear_flag(s_punch_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *phdr = lv_label_create(s_punch_card);
    lv_label_set_text(phdr, LV_SYMBOL_OK " PUNCH");
    lv_obj_set_style_text_color(phdr, lv_color_hex(0x66ff99), 0);
    lv_obj_set_style_text_font(phdr, &lv_font_montserrat_24, 0);
    lv_obj_align(phdr, LV_ALIGN_TOP_MID, 0, 0);

    s_punch_img = lv_img_create(s_punch_card);
    lv_obj_align(s_punch_img, LV_ALIGN_TOP_MID, 0, 36);

    s_punch_id = lv_label_create(s_punch_card);
    lv_obj_set_style_text_color(s_punch_id, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_punch_id, &lv_font_montserrat_20, 0);
    lv_obj_align(s_punch_id, LV_ALIGN_TOP_MID, 0, 146);
    lv_label_set_text(s_punch_id, "");

    s_punch_time = lv_label_create(s_punch_card);
    lv_obj_set_style_text_color(s_punch_time, lv_color_hex(0xcfe8ff), 0);
    lv_obj_set_style_text_font(s_punch_time, &lv_font_montserrat_16, 0);
    lv_obj_align(s_punch_time, LV_ALIGN_TOP_MID, 0, 178);
    lv_label_set_text(s_punch_time, "");

    s_punch_dist = lv_label_create(s_punch_card);
    lv_obj_set_style_text_color(s_punch_dist, lv_color_hex(0xcfe8ff), 0);
    lv_obj_set_style_text_font(s_punch_dist, &lv_font_montserrat_16, 0);
    lv_obj_align(s_punch_dist, LV_ALIGN_TOP_MID, 0, 204);
    lv_label_set_text(s_punch_dist, "");

    lv_obj_add_flag(s_punch_card, LV_OBJ_FLAG_HIDDEN);

    lvgl_port_unlock();
    ESP_LOGI(TAG, "UI initialized");
}

// 1 = decouple the LCD flush from the capture task (faster); 0 = old synchronous lv_refr_now.
// Async path: the capture task copies the finished frame into a display-owned back buffer and just
// invalidates the canvas; the esp_lvgl_port task does the full-screen render + CPU byte-swap + DSI
// transfer on its own 5 ms timer (off core 0). That ~60-130 ms flush no longer blocks capture, so
// the capture/AI FPS rises. The ~few-ms memcpy is the only cost left on core 0 for display.
// Tear-safe by construction: we only ever write the buffer that is NOT currently shown, and the
// pointer swap happens under the LVGL lock (mutually exclusive with the port task's render).
// REVERTED to 0 (Test 009): on-device this REGRESSED FPS (~7.5 -> erratic 6.5-7.7, cap jittered
// 56-272 ms, draw spiked 100-150 ms). Root cause: the esp_lvgl_port render task has affinity -1, so
// the ~80 ms full-screen render + CPU byte-swap floats onto core 0 and preempts capture (it just
// RELOCATES the work; on this 2-busy-core layout there is no free core to absorb it). Only a PPA
// hardware composite ELIMINATES that work. Kept behind the toggle for the future PPA path.
#define DISPLAY_ASYNC_FLUSH 0

void ui_update_camera_canvas(uint8_t *buf, uint32_t w, uint32_t h)
{
    if (!s_canvas) {
        return;
    }
#if DISPLAY_ASYNC_FLUSH
    static uint8_t *s_disp_buf[2] = {nullptr, nullptr};
    static int s_disp_back = 0;
    const size_t sz = (size_t)w * h * 2; // RGB565
    if (!s_disp_buf[0]) {
        s_disp_buf[0] = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
        s_disp_buf[1] = (uint8_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    }
    if (s_disp_buf[0] && s_disp_buf[1]) {
        uint8_t *back = s_disp_buf[s_disp_back]; // the buffer NOT currently on the canvas
        memcpy(back, buf, sz);                   // copy out so the V4L2 buffer can recycle immediately
        if (lvgl_port_lock(100)) {
            lv_canvas_set_buffer(s_canvas, back, (lv_coord_t)w, (lv_coord_t)h, LV_IMG_CF_TRUE_COLOR);
            lv_obj_invalidate(s_canvas); // port task renders on its timer; do NOT block here
            lvgl_port_unlock();
            s_disp_back ^= 1;
        }
        return;
    }
    // Allocation failed -> fall through to the synchronous path below.
#endif
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
