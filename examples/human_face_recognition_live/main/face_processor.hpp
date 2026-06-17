#pragma once
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Live pipeline + memory telemetry, updated by the capture task (core 0) and the
// AI task (core 1) and rendered on-screen by the UI. The fields are plain 32-bit
// scalars (naturally atomic on the ESP32-P4), so no lock is taken: a torn read can
// at worst flicker one number for one refresh, which does not matter for a display.
typedef struct {
    // Per-stage latency, in milliseconds.
    float cap_ms;   // camera inter-frame interval (capture cadence)
    float copy_ms;  // copy/crop of the frame into the AI working buffer
    float det_ms;   // face detection inference
    float rec_ms;   // face recognition inference (last time it ran)
    float draw_ms;  // box/keypoint overlay drawing
    float disp_ms;  // LVGL flush to the LCD (lv_refr_now)
    float fps;      // frames actually pushed to the panel per second

    // Per-core compute load (busy time / wall time, %). Labelled "compute load", not
    // scheduler CPU% - it is the fraction of wall-clock the main pipeline stages occupy.
    float load_core0; // capture+copy+overlay+display task (core 0)
    float load_core1; // detect+recognize AI task (core 1)

    // Current pipeline state.
    int faces;      // faces detected on the last AI frame
    int db_count;   // enrolled faces in the active model's database
    int det_busy;   // 1 while the AI task is mid inference, else 0
    int rec_state;  // 0 = idle (no face), 1 = ran, 2 = waiting for window, 3 = unavailable (no keypoints)
    int det_has_kp; // active detector provides 5-pt landmarks (needed for recognition)
    int spoof_mode; // 0 = off, 1 = texture, 2 = texture+motion

    // Exposure / glare (whole frame).
    float mean_luma; // average luma 0..255 of the analysed frame
    float sat_frac;  // fraction 0..1 of near-white (blown-out) pixels
    int bright;      // 1 = scene too bright
    int glare;       // 1 = strong glare / blown highlights

    // Subject positioning (largest detected face). A relative guide, not a precise rangefinder;
    // calibrate per deployment (see DIST_K_* in face_processor.cpp).
    int face_dist_mm;  // estimated distance to the face, mm (0 = no face)
    int dist_guide;    // 0 = no face, 1 = OK band, 2 = too close (move back), 3 = too far (come closer)
    int dist_delta_mm; // suggested movement magnitude, mm (for guide 2/3)
    int ipd_px;        // measured inter-pupil distance, px (for calibration; 0 if estimated from box)

    // Anti-spoof (liveness) of the most prominent face.
    int spoof_state; // 0 = n/a (off / no face), 1 = looks live, 2 = suspected spoof
    int live_score;  // 0..100 heuristic liveness score

    // Memory, in bytes.
    uint32_t free_internal, total_internal, largest_internal, min_free_internal;
    uint32_t free_psram, total_psram, largest_psram;

    // Face-database storage (the on-flash FAT partition).
    uint32_t store_total, store_free; // bytes on the storage partition
    int db_capacity;                  // approx. max faces that fit

    // Active model info.
    int model_in_w, model_in_h; // detector model input resolution
    int ai_w, ai_h;             // resolution actually fed to the detector (ROI crop or full frame)
    int feat_len;               // recognition feature vector length
    int range_mode;             // current range/crop mode index
    char det_model[24];         // detector model name
    char reco_model[24];        // recognizer model name
    char model_loc[16];         // where models are stored (flash_rodata / partition / sdcard)
    float feat_params;          // recognizer params (M)
    float feat_gflops;          // recognizer GFLOPs
    float feat_tar;             // recognizer accuracy TAR@FAR=1e-4 on IJB-C (%)
} pipeline_stats_t;

// Create the detector + recognizer (loading the feature DB from db_path), allocate
// the AI working buffer, register the camera frame callback, and start the AI task.
// Call AFTER display_init() and the face DB filesystem is mounted, but BEFORE
// camera_start().
esp_err_t face_processor_init(const char *db_path);

// Ask the AI task to enroll the most prominent currently-detected face. Safe to call
// from the LVGL/UI thread (e.g. a button callback).
void face_processor_request_enroll(void);

// Forget every enrolled face (clears the DB).
void face_processor_clear_db(void);

// Snapshot the live pipeline/memory telemetry for on-screen display. Safe to call
// from the UI thread (see the note on pipeline_stats_t).
void face_processor_get_stats(pipeline_stats_t *out);

// A recognition "punch" (attendance event): produced by the AI task on a fresh accepted
// match (gated on not-spoof + good distance + per-id debounce) and consumed by the UI.
typedef struct {
    int id;          // matched person id
    float sim;       // match similarity
    int dist_mm;     // distance at punch time
    long long epoch; // UTC seconds from the system clock (see time-source note in .cpp)
    int thumb_w;     // thumbnail width  (px, RGB565)
    int thumb_h;     // thumbnail height
} punch_event_t;

// If a punch is pending, fill *out and point *thumb at the RGB565 thumbnail, return 1.
// The buffer stays owned by the punch until face_processor_punch_consumed() is called, so
// the UI may point an lv_img at *thumb for the life of the card, then release it.
int face_processor_get_punch(punch_event_t *out, const uint16_t **thumb);
void face_processor_punch_consumed(void); // release the thumbnail buffer for the next punch

// Cycle the detection range/crop mode (Full -> Wide -> Med -> Tight -> ...). A tighter
// crop enlarges distant faces in the detector's input, extending usable range.
void face_processor_cycle_range(void);
const char *face_processor_range_name(void); // name of the current range mode

// Switch the detector / recognizer model at runtime (applied by the AI task between frames).
// Each returns the name of the newly-selected model (for the button caption). ESPDet detectors
// have no landmarks, so recognition auto-disables while one is active. Switching the recognizer
// swaps to that model's own face database (per-model .db file) so enrollments are kept per model.
const char *face_processor_cycle_det_model(void);
const char *face_processor_cycle_feat_model(void);
const char *face_processor_det_model_name(void);
const char *face_processor_feat_model_name(void);

// Cycle the (basic, heuristic) anti-spoof mode: Off -> Texture -> Texture+Motion. Returns its name.
const char *face_processor_cycle_spoof(void);
const char *face_processor_spoof_name(void);

#ifdef __cplusplus
}
#endif
