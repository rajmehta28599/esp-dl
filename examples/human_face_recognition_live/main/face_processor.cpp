#include "face_processor.hpp"
#include "ui.hpp"
#include "ppa_display.hpp"

#include "bsp_camera.h"

#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"
#include "yunet_detect.hpp"
#include "person_db.hpp"
#include "dl_image_define.hpp"
#include "dl_image_draw.hpp"
#include "dl_image_jpeg.hpp"
#include "dl_tensor_base.hpp"

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
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <list>
#include <string>
#include <sys/time.h>
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

// Mirror the shown image left<->right so the person's left appears on the screen's left.
// DISPLAY-ONLY: applied after overlays + after the AI copy, so detection, recognition and the
// face DB are unaffected, and the LVGL text/buttons (composited separately) are NOT mirrored.
// If the flip is the wrong way for your camera mounting, set this to 0.
// SPEED PROBE (Test 010): set to 0 to drop the full-frame ~20 ms/frame CPU mirror from core 0
// (the one work-ELIMINATING display lever that needs no rewrite). Cosmetic only — the live preview
// is no longer left-right flipped. Set back to 1 if the selfie-style flip is wanted on the kiosk.
#define DISPLAY_MIRROR_X 0

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

/* ---- Switchable models (all packed into flash via CONFIG_FLASH_*) ----------
 * Detector and recognizer can be swapped live from the on-screen DET/REC buttons.
 * The swap is performed by the AI task between frames (it exclusively owns g_detect/
 * g_recognizer); the buttons only post a request index.
 */
typedef enum { DET_KIND_HFD, DET_KIND_YUNET } det_kind_t;
typedef struct {
    det_kind_t kind;
    HumanFaceDetect::model_type_t hfd_type; // used only when kind == DET_KIND_HFD
    const char *name;
    bool has_kp; // provides 5-pt landmarks? ESPDet does not -> recognition disabled while active
} det_model_cfg_t;
static const det_model_cfg_t DET_MODELS[] = {
    // The two recognition-capable detectors (have 5 landmarks) are first/adjacent, so the DET
    // button toggles MSRMNP <-> YuNet in one tap. ESPDet (detection-only) follows.
    {DET_KIND_HFD, HumanFaceDetect::MSRMNP_S8_V1, "MSRMNP", true},
    {DET_KIND_YUNET, HumanFaceDetect::MSRMNP_S8_V1, "YuNet", true}, // YuNet emits 5 landmarks
    {DET_KIND_HFD, HumanFaceDetect::ESPDET_PICO_224_224_FACE, "ESPDet224", false},
    {DET_KIND_HFD, HumanFaceDetect::ESPDET_PICO_416_416_FACE, "ESPDet416", false},
};
#define DET_MODEL_COUNT ((int)(sizeof(DET_MODELS) / sizeof(DET_MODELS[0])))

// Build the detector for DET_MODELS[idx]. Only the AI task calls this (owns g_detect).
static dl::detect::Detect *make_detector(int idx)
{
    if (DET_MODELS[idx].kind == DET_KIND_YUNET) {
        return new YuNetDetect(); // score/nms/top_k defaults; tune in yunet_detect.cpp
    }
    HumanFaceDetect *h = new HumanFaceDetect(DET_MODELS[idx].hfd_type);
    h->set_score_thr(DETECT_SCORE_THR, 0);
    if (DET_MODELS[idx].hfd_type == HumanFaceDetect::MSRMNP_S8_V1) {
        h->set_score_thr(DETECT_SCORE_THR, 1); // MNP 2nd stage (single-stage ESPDet ignores idx 1)
    }
    return h;
}

typedef struct {
    HumanFaceFeat::model_type_t type;
    const char *name;
    const char *db_file;
    float params_m, gflops, tar; // metrics: params(M), GFLOPs, TAR@FAR=1e-4 on IJB-C (%)
} feat_model_cfg_t;
static const feat_model_cfg_t FEAT_MODELS[] = {
    {HumanFaceFeat::MFN_S8_V1, "MFN", "face_mfn.db", 1.2f, 0.46f, 90.03f},
    {HumanFaceFeat::MBF_S8_V1, "MBF", "face_mbf.db", 3.4f, 0.90f, 93.94f},
};
#define FEAT_MODEL_COUNT ((int)(sizeof(FEAT_MODELS) / sizeof(FEAT_MODELS[0])))

static const char *SPOOF_NAMES[] = {"Off", "Texture", "Tex+Motion"};
#define SPOOF_MODE_COUNT 3

static volatile int g_det_model_idx = 0;
static volatile int g_feat_model_idx = 0;
static volatile int g_spoof_mode = 0;
static volatile bool g_det_has_kp = true; // mirrors DET_MODELS[g_det_model_idx].has_kp

// Switch requests posted by the UI, consumed by the AI task (-1 = none).
static volatile int g_det_switch_req = -1;
static volatile int g_feat_switch_req = -1;

static char g_db_dir[80]; // directory holding the per-model face databases

// Recognition result cache (file scope so a model switch can invalidate it).
static int64_t g_last_reco_us = 0;
static bool g_reco_valid = false;
static bool g_reco_recognized = false;
static int g_reco_id = -1;
static float g_reco_sim = 0.0f;

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

static dl::detect::Detect *g_detect = nullptr; // MSRMNP/ESPDet (HumanFaceDetect) or YuNetDetect
static HumanFaceRecognizer *g_recognizer = nullptr; // kept for FEATURE EXTRACTION only (get_feat_model)

// Multi-template person database (see person_db.hpp). Owns enrollment storage + matching now;
// g_recognizer is used only to extract the aligned/L2-normalised feature. Touched ONLY by ai_task.
static PersonDB g_persons;

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
// Detection throttle: run the detector on every Nth AI frame and reuse the last boxes for the
// overlay in between. At ~7 fps a face barely moves between frames, so this ~halves the detector's
// core-1 duty (YuNet ~90 ms) and frees headroom for recognition. Recognition + enroll are gated to
// the frames where detection actually ran, so they always align on FRESH keypoints (a stale box
// would degrade the 5-pt alignment). 1 = every frame (old behaviour).
#define DET_EVERY_N 2

// TAR/FAR measurement: the recognizer's query threshold is set very low so recognize() always
// returns the top-1 match with its RAW cosine similarity; the firmware then accepts at
// RECO_ACCEPT_THR and logs every score (VERIFY,...) so genuine vs impostor distributions can be
// built on-device. Raise/lower RECO_ACCEPT_THR to trade TAR vs FAR.
#define RECO_QUERY_THR -1.0f
// INTERIM values (Test 005) — calibrate from a clean cross-person run; do NOT treat as final.
// 0.50 was far too permissive: these int8 face models (MFN/MBF) quote 90-94% TAR at FAR=1e-4,
// an operating point well ABOVE 0.50, so impostors routinely clear 0.50 -> unenrolled faces and
// other people were accepted as id1 (open-set false accept). 0.62 is a precision-leaning interim
// (payroll: a WRONG punch is worse than a re-scan); temporal voting + multiple frames recover the
// occasional genuine dip below it.
#define RECO_ACCEPT_THR 0.62f
// Top-1 must beat the runner-up identity by this cosine margin, else the frame is rejected as
// AMBIGUOUS instead of emitting a confident wrong id. This is what fixes the id1<->id2/id3
// cross-match: when a probe sits between two enrolled people, neither wins. Auto-passes when the
// DB has <2 people (no runner-up -> open-set is gated by RECO_ACCEPT_THR alone).
#define RECO_MARGIN 0.06f

/* ---- Enrollment quality gating + multi-template (Phase 1 accuracy) ----------
 * BENCHMARK_REPORT.md finding #1: a single bad enroll frame silently breaks recognition
 * (accept ~8%); a good enroll restores it (~98%). So ENROLL now runs a short capture SESSION,
 * keeps only the best quality-passing frames, and stores several templates per person which are
 * fused (max cosine) at match time. Probe frames are quality-gated too so a blurry/oblique frame
 * does not flip the decision. Thresholds are deliberately lenient; the dashboard shows the raw
 * numbers (sharp/frontal) so they can be tightened per deployment. */
#define ENROLL_WINDOW_US 2500000   // collect for ~2.5 s after ENROLL is tapped
#define ENROLL_TEMPLATES 5         // target templates per person (best-quality kept)
#define ENROLL_MIN_TEMPLATES 2     // commit if at least this many good frames were captured
#define ENROLL_MIN_GAP_US 200000   // >=200 ms between captures -> templates span pose/light
#define Q_MIN_DET_SCORE 0.40f      // detector confidence floor (just above the 0.30 detect threshold)
#define Q_MIN_SHARP 3.0f           // gentle focus floor; sharpness mainly RANKS enroll frames, not gates
#define Q_MIN_FRONTAL 0.50f        // 0..1 frontality from the 5 landmarks (1 = head-on); geometry-based
#define Q_MAX_SAT 0.25f            // reject faces this blown-out (screen glare / overexposed)
// Min face width AS SEEN BY THE DETECTOR INPUT TENSOR (px), = box_w * model_in_w / crop_w. This
// captures the range-crop -> tensor downscale: in Wide/Full the face shrinks in the tensor, landmarks
// get coarse, alignment degrades and the genuine person false-rejects (TEST_LOG Test 002, esp.
// YuNet+MFN). Gating small tensor-faces eliminates those false rejects (come closer / use Tight).
#define Q_MIN_TENSOR_FACE_W 50

/* ---- Temporal voting before a punch -----------------------------------------
 * Require the same person to win K of the last M recognitions before firing a PUNCH. This
 * tightens the false-accept rate on top of the per-id debounce: a one-frame mis-ID cannot punch. */
#define VOTE_M 5 // window of recent recognitions
#define VOTE_K 3 // agreements required

/* ---- Distance estimation + positioning guide -------------------------------
 * Estimated from the largest face using the pinhole model dist = K / size_px.
 * Inter-pupil distance (IPD, real ~63 mm) is the primary, steadier, pose-independent
 * estimator; the box width is a fallback. K is a per-deployment calibration constant:
 * stand at a known distance, read ipd_px/box width from the dashboard, then
 *   DIST_K_IPD = ipd_px * known_mm   (and DIST_K_BOX = box_px * known_mm).
 * The defaults are rough starting points - recalibrate on the real lens.
 */
#define DIST_K_IPD 39000.0f // ~ 65 px IPD at 600 mm (placeholder - calibrate!)
#define DIST_K_BOX 90000.0f // ~150 px box at 600 mm (placeholder - calibrate!)
#define DIST_OK_MIN_MM 450  // ideal capture band
#define DIST_OK_MAX_MM 750
#define DIST_HYST_MM 40     // hysteresis so the guide does not oscillate at the edges
#define DIST_EMA 0.4f       // distance smoothing factor (0..1, higher = snappier)

/* ---- Punch (attendance event) --------------------------------------------- */
#define PUNCH_W 100                 // thumbnail size (RGB565)
#define PUNCH_H 100
#define PUNCH_DEBOUNCE_US 5000000   // do not re-punch the same id within 5 s
#define PUNCH_REQUIRE_DIST_OK 0     // 1 = only punch in the OK distance band (enable AFTER calibrating
                                    // DIST_K_*; otherwise the placeholder band can block all punches)
// System clock has no RTC/NTP on this board; seed it from the build date so punches carry a
// plausible, advancing UTC stamp. For REAL UTC, set the clock from NTP (via the C6), an external
// RTC (e.g. DS3231), or a one-off settimeofday over serial. 2026-06-17 00:00:00 UTC:
#define PUNCH_BASE_EPOCH 1781654400LL

static volatile int g_punch_pending = 0; // 1 while a punch awaits/owns the thumbnail buffer
static uint16_t *g_punch_thumb = nullptr; // RGB565 thumbnail (PSRAM), owned by punch when pending
static punch_event_t g_punch = {};

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

#if DISPLAY_MIRROR_X
/* In-place horizontal mirror of an RGB565 frame (reverse each row). */
static inline void mirror_x_rgb565(uint8_t *buf, uint32_t w, uint32_t h)
{
    uint16_t *px = (uint16_t *)buf;
    for (uint32_t y = 0; y < h; y++) {
        uint16_t *row = px + (size_t)y * w;
        for (uint32_t a = 0, b = w - 1; a < b; a++, b--) {
            uint16_t t = row[a];
            row[a] = row[b];
            row[b] = t;
        }
    }
}
#endif

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

    float copy_this = 0.0f; // copy time charged to THIS frame (0 if AI was busy)

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
        copy_this = g_stats.copy_ms;

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
#if DISPLAY_MIRROR_X
    mirror_x_rgb565(buf, w, h); // flip image+overlay together for display; AI copy already taken
#endif
    g_stats.draw_ms = (float)(esp_timer_get_time() - d0) / 1000.0f;

    int64_t f0 = esp_timer_get_time();
#if USE_PPA_DISPLAY
    ppa_display_blit(buf, w, h); // camera -> DSI FB in hardware (no lv_refr_now flush on core 0)
#else
    ui_update_camera_canvas(buf, w, h);
#endif
    g_stats.disp_ms = (float)(esp_timer_get_time() - f0) / 1000.0f;

    // Displayed frame rate + core-0 compute load, averaged over ~1 s windows.
    static int64_t s_fps_t0 = 0;
    static int s_fps_n = 0;
    static float s_l0_busy = 0.0f;
    s_fps_n++;
    s_l0_busy += copy_this + g_stats.draw_ms + g_stats.disp_ms;
    if (s_fps_t0 == 0) {
        s_fps_t0 = t_frame;
    } else if (t_frame - s_fps_t0 >= 1000000) {
        float secs = (float)(t_frame - s_fps_t0) / 1000000.0f;
        g_stats.fps = (float)s_fps_n / secs;
        float load = s_l0_busy / (secs * 1000.0f) * 100.0f;
        g_stats.load_core0 = load > 100.0f ? 100.0f : load;
        s_fps_n = 0;
        s_l0_busy = 0.0f;
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

/* Downscale the face box region of the (BGR565) AI buffer into the RGB565 punch thumbnail,
 * swapping R/B so the saved photo has correct colours (the live canvas keeps its cosmetic swap). */
static void capture_punch_thumb(const uint16_t *buf, int bw, int bh, int x0, int y0, int x1, int y1)
{
    if (!g_punch_thumb) {
        return;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > bw) x1 = bw;
    if (y1 > bh) y1 = bh;
    int rw = x1 - x0, rh = y1 - y0;
    if (rw < 2 || rh < 2) {
        return;
    }
    for (int dy = 0; dy < PUNCH_H; dy++) {
        int sy = y0 + dy * rh / PUNCH_H;
        const uint16_t *srow = buf + (size_t)sy * bw;
        uint16_t *drow = g_punch_thumb + (size_t)dy * PUNCH_W;
        for (int dx = 0; dx < PUNCH_W; dx++) {
            int sx = x0 + dx * rw / PUNCH_W;
            uint16_t p = srow[sx]; // BGR565: B[15:11] G[10:5] R[4:0]
            uint16_t r = p & 0x1F, g = (p >> 5) & 0x3F, b = (p >> 11) & 0x1F;
            drow[dx] = (uint16_t)((r << 11) | (g << 5) | b); // -> RGB565
        }
    }
}

/* ==================== Phase 1: recognition accuracy helpers ==================== */

// Per-(recognizer x detector) DB file. Alignment is not interchangeable across detectors
// (BENCHMARK_REPORT.md 3.2), so each pair keeps its own person database.
static std::string persons_path()
{
    return std::string(g_db_dir) + "/persons_" + FEAT_MODELS[g_feat_model_idx].name + "_" +
           DET_MODELS[g_det_model_idx].name + ".bin";
}

static void load_persons()
{
    int flen = g_recognizer->get_feat_model()->get_feat_len(); // lazy-loads the feat model
    g_persons.load(persons_path(), flen);
}

// Extract the aligned, L2-normalised feature for a detected face into `out` (feat_len floats).
// The feature tensor is owned/reused by the model, so copy it out immediately.
static bool extract_feat(dl::image::img_t &img, const dl::detect::result_t &det, std::vector<float> &out)
{
    if (det.keypoint.size() != 10) {
        return false;
    }
    dl::TensorBase *f = g_recognizer->get_feat_model()->run(img, det.keypoint);
    if (!f || f->dtype != dl::DATA_TYPE_FLOAT || f->size <= 0) {
        return false;
    }
    out.resize(f->size);
    memcpy(out.data(), f->data, (size_t)f->size * sizeof(float));
    return true;
}

// Face quality used to gate both enrollment and probe frames.
struct face_quality_t {
    int width;
    int tensor_w;  // face width as seen by the detector input tensor (box_w * model_in_w / crop_w)
    float det_score;
    float sharp;   // mean |Laplacian| over the face ROI (higher = sharper / in focus)
    float frontal; // 0..1 from the 5 landmarks (1 = head-on)
    float sat;     // 0..1 blown-out fraction inside the face
    bool ok;
};

static face_quality_t score_face(const uint16_t *buf, int bw, int bh, const dl::detect::result_t &det)
{
    face_quality_t q = {0, 0, 0.0f, 0.0f, 0.0f, 0.0f, false};
    if (det.box.size() < 4) {
        return q;
    }
    int x0 = det.box[0], y0 = det.box[1], x1 = det.box[2], y1 = det.box[3];
    q.width = x1 - x0;
    // Face size in the detector INPUT tensor (accounts for the range-crop -> input resize). bw is the
    // crop width fed to the detector; g_stats.model_in_w is the detector input width.
    q.tensor_w = (g_stats.model_in_w > 0 && bw > 0) ? q.width * g_stats.model_in_w / bw : q.width;
    q.det_score = det.score;
    if (x0 < 1) x0 = 1;
    if (y0 < 1) y0 = 1;
    if (x1 > bw - 1) x1 = bw - 1;
    if (y1 > bh - 1) y1 = bh - 1;
    if (x1 <= x0 + 2 || y1 <= y0 + 2) {
        return q;
    }

    // Sharpness: mean |4-neighbour Laplacian| of luma over the face (subsampled).
    long lap_sum = 0, lap_n = 0;
    for (int y = y0 + 1; y < y1 - 1; y += 2) {
        const uint16_t *row = buf + (size_t)y * bw;
        const uint16_t *up = row - bw, *dn = row + bw;
        for (int x = x0 + 1; x < x1 - 1; x += 2) {
            int c = luma_bgr565(row[x]);
            int l = 4 * c - luma_bgr565(row[x - 1]) - luma_bgr565(row[x + 1]) - luma_bgr565(up[x]) -
                    luma_bgr565(dn[x]);
            lap_sum += (l < 0 ? -l : l);
            lap_n++;
        }
    }
    q.sharp = lap_n > 0 ? (float)lap_sum / (float)lap_n : 0.0f;

    // Exposure inside the face box.
    region_stats_t fs = analyze_region(buf, bw, bh, x0, y0, x1, y1, 3);
    q.sat = fs.sat_frac;

    // Frontality from landmarks (esp-dl order: LE=0,1 LM=2,3 nose=4,5 RE=6,7 RM=8,9).
    if (det.keypoint.size() == 10) {
        const std::vector<int> &k = det.keypoint;
        float lex = k[0], ley = k[1], rex = k[6], rey = k[7], nx = k[4];
        float eye_dx = rex - lex;
        if (fabsf(eye_dx) > 1.0f) {
            float ratio = (nx - lex) / eye_dx;                                  // ~0.5 head-on
            float horiz = 1.0f - fminf(1.0f, fabsf(rey - ley) / fabsf(eye_dx)); // eye line level
            float center = 1.0f - fminf(1.0f, 2.0f * fabsf(ratio - 0.5f));      // nose centred
            q.frontal = fmaxf(0.0f, 0.5f * horiz + 0.5f * center);
        }
    }

    q.ok = q.width >= MIN_FACE_WIDTH && q.tensor_w >= Q_MIN_TENSOR_FACE_W &&
           q.det_score >= Q_MIN_DET_SCORE && q.sharp >= Q_MIN_SHARP && q.frontal >= Q_MIN_FRONTAL &&
           q.sat <= Q_MAX_SAT;
    return q;
}

// Temporal voting over recent recognitions (person id, or -1 for reject/none).
static int g_vote[VOTE_M] = {0};
static int g_vote_idx = 0;
static int g_vote_n = 0;
static void vote_reset()
{
    g_vote_idx = 0;
    g_vote_n = 0;
}
static void vote_push(int pid)
{
    g_vote[g_vote_idx] = pid;
    g_vote_idx = (g_vote_idx + 1) % VOTE_M;
    if (g_vote_n < VOTE_M) {
        g_vote_n++;
    }
}
// Person id with >= VOTE_K of the last VOTE_M votes, else -1.
static int vote_majority()
{
    for (int i = 0; i < g_vote_n; i++) {
        int pid = g_vote[i];
        if (pid <= 0) {
            continue;
        }
        int c = 0;
        for (int j = 0; j < g_vote_n; j++) {
            if (g_vote[j] == pid) {
                c++;
            }
        }
        if (c >= VOTE_K) {
            return pid;
        }
    }
    return -1;
}

// Enrollment session: a bounded best-N (by quality) set of candidate templates.
struct enroll_cand_t {
    float quality;
    std::vector<float> feat;
};
static bool g_enroll_active = false;
static int64_t g_enroll_deadline_us = 0;
static int64_t g_enroll_last_cap_us = 0;
static std::vector<enroll_cand_t> g_enroll_cands;

static void enroll_consider(float quality, std::vector<float> &&feat)
{
    if ((int)g_enroll_cands.size() < ENROLL_TEMPLATES) {
        g_enroll_cands.push_back({quality, std::move(feat)});
        return;
    }
    int worst = 0; // replace the worst kept candidate if this one is better
    for (int i = 1; i < (int)g_enroll_cands.size(); i++) {
        if (g_enroll_cands[i].quality < g_enroll_cands[worst].quality) {
            worst = i;
        }
    }
    if (quality > g_enroll_cands[worst].quality) {
        g_enroll_cands[worst] = {quality, std::move(feat)};
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

/* -------- model switching (performed ONLY by the AI task) -------- */

// Re-read the active detector's input resolution, name and keypoint capability into g_stats.
static void requery_det_info(void)
{
    dl::Model *m = g_detect->get_raw_model(0); // forces the lazy load
    if (m && m->get_input() && m->get_input()->shape.size() >= 4) {
        const std::vector<int> &sh = m->get_input()->shape;
        if (sh[1] <= 4) { // NCHW [1,C,H,W] (YuNet)
            g_stats.model_in_h = sh[2];
            g_stats.model_in_w = sh[3];
        } else { // NHWC [1,H,W,C] (esp-dl MSRMNP/ESPDet)
            g_stats.model_in_h = sh[1];
            g_stats.model_in_w = sh[2];
        }
    }
    g_det_has_kp = DET_MODELS[g_det_model_idx].has_kp;
    g_stats.det_has_kp = g_det_has_kp ? 1 : 0;
    strncpy(g_stats.det_model, DET_MODELS[g_det_model_idx].name, sizeof(g_stats.det_model) - 1);
    g_stats.det_model[sizeof(g_stats.det_model) - 1] = '\0';
}

static void apply_det_model(int idx)
{
    if (idx < 0 || idx >= DET_MODEL_COUNT) {
        return;
    }
    dl::detect::Detect *nd = make_detector(idx);
    dl::detect::Detect *old = g_detect;
    g_detect = nd; // AI task owns g_detect; safe to swap here
    delete old;
    g_det_model_idx = idx;
    g_reco_valid = false; // old recognition no longer applies
    g_last_reco_us = 0;
    requery_det_info();
    load_persons(); // alignment differs per detector -> load this (reco x det) pair's DB
    vote_reset();
    g_enroll_active = false;
    ESP_LOGI(TAG, "detector -> %s (%dx%d, keypoints=%d)", DET_MODELS[idx].name, g_stats.model_in_w,
             g_stats.model_in_h, (int)g_det_has_kp);
    char st[80];
    snprintf(st, sizeof(st), "Detector: %s%s", DET_MODELS[idx].name,
             g_det_has_kp ? "" : "  (recognition off - no landmarks)");
    ui_set_enroll_status(st);
}

static std::string feat_db_path(int idx)
{
    return std::string(g_db_dir) + "/" + FEAT_MODELS[idx].db_file;
}

static void apply_feat_model(int idx)
{
    if (idx < 0 || idx >= FEAT_MODEL_COUNT) {
        return;
    }
    // Each recognizer keeps its OWN database file (different embedding spaces / feat_len), so
    // switching never corrupts the other model's enrollments and needs no DB reset.
    HumanFaceRecognizer *nr = new HumanFaceRecognizer(feat_db_path(idx), FEAT_MODELS[idx].type);
    nr->set_thr(RECO_QUERY_THR); // return raw top-1 similarity; firmware decides accept
    HumanFaceRecognizer *old = g_recognizer;
    g_recognizer = nr; // AI task owns g_recognizer
    delete old;
    g_feat_model_idx = idx;
    g_reco_valid = false;
    g_last_reco_us = 0;
    g_stats.feat_len = g_recognizer->get_feat_model()->get_feat_len();
    g_stats.feat_params = FEAT_MODELS[idx].params_m;
    g_stats.feat_gflops = FEAT_MODELS[idx].gflops;
    g_stats.feat_tar = FEAT_MODELS[idx].tar;
    strncpy(g_stats.reco_model, FEAT_MODELS[idx].name, sizeof(g_stats.reco_model) - 1);
    g_stats.reco_model[sizeof(g_stats.reco_model) - 1] = '\0';
    load_persons(); // each (reco x det) pair has its own multi-template DB
    vote_reset();
    g_enroll_active = false;
    ESP_LOGI(TAG, "recognizer -> %s (feat_len=%d, db has %d person(s) / %d template(s))",
             FEAT_MODELS[idx].name, g_stats.feat_len, g_persons.num_persons(), g_persons.num_templates());
    char st[96];
    snprintf(st, sizeof(st), "Recognizer: %s  (%d person(s), %d template(s))", FEAT_MODELS[idx].name,
             g_persons.num_persons(), g_persons.num_templates());
    ui_set_enroll_status(st);
}

/* -------- AI task (core 1): detect + recognize on the clean copy -------- */
static void ai_task(void *arg)
{
    (void)arg;
    char status[96];

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (g_clear_request) {
            g_persons.clear();
            g_persons.save();
            g_enroll_active = false;
            g_reco_valid = false;
            vote_reset();
            g_clear_request = false;
            ui_set_enroll_status("Database cleared");
        }

        // Apply any pending model switch (only the AI task touches g_detect/g_recognizer).
        if (g_det_switch_req >= 0) {
            int req = g_det_switch_req;
            g_det_switch_req = -1;
            apply_det_model(req);
        }
        if (g_feat_switch_req >= 0) {
            int req = g_feat_switch_req;
            g_feat_switch_req = -1;
            apply_feat_model(req);
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
        // --- Detection: throttled to every DET_EVERY_N-th AI frame (see DET_EVERY_N) ---
        // On run frames we copy the result list out (run() returns a ref to the detector's internal
        // storage); on skip frames we reuse that copy so the overlay still tracks. det_ran gates the
        // recognition + enroll paths below to fresh keypoints.
        static uint32_t s_ai_frame = 0;
        static std::list<dl::detect::result_t> s_dets_cache;
        bool det_ran = (s_ai_frame++ % DET_EVERY_N) == 0;
        if (det_ran) {
            int64_t det0 = esp_timer_get_time();
            s_dets_cache = g_detect->run(img);
            g_stats.det_ms = (float)(esp_timer_get_time() - det0) / 1000.0f;
        }
        std::list<dl::detect::result_t> &dets = s_dets_cache;

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
        // Recognition needs 5-pt landmarks; ESPDet detectors return none -> auto-disable.
        bool can_recognize = largest_ok && (int)largest_det->keypoint.size() == 10;

        // --- Distance estimate + positioning guide (for ANY detected face, pre-recognition) ---
        static float s_dist_ema = 0.0f;
        if (largest_ok) {
            int ipd_px = 0;
            float size_px = 0.0f, K = DIST_K_BOX;
            const std::vector<int> &kp = largest_det->keypoint;
            if (kp.size() == 10) {
                // esp-dl order [L-eye, L-mouth, nose, R-eye, R-mouth] -> eyes at flat idx 0 and 3.
                int dx = kp[6] - kp[0], dy = kp[7] - kp[1];
                ipd_px = (int)(sqrtf((float)(dx * dx + dy * dy)) + 0.5f);
                if (ipd_px > 1) {
                    size_px = (float)ipd_px;
                    K = DIST_K_IPD;
                }
            }
            if (size_px < 1.0f) { // fall back to box width
                size_px = (float)(largest_det->box[2] - largest_det->box[0]);
                ipd_px = 0;
            }
            int dist = (size_px > 1.0f) ? (int)(K / size_px) : 0;
            if (dist > 0) {
                s_dist_ema = (s_dist_ema > 0.0f) ? (1.0f - DIST_EMA) * s_dist_ema + DIST_EMA * dist : dist;
                dist = ((int)(s_dist_ema + 5.0f) / 10) * 10; // smooth + round to 10 mm
            }
            int center = (DIST_OK_MIN_MM + DIST_OK_MAX_MM) / 2;
            int was_ok = (g_stats.dist_guide == 1) ? DIST_HYST_MM : 0; // hysteresis: OK band sticks
            g_stats.face_dist_mm = dist;
            g_stats.ipd_px = ipd_px;
            if (dist < DIST_OK_MIN_MM - was_ok) {
                g_stats.dist_guide = 2; // too close -> move back
                g_stats.dist_delta_mm = ((center - dist) / 10) * 10;
            } else if (dist > DIST_OK_MAX_MM + was_ok) {
                g_stats.dist_guide = 3; // too far -> come closer
                g_stats.dist_delta_mm = ((dist - center) / 10) * 10;
            } else {
                g_stats.dist_guide = 1; // OK
                g_stats.dist_delta_mm = 0;
            }
        } else {
            g_stats.face_dist_mm = 0;
            g_stats.dist_guide = 0;
            g_stats.dist_delta_mm = 0;
            g_stats.ipd_px = 0;
            s_dist_ema = 0.0f;
        }

        // --- Recognition: throttled (slow, ~200ms), only on the most prominent face ---
        if (!g_det_has_kp) {
            g_stats.rec_state = 3; // unavailable (detector has no landmarks)
        } else {
            g_stats.rec_state = largest_ok ? 2 : 0; // 2 = face present, waiting for window
        }
        if (det_ran && can_recognize && !g_enroll_active && (now_us - g_last_reco_us > RECOGNIZE_INTERVAL_US)) {
            if (g_persons.num_persons() == 0) {
                // Empty DB: nothing to match against -> skip the costly ~165-320 ms extraction entirely.
                g_last_reco_us = now_us;
                g_stats.rec_ms = 0.0f;
                g_stats.rec_state = 2; // face present, but DB empty -> tap ENROLL first
                g_reco_valid = true;
                g_reco_recognized = false;
                g_reco_id = -1;
                g_reco_sim = 0.0f;
            } else {
                // Probe quality gate (cheap): skip blurry / oblique / blown probes so a bad frame
                // cannot flip the decision. Only pay the feature extraction on a good frame.
                face_quality_t pq = score_face((const uint16_t *)g_ai_buf, aw, ah, *largest_det);
                if (pq.ok) {
                    int64_t rec0 = esp_timer_get_time();
                    std::vector<float> probe;
                    if (extract_feat(img, *largest_det, probe)) {
                        PersonDB::MatchResult m = g_persons.match(probe.data());
                        g_stats.rec_ms = (float)(esp_timer_get_time() - rec0) / 1000.0f;
                        g_stats.rec_state = 1; // ran this frame
                        g_last_reco_us = now_us;
                        g_reco_valid = true;
                        g_reco_id = m.person_id;
                        g_reco_sim = m.sim;                              // fused raw cosine, kept for log
                        // Accept only if the top-1 clears the absolute threshold AND beats the
                        // runner-up identity by RECO_MARGIN. No runner-up (single-person DB) -> margin
                        // auto-passes, so open-set is gated by the absolute threshold alone. Use
                        // second_id (not second_sim) for the "no runner-up" test: cosine can be < 0.
                        bool has_runner = (m.second_id >= 0);
                        float margin = has_runner ? (m.sim - m.second_sim) : 99.0f;
                        g_reco_recognized =
                            (m.person_id > 0 && m.sim >= RECO_ACCEPT_THR && margin >= RECO_MARGIN);
                        vote_push(g_reco_recognized ? m.person_id : -1);
                        // TAR/FAR proof + margin calibration: top-1, runner-up, and gap per frame.
                        // Grep "VERIFY," in monitor. Genuine should show a CLEAR gap to 2nd; an
                        // impostor/cross-match shows top-1 ~= 2nd (small margin -> now REJECTed).
                        ESP_LOGI("VERIFY", "%s,id=%d,sim=%.4f,2nd=%d/%.4f,margin=%.4f,thr=%.2f,mgn=%.2f,%s,db=%d",
                                 g_stats.reco_model, g_reco_id, g_reco_sim, m.second_id, m.second_sim,
                                 (has_runner ? (double)margin : -1.0), (double)RECO_ACCEPT_THR,
                                 (double)RECO_MARGIN, g_reco_recognized ? "ACCEPT" : "REJECT",
                                 g_persons.num_templates());
                    }
                }
                // else: poor probe quality -> leave rec_state at 2 (waiting) and keep sticky result.
            }
        }
        // Reuse the most recent recognition result for the prominent face's box.
        if (largest_idx >= 0 && g_reco_valid && (now_us - g_last_reco_us < RECO_STICKY_US)) {
            local[largest_idx].recognized = g_reco_recognized;
            local[largest_idx].id = g_reco_id;
            local[largest_idx].sim = g_reco_sim;
        }

        // --- Multi-frame, quality-gated enrollment session (started by the ENROLL button) ---
        // Rising edge: begin a capture session (unless the detector emits no landmarks).
        if (g_enroll_request) {
            g_enroll_request = false;
            if (!g_det_has_kp) {
                ui_set_enroll_status("Switch to MSRMNP/YuNet to enroll (ESPDet has no landmarks)");
            } else {
                g_enroll_active = true;
                g_enroll_deadline_us = now_us + ENROLL_WINDOW_US;
                g_enroll_last_cap_us = 0;
                g_enroll_cands.clear();
                ui_set_enroll_status("Enrolling: hold still, look at the camera...");
            }
        }
        // While active: collect the best quality-passing frames (>=ENROLL_MIN_GAP_US apart), then commit.
        if (g_enroll_active) {
            if (det_ran && can_recognize && (now_us - g_enroll_last_cap_us > ENROLL_MIN_GAP_US)) {
                face_quality_t q = score_face((const uint16_t *)g_ai_buf, aw, ah, *largest_det);
                // Calibration aid: grep "ENROLL," to see real quality numbers and tune the Q_* gates.
                ESP_LOGI("ENROLL", "frame sharp=%.1f frontal=%.2f w=%d tw=%d sat=%.2f -> %s", q.sharp,
                         q.frontal, q.width, q.tensor_w, q.sat, q.ok ? "keep" : "skip");
                if (q.ok) {
                    std::vector<float> feat;
                    if (extract_feat(img, *largest_det, feat)) {
                        // rank by combined quality (sharper + more frontal + larger = better)
                        float rank = q.sharp + 40.0f * q.frontal + 0.1f * q.width;
                        enroll_consider(rank, std::move(feat));
                        g_enroll_last_cap_us = now_us;
                        snprintf(status, sizeof(status), "Enrolling... %d/%d good frame(s)",
                                 (int)g_enroll_cands.size(), ENROLL_TEMPLATES);
                        ui_set_enroll_status(status);
                    }
                }
            }
            if ((int)g_enroll_cands.size() >= ENROLL_TEMPLATES || now_us >= g_enroll_deadline_us) {
                g_enroll_active = false;
                if ((int)g_enroll_cands.size() >= ENROLL_MIN_TEMPLATES) {
                    int pid = g_persons.add_person(nullptr); // name assignable later (UI/serial)
                    for (auto &c : g_enroll_cands) {
                        g_persons.add_template(pid, c.feat.data());
                    }
                    g_persons.save();
                    snprintf(status, sizeof(status), "Enrolled person %d (%d templates) on %s", pid,
                             (int)g_enroll_cands.size(), FEAT_MODELS[g_feat_model_idx].name);
                    g_last_reco_us = 0; // re-recognize immediately so the new face turns green
                    vote_reset();
                } else {
                    snprintf(status, sizeof(status),
                             "Enroll failed: need a clear, well-lit, front-facing face. Try again.");
                }
                ui_set_enroll_status(status);
                g_enroll_cands.clear();
            }
        }

        // --- Exposure / glare on the whole analysed crop (throttled ~3 Hz: it only drives a
        //     human-readable warning, so no need to scan the full crop every frame) ---
        static int64_t s_expo_us = 0;
        if (now_us - s_expo_us > 333000) {
            s_expo_us = now_us;
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
        if (g_spoof_mode != 0 && largest_ok) {
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
            if (g_spoof_mode == 2 && s_live_cx >= 0 && (abs(cx - s_live_cx) + abs(cy - s_live_cy)) > 4) {
                score += 10; // Tex+Motion mode: natural movement -> live (bonus only)
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

        // --- Attendance punch: fresh accepted match, not a suspected spoof, at a good distance ---
        static int64_t s_last_punch_us = 0;
        static int s_last_punch_id = -1;
        bool accepted = (largest_idx >= 0) && local[largest_idx].recognized;
        // Temporal vote: the same person must win VOTE_K of the last VOTE_M recognitions. A single
        // mis-ID frame cannot punch (tightens FAR on top of the per-id debounce below).
        int voted = vote_majority();
        bool vote_ok = accepted && (voted > 0) && (voted == local[largest_idx].id);
        bool not_spoof = (g_spoof_mode == 0) || (g_stats.spoof_state != 2);
        bool dist_ok = !PUNCH_REQUIRE_DIST_OK || (g_stats.dist_guide == 1);
        if (vote_ok && not_spoof && dist_ok && !g_punch_pending) {
            int pid = local[largest_idx].id;
            if (pid != s_last_punch_id || (now_us - s_last_punch_us > PUNCH_DEBOUNCE_US)) {
                s_last_punch_us = now_us;
                s_last_punch_id = pid;
                // Thumbnail from the crop (box is in crop coords -> indexes g_ai_buf directly).
                capture_punch_thumb((const uint16_t *)g_ai_buf, aw, ah, largest_det->box[0],
                                    largest_det->box[1], largest_det->box[2], largest_det->box[3]);
                g_punch.id = pid;
                g_punch.sim = local[largest_idx].sim;
                g_punch.dist_mm = g_stats.face_dist_mm;
                g_punch.epoch = (long long)time(nullptr);
                g_punch.thumb_w = PUNCH_W;
                g_punch.thumb_h = PUNCH_H;
                g_punch_pending = 1; // hand off to UI; MUST be the last write (ownership flag)
                char ts[24];
                struct tm tmv;
                time_t e = (time_t)g_punch.epoch;
                gmtime_r(&e, &tmv);
                strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
                ESP_LOGI("PUNCH", "id=%d,sim=%.2f,dist=%dmm,utc=%s", pid, (double)g_punch.sim,
                         g_punch.dist_mm, ts);
            }
        }

        // Publish results for the display overlay.
        if (xSemaphoreTake(g_results_mtx, portMAX_DELAY) == pdTRUE) {
            memcpy(g_results, local, sizeof(FaceBox) * n);
            g_result_count = n;
            xSemaphoreGive(g_results_mtx);
        }

        int ms = (int)((esp_timer_get_time() - t0) / 1000);
        int show = (largest_idx >= 0) ? largest_idx : 0;
        int db_count = g_persons.num_templates(); // total enrolled templates (feature vectors)
        int db_people = g_persons.num_persons();
        if (n == 0) {
            snprintf(status, sizeof(status), "Faces: 0    DB: %d ppl", db_people);
        } else if (local[show].recognized) {
            const char *nm = g_persons.person_name(local[show].id);
            snprintf(status, sizeof(status), "Faces: %d    ID %d %s (%.2f)    DB: %d ppl", n,
                     local[show].id, nm, local[show].sim, db_people);
        } else {
            snprintf(status, sizeof(status), "Faces: %d    unknown (best %.2f)    DB: %d ppl", n,
                     local[show].sim, db_people);
        }
        ui_set_status(status);

        // Publish live telemetry for the on-screen stats panel.
        g_stats.faces = n;
        g_stats.db_count = db_count;
        g_stats.spoof_mode = g_spoof_mode;
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
            // Comprehensive CSV benchmark row (grep "BENCH," in the monitor; import to a sheet).
            (void)ms;
            (void)show;
            ESP_LOGI("BENCH",
                     "%s,%dx%d,%s,f%d,%s,fps=%.1f,cap=%.1f,det=%.1f,rec=%.1f,draw=%.1f,disp=%.1f,"
                     "load0=%.0f,load1=%.0f,faces=%d,db=%d,int=%uKB,psram=%.1fMB,luma=%.0f,sat=%.0f,spoof=%s",
                     g_stats.det_model, g_stats.model_in_w, g_stats.model_in_h, g_stats.reco_model,
                     g_stats.feat_len, RANGE_MODES[g_range_mode].name, g_stats.fps, g_stats.cap_ms,
                     g_stats.det_ms, g_stats.rec_ms, g_stats.draw_ms, g_stats.disp_ms, g_stats.load_core0,
                     g_stats.load_core1, n, db_count, (unsigned)(g_stats.free_internal / 1024),
                     g_stats.free_psram / 1048576.0f, g_stats.mean_luma, g_stats.sat_frac * 100.0f,
                     SPOOF_NAMES[g_spoof_mode]);
        }

        // Core-1 "compute load": fraction of wall-clock the AI task spends working (1 s window).
        {
            int64_t job_end = esp_timer_get_time();
            static int64_t s_l1_t0 = 0;
            static float s_l1_busy = 0.0f;
            s_l1_busy += (float)(job_end - t0) / 1000.0f;
            if (s_l1_t0 == 0) {
                s_l1_t0 = job_end;
            } else if (job_end - s_l1_t0 >= 1000000) {
                float load = s_l1_busy / ((float)(job_end - s_l1_t0) / 1000.0f) * 100.0f;
                g_stats.load_core1 = load > 100.0f ? 100.0f : load;
                s_l1_busy = 0.0f;
                s_l1_t0 = job_end;
            }
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

    g_punch_thumb = (uint16_t *)heap_caps_malloc(PUNCH_W * PUNCH_H * 2, MALLOC_CAP_SPIRAM);
    if (!g_punch_thumb) {
        ESP_LOGW(TAG, "no PSRAM for punch thumbnail; punch photo disabled");
    }

    // No RTC/NTP on this board: seed the clock from the build-date epoch so punch timestamps are
    // plausible + advancing. Replace with NTP (C6) / external RTC / serial settimeofday for real UTC.
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (tv.tv_sec < PUNCH_BASE_EPOCH) {
        tv.tv_sec = PUNCH_BASE_EPOCH;
        tv.tv_usec = 0;
        settimeofday(&tv, nullptr);
    }

    // Per-model face DBs live in the directory of the db_path app_main provided.
    {
        const char *slash = strrchr(db_path, '/');
        size_t dlen = slash ? (size_t)(slash - db_path) : 0;
        if (dlen == 0 || dlen >= sizeof(g_db_dir)) {
            strncpy(g_db_dir, CONFIG_SPIFLASH_MOUNT_POINT, sizeof(g_db_dir) - 1);
        } else {
            memcpy(g_db_dir, db_path, dlen);
            g_db_dir[dlen] = '\0';
        }
    }

    g_detect = make_detector(g_det_model_idx);
    g_recognizer = new HumanFaceRecognizer(feat_db_path(g_feat_model_idx), FEAT_MODELS[g_feat_model_idx].type);
    g_recognizer->set_thr(RECO_QUERY_THR); // (legacy esp-dl DB unused now; kept for feature extraction)
    load_persons();                        // this (recognizer x detector) pair's multi-template person DB
    ESP_LOGI(TAG, "models created; person DB has %d person(s) / %d template(s)", g_persons.num_persons(),
             g_persons.num_templates());

    // Active model info for the panel.
    g_stats.feat_len = g_recognizer->get_feat_model()->get_feat_len();
    g_stats.feat_params = FEAT_MODELS[g_feat_model_idx].params_m;
    g_stats.feat_gflops = FEAT_MODELS[g_feat_model_idx].gflops;
    g_stats.feat_tar = FEAT_MODELS[g_feat_model_idx].tar;
    strncpy(g_stats.reco_model, FEAT_MODELS[g_feat_model_idx].name, sizeof(g_stats.reco_model) - 1);
    static const char *LOC[] = {"flash_rodata", "flash_part", "sdcard"};
    int loc = CONFIG_HUMAN_FACE_DETECT_MODEL_LOCATION;
    strncpy(g_stats.model_loc, (loc >= 0 && loc < 3) ? LOC[loc] : "?", sizeof(g_stats.model_loc) - 1);
    g_stats.range_mode = g_range_mode;
    g_stats.ai_w = RANGE_MODES[g_range_mode].w;
    g_stats.ai_h = RANGE_MODES[g_range_mode].h;

    ESP_LOGI("BENCH", "columns: detector,input,recognizer,featlen,range,fps,cap_ms,det_ms,rec_ms,"
                      "draw_ms,disp_ms,load0%%,load1%%,faces,db,int_free,psram_free,luma,sat%%,spoof");
    ESP_LOGI("VERIFY", "columns: recognizer,id,sim(raw cosine),2nd=runnerup_id/sim,margin(sim-2nd),"
                       "accept_thr,margin_thr,decision,db_count | genuine=same enrolled person, "
                       "impostor=different person; TAR=accepts/genuine, FAR=accepts/impostor. A genuine "
                       "match shows a CLEAR margin to 2nd; cross-matches show sim~=2nd -> REJECT");

    detect_selftest();  // forces the (lazy) detector load so requery can read the input tensor
    requery_det_info(); // input resolution + name + keypoint capability
    ESP_LOGI(TAG, "detector '%s' input %dx%d; default range '%s' (%dx%d) of %dx%d frame", g_stats.det_model,
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

int face_processor_get_punch(punch_event_t *out, const uint16_t **thumb)
{
    if (!g_punch_pending) {
        return 0;
    }
    if (out) {
        *out = g_punch;
    }
    if (thumb) {
        *thumb = g_punch_thumb;
    }
    return 1;
}

void face_processor_punch_consumed(void)
{
    g_punch_pending = 0; // release the thumbnail buffer; the AI task may refill on the next punch
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

const char *face_processor_cycle_det_model(void)
{
    int next = (g_det_model_idx + 1) % DET_MODEL_COUNT;
    g_det_switch_req = next; // applied by the AI task between frames
    return DET_MODELS[next].name;
}

const char *face_processor_cycle_feat_model(void)
{
    int next = (g_feat_model_idx + 1) % FEAT_MODEL_COUNT;
    g_feat_switch_req = next;
    return FEAT_MODELS[next].name;
}

const char *face_processor_det_model_name(void)
{
    int i = g_det_model_idx;
    return (i >= 0 && i < DET_MODEL_COUNT) ? DET_MODELS[i].name : "?";
}

const char *face_processor_feat_model_name(void)
{
    int i = g_feat_model_idx;
    return (i >= 0 && i < FEAT_MODEL_COUNT) ? FEAT_MODELS[i].name : "?";
}

const char *face_processor_cycle_spoof(void)
{
    int next = (g_spoof_mode + 1) % SPOOF_MODE_COUNT;
    g_spoof_mode = next;
    return SPOOF_NAMES[next];
}

const char *face_processor_spoof_name(void)
{
    int i = g_spoof_mode;
    return (i >= 0 && i < SPOOF_MODE_COUNT) ? SPOOF_NAMES[i] : "?";
}
