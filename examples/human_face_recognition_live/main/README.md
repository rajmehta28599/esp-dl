# `main/` — firmware architecture

The application logic: bring‑up, the dual‑core AI/display pipeline, model switching, distance, the
attendance punch, and the on‑screen UI.

## Files

| File | Role |
|---|---|
| `app_main.cpp` | Boot: LDO power rails → GPIO ISR service → `display_init()` → `ui_init()` → `face_processor_init()` → `camera_video_init()` + `camera_start()`. |
| `face_processor.cpp` / `.hpp` | **Core.** The AI task (core 1), the camera frame callback (core 0), model switching, distance, punch, exposure/spoof, and the telemetry/punch APIs the UI reads. |
| `ui.cpp` / `.hpp` | LVGL widgets: full‑screen camera canvas, R&D dashboard, 6 touch buttons, the punch card, the warning banner. |
| `human_face.jpg` | Embedded known‑good test image for the boot self‑test (and YuNet calibration). |

## Threading & data flow

```
core 0 (capture task, prio 3)                         core 1 (face_ai task, prio 4)
─────────────────────────────                         ─────────────────────────────
SC2336 ─► esp_video/ISP ─► frame_cb(buf):              ai_task:  (woken per frame, if idle)
  • capture cadence (cap_ms)                             • apply pending model switch (DET/REC)
  • if AI idle: crop centred ROI ─► g_ai_buf ──notify──► • detect on g_ai_buf  (det_ms)
  • draw_overlays(buf)  (last results, mutex)            • distance from largest face + guide
  • mirror_x for display (DISPLAY_MIRROR_X)              • recognize largest (throttled, rec_ms)
  • LVGL canvas + lv_refr_now  (disp_ms)                 • exposure/glare + (opt) liveness
                                                         • punch on fresh accepted match
                                                         • publish FaceBox[] under g_results_mtx
LVGL task (esp_lvgl_port): stats_timer_cb @3 Hz           • update g_stats (lock‑free scalars)
  renders dashboard + punch card from g_stats / punch
```

**Why this split:** detection (~45 ms) + recognition (~165 ms) on core 1 never blocks the display loop
on core 0, so the camera stays smooth. Recognition is far slower than detection, so it's **throttled**
(`RECOGNIZE_INTERVAL_US`, ~2 Hz) on the largest face only and the result is reused ("sticky") between runs.

## Cross‑thread contracts (no heavy locks)

* **`g_results` (FaceBox[])** — written by `ai_task`, read by `draw_overlays`; guarded by `g_results_mtx`.
* **`g_stats` (pipeline_stats_t)** — lock‑free plain 32‑bit scalars; written by both tasks (disjoint
  fields), read by the LVGL timer. A torn read is cosmetically harmless.
* **Per‑job dims** (`g_ai_w/h/xoff/yoff`) — `frame_cb` stamps them, then sets `g_ai_busy=true` and notifies;
  the busy flag guarantees `ai_task` reads the same dims `frame_cb` used for the crop.
* **Punch handshake** — `ai_task` fills the thumbnail + `g_punch`, sets `g_punch_pending=1` **last**; the
  UI shows the card and calls `face_processor_punch_consumed()` only when the card hides, so the AI task
  never overwrites a thumbnail that's on screen.

## Model switching (R&D bench)

`DET_MODELS[]` (MSRMNP / ESPDet224 / ESPDet416) and `FEAT_MODELS[]` (MFN / MBF) are switched **only inside
`ai_task`** via request flags set by the buttons — never from the UI thread (the AI task exclusively owns
`g_detect`/`g_recognizer`). On a detector switch the input resolution + keypoint capability are re‑queried.
Each recognizer uses its **own DB file** (`face_mfn.db` / `face_mbf.db`) so enrollments are kept per model.

Recognition needs 5 landmarks; ESPDet emits none, so `recognize()`/`enroll()` are guarded on
`keypoint.size()==10` and auto‑disable (`rec_state=3`, dashboard `N/A`) under an ESPDet detector.

## Public API (`face_processor.hpp`)

```c
esp_err_t face_processor_init(const char *db_path);
void face_processor_request_enroll(void);
void face_processor_clear_db(void);
void face_processor_get_stats(pipeline_stats_t *out);       // dashboard telemetry
void face_processor_cycle_range(void);                       // RANGE button
const char *face_processor_cycle_det_model(void);            // DET button
const char *face_processor_cycle_feat_model(void);           // REC button
const char *face_processor_cycle_spoof(void);                // SPF button
int  face_processor_get_punch(punch_event_t*, const uint16_t **thumb);  // + _punch_consumed()
```

## Pixel format note

The SC2336 buffer is **BGR565** (verified empirically). Inference uses `g_infer_pix =
DL_IMAGE_PIX_TYPE_BGR565LE`; the punch thumbnail swaps R/B so the saved photo is correctly coloured. The
live LVGL canvas is fed RGB565LE, so it shows a cosmetic R/B swap — consistent across the whole display.
Set `CAM_AUTODETECT_PIX 1` to re‑run the 4‑way format calibration if a different camera is fitted.

See the top‑level `README.md` for the tunables table, models/performance, and serial‑logging instructions.
