# 3-Day Benchmark — CrowPanel Advance 7″ ESP32-P4 Face Detect + Recognize

**Window:** 2026-06-17 → 2026-06-19 · **Board:** Elecrow CrowPanel P4 (360 MHz dual-core RISC-V + AI ext,
32 MB PSRAM, SC2336 MIPI-CSI cam, EK79007 MIPI-DSI 1024×600). All numbers measured on-device (COM12 @115200)
from the `BENCH,`/`VERIFY,` serial telemetry. Ranges = observed min–max across frames/runs.

---

## 1. Detection latency (end-to-end `detect()` on the P4)

| Detector | Input W×H | 5-landmarks? | det (ms) | datasheet model-only (ms) | Notes |
|---|---|---|---|---|---|
| **MSRMNP_S8_V1** | 160×120 | ✅ | **32–53** | ~17 | recognition-compatible, fastest |
| YuNet (initial) | 640×640 | ✅ | ~790 | — | 2026-06-17 port; too heavy |
| YuNet (mid) | 320×320 | ✅ | 182–213 | — | 2026-06-18 |
| **YuNet (current)** | 256×192 | ✅ | **88–106** | — | 4:3 matches the camera crop → no distortion |
| ESPDet-Pico 224 | 224×224 | ❌ | 100–106 | ~52 | detect-only (no recognition) |
| ESPDet-Pico 416 | 416×416 | ❌ | 367–385 | ~193 | detect-only |

## 2. Recognition latency (end-to-end `recognize()`, incl. 5-pt alignment)

| Recognizer | feat_len | rec (ms) | datasheet model-only | params | GFLOPs | TAR@FAR=1e-4 (IJB-C) |
|---|---|---|---|---|---|---|
| **MFN_S8_V1** | 512 | **157–197** | ~96 | 1.2 M | 0.46 | 90.03 % |
| **MBF_S8_V1** | 512 | **320–372** | ~191 | 3.4 M | 0.90 | 93.94 % |

> End-to-end is ~2× the datasheet model-only number because `rec_ms` includes the 5-point similarity-alignment
> warp + format-convert + DB match (~70–90 ms, reads the face from PSRAM), not just `model.run()`.

## 3. Accuracy — genuine vs cross-identity (the main arc of the 3 days)

Decision rule (final): **accept if `sim ≥ 0.62` AND `(sim − runner-up) ≥ 0.06`** (interim, precision-leaning).

| Combo | genuine sim | cross-identity (`2nd`-best) | verdict | evidence |
|---|---|---|---|---|
| MSRMNP + MFN | 0.62–0.95 | **0.12–0.25** | clean | Test 005 (control) |
| MSRMNP + MBF | 0.52–0.90 (≈0.72) | low | clean | Test 002 |
| YuNet + MFN — **before** align fix | 0.60–0.93 | **0.62–0.78** | cross-matched ❌ | Test 006 |
| YuNet + MFN — **after** align fix | 0.85–0.97 | **0.18–0.36** | clean ✅ | Test 007/008 |
| YuNet + MBF — **before** align fix | 0.78–0.93 | **0.76–0.86** | cross-matched ❌ | Test 006 |
| YuNet + MBF — **after** align fix | 0.71–0.95 | **0.14–0.24** | clean ✅ | Test 007/008 |

> Enrollment-quality finding (baseline): a good multi-frame enroll → ~98 % accept; a single bad enroll → ~8 %.
> The YuNet cross-match was a **landmark-reorder bug** (slots filled by anatomical L/R instead of image side →
> reflected alignment → identities collapsed). Fixed in `aa0b124`; cross-id dropped from ~0.8 to ~0.2.

## 4. Pipeline / FPS / memory (and the speed increments)

| Build | det cadence | LCD flush | **FPS** | cap ms | disp ms | draw ms | core0 % | core1 % | int free | PSRAM free |
|---|---|---|---|---|---|---|---|---|---|---|
| Baseline (Tests 001–07) | every frame | sync | 6.5–9.0 | 96–179 | 58–131 | 15–72 | 66–100 | **68–95** | 207–262 KB | 11.4–12.3 MB |
| **inc 1** `v3.3.5-30` (Test 008) | every 2nd | sync | 6.8–8.7 | 96–174 | 58–129 | 19–71 | 77–93 | **40–48** (idle) | 246 KB | 12.0 MB |
| **inc 2** `v3.3.5-32` async flush | every 2nd | **async** | _pending_ (target 12–18) | — | target **~5–10** | — | — | — | — | — |

> **FPS is core-0 / display-bound**, not AI-bound — it held ~7 across a 9× detector-cost swing and across
> rec 160 ms (MFN) vs 330 ms (MBF). inc 1 freed core 1 (det every 2nd frame); inc 2 moves the full-screen
> flush off the capture task. The headline 20–30 fps needs the **PPA** step (byte-swap/composite in hardware).
> The **ESP32-C6 cannot speed the pipeline** (160 MHz single-core, no AI accel, SDIO) — it is connectivity-only
> (real UTC time for punches + backend).

## 5. Chronological progression

| Date | Build / Test | Change | Headline result |
|---|---|---|---|
| 06-17 | baseline demo | live SC2336+EK79007 pipeline; 6 models packed; runtime DET/REC/RANGE switch | FPS ~9 display-bound; per-model det/rec latencies measured |
| 06-17 | accuracy study | enrollment-quality experiment | good enroll ~98 % vs bad ~8 % → multi-template plan |
| 06-17→18 | YuNet port | ESP-PPQ quantize 640→320; fix empty-Resize-roi load bug | det 790→190 ms; loads clean on P4 |
| 06-19 | `a2ba850` | YuNet re-export 256×192 (4:3) | det ~90 ms; landmarks anatomically correct |
| 06-19 | Test 001 `g8a11d45` | Phase 1: multi-template DB, quality-gated enroll, score fusion, temporal vote, per-(det×reco) DBs | clean boot, no regressions (accuracy not yet exercised) |
| 06-19 | Test 002 `g12e3a84` | Phase 1 accuracy run (1 subject) | YuNet+MBF ≈0.82 best; MSRMNP ≈0.72; YuNet+MFN weak (≈0.60) |
| 06-19 | Test 003 | power-cycle reload → enrollments lost/corrupt | root cause = unflushed FATFS; **fsync fix** |
| 06-19 | Test 004 | fsync + PSRAM templates + min-tensor-face gate; **first 3-person test** | gate works; **CRITICAL false-accept / cross-match found** |
| 06-19 | Test 005 `g663d73f` | top-1-vs-top-2 margin + thr 0.50→0.62; MSRMNP control | clean 0.12–0.25, 0 false accepts → decision-layer diagnosis confirmed |
| 06-19 | Test 006 | YuNet vs MSRMNP with `2nd=` logging | **YuNet alignment bug isolated** (cross-id 0.6–0.86 vs 0.12–0.25) |
| 06-19 | Test 007 `aa0b124` | landmark reorder fixed (fill slots by image side) | **YuNet cross-id → 0.14–0.36 ⇒ accuracy DONE** |
| 06-19 | Test 008 `g6658b62` | speed inc 1: detection throttle (every 2nd frame) | **core 1 68–95 % → 40–48 %**; FPS flat (display-bound); accuracy intact |
| 06-19 | inc 2 `gf950eec` | speed inc 2: async LCD flush (display-owned double buffer) | built; **pending on-device** (target FPS 12–18) |

## 6. Current production recommendation
- **Detector:** YuNet 256×192 (range/quality) or MSRMNP 160×120 (speed / lower power).
- **Recognizer:** MBF (best margins) or MFN (½ the latency, also clean post-fix).
- **Threshold:** 0.62 / margin 0.06 (interim — calibrate per combo from a clean multi-person run; consider ↓0.55 to catch off-angle genuine dips).
- **Open items:** FPS (async flush pending verify → then PPA); ESP32-C6 connectivity for real UTC time + backend (product, not speed).
