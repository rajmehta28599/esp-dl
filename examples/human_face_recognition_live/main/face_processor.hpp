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

    // Current pipeline state.
    int faces;      // faces detected on the last AI frame
    int db_count;   // enrolled faces in the database
    int det_busy;   // 1 while the AI task is mid inference, else 0
    int rec_state;  // 0 = idle (no face), 1 = ran this frame, 2 = waiting for throttle window

    // Memory, in bytes.
    uint32_t free_internal;     // free internal RAM
    uint32_t free_psram;        // free PSRAM
    uint32_t min_free_internal; // lowest internal RAM seen since boot

    // Static configuration (filled once at init).
    int model_in_w, model_in_h; // detector model input resolution
    int ai_w, ai_h;             // resolution actually fed to the detector (ROI crop or full frame)
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

#ifdef __cplusplus
}
#endif
