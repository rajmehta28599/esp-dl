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
#include "esp_vfs_fat.h"
#include "sdkconfig.h"

#include <algorithm>
#include <cstdlib>
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
#define RGB565_MAGENTA 0xF81F // suspected-spoof face box

#define MAX_FACES 5
// Only enroll/recognize faces at least this wide (px) to avoid junk features.
#define MIN_FACE_WIDTH 40
// Detector score threshold (lower = more permissive). MSRMNP has two stages.
#define DETECT_SCORE_THR 0.3f

/* ---- Detection range / ROI crop -------------------------------------------
 * The SC2336 delivers 1024x600 but the detector squashes its input down to a tiny
 * 160x120 (4:3) tensor (logged at boot). Feeding the full 1024-wide frame squashes
 * X by ~0.16, so a face at ~1 m shrinks below the detector's smallest anchor and is
 * missed. Feeding a centred 4:3 crop instead reduces the squash (a tighter crop ->
 * bigger faces in the tensor -> longer usable range), at the cost of field of view.
 * All crops are 4:3 to match the tensor so faces are not distorted.
 *
 * The crop is taken row by row during the frame->AI-buffer copy (cheaper than the
 * old full-frame memcpy). The detector returns boxes/keypoints in CROP space; the
 * crop offset is added only where boxes are stored for the overlay (draw_overlays
 * stays in full-frame space). Recognition runs on the same crop, so its keypoints
 * stay self-consistent and the MSRMNP keypoint -> alignment path is untouched.
 *
 * The mode is switchable live from the on-screen RANGE button.
 */
typedef enum { RANGE_FULL = 0, RANGE_WIDE, RANGE_MED, RANGE_TIGHT, RANGE_COUNT } range_mode_t;
typedef struct {
    const char *name;
    int w, h; // centred crop size, must fit inside CAM_H_RES x CAM_V_RES and be 4:3-ish
} range_cfg_t;
static const range_cfg_t RANGE_MODES[RANGE_COUNT] = {
    {"Full", CAM_H_RES, CAM_V_RES}, // whole frame, widest FOV, most squash (shortest range)
    {"Wide", 800, 600},             // 4:3
    {"Med", 640, 480},              // 4:3
    {"Tight", 480, 360},            // 4:3, biggest faces in tensor (longest range), narrow FOV
};
static volatile int g_range_mode = RANGE_MED; // default; changed by the RANGE button

// Runtime feature toggles (driven by the on-screen buttons).
static volatile bool g_reco_enabled = true;
static volatile bool g_spoof_enabled = false;

// Dimensions/offset actually used for the in-flight AI job. frame_cb fills these in
// when it hands a frame to the AI task; the busy handshake guarantees the AI task
// reads the same values frame_cb used for the copy (no mid-job change).
static volatile int g_ai_w = 640, g_ai_h = 480, g_ai_xoff = 192, g_ai_yoff = 60;

// Live telemetry shared with the UI (see pipeline_stats_t). Plain scalars, no lock.
static pipeline_stats_t g_stats = {};

struct FaceBox {
    int x1, y1, x2, y2;
    int kp[10];
    int kp_n;
    bool recognized;
    bool spoof; // suspected spoof (basic liveness heuristic)
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
        uint16_t bc = local[i].spoof ? RGB565_MAGENTA : (local[i].recognized ? RGB565_GREEN : RGB565_RED);
        auto color = color565(bc);
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

    // Hand a clean, cropped copy of the frame to the AI task if it is idle. The crop is a
    // centred window (size = the current range mode) taken row by row out of the full frame.
    if (!g_ai_busy && g_ai_buf) {
        int m = g_range_mode;
        if (m < 0 || m >= RANGE_COUNT) {
            m = RANGE_MED;
        }
        int cw = RANGE_MODES[m].w, ch = RANGE_MODES[m].h;
        if (cw > (int)w) cw = (int)w;
        if (ch > (int)h) ch = (int)h;
        int xoff = ((int)w - cw) / 2, yoff = ((int)h - ch) / 2;

        int64_t c0 = esp_timer_get_time();
        const int src_stride = (int)w * 2;     // bytes per source row (RGB565)
        const int dst_stride = cw * 2;         // bytes per crop row
        const uint8_t *src = buf + (size_t)yoff * src_stride + (size_t)xoff * 2;
        uint8_t *dst = g_ai_buf;
        for (int row = 0; row < ch; row++) {
            memcpy(dst, src, dst_stride);
            src += src_stride;
            dst += dst_stride;
        }
        g_stats.copy_ms = (float)(esp_timer_get_time() - c0) / 1000.0f;

        // Publish the dims used for THIS job before waking the AI task. Safe because the
        // AI task will not run (and frame_cb will not overwrite) until g_ai_busy clears.
        g_ai_w = cw;
        g_ai_h = ch;
        g_ai_xoff = xoff;
        g_ai_yoff = yoff;
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

/* -------- exposure / glare / liveness analysis on the AI buffer -------- */
// Thresholds (tunable; the live panel shows the raw numbers so they can be recalibrated).
#define EXPOSURE_SAMPLE_STEP 6 // sample every Nth pixel for the whole-frame exposure scan
#define BRIGHT_LUMA_THR 200.0f // mean luma above this -> "too bright"
#define GLARE_SAT_THR 0.18f    // blown-highlight fraction above this -> "glare"
#define LIVE_TEXTURE_K 4.0f    // face texture energy -> score gain
#define LIVE_SPOOF_THR 25      // liveness score below this -> suspected spoof
#define LIVE_SAT_THR 0.25f     // face this blown-out -> likely a screen/glare -> penalise

static inline int luma_bgr565(uint16_t p)
{
    // RGB565/BGR565 differ only in which 5-bit field is R vs B; for a luma estimate the
    // small weight swap is irrelevant. Expand 5/6-bit fields to 8-bit and take ~(a+2g+b)/4.
    int a = ((p >> 11) & 0x1F) << 3;
    int g = ((p >> 5) & 0x3F) << 2;
    int b = (p & 0x1F) << 3;
    return (a + 2 * g + b) >> 2;
}

struct region_stats_t {
    float mean_luma; // 0..255
    float sat_frac;  // 0..1 fraction of near-white pixels
    float texture;   // mean |horizontal luma gradient| (high = lots of detail)
};

// Scan a rectangular region of the (BGR565) AI buffer with the given pixel step.
static region_stats_t analyze_region(const uint16_t *buf, int bw, int bh, int x0, int y0, int x1,
                                     int y1, int step)
{
    region_stats_t s = {0.0f, 0.0f, 0.0f};
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > bw) x1 = bw;
    if (y1 > bh) y1 = bh;
    if (step < 1) step = 1;
    long sum = 0, sat = 0, grad = 0, n = 0, gn = 0;
    for (int y = y0; y < y1; y += step) {
        const uint16_t *row = buf + (size_t)y * bw;
        int prev = -1;
        for (int x = x0; x < x1; x += step) {
            int l = luma_bgr565(row[x]);
            sum += l;
            n++;
            if (l >= 245) {
                sat++;
            }
            if (prev >= 0) {
                grad += abs(l - prev);
                gn++;
            }
            prev = l;
        }
    }
    if (n > 0) {
        s.mean_luma = (float)sum / (float)n;
        s.sat_frac = (float)sat / (float)n;
    }
    if (gn > 0) {
        s.texture = (float)grad / (float)gn;
    }
    return s;
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
        // Dimensions/offset for this job, as set by frame_cb (stable until g_ai_busy clears).
        const int aw = g_ai_w, ah = g_ai_h, axoff = g_ai_xoff, ayoff = g_ai_yoff;
        dl::image::img_t img = {
            .data = g_ai_buf,
            .width = (uint16_t)aw,
            .height = (uint16_t)ah,
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
        int64_t det0 = esp_timer_get_time();
        std::list<dl::detect::result_t> &dets = g_detect->run(img);
        g_stats.det_ms = (float)(esp_timer_get_time() - det0) / 1000.0f;

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
            // Detector coordinates are in CROP space; shift to full-frame space for the
            // on-screen overlay. (offset is 0 in Full mode.)
            FaceBox fb;
            fb.x1 = d.box[0] + axoff;
            fb.y1 = d.box[1] + ayoff;
            fb.x2 = d.box[2] + axoff;
            fb.y2 = d.box[3] + ayoff;
            fb.kp_n = std::min<int>(d.keypoint.size(), 10);
            for (int k = 0; k < fb.kp_n; k++) {
                fb.kp[k] = d.keypoint[k] + ((k & 1) ? ayoff : axoff);
            }
            fb.recognized = false;
            fb.spoof = false;
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
        g_stats.rec_state = (g_reco_enabled && largest_ok) ? 2 : 0; // 2 = face present, waiting for window
        if (g_reco_enabled && largest_ok && (now_us - s_last_reco_us > RECOGNIZE_INTERVAL_US)) {
            std::list<dl::detect::result_t> one = {*largest_det};
            int64_t rec0 = esp_timer_get_time();
            std::vector<dl::recognition::result_t> rec = g_recognizer->recognize(img, one);
            g_stats.rec_ms = (float)(esp_timer_get_time() - rec0) / 1000.0f;
            g_stats.rec_state = 1; // ran this frame
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

        // --- Exposure / glare on the whole analysed crop ---
        {
            region_stats_t rs =
                analyze_region((const uint16_t *)g_ai_buf, aw, ah, 0, 0, aw, ah, EXPOSURE_SAMPLE_STEP);
            g_stats.mean_luma = rs.mean_luma;
            g_stats.sat_frac = rs.sat_frac;
            g_stats.bright = (rs.mean_luma > BRIGHT_LUMA_THR) ? 1 : 0;
            g_stats.glare = (rs.sat_frac > GLARE_SAT_THR) ? 1 : 0;
        }

        // --- Anti-spoof (basic heuristic) on the most prominent face ---
        // Real faces carry rich texture; a flat photo has little, and a phone/monitor held up
        // tends to blow out (glare). NOT bank-grade - advisory only (esp-dl ships no liveness model).
        static int s_live_cx = -1, s_live_cy = -1;
        if (g_spoof_enabled && largest_ok) {
            // largest_det box is in CROP space, so it indexes g_ai_buf directly.
            region_stats_t fs = analyze_region((const uint16_t *)g_ai_buf, aw, ah, largest_det->box[0],
                                               largest_det->box[1], largest_det->box[2],
                                               largest_det->box[3], 2);
            int score = (int)(fs.texture * LIVE_TEXTURE_K);
            if (fs.sat_frac > LIVE_SAT_THR) {
                score -= 40; // blown-out face -> likely a screen/photo under glare
            }
            int cx = (largest_det->box[0] + largest_det->box[2]) / 2;
            int cy = (largest_det->box[1] + largest_det->box[3]) / 2;
            if (s_live_cx >= 0 && (abs(cx - s_live_cx) + abs(cy - s_live_cy)) > 4) {
                score += 10; // natural movement -> evidence of a live subject (bonus only)
            }
            s_live_cx = cx;
            s_live_cy = cy;
            score = score < 0 ? 0 : (score > 100 ? 100 : score);
            g_stats.live_score = score;
            g_stats.spoof_state = (score < LIVE_SPOOF_THR) ? 2 : 1;
            if (largest_idx >= 0 && g_stats.spoof_state == 2) {
                local[largest_idx].spoof = true;
            }
        } else {
            g_stats.spoof_state = 0;
            g_stats.live_score = 0;
            s_live_cx = s_live_cy = -1;
        }

        // Publish results for the display overlay.
        if (xSemaphoreTake(g_results_mtx, portMAX_DELAY) == pdTRUE) {
            memcpy(g_results, local, sizeof(FaceBox) * n);
            g_result_count = n;
            xSemaphoreGive(g_results_mtx);
        }

        int ms = (int)((esp_timer_get_time() - t0) / 1000);
        int show = (largest_idx >= 0) ? largest_idx : 0;
        int db_count = g_recognizer->get_num_feats();
        if (n == 0) {
            snprintf(status, sizeof(status), "Faces: 0    DB: %d", db_count);
        } else if (local[show].recognized) {
            snprintf(status, sizeof(status), "Faces: %d    ID %d  (%.2f)    DB: %d",
                     n, local[show].id, local[show].sim, db_count);
        } else {
            snprintf(status, sizeof(status), "Faces: %d    unknown    DB: %d", n, db_count);
        }
        ui_set_status(status);

        // Publish live telemetry for the on-screen stats panel.
        g_stats.faces = n;
        g_stats.db_count = db_count;
        g_stats.reco_on = g_reco_enabled ? 1 : 0;
        g_stats.spoof_on = g_spoof_enabled ? 1 : 0;
        g_stats.range_mode = g_range_mode;
        g_stats.ai_w = aw;
        g_stats.ai_h = ah;
        g_stats.free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        g_stats.total_internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
        g_stats.largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        g_stats.min_free_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        g_stats.free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        g_stats.total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        g_stats.largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

        // Throttled serial diagnostics + storage capacity (~1 Hz; FAT info is relatively costly).
        static int64_t last_log_us = 0;
        if (now_us - last_log_us > 1000000) {
            last_log_us = now_us;
            uint64_t st = 0, sf = 0;
            if (esp_vfs_fat_info(CONFIG_SPIFLASH_MOUNT_POINT, &st, &sf) == ESP_OK) {
                g_stats.store_total = (uint32_t)st;
                g_stats.store_free = (uint32_t)sf;
                int rec_sz = g_stats.feat_len * (int)sizeof(float) + (int)sizeof(uint16_t);
                if (rec_sz > 0) {
                    g_stats.db_capacity = db_count + (int)(sf / (uint64_t)rec_sz);
                }
            }
            ESP_LOGI(TAG, "DET %d face(s) %dms  %s", n, ms,
                     (n && local[show].recognized) ? "[recognized]" : "");
        }

        g_stats.det_busy = 0;
        g_ai_busy = false;
    }
}

/* Run the detector once on the embedded known-good image. This isolates a camera-image
 * problem (selftest finds the face, live camera doesn't) from a model/threshold problem
 * (selftest also finds nothing). */
static void detect_selftest(void)
{
    dl::image::jpeg_img_t jpeg = {
        .data = (void *)human_face_jpg_start,
        .data_len = (size_t)(human_face_jpg_end - human_face_jpg_start),
    };
    dl::image::img_t img = dl::image::sw_decode_jpeg(jpeg, dl::image::DL_IMAGE_PIX_TYPE_RGB888);
    if (!img.data) {
        ESP_LOGE(TAG, "SELFTEST: jpeg decode failed");
        return;
    }
    std::list<dl::detect::result_t> &dets = g_detect->run(img);
    ESP_LOGW(TAG, "SELFTEST embedded %dx%d image -> %d face(s)", img.width, img.height, (int)dets.size());
    int i = 0;
    for (auto &d : dets) {
        ESP_LOGW(TAG, "  selftest #%d score=%.2f box=[%d,%d,%d,%d]", i++, d.score, d.box[0], d.box[1],
                 d.box[2], d.box[3]);
    }
    heap_caps_free(img.data);
}

esp_err_t face_processor_init(const char *db_path)
{
    g_results_mtx = xSemaphoreCreateMutex();
    if (!g_results_mtx) {
        return ESP_ERR_NO_MEM;
    }

    g_ai_buf_len = (size_t)CAM_H_RES * CAM_V_RES * 2;
    g_ai_buf = (uint8_t *)heap_caps_malloc(g_ai_buf_len, MALLOC_CAP_SPIRAM);
    if (!g_ai_buf) {
        ESP_LOGE(TAG, "failed to allocate AI buffer (%u bytes)", (unsigned)g_ai_buf_len);
        return ESP_ERR_NO_MEM;
    }

    g_detect = new HumanFaceDetect();
    g_detect->set_score_thr(DETECT_SCORE_THR, 0); // MSR stage
    g_detect->set_score_thr(DETECT_SCORE_THR, 1); // MNP stage
    g_recognizer = new HumanFaceRecognizer(std::string(db_path));
    ESP_LOGI(TAG, "models created, DB has %d face(s)", g_recognizer->get_num_feats());

    // Static info for the panel: model names, feature length, initial toggle/range state.
    strncpy(g_stats.det_model, "msr+mnp_s8_v1", sizeof(g_stats.det_model) - 1);
    strncpy(g_stats.reco_model, "mfn_s8_v1", sizeof(g_stats.reco_model) - 1);
    g_stats.feat_len = g_recognizer->get_feat_model()->get_feat_len();
    g_stats.reco_on = g_reco_enabled ? 1 : 0;
    g_stats.spoof_on = g_spoof_enabled ? 1 : 0;
    g_stats.range_mode = g_range_mode;

    detect_selftest(); // also forces the (lazy) model load, so get_raw_model() is valid below

    // Record the detector's true input resolution (NHWC) for the on-screen panel and
    // to make the ROI squash factor visible. This is the number the range math hinges on.
    g_stats.ai_w = RANGE_MODES[RANGE_MED].w;
    g_stats.ai_h = RANGE_MODES[RANGE_MED].h;
    dl::Model *msr = g_detect->get_raw_model(0);
    if (msr && msr->get_input() && msr->get_input()->shape.size() >= 3) {
        g_stats.model_in_h = msr->get_input()->shape[1];
        g_stats.model_in_w = msr->get_input()->shape[2];
    }
    ESP_LOGI(TAG, "detector input %dx%d; range modes available, default '%s' (%dx%d) of %dx%d frame",
             g_stats.model_in_w, g_stats.model_in_h, RANGE_MODES[g_range_mode].name,
             RANGE_MODES[g_range_mode].w, RANGE_MODES[g_range_mode].h, CAM_H_RES, CAM_V_RES);

    video_register_frame_operation_cb(frame_cb);

    // Pin AI to core 1 (APP CPU); camera+display run on core 0.
    BaseType_t ok = xTaskCreatePinnedToCore(ai_task, "face_ai", 1024 * 12, nullptr, 4, &g_ai_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create AI task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void face_processor_request_enroll(void)
{
    g_enroll_request = true;
}

void face_processor_clear_db(void)
{
    g_clear_request = true;
}

void face_processor_get_stats(pipeline_stats_t *out)
{
    if (out) {
        *out = g_stats; // scalar fields; a torn snapshot is cosmetically harmless
    }
}

void face_processor_cycle_range(void)
{
    int m = g_range_mode + 1;
    g_range_mode = (m >= RANGE_COUNT) ? 0 : m;
}

const char *face_processor_range_name(void)
{
    int m = g_range_mode;
    if (m < 0 || m >= RANGE_COUNT) {
        m = RANGE_MED;
    }
    return RANGE_MODES[m].name;
}

int face_processor_toggle_reco(void)
{
    g_reco_enabled = !g_reco_enabled;
    return g_reco_enabled ? 1 : 0;
}

int face_processor_toggle_spoof(void)
{
    g_spoof_enabled = !g_spoof_enabled;
    return g_spoof_enabled ? 1 : 0;
}
