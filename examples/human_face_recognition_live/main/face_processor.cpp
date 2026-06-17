#include "face_processor.hpp"
#include "ui.hpp"

#include "bsp_camera.h"

#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"
#include "dl_image_define.hpp"
#include "dl_image_draw.hpp"
#include "dl_image_jpeg.hpp"

// Embedded known-good test image (from the static human_face_detect example) used for
// the boot self-test that isolates "camera image problem" from "model/threshold problem".
extern const uint8_t human_face_jpg_start[] asm("_binary_human_face_jpg_start");
extern const uint8_t human_face_jpg_end[] asm("_binary_human_face_jpg_end");

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <algorithm>
#include <cstring>
#include <list>
#include <vector>

static const char *TAG = "face_proc";

/*
 * If on-screen overlay colors look wrong (e.g. the box appears blue instead of
 * green), the camera's RGB565 byte order differs from what we assume here -
 * set this to 1 and rebuild.
 */
#ifndef FACE_RGB565_BYTE_SWAP
#define FACE_RGB565_BYTE_SWAP 0
#endif

#define RGB565_GREEN 0x07E0
#define RGB565_RED 0xF800
#define RGB565_YELLOW 0xFFE0

#define MAX_FACES 5
// Only enroll/recognize faces at least this wide (px) to avoid junk features.
#define MIN_FACE_WIDTH 40
// Detector score threshold (lower = more permissive). MSRMNP has two stages.
#define DETECT_SCORE_THR 0.3f

/* ---- Detection ROI (range + speed) ----------------------------------------
 * The SC2336 delivers 1024x600. The detector internally squashes whatever we feed
 * it to a small square model input (resolution logged at boot as model_in_w/h).
 * Feeding the full 1024-wide frame squashes the X axis by ~0.25, so a face at ~1 m
 * shrinks below the smallest detector anchor and is missed. Cropping a centred
 * square (full sensor height) roughly halves the squash, enlarging a 1 m face
 * ~1.7x in model space so it lands on a real anchor and is detected at range.
 * It is also CHEAPER: we copy AI_W*AI_H instead of the whole 1024x600 frame.
 *
 * The detector returns boxes/keypoints in CROP space; we add the crop offset only
 * where boxes are stored for the on-screen overlay (draw_overlays stays in full-
 * frame space). Recognition runs on the very same crop, so its keypoints remain
 * self-consistent and the proven MSRMNP keypoint -> alignment path is untouched.
 *
 * Set CROP_ENABLE to 0 to fall back to the full frame (e.g. if a coordinate bug is
 * suspected) without otherwise changing the pipeline.
 */
#define CROP_ENABLE 1
#if CROP_ENABLE
#define AI_W 600 // centred crop width  (<= CAM_H_RES)
#define AI_H 600 // centred crop height (<= CAM_V_RES)
#else
#define AI_W CAM_H_RES
#define AI_H CAM_V_RES
#endif
#define AI_X_OFF ((CAM_H_RES - (AI_W)) / 2)
#define AI_Y_OFF ((CAM_V_RES - (AI_H)) / 2)
static_assert(AI_W <= CAM_H_RES && AI_H <= CAM_V_RES, "detection ROI must fit inside the camera frame");

// Live telemetry shared with the UI (see pipeline_stats_t). Plain scalars, no lock.
static pipeline_stats_t g_stats = {};

struct FaceBox {
    int x1, y1, x2, y2;
    int kp[10];
    int kp_n;
    bool recognized;
    int id;
    float sim;
    float score;
};

static HumanFaceDetect *g_detect = nullptr;
static HumanFaceRecognizer *g_recognizer = nullptr;

static uint8_t *g_ai_buf = nullptr;
static size_t g_ai_buf_len = 0;

static FaceBox g_results[MAX_FACES];
static int g_result_count = 0;
static SemaphoreHandle_t g_results_mtx = nullptr;

static TaskHandle_t g_ai_task = nullptr;
static volatile bool g_ai_busy = false;
static volatile bool g_enroll_request = false;
static volatile bool g_clear_request = false;

// Camera RGB565 byte/channel order for inference. Determined empirically on this board
// (the runtime calibration locked onto BGR565LE). Hardcoded so there is no startup delay
// or per-frame calibration cost. If you ever fit a different camera module and detection
// returns 0 faces, set CAM_AUTODETECT_PIX to 1 to re-run the 4-way calibration.
#define CAM_AUTODETECT_PIX 0
static dl::image::pix_type_t g_infer_pix = dl::image::DL_IMAGE_PIX_TYPE_BGR565LE;
#if CAM_AUTODETECT_PIX
static bool g_pix_locked = false;
#else
static bool g_pix_locked = true;
#endif

// Recognition (feature extraction, ~200ms) is far slower than detection (~40ms), so we
// run it at most a few times per second on the most prominent face and reuse the result
// for the boxes in between. This keeps box tracking smooth (~detection rate).
#define RECOGNIZE_INTERVAL_US 450000 // ~2 Hz
#define RECO_STICKY_US 2000000       // keep the last recognition on the box for up to 2s

static inline int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static std::vector<uint8_t> color565(uint16_t c)
{
#if FACE_RGB565_BYTE_SWAP
    return {(uint8_t)(c >> 8), (uint8_t)(c & 0xFF)};
#else
    return {(uint8_t)(c & 0xFF), (uint8_t)(c >> 8)};
#endif
}

/* -------- overlay the latest AI results onto the frame about to be displayed -------- */
static void draw_overlays(dl::image::img_t &img)
{
    FaceBox local[MAX_FACES];
    int n = 0;
    if (xSemaphoreTake(g_results_mtx, pdMS_TO_TICKS(5)) == pdTRUE) {
        n = g_result_count;
        memcpy(local, g_results, sizeof(FaceBox) * n);
        xSemaphoreGive(g_results_mtx);
    }

    const int w = img.width;
    const int h = img.height;
    for (int i = 0; i < n; i++) {
        int x1 = clampi(local[i].x1, 0, w - 1);
        int y1 = clampi(local[i].y1, 0, h - 1);
        int x2 = clampi(local[i].x2, 0, w - 1);
        int y2 = clampi(local[i].y2, 0, h - 1);
        if (x2 <= x1 || y2 <= y1) {
            continue;
        }
        auto color = color565(local[i].recognized ? RGB565_GREEN : RGB565_RED);
        dl::image::draw_hollow_rectangle(img, x1, y1, x2, y2, color, 5);

        auto kp_color = color565(RGB565_YELLOW);
        for (int k = 0; k + 1 < local[i].kp_n; k += 2) {
            int kx = clampi(local[i].kp[k], 0, w - 1);
            int ky = clampi(local[i].kp[k + 1], 0, h - 1);
            dl::image::draw_point(img, kx, ky, kp_color, 3);
        }
    }
}

/* -------- camera frame callback (runs in the capture task, core 0) -------- */
static void frame_cb(uint8_t *buf, uint8_t idx, uint32_t w, uint32_t h, size_t len)
{
    (void)idx;
    (void)len;
    int64_t t_frame = esp_timer_get_time();

    // Capture cadence: interval between frames the sensor hands us.
    static int64_t s_last_frame_us = 0;
    if (s_last_frame_us) {
        g_stats.cap_ms = (float)(t_frame - s_last_frame_us) / 1000.0f;
    }
    s_last_frame_us = t_frame;

    // Hand a clean (optionally cropped) copy of the frame to the AI task if it is idle.
    // The crop is a centred AI_W x AI_H window taken row by row out of the full frame.
    if (!g_ai_busy && g_ai_buf && (int)w >= AI_X_OFF + AI_W && (int)h >= AI_Y_OFF + AI_H) {
        int64_t c0 = esp_timer_get_time();
        const int src_stride = (int)w * 2;     // bytes per source row (RGB565)
        const int dst_stride = AI_W * 2;       // bytes per crop row
        const uint8_t *src = buf + (size_t)AI_Y_OFF * src_stride + (size_t)AI_X_OFF * 2;
        uint8_t *dst = g_ai_buf;
        for (int row = 0; row < AI_H; row++) {
            memcpy(dst, src, dst_stride);
            src += src_stride;
            dst += dst_stride;
        }
        g_stats.copy_ms = (float)(esp_timer_get_time() - c0) / 1000.0f;
        g_ai_busy = true;
        if (g_ai_task) {
            xTaskNotifyGive(g_ai_task);
        }
    }

    dl::image::img_t img = {
        .data = buf,
        .width = (uint16_t)w,
        .height = (uint16_t)h,
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
    };
    int64_t d0 = esp_timer_get_time();
    draw_overlays(img);
    g_stats.draw_ms = (float)(esp_timer_get_time() - d0) / 1000.0f;

    int64_t f0 = esp_timer_get_time();
    ui_update_camera_canvas(buf, w, h);
    g_stats.disp_ms = (float)(esp_timer_get_time() - f0) / 1000.0f;

    // Displayed frame rate, averaged over ~1 s windows.
    static int64_t s_fps_t0 = 0;
    static int s_fps_n = 0;
    s_fps_n++;
    if (s_fps_t0 == 0) {
        s_fps_t0 = t_frame;
    } else if (t_frame - s_fps_t0 >= 1000000) {
        g_stats.fps = (float)s_fps_n * 1000000.0f / (float)(t_frame - s_fps_t0);
        s_fps_n = 0;
        s_fps_t0 = t_frame;
    }
}

#if CAM_AUTODETECT_PIX
/* -------- one-time runtime calibration of the camera pixel format -------- */
static void calibrate_pix(dl::image::img_t &img)
{
    static const dl::image::pix_type_t cands[4] = {
        dl::image::DL_IMAGE_PIX_TYPE_RGB565LE,
        dl::image::DL_IMAGE_PIX_TYPE_RGB565BE,
        dl::image::DL_IMAGE_PIX_TYPE_BGR565LE,
        dl::image::DL_IMAGE_PIX_TYPE_BGR565BE,
    };
    static const char *names[4] = {"RGB565LE", "RGB565BE", "BGR565LE", "BGR565BE"};
    int counts[4] = {0, 0, 0, 0};
    int winner = -1;
    for (int c = 0; c < 4; c++) {
        img.pix_type = cands[c];
        counts[c] = (int)g_detect->run(img).size();
        if (winner < 0 && counts[c] >= 1) {
            winner = c;
        }
    }
    ESP_LOGW(TAG, "CALIB faces by format: RGB565LE=%d RGB565BE=%d BGR565LE=%d BGR565BE=%d (show your face!)",
             counts[0], counts[1], counts[2], counts[3]);
    if (winner >= 0) {
        g_infer_pix = cands[winner];
        g_pix_locked = true;
        ESP_LOGW(TAG, "==> LOCKED camera inference format = %s", names[winner]);
    }
}
#endif // CAM_AUTODETECT_PIX

/* -------- AI task (core 1): detect + recognize on the clean copy -------- */
static void ai_task(void *arg)
{
    (void)arg;
    char status[96];

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (g_clear_request) {
            g_recognizer->clear_all_feats();
            g_clear_request = false;
            ui_set_enroll_status("Database cleared");
        }

        g_stats.det_busy = 1;
        dl::image::img_t img = {
            .data = g_ai_buf,
            .width = (uint16_t)AI_W,
            .height = (uint16_t)AI_H,
            .pix_type = g_infer_pix,
        };

        int64_t t0 = esp_timer_get_time();
#if CAM_AUTODETECT_PIX
        if (!g_pix_locked) {
            calibrate_pix(img); // try all 4 formats this frame; locks g_infer_pix on first hit
        }
        img.pix_type = g_infer_pix;
#endif
        // --- Detection: runs every frame (cheap, ~40ms) ---
        std::list<dl::detect::result_t> &dets = g_detect->run(img);

        FaceBox local[MAX_FACES];
        int n = 0;
        int largest_idx = -1;
        int largest_area = 0;
        const dl::detect::result_t *largest_det = nullptr;

        for (auto it = dets.begin(); it != dets.end() && n < MAX_FACES; ++it) {
            const dl::detect::result_t &d = *it;
            if (d.box.size() < 4) {
                continue;
            }
            FaceBox fb;
            fb.x1 = d.box[0];
            fb.y1 = d.box[1];
            fb.x2 = d.box[2];
            fb.y2 = d.box[3];
            fb.kp_n = std::min<int>(d.keypoint.size(), 10);
            for (int k = 0; k < fb.kp_n; k++) {
                fb.kp[k] = d.keypoint[k];
            }
            fb.recognized = false;
            fb.id = -1;
            fb.sim = 0.0f;
            fb.score = d.score;

            int area = (fb.x2 - fb.x1) * (fb.y2 - fb.y1);
            if (area > largest_area) {
                largest_area = area;
                largest_idx = n;
                largest_det = &d;
            }
            local[n++] = fb;
        }

        int64_t now_us = esp_timer_get_time();
        bool largest_ok = largest_det && (largest_det->box[2] - largest_det->box[0]) >= MIN_FACE_WIDTH;

        // --- Recognition: throttled (slow, ~200ms), only on the most prominent face ---
        static int64_t s_last_reco_us = 0;
        static bool s_reco_valid = false;
        static bool s_reco_recognized = false;
        static int s_reco_id = -1;
        static float s_reco_sim = 0.0f;
        if (largest_ok && (now_us - s_last_reco_us > RECOGNIZE_INTERVAL_US)) {
            std::list<dl::detect::result_t> one = {*largest_det};
            std::vector<dl::recognition::result_t> rec = g_recognizer->recognize(img, one);
            s_last_reco_us = now_us;
            s_reco_valid = true;
            if (!rec.empty()) {
                s_reco_recognized = true;
                s_reco_id = rec[0].id;
                s_reco_sim = rec[0].similarity;
            } else {
                s_reco_recognized = false;
                s_reco_id = -1;
                s_reco_sim = 0.0f;
            }
        }
        // Reuse the most recent recognition result for the prominent face's box.
        if (largest_idx >= 0 && s_reco_valid && (now_us - s_last_reco_us < RECO_STICKY_US)) {
            local[largest_idx].recognized = s_reco_recognized;
            local[largest_idx].id = s_reco_id;
            local[largest_idx].sim = s_reco_sim;
        }

        // Handle an enroll request on the most prominent face.
        if (g_enroll_request) {
            if (largest_ok) {
                std::list<dl::detect::result_t> one = {*largest_det};
                if (g_recognizer->enroll(img, one) == ESP_OK) {
                    snprintf(status, sizeof(status), "Enrolled. DB now has %d face(s)",
                             g_recognizer->get_num_feats());
                    s_last_reco_us = 0; // re-recognize immediately so the new face turns green
                } else {
                    snprintf(status, sizeof(status), "Enroll failed");
                }
                ui_set_enroll_status(status);
                g_enroll_request = false;
            } else {
                ui_set_enroll_status("No clear face - face the camera at ~arm's length");
            }
        }

        // Publish results for the display overlay.
        if (xSemaphoreTake(g_results_mtx, portMAX_DELAY) == pdTRUE) {
            memcpy(g_results, local, sizeof(FaceBox) * n);
            g_result_count = n;
            xSemaphoreGive(g_results_mtx);
        }

        int ms = (int)((esp_timer_get_time() - t0) / 1000);
  