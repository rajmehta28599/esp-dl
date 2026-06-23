# Improvement Roadmap — Face Detect + Recognize (CrowPanel Advance 7″ ESP32-P4)

**Status:** proposed · **Author:** R&D · **Board:** Elecrow CrowPanel Advance 7″ ESP32-P4
(ESP32-P4 @360 MHz + ESP32-C6-MINI-1 WiFi6/BT + NS4168 audio + SC2336 MIPI-CSI + EK79007 MIPI-DSI)

## Context

The live demo works (detect + recognize + distance + attendance punch), but four asks are open:
make detection *faster*, recognition *more accurate/powerful*, use the **C6** to speed the app up, and
use the **mic/speaker**. The on-device benchmark (`BENCHMARK_REPORT.md`) and a code read change how
these should be approached. Three findings drive everything below:

1. **The app is display/capture-bound, not AI-bound.** FPS held 7–9 across a **9× swing** in detector
   cost (44 ms → 380 ms). Root cause found in code: `ui_update_camera_canvas()` calls **`lv_refr_now()`
   inside the camera capture callback**, forcing a **full-screen LVGL recomposite every frame** with
   `swap_bytes=true` (a full-frame CPU `lv_draw_sw_rgb565_swap`) + `sw_rotate=true`, over PSRAM buffers,
   while the V4L2 ring has only **2 buffers**. ⇒ *Speeding up YuNet will not raise FPS; fixing this path will.*
2. **The C6 cannot accelerate AI** (single 160 MHz RISC-V, no AI accel, SDIO-attached). Its value is
   **connectivity**: real UTC time (punch timestamps are currently *faked* from the build date — unusable
   for payroll), backend push, OTA.
3. **Enrollment quality — not the model — dominates recognition accuracy.** Benchmark: a bad enroll →
   ~8 % accept; re-enrolling the *same* model → ~98 %. Lever = multi-frame quality-gated enrollment +
   multi-template + consistent alignment, not a heavier net.

## Hard constraints / risks (read first)

- **On-device verification is the user's loop.** Claude can build but cannot flash/observe the panel.
  Every phase ships in a **reviewable increment** with explicit "build → flash → what to look for" steps.
- **C6 firmware must be upgraded before any WiFi work.** Stock `esp_hosted v2.3.0` drops WiFi after
  ~4 min (SDIO bug). Need **v2.9.4+ on host *and* C6 slave** (OTA-over-SDIO tool exists, no soldering).
  This is a **feasibility gate** for Phase 4.
- **Display rewrite is the highest-risk change** (can introduce tearing/color regressions on a panel
  Claude can't see). It is sequenced behind the zero-display-risk recognition work and gated on review.
- **NS4168 is output-only** (speaker/beep/TTS = easy). Far-field **mic** capture is a separate, larger
  project and is **out of scope** for the first audio phase.

---

## Phase 1 — Recognition accuracy & robustness (pure SW, zero display risk) ✅ IMPLEMENTED (builds clean; pending on-device verify)

> Done: `main/person_db.{hpp,cpp}` (multi-template DB) + `face_processor.cpp` rewired for
> quality-gated multi-frame enrollment, score fusion, probe gating, temporal voting, per-(det×reco)
> DB namespacing. Builds green for esp32p4. **Behavioural change:** ENROLL is now a ~2.5 s capture
> session (hold still), and old `face_*.db` enrollments are superseded by `persons_*.bin` (re-enroll once).

**Goal:** make recognition trustworthy for attendance: resist bad enroll frames, tolerate pose/lighting,
cut false accepts. **Files:** `main/face_processor.cpp` (+ new `main/enroll_quality.hpp/.cpp`,
`main/person_db.hpp/.cpp`). **Reuses (verified):** `HumanFaceRecognizer::{enroll,recognize,delete_feat,
get_num_feats,set_top_k}` (`models/human_face_recognition/human_face_recognition.hpp`); auto L2-norm +
cosine match in `esp-dl/vision/recognition/dl_recognition_database.cpp`; 5-pt similarity alignment to
112×112 in `dl_feat_image_preprocessor.cpp`.

1. **Quality gate (`enroll_quality`)** — score a face before use on: detection score, box width
   (≥`MIN_FACE_WIDTH`), **sharpness** (Laplacian-variance over the face ROI in `g_ai_buf`), **frontality**
   (eye/mouth symmetry + nose centering from the 5 keypoints), and exposure (reuse `analyze_region`).
   One `face_quality_t score_face(...)` used by both enroll and probe paths.
2. **Multi-frame, multi-template enrollment** — ENROLL becomes a short **capture session**: collect the
   best *N*=3–5 quality-passing frames over ~1.5 s, enroll each (the DB stores one feat per auto-id), and
   record `{person_id → [db_ids], name}` in a **sidecar** (`person_db`, e.g. `face_mfn.persons`). Reject
   the session with on-screen guidance if too few good frames. (DB has no native multi-template — sidecar
   is required; confirmed from `dl_recognition_database.hpp`.)
3. **Score fusion at recognize** — `recognize()` with `set_top_k(k)`; map matched db_ids → persons via the
   sidecar; fuse per-person template sims (max, or mean-of-top-2); accept on fused sim ≥ `RECO_ACCEPT_THR`.
4. **Probe quality gating + temporal voting** — skip recognition on low-quality probe frames; require
   **K-of-M** recent recognitions to agree on the same person before a PUNCH (tightens FAR; complements the
   existing debounce).
5. **Per-detector×recognizer DB namespacing** — extend the per-recognizer DB naming to include the detector
   (benchmark §3.2: cross-detector alignment sim ~0.05). Prevents silent breakage on DET switch.

**Verify:** enroll one person (good light), confirm `VERIFY` sims rise and stabilize; enroll a 2nd person,
check no cross-accept (FAR); force a blurry enroll → session rejected; switch DET → no false rejects.

## Phase 2 — Detection pipeline: tracking + decoupling (mostly SW)

**Goal:** a "correctly designed" detection pipeline — fresh boxes every frame, core 1 freed for
recognition, lower box latency. **Files:** `main/face_processor.cpp` (+ `main/face_tracker.hpp/.cpp`).

1. **Lightweight IoU/centroid tracker** — run the detector every *Nth* AI frame (or until track confidence
   drops); between detections, carry boxes/keypoints forward via the tracker. Stable IDs also make Phase 1's
   temporal voting and "largest face" selection robust.
2. **Throttle detection independent of capture** — detection no longer needs to run on every handed frame;
   recognition cadence stays throttled. Net: more headroom on core 1, steadier overlays.
3. **(Optional) faster YuNet input** — expose a 192×144 YuNet path (≈½ the 256×192 cost) behind the DET
   cycle for range/speed trade-off. *Note: won't raise FPS (display-bound) — it lowers detection latency.*

**Verify:** boxes track smoothly between detections; `det` effective rate drops while overlay stays fresh;
core-1 load falls in the dashboard.

## Phase 3 — Display speed / FPS (HW-accelerated; highest perceptible win, higher risk)

**Goal:** the change that actually makes the app feel fast: 3–4× FPS by getting the flush off the capture
task and onto the PPA. **Files:** `components/bsp_display/*`, `main/face_processor.cpp` (`frame_cb`),
`main/ui.cpp` (`ui_update_camera_canvas`). **Reuses (verified in IDF 5.4.4):** `ppa_register_client` +
`ppa_do_scale_rotate_mirror` (RGB565 + `mirror_x` + `byte_swap` + scale in **one pass**);
`esp_lcd_dpi_panel_get_frame_buffer()` for direct FB; `num_fbs` up to 3.

1. **3rd V4L2 buffer** (`bsp_camera.c`, 2→3) — cheap; lets the sensor fill while `frame_cb` works.
2. **PPA composite path** — replace the SW `mirror_x_rgb565` + LVGL full-screen recomposite of the live
   image with a **PPA SRM** pass: camera buf → panel FB with mirror + byte-swap (+ optional scale) in
   hardware. Draw boxes/keypoints into the frame (existing `dl_image_draw`) before/after the PPA blit.
3. **LVGL for chrome only** — keep LVGL for text/buttons/dashboard/punch-card on a small partial-refresh
   layer; stop calling `lv_refr_now()` from the capture task. Kill `swap_bytes`/`sw_rotate` SW passes
   (`bsp_display.c` `lvgl_init`) once PPA owns orientation/byte-order.
4. **Async flush** — drive the panel from `on_refresh_done`/`on_color_trans_done` callbacks so capture is
   never blocked by the flush.

**Verify:** dashboard `disp`/`cap` drop sharply; FPS climbs to ~20–30; no tearing; overlay colors correct
(watch the documented BGR/RGB swap); buttons still respond.

## Phase 4 — C6 connectivity: real time + backend (gated on C6 FW upgrade)

**Goal:** turn it into a real attendance device. **Gate:** upgrade C6 to esp_hosted ≥ v2.9.7 first
(OTA-over-SDIO). **Adds:** `espressif/esp_wifi_remote` + `espressif/esp_hosted` (component manager),
WiFi creds (NVS/Kconfig), `main/net_*` module.

1. **Bring-up spike** — add hosted components, confirm SDIO link + WiFi join; document pins/firmware state.
2. **SNTP → real UTC** — replace the `PUNCH_BASE_EPOCH` build-date hack with NTP; punches carry true UTC.
3. **Backend push** — queue accepted punches and POST (HTTPS) to a configurable endpoint (Petpooja/Supabase/
   custom), with offline buffering + retry on the FAT partition. *(Endpoint/auth TBD with user.)*
4. **(Optional) OTA** — app-image OTA for the P4 over WiFi.

**Verify:** time syncs (punch UTC matches wall clock); a punch reaches the backend; WiFi survives >10 min
(post-upgrade); offline punches flush on reconnect.

## Phase 5 — Audio feedback (speaker; quick UX win)

**Goal:** audible punch confirmation so staff needn't watch the screen. **Pins (confirm vs schematic):**
NS4168 I2S — BCLK=22, LRCLK=21, DATA=23, PA-enable=30. **Files:** `components/bsp_audio/*`, hook in the
punch path of `face_processor.cpp`.

1. **I2S std TX bring-up** (`i2s_std`) + PA-enable GPIO; play a short beep/WAV on a fresh accepted PUNCH.
2. **(Optional) spoken confirmation** — pre-rendered clips ("attendance recorded" / person name) from flash.
3. **(Optional, separate project) mic** — far-field capture is a larger effort; **out of scope** here.

**Verify:** distinct beep on each accepted punch; no audible glitch in the AI/display loop; PA mutes idle.

## Phase 6 — Model re-export (PC-side, ESP-PPQ; follow-on)

**Goal:** a smaller/faster YuNet (and/or recognizer) if Phase 2's runtime wins aren't enough. **Toolchain:**
ESP-PPQ on PC (locate the existing `yunet_port/` export scripts). Export e.g. 192×144 YuNet, re-quantize,
re-embed (`main/CMakeLists.txt` `EMBED_FILES`). *Lowest priority — detection latency, not FPS.*

---

## Recommended sequence

**1 → 2 → 5 → 3 → 4 → 6.** Rationale: Phase 1 is the biggest accuracy ROI with **zero display risk** and
is independently testable; Phase 2 cleans the detection pipeline on the same files; Phase 5 is a quick,
low-risk UX win; Phase 3 (highest perceptible speedup but display-rewrite risk) goes once the safe wins are
banked; Phase 4 waits on the C6 firmware upgrade; Phase 6 is an optional follow-on. Each phase is a separate
build/flash/verify increment.
