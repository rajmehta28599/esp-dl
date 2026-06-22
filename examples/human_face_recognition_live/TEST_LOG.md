# On-Device Test Log — running, comparative

Purpose: every flash/test is recorded here and compared to the **previous** entry, so the next-step
decision is grounded in measured progression (not guesswork). Newest entry on top. Keep each entry
short: what changed, what the numbers were, how they moved vs last time, and the decision taken.

Baselines for comparison live in `BENCHMARK_REPORT.md`. Roadmap/phases in `IMPROVEMENT_PLAN.md`.

**Key reference numbers (from BENCHMARK_REPORT.md, prior to Phase 1):**
- Genuine cosine sim: **good enroll ~0.64–0.71** (accept ≥88%); **bad enroll ~0.34** (accept ~8%) ← the problem Phase 1 targets.
- FPS **7–9, display-bound** (flat across a 9× detector-cost swing).
- Latency: MSRMNP det ~44 ms · YuNet ~92 ms · MFN rec ~165 ms · MBF ~332 ms · disp ~60–126 ms.
- Memory: int ~207–262 KB free, PSRAM ≥11.4 MB, no leak.

---

## Test 022 — 2026-06-22 · Task C: split range gates → recognition to ~1.2 m, enroll stays tight ✅
**Build:** `v3.3.5-45+` (`f9b04df`; `Q_MIN_TENSOR_FACE_W` split → ENROLL=50, RECOG=30; tensor-width pulled out
of `score_face().ok`, applied per-path). Recognition PUNCHes now fire at **dist = 960 / 1140 / 1190 mm**
(genuine sim 0.83–0.86 at >1 m, 0.79 at 1.19 m) — previously silent past ~600–700 mm. Close range still clean
(0.94–0.96 @ ~450 mm). **Enroll gate held TIGHT:** every far frame (tw 33–45) `-> skip`, only close (tw 51–58)
`-> keep` → no template poisoning (the advisor's guardrail). fps 30, no storm; per-resolution DBs persisted
across reboot (YuNet256 = 3 people/15 templates loaded). **Exceeds the 1 m target (~1.2 m).** Some far frames
dip 0.55–0.62 → intermittent reject (fine for a hold-still punch; per-site drop accept 0.62→0.55 if far matches
must accept harder). **All 3 asks (PPA full UI · YuNet switching · range) COMPLETE.**

---

## Test 021 — 2026-06-22 · Full live YuNet sweep (128/256/384/512) on the PRODUCTIONIZED PPA full UI
**Build:** `v3.3.5-43+` = PPA full UI (increment A, `784ac47`) + runtime YuNet switch (increment B, `1cabaa7`).
All 7 detectors selectable via DET; full app (buttons + status + punch card) at 30 fps.

**HEADLINE: every detector holds fps = 30 with NO `task_wdt` storm** — PPA frees core 0 (load0 ~6–30%), so
detection cost lands on core 1 as latency, never dropped frames. **The 384/512 "overload" from Tests 019/020
was purely the LVGL display path saturating core 0 — gone on the PPA path.** Punch-card pause-handoff ran clean
across many punches over a ~7-min run (no tear/hang/crash) → increment A validated.

| YuNet | det ms | core1 % | genuine sim | enroll | note |
|---|---|---|---|---|---|
| 128×96 | ~15 | 27–35 | 0.82–0.90 (≤350 mm) | **HARD** (tensor tw 31–46 < 50 gate) | lightest, noisier |
| **256×192** | ~60 | 50–89 | **0.91–0.95** (to 600 mm) | easy (tw~58) | ★ WINNER |
| 384×288 | ~150 | 57–92 | 0.82–0.94 | very easy | heavier, no gain |
| 512×384 | ~290 | 78–96 | 0.71–0.95 | very easy | heaviest, no gain |

**256×192 confirmed best** across the full live sweep — cleanest recognition + snappy (~6 rec/s) + easy enroll.
128 enroll-hard (small tensor face fails the enroll gate) + noisier; 384/512 viable on PPA but 2.5–5× det cost
for no quality gain. Full table report + validation/use-case plan → `PPA_BENCHMARK_REPORT.md`.

---

## Test 020 — 2026-06-22 · YuNet 128×96 study (too noisy) → 256 LOCKED; LVGL full-UI path TWDT-storms
**Build:** `v3.3.5-40` + `yunet_128x96` (full UI, db=20). **128×96: lightest (det ~38–44 ms) but recognition
NOISIER.** At close range (300–440 mm): genuine 0.51–0.88 (vs 256's tight 0.62–0.97), several REJECTs at
0.51–0.62, cross `2nd` 0.28–0.42 (vs 256's 0.07–0.33). Coarse 128 landmarks reduce alignment precision.

**YuNet resolution study COMPLETE — 256×192 WINS (sweet spot):**
| YuNet | det ms | genuine | cross `2nd` | verdict |
|---|---|---|---|---|
| 128×96 | ~40 | 0.51–0.88 (noisy, REJECTs) | 0.28–0.42 | lightest, recognition noisier |
| **256×192** | ~90 | **0.62–0.97 (clean)** | **0.07–0.33** | **WINNER** |
| 384×288 | ~235 | 0.6–0.93 | 0.25–0.46 | too heavy (overload) |
| 512×384 | ~420 | — | — | ruled out |
Locked `YUNET_RES=256`. Toggle + 128/384 models kept (yunet_port + EMBED, GC'd when unused).

**BIGGER FINDING — the full-UI LVGL path is unhealthy (not detector-related):** the `task_wdt: IDLE0 (CPU0)`
storm fired with **128 too** (not just 384) — CPU0 pegged **load0 96–100%** in `lv_draw_sw_blend`. Cause: the
full-screen LVGL flush (~70 ms) exceeds the 33 ms camera period, so `video_stream` never yields → IDLE0 starves.
**NOT a crash** (warn-only; recognition + punches kept working) but not shippable. The 30 fps **PPA** build
(Test 018) ran **clean at load0 ~30%**. **=> PPA productionization (full UI on the PPA engine) is now MANDATORY,
not optional** — it's the only path that gives the full app BOTH 30 fps AND core-0 headroom (no storm). The
3-buffer ring (added for PPA) may also nudge the LVGL path's starvation, but the root cause is core-0 flush
saturation; the cure is PPA, not buffer count.

---

## Test 019 — 2026-06-22 · YuNet resolution study: 384×288 too heavy (TWDT overload); 256×192 WINS
**Build:** `v3.3.5-38` + `yunet_384x288` (full UI, `USE_PPA_DISPLAY=0`, db=20/4 people). Switched DET→YuNet
(384×288), tested recognition out to ~730 mm.

| YuNet input (full-UI build) | det ms | fps | load0 | range | recognition |
|---|---|---|---|---|---|
| 256×192 (baseline) | ~90 | ~9 | 85–100 | medium | genuine 0.62–0.97, `2nd` 0.07–0.33 (clean) |
| **384×288 (this)** | **210–246** | 10–11 erratic | **97–100** | **~730 mm** | genuine 0.6–0.93, `2nd` **0.25–0.46** (tighter) |

**384×288 extends range** (recognized at dist 690–730 mm, sim 0.74–0.79) **but is too heavy:** det ~235 ms
hammers PSRAM → core 0's full-UI LVGL flush can't finish → **load0 pegs 97–100%, IDLE0 starves → Task Watchdog
storm** (fires every 5 s; CPU0 stuck in `lv_draw_sw_blend`). **NOT a crash** — app keeps running ~10–11 fps — but
overloaded/unstable. Cross-identity margins also got *tighter* (`2nd` 0.25–0.46 vs 256's 0.07–0.33).

**DECISION: 256×192 WINS.** Product is a **COMPACT, CLOSE-RANGE, mobile-like device (NOT a kiosk** — per user),
so detection range is exactly what it does NOT need → 384's one advantage is wasted while its cost (2.5× det,
overload, sluggish recognition) is harmful. Reverted `YUNET_USE_384=0`. The watchdog storm is purely the 384
config; 256×192 runs clean. Optional future: **128×96** if an even lighter detector is wanted (face always
close). The 384 model + `quantize_yunet_384x288.py` are kept in `yunet_port/` for the record.

---

## Test 018 — 2026-06-22 · 30 FPS ACHIEVED — non-blocking PPA (speed track COMPLETE, 9→30 = 3.3×)
**Build:** `v3.3.5-38` (3-buffer camera ring + non-blocking PPA + ISR completion semaphore). **Result: the full win.**

| metric | LVGL keeper (T010) | PPA blocking (T015–17) | **PPA non-blocking (this)** |
|---|---|---|---|
| FPS | ~9 | 20 | **30.0 (locked)** |
| disp ms | 60–78 | 22–25 | **0.1–0.4** (rare 5–10 spikes) |
| cap ms | ~110 | 33/66 bimodal | **33.3 (no drops)** |
| load0 / load1 | core-0 bound | 65 / 22 | **28–35 / 27–32** |

**How:** `ppa_display_blit` now = take-semaphore + submit NON-BLOCKING + return; the ~22 ms PPA op runs
concurrently with the next capture; `ppa_on_done` (ISR, `xSemaphoreGiveFromISR`) releases the semaphore. The
3-deep camera ring keeps `cam_buf` valid ~66 ms ≫ the 22 ms blit, so no torn frames. Ran 47 s with **no
`did not finish` / `submit failed` / crash**; color correct (0,0); PSRAM 12.3→11.2 MB (3rd ring buffer). Both
cores ~70% idle → the pipeline is now camera-bound at 30 fps, exactly as predicted.

**SPEED TRACK COMPLETE: 9 → 30 fps (3.3×) at correct color = the SC2336's hardware ceiling.** Beyond 30 needs a
higher-fps sensor mode (panel caps ~56 Hz anyway). **REMAINING is productionizing, NOT speed:** (1) restore the
full UI chrome (buttons / stats / punch card) over/around the PPA camera — the *overlay-coexistence* case the
advisor flagged as harder than the spike's side-by-side layout; (2) re-add detection-box overlays (needs a
per-buffer `esp_cache_msync` before each submit, since CPU-drawn boxes must reach PSRAM before the PPA DMA
reads); (3) optional `num_fbs=2` for tearing (reopens "chrome in both buffers"). `USE_PPA_DISPLAY 0` = keeper.

---

## Test 017 — 2026-06-22 · PPA color SOLVED — working path: correct color + 20 fps + stable coexistence
**Build:** `v3.3.5-38` (color-combo cycler). On-device cycle of the 4 `(byte_swap, rgb_swap)` combos → user
confirmed **COMBO 0 `(byte_swap=0, rgb_swap=0)` renders CORRECT color**; (1,1)/(1,0)/(0,1) wrong. Locked it +
removed the cycler. **The PPA path now also FIXES the R/B swap the LVGL canvas path always had** (cosmetic bug
since the start — see [[face-demo-project]]).

**Net state of the PPA path (increment 1 complete):** correct color · **~20 fps** (vs 9 on LVGL) · LVGL chrome
coexists with the PPA camera · stable over minutes · tearing (num_fbs=1) not visually objectionable at 20 fps.
**This is strictly better than the keeper on two axes** (2.2× fps AND correct color).

**Decision point (user):**
- **(A) Ship ~20 fps**, move to restoring the FULL UI chrome (buttons / stats / punch card) over/around the PPA
  camera — note this is the *overlay-chrome* coexistence case the advisor flagged as harder + untested (LVGL
  redrawing regions the PPA camera owns). The minimal spike UI proved side-by-side, not overlay.
- **(B) Push ~30 fps** first via **non-blocking PPA** (overlap the 22 ms blit with the next capture) — the one
  genuinely risky remaining piece (input-buffer lifecycle). 20→30 the kiosk doesn't strictly need.

Recommendation: (A) — 20 fps is smooth for a kiosk and correct-color is the bigger UX win; treat 30 fps as
optional later. Keeper stays `v3.3.5-37` (`USE_PPA_DISPLAY 0`).

---

## Test 016 — 2026-06-22 · PPA bisect: the 23 ms is the PPA op, not the cache flush; color = swap combo
**Build:** `v3.3.5-38` (removed the per-frame ~960 KB `esp_cache_msync`). **Result: `disp` UNCHANGED at 22–25 ms,
fps still 20** (cap bimodal 33/66, load0 65%). So the msync was NOT the cost — **the PPA SRM op itself is the
~22 ms**: it reads ~1 MB (1024×480 camera) + writes ~1 MB (FB) per frame and is PSRAM-bandwidth bound (~86 MB/s
effective), blocking core 0. 22 ms + ~10 ms other core-0 work intermittently exceeds the 33 ms camera period →
every-other-frame drop → 20 fps.
**Color:** user reports **(a) recognizable but WRONG colors** (not torn/smeared) → it's the `byte_swap`/`rgb_swap`
combo (`1,1` is wrong), NOT tearing. Built a **self-cycling color-combo finder** (cycles the 4 combos ~every 3–4 s,
announces each on serial as `COLOR COMBO n`) to identify the correct one in a single flash.
**Speed lever confirmed = non-blocking PPA** (overlap the 22 ms op with the next capture; camera ring is ≥2
buffers, enforced in bsp_camera.c). 20 fps is already smooth + 2.2× the 9 fps baseline — whether to invest in the
risky non-blocking buffer-lifecycle path for 30 is a user call. Coexistence + stability remain solid.

---

## Test 015 — 2026-06-22 · PPA increment-1 coexistence spike — COEXISTENCE PROVEN, fps 9→20 (partial)
**Build:** `v3.3.5-38-g983f2a2-dirty` (`USE_PPA_DISPLAY=1`). PPA SRM blits camera → top 1024×480 of the DSI FB
directly (no draw_bitmap); LVGL draws a live stats line in the bottom strip. **Result: the architecture WORKS.**

**THE DAY-1 GO/NO-GO = GO.** `ppa_disp: PPA display ready: fb=0x48910a80 (1228800 B)`, then ran **138+ s with
zero hang / reboot / chrome corruption**; LVGL bottom chrome stayed live while PPA showed the camera on top. **The
feared esp_lvgl_port trans-done desync did NOT occur** — direct-to-FB (not draw_bitmap) is the correct mechanism.

| metric | LVGL keeper (Test 010) | PPA spike (this) |
|---|---|---|
| FPS | ~9 | **20.0** |
| disp ms | 60–78 | **22–25** (PPA blit, BLOCKING, on core 0) |
| cap ms | ~33 (1 drop) | **33 / 66 bimodal** = every-other-frame drop |
| load0 | core-0 bound | 65% |

**Why only 20, not 30:** `ppa_display_blit` blocks core 0 for ~23 ms = (a) a **~960 KB `esp_cache_msync`
every frame** (added to flush CPU-drawn overlay boxes — wasteful: walks the whole region for a few box pixels)
+ (b) the **blocking PPA SRM** (reads ~1 MB camera + writes ~1 MB FB → PSRAM-bandwidth bound). 23 ms + other
core-0 work intermittently exceeds the 33 ms camera period → drops every other frame → 20 fps.
**Image "not clear" (user):** num_fbs=1 → the FB is overwritten while being scanned → **tearing**; plus the
`rgb_swap=1 + byte_swap=1` color combo is unverified (may need a flip).

**NEXT = increment 2 (target ~30 + clean image):** (1) drop the per-frame cache msync — skip `draw_overlays` in
PPA mode so PPA reads the raw DMA-resident ISP frame (no CPU writes → no sync); (2) **non-blocking PPA** so the
~20 ms op overlaps the next capture instead of stalling core 0; (3) **num_fbs=2 + page-flip** to kill tearing;
(4) re-check the color combo. PPA is PSRAM-BW-bound (~2 MB/frame) so overlap (not raw speed) is the lever.
Coexistence is no longer in question. `USE_PPA_DISPLAY 0` restores the keeper.

---

## Test 014 — 2026-06-22 · YuNet+MBF display-off — completes the 2×2 matrix; best margins
**Build:** same probe binary (`DISPLAY_PROBE_NO_FLUSH=1`). Switched to YuNet+MBF, cleared + re-enrolled 3.
**Result: same display-bound story; YuNet+MBF is the best-accuracy combo.**

| combo (display flush OFF) | det ms | rec ms | fps | load0 | load1 |
|---|---|---|---|---|---|
| YuNet+MBF (this) | 56–69 | 208–230 | 27–30 | 3–23 | 48–92 |

fps holds ~30 on the **heaviest** combo (YuNet det + MBF rec); core 0 idle (3–23%), core 1 busy but never
pegged (90–92% peaks during enroll+recognize). rec ~219 ms vs 320–372 display-on (Test 008) = same ~⅓ speedup.
**Accuracy = best of all four** (MBF margins, as expected): genuine id1 0.82–0.90 / id2 0.85–0.92 / id3
0.92–0.94; cross-identity (`2nd`) **0.07–0.24**; margins **0.72–0.79**; impostor/off-angle REJECT (0.10–0.26);
punches correct for all 3.

**2×2 MATRIX COMPLETE (all display-off, ~30 fps cap):** MSRMNP+MFN 30 · YuNet+MFN 28–30 · MSRMNP+MBF 27–30 ·
YuNet+MBF 27–30. **Across every combo: the display path is the sole fps cap, ~30 fps is reachable, accuracy is
clean, memory is leak-free.** The probe has now told us everything it can — next step is a build track (PPA
coexistence spike, or the punch/employee store), not more measurement.

---

## Test 013 — 2026-06-22 · MSRMNP+MBF display-off + cross-build finding: the flush also taxes AI latency
**Build:** same probe binary (`v3.3.5-37-g9d6c10f-dirty`, `DISPLAY_PROBE_NO_FLUSH=1`). Switched recognizer→MBF,
enrolled 2 people. **Result: fps holds 27–30 even with MBF (rec ~220 ms); accuracy clean; recognition is ~⅓
FASTER than display-on.**

| combo (display flush OFF) | det ms | rec ms | fps | load0 | load1 |
|---|---|---|---|---|---|
| MSRMNP+MBF (this) | 17–21 | 219–227 | 27–30 | 14–37 | 26–92 |

**Cross-build finding (vs Test 008, display ON):** rec MBF **320–372 → ~220 ms (−33%)**, det MSRMNP **32–53 → 17
ms (−~50%)**. The full-screen `lv_refr_now` flush (RGB565 byte-swap of a 1024×600 framebuffer) was **contending
for PSRAM bandwidth** with the AI task on core 1 (det/rec stream weights + the camera image from PSRAM). So PPA
removing that flush buys **not just fps but ~1.5–2× faster det+rec too** — a second, independent win. (Caveat:
cross-build comparison; the ratio is consistent across MFN/MBF/MSRMNP/YuNet, so the effect is real.)
**Accuracy clean (thr 0.62/mgn 0.06):** id2 genuine 0.89–0.95 (`2nd`=1/0.19, mgn ~0.75), id1 0.62–0.83
(`2nd`=2/0.18), impostor 0.14–0.20 REJECT, punches correct.
**Long-run stability:** fps pinned **30.0 for 3+ min** idle at db=10, MBF rec steady 219 ms, internal/PSRAM flat
(261 KB / 12.3 MB — **no leak**), cores ~23%/33%. `cap` jitters bimodally 16↔33 ms (V4L2 buffer cadence) but
averaged fps holds 30 — all-day-kiosk stable.
**Enroll gotcha:** many frames `skip` (tw=30–47 < `Q_MIN_TENSOR_FACE_W=50`) — MSRMNP's 160-px input over a Med
(640) crop shrinks a stood-back face's tensor width below the gate, so MBF/MSRMNP enroll needs the subject
closer. YuNet's 256-px input cleared the gate easily (Test 012). Storage: punches are NOT persisted yet (live
card + serial only); face DB = 1 MB FAT partition, 2048 B/template at feat_len=512 (see benchmark/sizing notes).

---

## Test 012 — 2026-06-22 · Production-combo ceiling confirmation (YuNet+MFN, display-off) — caveat removed
**Build:** same probe binary as Test 011 (`v3.3.5-37-g9d6c10f-dirty`, `DISPLAY_PROBE_NO_FLUSH=1`, disp=0). On
the running probe: switched detector→YuNet (DET button), enrolled 3 people (15 templates), ran live recognition
+ punch. **Result: the REAL production pipeline is display-bound too — fps holds ~28–30 with full AI load.**

| combo (display flush STUBBED) | fps | det ms | rec ms | load0 | load1 |
|---|---|---|---|---|---|
| MSRMNP+MFN, empty (Test 011) | 30.0 | 17 | — | ~24% | ~27% |
| **YuNet+MFN, 3 enrolled, recognizing** | **28–30** | 56–63 | 105–117 | **10–24%** | 46–93% |

- det cost 17→57 ms + live MFN rec + 15-template DB → **fps barely moved (30 → ~29)**; core 0 (capture) stays
  ~80% IDLE. Confirms fps is NOT AI-bound — the heavy work is on core 1 in parallel and never gates fps.
- core 1 carries the AI: 50–65% typical, brief **84–93% peaks** during simultaneous enroll+recognize — busy
  but not pegged. **Recognition cadence, not fps, is what core 1 gates** (the two are decoupled).
- **Accuracy clean (thr 0.62/mgn 0.06), consistent with Test 007:** genuine id1 0.62–0.93 / id2 0.88–0.94 /
  id3 0.95–0.97; cross-identity (`2nd`) 0.18–0.33; impostor/off-angle correctly REJECTED (0.25–0.41); PUNCH
  fired correctly for all 3 ids. Transient genuine dips→REJECT on turn/blur = probe robustness, not a bug.

**Conclusion: removes the last caveat on Test 011's PPA GO.** The shipping config (YuNet+MFN, not just the light
MSRMNP probe) is display-bound, so PPA's ~3× headroom applies to production. **Design note for PPA:** fps will be
core-0/PPA-bound, recognition rate core-1/AI-bound — independent. (YuNet+MBF, rec ~330 ms, untested — would load
core 1 harder and may throttle rec cadence, but fps stays core-0-bound.) **NEXT unchanged:** increment-1
LVGL-chrome + PPA-camera coexistence spike. (Board currently runs the probe binary; rebuild+flash the reverted
source to restore a normal display build.)

---

## Test 011 — 2026-06-22 · Capture-ceiling probe (PPA go/no-go GATE) → **PPA GO**
**Build:** `v3.3.5-37-g9d6c10f-dirty` (`DISPLAY_PROBE_NO_FLUSH=1` → `ui_update_camera_canvas` early-returns;
display FROZEN — expected, no camera output). MSRMNP+MFN, Med range, empty DB. Numbers read off the ~1 Hz
`BENCH` serial line (the panel is frozen, but BENCH prints from the AI task on core 1). **Result: DECISIVE.**

| metric | Test 010 (display ON, keeper) | Test 011 (flush STUBBED) |
|---|---|---|
| FPS | ~9.0–9.4 | **30.0 (steady, locked)** |
| cap ms | ~21–33 | 33.3 (= 1000/30 = camera cap) |
| disp ms | 60–78 | **0.0** (flush gone) |
| draw ms | ~0 | 0.0 |
| det ms (MSRMNP, core 1) | ~17 | ~17 |
| load core0 | core-0 bound | **23–27%** |
| load core1 | — | **26–29%** |

Same-combo apples-to-apples: MSRMNP+MFN was **8.0–9.7 fps with the display ON** (Test 008) → **30.0 with the
flush removed = 3.3×**. `cap=33.3 ms` means we hit the SC2336's configured **30 fps cap exactly**; both cores
sit **~75% IDLE** → capture+AI are nowhere near the limit. The display flush (`lv_refr_now` = LVGL render +
RGB565 byte-swap + DSI push, ~60–78 ms ≈ ⅔ of the frame budget) **IS the bottleneck, beyond doubt.**

**DECISION: PPA GO.** Removing one core-0 display pass moved FPS 9 → 30 — the gate's "~15–30 → PPA worth it"
branch, and we hit the TOP of it (the camera cap). Idle headroom on BOTH cores means even the heavier production
combo (YuNet det ~90 ms / MBF rec ~330 ms, throttled, on core 1) should lift substantially — core 1 was ~74%
idle here and Test 008 showed FPS flat across a 9× detector swing.

**GO means: build the smallest COEXISTENCE SPIKE first — NOT the full pipeline rewrite.** The probe proved the
*prize* is real (9→30) but did NOT touch the *risk*: can an LVGL chrome layer and a PPA-blitted camera region
share the DSI panel, given esp_lvgl_port owns the panel today? That integration, debugged blind (user is the
only eyes), is the multi-day risk and is 100% unaddressed by stubbing the flush. So increment 1 = one static
LVGL chrome element + one PPA SRM blit of the camera region to the DSI FB (`esp_lcd_dpi_panel_get_frame_buffer`,
`num_fbs`=2), shown together — success = user sees chrome AND camera, no tearing/panel-fight. If they can't
coexist, we learn it day 1, not day 4. Then expand to the full composite (IMPROVEMENT_PLAN Phase 3, LVGL→
chrome-only). **30 fps is the CEILING, not the deliverable** — PPA leaves residual core-0 cost (blit trigger +
chrome composite), so the honest target is "well above 9, approaching 30." Probe reverted; keeper `v3.3.5-37`
clean. Optional/lower-priority: YuNet+MBF + enrolled re-probe (the core-independence argument + Test 008 already
answer the production-combo ceiling — don't lead with it).

---

## Test 010 — 2026-06-19 · Speed inc 3 (mirror OFF) — first real FPS win (+20%, steady); kept
**Build:** `v3.3.5-35-gb5ef9ce` (`DISPLAY_MIRROR_X=0`). **Result: kept.**
| metric | inc 1 (mirror on) | inc 3 (mirror off) |
|---|---|---|
| FPS (YuNet+MFN) | ~7.5 (jittery) | **~9.0–9.4 (steady)** |
| draw ms | 19–71 | **~0** (CPU mirror eliminated) |
| disp ms | 58–129 | 60–78 |

Eliminating the full-frame CPU mirror from core 0 raised FPS ~7.5→9 and removed the variance.
**This is the GATE result the advisor wanted:** core-0 CPU-pass elimination DOES move FPS → PPA is
*not* a downstream-bound trap, so it's worth scoping. Accuracy unchanged. Non-mirrored preview is
cosmetic (confirm acceptable for the kiosk; if the flip is wanted, PPA can do it in hardware free).

**NEXT (Monday) = Test 011, capture-ceiling probe (gate 2):** flash a throwaway build with the LCD
flush stubbed (`DISPLAY_PROBE_NO_FLUSH=1` → early-return at the top of `ui_update_camera_canvas`,
~2 lines) and read `fps`/`cap` = max capture+AI speed with the display removed. **If ~15–30 fps →
PPA is worth the multi-day rewrite; if ~9–10 → capture/AI-bound, stop and pivot to the C6.** (Probe
was built once as `-dirty` but not flashed; reverted from the tree so the keeper stays clean.)

---

## Test 009 — 2026-06-19 · Speed inc 2 (async LCD flush) FAILED + reverted
**Build:** `v3.3.5-33-gbb7d7af` (async flush ON, clean flash). **Result: regressed — reverted to
`v3.3.5-34-g65ac208` (`DISPLAY_ASYNC_FLUSH 0`).**

| metric | inc 1 (sync, v...30) | inc 2 (async, v...33) |
|---|---|---|
| FPS (YuNet+MFN) | ~7.5 (steady) | **6.5–7.7, erratic** |
| `cap` ms | 96–174 | **56–272 (bimodal)** |
| `disp` ms | 58–129 | 18–30 (flush left the path) |
| `draw` ms | 19–71 | **spikes 100–154** |
| PSRAM free | 12.0 MB | 9.6 MB (2 display buffers) |

**Why it failed:** moving the flush off `lv_refr_now` made the esp_lvgl_port render task (affinity −1) do
the ~80 ms full-screen render + CPU byte-swap **asynchronously — and it floats onto core 0**, preempting the
capture task (that's the `draw` 100–150 ms spikes + bimodal `cap`). Async **relocates** the render; it doesn't
**eliminate** it, and on this 2-busy-core layout (core0=capture, core1=AI) there's no idle core to absorb it.
Accuracy stayed clean (genuine 0.85–0.96, `2nd` ~0.25–0.34).

**Conclusion: FPS is architecturally capped ~7–8 by the display path.** The only lever that lifts it is a
**PPA hardware composite** (byte-swap/mirror/blit in silicon → work eliminated) — a multi-day, panel-blind
rewrite (LVGL demoted to chrome-only). Core-affinity pinning won't help (core 1 can't absorb the render without
starving the AI). **inc 1 was still a genuine win** (core1 68–95% → 40–48%). Decision deferred to user: PPA
rewrite vs accept ~7–8 fps (adequate for an attendance kiosk) vs redirect effort to C6 connectivity.
Free probe available: `DISPLAY_MIRROR_X 0` removes a ~20 ms/frame CPU mirror (cosmetic selfie flip) → ~7→8 fps.

---

## Test 008 — 2026-06-19 · Speed inc 1 (detection throttle) — core-1 freed; benchmark table
**Build:** `v3.3.5-30-g6658b62` (DET_EVERY_N=2). Flashed clean. **Result: core 1 freed as predicted, FPS
unchanged (display-bound, expected), accuracy intact** (YuNet+MFN genuine 0.87–0.96, `2nd` 0.15–0.33; YuNet+MBF
genuine 0.66–0.94, `2nd` 0.01–0.21).

**Benchmark (this build, Med range, on-device COM12):**
| Detector | Recognizer | det (ms) | rec (ms) | draw (ms) | disp (ms) | cap (ms) | FPS | core0 % | core1 % |
|---|---|---|---|---|---|---|---|---|---|
| MSRMNP | MFN | 32–53 | ~160 | 20–24 | 60–98 | 99–133 | 8.0–9.7 | 85–100 | 33–43 |
| YuNet | MFN | 90–106 | 157–197 | 19–71 | 58–129 | 96–174 | 6.8–8.7 | 77–93 | 47–74 |
| YuNet | MBF | 89–107 | 320–372 | 15–72 | 58–131 | 94–179 | 6.5–8.7 | 66–93 | 35–91 |

**Increment-1 effect (core-1 relief):**
| Build | detection cadence | core1 % idle | core1 % recognizing |
|---|---|---|---|
| v3.3.5-29 (Test 007) | every AI frame | ~75–95 | 85–95 |
| v3.3.5-30 (this) | every 2nd frame | **40–48** | **47–74 (MFN)** |

**Bottleneck unchanged:** FPS ~7 is capped by core-0 (cap + disp + draw = the LVGL full-screen flush w/
swap_bytes+sw_rotate + the mirror, all full-frame CPU passes). det/rec on core-1 now have big headroom.
**Next:** display path (PPA composite + async flush + 3rd V4L2 buffer) is the only FPS lever — the panel-blind
risky part. C6 CANNOT accelerate the pipeline (160 MHz single-core, no AI accel, SDIO) — it's connectivity-only.

---

## Test 007 — 2026-06-19 · YuNet alignment fix VALIDATED on-device (accuracy DONE)
**Build:** `v3.3.5-29-gf6fe0dc` (alignment fix `aa0b124` included), flashed clean — **no checksum mismatch**,
boots `v3.3.5-29`. (Took 3 attempts to get the fix onto the board: the `--make` monitor command only monitors,
never flashes; then `idf.py flash` hit the stale-bootloader link error `undefined reference to rtc_clk_init` →
fixed by deleting `build/bootloader` + `build/bootloader-prefix`; see [[idf-build-env]].) DBs CLEARED + 3 people
re-enrolled fresh on YuNet+MFN and YuNet+MBF.

**Result — alignment fix works, cross-matching GONE:**
| combo | genuine (top-1) | cross-identity (`2nd`) | margin | vs Test 006 (broken) |
|---|---|---|---|---|
| YuNet+MFN | 0.85–0.97 | **0.18–0.36** | 0.5–0.77 | 2nd was 0.62–0.78 |
| YuNet+MBF | 0.71–0.95 | **0.14–0.24** | 0.47–0.80 | 2nd was 0.76–0.86 |

Cross-identity collapsed from ~0.7 to ~0.2 — **matches the MSRMNP control (Test 005, 0.12–0.25).** Every genuine
match now beats the runner-up by a huge margin; id1 recognizes normally; no id1↔id2/id3 confusion. User confirms
"superb working." **Root-cause fix (reorder slots by image side) is correct.** NOTE: transient REJECT streaks
(genuine sim dips to 0.1–0.3) occur when the subject turns/blurs — that's probe robustness, not the bug; temporal
voting still fires the PUNCH. thr 0.62 / margin 0.06 held well.

**Decision:** **ACCURACY IS DONE.** Production combos = YuNet+MBF (biggest margins) or YuNet+MFN (½ the latency).
Optional later: lower thr→~0.55 to catch the off-angle genuine dips. **Next focus (user): SPEED.** Perf baseline
from this run (still display-bound, fps ~7): YuNet det ~90–106 ms · MFN rec ~160–175 ms · MBF rec ~320–370 ms ·
draw ~19–65 ms · disp ~70–130 ms · load0 77–93% / load1 68–95%.

---

## Test 006 — 2026-06-19 · YuNet vs MSRMNP comparison ISOLATES a YuNet alignment bug (found + fixed)
**Build:** `v3.3.5-27-g663d73f` (same as Test 005). Ran the clean 3-person protocol on **YuNet+MFN** and
**YuNet+MBF**, with the MSRMNP run as the control. The `2nd=` instrumentation made the cause unmistakable.

**Cross-identity (`2nd` = a DIFFERENT enrolled person) — the decisive metric:**
| combo | genuine (top-1) | cross-identity (`2nd`) | verdict |
|---|---|---|---|
| MSRMNP+MFN (control) | 0.6–0.95 | **0.12–0.25** | clean |
| YuNet+MFN | 0.6–0.93 | **0.62–0.78** | broken |
| YuNet+MBF | 0.78–0.93 | **0.76–0.86** | broken (≈ genuine) |

Example YuNet lines: `MBF,id=1,sim=0.8627,2nd=2/0.8607,margin=0.0020`; `MBF,id=3,sim=0.8382,2nd=2/0.8368`;
`MFN,id=2,sim=0.7379,2nd=1/0.7298`. Different people score ~0.8 against each other → id1↔id2/id3 confusion.
The margin gate rejected most cross-matches (tiny margins) but also killed genuine matches and let a few
wrong-IDs through — matching the user's "id1 matches id2" report.

**ROOT CAUSE = YuNet landmark reorder (alignment), NOT the decision layer.** Same recognizer + margin code;
only the detector differs. MSRMNP (esp-dl-native landmark order) is clean at 0.12–0.25; YuNet is 0.6–0.86.
`yunet_detect.cpp:160` fed the image-RIGHT eye into the slot `s_std_ldks_112` expects the image-LEFT eye (and
likewise mouth) — a reflected correspondence esp-dl's similarity transform can't represent, so it warps every
face toward the same distorted pose and identities collapse together (genuine stays ~0.8 because same-person
warps consistently; impostors rise to ~0.8 too). My earlier "consistent flip = no-op" reasoning was WRONG
(only true for a *clean* mirror; this is a degenerate non-reflective fit).

**Fix → build `v3.3.5-28-gaa0b124`:** fill the 5 alignment slots BY IMAGE SIDE to match the template + MSRMNP:
slot0←yunet0(img-L eye), slot1←yunet3(img-L mouth), slot2←yunet2(nose), slot3←yunet1(img-R eye), slot4←yunet4
(img-R mouth) — i.e. swap the eye-pair and mouth-pair vs the old `{1,4,2,0,3}`. Verified against on-device DBG
pixel positions + `s_std_ldks_112` decode + advisor (independently derived). Distance/frontality unaffected.

**Decision / Test 007 procedure:** flash `v3.3.5-28-gaa0b124` → **CLEAR `persons_MFN_YuNet.bin` +
`persons_MBF_YuNet.bin`** (old templates are in the flipped-alignment space — reloading them poisons the test;
MSRMNP DBs are separate + untouched) → re-enroll the 3 people on YuNet+MFN and YuNet+MBF → verify each + an
unenrolled person. **Success = YuNet `2nd` collapses to ~0.1–0.3 and genuine margins open to 0.3+ (matching
MSRMNP).** Keep thr 0.62 / margin 0.06 for this run; once YuNet is confirmed clean, consider lowering thr to
~0.55 to recover weak-enrollee (id1) dips. If YuNet `2nd` only partially drops (~0.4), a residual issue remains.

---

## Test 005 — 2026-06-19 · MSRMNP control: margin+threshold fix VALIDATED (clean separation, 0 false accepts)
**Build:** `v3.3.5-27-g663d73f` (margin + threshold + top-2 logging). **Control run** (advisor's falsifier):
fresh — all DBs cleared, then enrolled 3 known people on **MSRMNP+MFN** and verified each + an unenrolled person.

**Result — clean separation at thr 0.62 / margin 0.06:**
| | genuine (correct id) | cross-id (`2nd`) | margin | false accepts |
|---|---|---|---|---|
| id1 (weak enroll) | 0.53–0.92 | — | — | 0 |
| id2 | 0.88–0.95 | 0.18–0.23 | 0.67–0.76 | 0 |
| id3 | 0.87–0.94 | 0.19–0.25 | 0.63–0.73 | 0 |
| unenrolled→id1 | — | 0.12–0.25 → REJECT | — | 0 |

- **DECISION-LAYER DIAGNOSIS CONFIRMED.** MSRMNP cross-identity sims are 0.12–0.25 (max ~0.47 only on a heavily
  off-angle id3 frame, where the runner-up id1 rose) — well below genuine. **Zero cross-matches, zero false
  accepts** — the exact opposite of Test 004's YuNet 0.50/no-margin behaviour. The bug was the rule, not the models.
- This is the **FAR data Test 002/004 never had**: at 0.62 the impostor ceiling is ~0.25, genuine (good frames)
  0.62–0.95. Huge headroom. id1 was the weak enrollee (some genuine 0.53–0.62 rejects; still punched via voting).
- min-face gate confirmed distance-sensitive: id3 enroll skipped far frames (`tw=24–48`), succeeded close (`tw=50–51`).

**Decision / next:** fix works. **Test 006 = repeat this exact clean run on YuNet+MFN and YuNet+MBF** (the combos
where the original cross-matching happened). If YuNet's `2nd`/cross-id sims are also ~0.12–0.25 → YuNet is fixed too,
lock the threshold (possibly lower to ~0.55 given the headroom; per-combo). If YuNet's `2nd` comes back HIGH (0.4–0.6)
while MSRMNP's was 0.12–0.25 → isolates a YuNet-alignment problem (fix the landmark reorder in yunet_detect.cpp:160).

---

## Test 004 — 2026-06-19 · min-face gate OK + CRITICAL false-accept/cross-match found (first 3-person test)
**Build flashed:** Test 004 bundle (fsync + PSRAM templates + min-tensor-face gate); boot reported
`v3.3.5-24-g12e3a84-dirty` (built dirty before the commits; the live `tw=` field + gate firing confirm the
bundle is in the image). **First test with THREE people (id1/id2/id3) → first real FAR data.**

**Worked (vs Test 003):** min-face gate fires correctly — `ENROLL` logs `tw=58 -> keep` in Med and
`tw=46..49 -> skip` as the face shrinks. Enroll quality good (frontal 0.87–1.00, sat 0.00, 5 tmpl/person).
No crash/leak (int 240–247 KB, PSRAM 12.0 MB).

**CRITICAL — false accepts / identity confusion (the FAR gap Test 002 left unproven):**
- **Open-set:** with only id1 enrolled, *unenrolled* people matched id1 (sim ≥ 0.50).
- **Closed-set:** after enrolling id2/id3, enrolled people cross-matched — id1→id2, then id1&id2→id3
  (worse on **YuNet+MBF**; YuNet+MFN stayed mostly distinct closed-set).
- **Root cause = DECISION LAYER, not alignment.** Verified this session: features are L2-normalised
  (`FeatImpl::run`→`postprocess`→`l2_norm`); esp-dl `s_std_ldks_112` order = `[LE,LM,nose,RE,RM]` (matches the
  YuNet reorder); and YuNet+MBF **genuine sims hit 0.80–0.92** in this same run — a degenerate warp can't do
  that. The bug: accept was `sim ≥ 0.50` with **no margin**, and 0.50 sits below these int8 models' FAR=1e-4
  operating point, so impostors (~0.5–0.65) clear it.
- **Data caveat (why this run can't calibrate a threshold):** id1 was a **stale prior-session** enrollment
  (`loaded 1 person/5 templates` *before* any enroll), and the MBF section had a **mid-session CLEAR** (db→0
  then re-enroll). Numbers are confounded.

**Fix → Test 005 build `v3.3.5-26-g8b27713`:** (1) `PersonDB::match()` returns the runner-up identity;
(2) accept now needs `sim ≥ RECO_ACCEPT_THR` **AND** `(sim − second_sim) ≥ RECO_MARGIN` (margin auto-passes
with <2 people → open-set gated by the absolute threshold); (3) `RECO_ACCEPT_THR 0.50→0.62`, `RECO_MARGIN
0.06` — **INTERIM, precision-leaning, NOT final**; (4) `VERIFY` log now prints `2nd=id/sim` + `margin`.

**Decision / Test 005 procedure (CLEAN controlled run):**
1. Flash `v3.3.5-26-g8b27713`; **CLEAR every DB** (all 4 det×reco files — current ones are stale/damaged).
2. Enroll a **fixed set of 3 known people** on the combo under test (Med/Tight range, good light).
3. **Systematic verify:** each enrolled person in turn + at least one **unenrolled** person. Per `VERIFY`
   line record matched id, sim, 2nd id/sim, margin, decision. Genuine → clear margin to 2nd; impostor/
   cross-match → sim ≈ 2nd → REJECT.
4. **Control: run MSRMNP+MFN too** (no landmark reorder). MSRMNP cleanly separating at 0.62 while YuNet
   doesn't ⇒ YuNet alignment after all; both behaving alike ⇒ confirmed decision-layer (expected).
5. Set the FINAL `RECO_ACCEPT_THR`/`RECO_MARGIN` (per-combo if needed) from the genuine-vs-impostor sims.

---

## Test 003 — 2026-06-19 · Persistence failure across power-cycle (CRITICAL bug found + fixed)
**Build flashed:** same as Test 002 (`v3.3.5-24-g12e3a84`). **Power-cycled** after Test 002, did NOT reflash.
**Symptom (user):** "wrong recognition accuracy" — genuine person REJECTED.

**Root cause = enrollment persistence, NOT recognition.** Of the 4 (det×reco) DBs saved in Test 002:
| File | reloaded | live (T002) | reload sim (T003) |
|---|---|---|---|
| persons_MFN_MSRMNP.bin | **0 templates (empty)** | 0.85 | gone |
| persons_MBF_MSRMNP.bin | 5 templates | 0.85 | **0.15–0.23 REJECT** (corrupt/cross-linked) |
| persons_MFN_YuNet.bin | **0 templates (empty)** | 0.85 | gone |
| persons_MBF_YuNet.bin | 5 templates | 0.84 | **0.76–0.89 OK** |
Only the **last-written** file survived intact → unflushed-FATFS-on-power-cycle signature. `save()` relied on
`fclose()` and never forced a flash commit, so buffered FAT/dir/data writes were lost or cross-linked.
(Recognition math is fine: YuNet+MBF reloaded at 0.76–0.89; all live sims were good. The 0.18 cluster is a
damaged file, not a model failure.)

**Fix:** `PersonDB::save()` now `fflush()` + `fsync(fileno(f))` to commit to flash before `fclose()`.

**Decision/next:** Test 004 build now bundles **three** changes (per user): (1) `fsync` persistence fix;
(2) PSRAM template storage (file-format unchanged → doesn't affect the persistence conclusion; scales to
many staff without eating internal RAM); (3) min-face-size gate (`Q_MIN_TENSOR_FACE_W=50`) to kill the
Wide/Full small-face false rejects, esp. **YuNet+MFN**. **Production combos = YuNet+MBF / YuNet+MFN.**

Test 004 procedure: flash → **CLEAR each pair's DB** (current files are damaged) → enroll once on YuNet+MBF
and YuNet+MFN → verify live → **power-cycle** → confirm reload sim == live sim. Watch the new `tw=` field in
`ENROLL,` logs (tensor face width); in Wide/Full it should drop below 50 and frames `skip` (intended).
Open question still: does MSRMNP+MBF reload clean after a fresh enroll, or is there a separate MSRMNP issue.

---

## Test 002 — 2026-06-19 · Phase 1 accuracy measurement (multi-template enrollment)
**Build:** `v3.3.5-24-g12e3a84`, Phase 1 + empty-DB skip. **~285 s run, single subject (id=1).**
Enrolled the same person under each (det×reco) pair (5 templates each), cycled all RANGE modes.

**Genuine similarity by combo (eyeball means; raw cosine):**
| Combo | ~mean | range | TAR (accept) | rejects | vs baseline |
|---|---|---|---|---|---|
| **YuNet + MBF** | **~0.82** | 0.55–0.93 | **100%** | 0 / ~160 | **0.68 → 0.82** ✅ big gain |
| MSRMNP + MFN | ~0.72 | 0.53–0.90 | 100% | 0 / ~40 | 0.71 → 0.72 (flat, already good) |
| MSRMNP + MBF | ~0.72 | 0.52–0.90 | 100% | 0 / ~44 | 0.71 → 0.72 (flat) |
| **YuNet + MFN** | **~0.60** | 0.36–0.91 | **~71%** | ~24 / ~84 | 0.64 → 0.60 ▼ (range-confounded) |

**What's confirmed working:**
- Multi-frame quality-gated enrollment: every kept frame logged `frontal 0.75–0.99, sat 0.00` → 5 good templates/session.
- **Score fusion lifted the YuNet combos' genuine sims** (YuNet+MBF 0.68→0.82). MSRMNP combos already near-ceiling, held ~0.72, **100% TAR**.
- Persistence + namespacing: `persons_MBF_YuNet.bin` reloaded "1 person / 5 templates" after a model-switch round-trip.
- Empty-DB skip: `rec=0.0` whenever `db=0` (no wasted extraction). Temporal-vote PUNCH fires on sustained match. No leak (int 235–262 KB, PSRAM 12.0–12.3 MB), no crash.

**Findings / problems:**
1. **YuNet+MFN is the weak combo — ~71% TAR**, sim dips to 0.36–0.49 → many false rejects. Clusters in **Wide/Full** range (small face). In Med/Tight it can hit 0.85. ⇒ high variance, low margin.
2. **Range mode strongly affects sim** (face size). Wide/Full shrink the face and push the weaker combos under thr 0.50.
3. **No FAR data** — single subject, every line `id=1`. The 0.50 threshold's safety is unproven (need a 2nd/3rd person).
4. **Scaling signal:** templates live in **internal RAM** (~2 KB each; int_free dipped to 235 KB under YuNet+MBF). 50 employees × 5 templates ≈ 500 KB → would exhaust internal RAM. Move template storage to PSRAM before deployment (file format unchanged → no re-enroll).
5. Liveness/spoof was **Off** the whole run — not exercised.

**Compare vs baseline:** genuine sims **improved or held** for 3 of 4 combos; YuNet+MBF is now the best by a clear margin (matches theory: MBF's stronger embedding + multi-template fusion). Perf/memory unchanged (still display-bound, FPS 6.4–8.4).

**Decision/next:**
- **Production combo = YuNet+MBF** (best genuine + 100% TAR) or **MSRMNP+MFN** (0.72, ~½ the latency). **Avoid YuNet+MFN.**
- **Test 003 = FAR check** (no flash needed): enroll a 2nd & 3rd person, confirm cross-person sims stay below thr.
- Code improvements queued for next build: (a) PSRAM template storage; (b) raise min-face-size gate for recognition to kill small-face false rejects; (c) per-combo accept threshold once FAR is known.

---

## Test 001 — 2026-06-19 · Phase 1 firmware, first flash
**Build:** `v3.3.5-22-g8a11d45-dirty`, Phase 1 (multi-template `person_db`, quality-gated multi-frame
enrollment, score fusion, probe gating, temporal voting, per-(reco×det) DB namespacing).
**What was tested:** ~107 s live run; user cycled DET (MSRMNP→YuNet), REC (MFN→MBF), RANGE
(Med/Tight/Full/Wide). **No ENROLL performed.**

**Result:**
- **Accuracy: NOT measured** — DB empty the whole run (`db=0`); every `VERIFY` is a correct empty-DB
  `REJECT` (`sim=0.0000`). Zero `ENROLL,` lines → enroll button never tapped while a face was in view.
- **No regressions (confirmed):**
  - Clean boot, no errors/crash from new code over the run.
  - Per-(reco×det) DBs load correctly: `persons_MFN_MSRMNP.bin`, `persons_MFN_YuNet.bin`, `persons_MBF_YuNet.bin`.
  - Model switching reloads the right DB; detection healthy (selftest 1 face @0.89; `faces=1` throughout).
  - Latency matches baseline: MSRMNP det 32–52 ms, YuNet ~90–100 ms, MFN rec ~165 ms, MBF ~320–360 ms, disp ~63–120 ms, FPS ~7–9.
  - Memory flat: int 246–262 KB free, PSRAM 12.0–12.3 MB. No leak.

**Compare vs baseline:** perf/memory unchanged (expected — Phase 1 is recognition-logic only). Accuracy
delta unknown (not exercised).

**Decision/next:** (1) Old `face_mfn.db` is intentionally not migrated → re-enroll once. (2) Added
empty-DB extraction skip (don't burn ~165–320 ms matching against nothing) → Build for Test 002.
(3) User to run the Test 002 procedure and return `ENROLL,`/`VERIFY,` lines for the accuracy comparison.
