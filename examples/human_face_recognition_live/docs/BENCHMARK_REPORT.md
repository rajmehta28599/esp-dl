# Face Detect + Recognize — On-Device Benchmark Report (ESP32-P4)

**Board:** Elecrow CrowPanel Advanced 7″ ESP32-P4 (1024×600), SC2336 MIPI-CSI camera.
**Build:** `human_face_recognition_live` @ `54e7a36` (YuNet detector = 256×192 4:3).
**Source:** one continuous live `BENCH`/`VERIFY` serial run, COM12 @115200, ~8 min wall
(t ≈ 51 s → 530 s). Single subject, single session. Run ended on a **host-side** serial drop
(`GetOverlappedResult … Access is denied`), **not** a device fault.

> Caveat: these are field numbers from a live R&D bench (one person, one lighting condition,
> camera hand-held at varying distance), not a controlled lab sweep. Treat them as **operating
> ranges**, not datasheet points. Aggregates are read from the log (means are eyeball averages
> of the per-frame samples).

`BENCH` CSV schema:
`detector,input,recognizer,featlen,range,fps,cap_ms,det_ms,rec_ms,draw_ms,disp_ms,load0%,load1%,faces,db,int_free,psram_free,luma,sat%,spoof`
`VERIFY` schema: `recognizer,id,sim(raw cosine),accept_thr,decision,db_count`

---

## 1. Executive summary

| Question | Verdict |
|---|---|
| Do the **speed** numbers match the established theory? | **Yes.** Detector ordering, recognizer 1:2 (MFN:MBF) ratio, and the ~2× gap over the bare-`model.run()` datasheet all reproduce the prior bench. |
| Is the pipeline **FPS** AI-bound? | **No — display/capture-bound**, confirmed. FPS stays ~7–9 whether detection costs 43 ms (MSRMNP) or 380 ms (ESPDet416). |
| Is recognition **discriminative**? | **Yes.** With a good enrollment, every detector×recognizer combo clears thr 0.50 reliably (genuine mean 0.64–0.71, accept ≥88 %). |
| What dominates **accuracy**? | **Enrollment quality**, not the model. A bad enroll cratered genuine sim to ~0.34 (≈8 % accept); a re-enroll restored it to ~0.64 (≈98 %). |
| **Memory** headroom? | Comfortable and **leak-free** over the whole run. Internal RAM is the tight resource (down to 207 KB free on ESPDet); PSRAM never below 11.4 MB free. |

---

## 2. Speed

### 2.1 Detection latency (`det_ms`)

| Detector | Input | Observed range | ~Mean | esp-dl datasheet (bare `model.run`) | Prior bench (end-to-end) | This run vs prior |
|---|---|---|---|---|---|---|
| **MSRMNP_S8_V1** | 160×120 | 20–57 ms | **~44 ms** | 17 ms | ~45 ms | ✅ matches |
| **YuNet** | 256×192 | 83–106 ms | **~92 ms** | — (custom port) | ~190 ms @ 320² | ✅ ~½ of 320² (½ the pixels) |
| **ESPDet-PICO 224** | 224×224 | 112 ms (1 sample) | **112 ms** | 52 ms | ~100 ms | ✅ matches |
| **ESPDet-PICO 416** | 416×416 | 365–387 ms | **~380 ms** | 193 ms | ~385 ms | ✅ matches |

- Detector cost ordering MSRMNP < YuNet < ESPDet224 < ESPDet416 is exactly as predicted.
- YuNet 256×192 ≈ **2.1× MSRMNP** — in line with the pixel-count ratio (49 152 / 19 200 ≈ 2.6×).
- MSRMNP `det_ms` is visibly **bimodal** (~32 ms vs ~52 ms clusters) — the two-stage proposal/refine
  cost shifts with how many candidate boxes survive; not a measurement artifact.

### 2.2 Recognition latency (`rec_ms`, aligned 112×112)

| Recognizer | feat | Observed range | ~Mean | Datasheet (bare) | Prior bench |
|---|---|---|---|---|---|
| **MFN** (MobileFaceNet) | 512 | 133–205 ms | **~165 ms** | 96 ms | ~165 ms |
| **MBF** (MobileFaceNet-Big) | 512 | 309–368 ms | **~332 ms** | 191 ms | ~335 ms |

- **MFN : MBF = 165 : 332 ≈ 1 : 2.01** — the datasheet ratio is 96 : 191 = 1 : 1.99. **Reproduced almost exactly.**
- Both run ~1.7× the bare-`model.run()` datasheet, because `rec_ms` here is **end-to-end**: 5-point
  affine alignment warp + format-convert + `model.run()` + DB cosine query. The alignment/preprocess
  (reads the face from PSRAM) is the bulk of the gap and is independent of the model — exactly the
  prior session's conclusion.

### 2.3 Frame rate — display/capture-bound (the key systems result)

FPS held **5.8–9.1 (typ. 7–9) across the entire sweep**, regardless of detector:

| Detector | det_ms | FPS band |
|---|---|---|
| MSRMNP | ~44 | 6.5–9.1 |
| YuNet | ~92 | 6.5–8.6 |
| ESPDet416 | ~380 | 6.0–6.8 |

A ~9× swing in detector cost barely moved FPS → the loop is **not AI-bound**. Per-frame core-0 cost
is dominated by **capture cadence** (`cap` 95–237 ms ⇒ a ~5–10 fps ceiling) and **display flush**
(`disp` 60–126 ms); detection runs in parallel on core 1 and is hidden until it approaches the
capture period (which is why ESPDet416 shaves FPS only slightly). `draw_ms` is normally ~18–26 ms
but spikes intermittently to ~55–76 ms — consistent with the known `DISPLAY_MIRROR_X` flip cost.

---

## 3. Accuracy

Single enrolled subject (`id=1`), threshold **0.50**, raw cosine. Only **one** identity was ever
enrolled and there was no second person, so `id=-1, sim=0` lines are **empty-DB / no-face rejects,
not true impostors** → **TAR (genuine accept rate) is measurable; FAR is not** from this run.

### 3.1 Genuine similarity by configuration

| Detector × Recognizer | Enrollment | sim range | ~Mean | Accept rate | n (verifies) |
|---|---|---|---|---|---|
| MSRMNP × MFN | good | 0.59–0.88 | **~0.71** | ~88 % | ~32 |
| MSRMNP × MBF | good (post re-enroll) | 0.55–0.88 | **~0.71** | ~91 % | ~24 |
| YuNet × MBF | good | 0.46–0.78 | **~0.68** | ~98 % | ~52 |
| YuNet × MFN | **bad enroll** | 0.13–0.65 | **~0.34** | **~8 %** | ~47 |
| YuNet × MFN | re-enroll | 0.44–0.78 | **~0.64** | ~98 % | ~62 |

### 3.2 Findings

1. **With a good enrollment, all four combos are reliably discriminative** — genuine mean 0.64–0.71,
   accept ≥88 %, comfortably above the 0.50 threshold.

2. **Enrollment quality is the dominant accuracy variable — not the detector/recognizer.**
   The first YuNet+MFN enrollment was poor → genuine mean **0.34**, ~8 % accept (a storm of
   false-rejects, sims 0.13–0.49). The log shows an explicit **CLEAR-DB + re-enroll** (`db 1→0→1`
   at t≈380 s); immediately after, the *same* YuNet+MFN combo jumped to **~0.64 mean, ~98 % accept**.
   Operational takeaway: **a single bad enroll frame silently breaks recognition until re-enrolled.**

3. **Enrollment looks detector-specific (re-enroll after switching detector).** When the recognizer
   was switched back to MBF under the MSRMNP detector, the MBF template *enrolled earlier under
   YuNet* scored **~0.05 (near-orthogonal)** on the first MSRMNP-detected probes, recovering to ~0.82
   only after a fresh enroll. This strongly suggests **alignment from different detectors is not
   interchangeable in the embedding space.** (Inference from the sim jump — worth a dedicated A/B test.)

4. **MSRMNP gives the highest genuine sims (~0.71); YuNet a touch lower (~0.64–0.68).** Consistent
   with the custom YuNet landmark set aligning marginally differently from MSRMNP's native landmarks —
   enough to shave similarity, not enough to break recognition (with a good enroll).

5. **vs prior theory** (memory: MFN genuine avg ~0.85, MBF ~0.70 under MSRMNP): **MBF reproduces
   (~0.71)**; **MFN is lower than the prior session's 0.85 but the same ballpark** (different subject /
   lighting / session). Both remain clearly discriminative.

6. **ESPDet224 / ESPDet416 produced no genuine verifies** (`keypoints=0`). With no landmarks they
   can't feed the recognizer's 5-point alignment — `rec_ms` simply freezes at its last value. This
   **confirms the hard constraint**: ESPDet detectors are detect-only; only MSRMNP and YuNet can drive
   recognition.

### 3.3 Attendance punch + distance

`PUNCH` fired under **all recognition-capable combos** (YuNet+MBF, YuNet+MFN, MSRMNP+MFN, MSRMNP+MBF)
with sim **0.58–0.88**, distance **370–620 mm**, and an **advancing UTC** stamp
(`2026-06-17 00:05:10 → 00:08:13`, build-epoch seeded — no RTC/NTP on board). Punch debounce,
distance estimate, and the build-epoch clock are all functional across detector/recognizer switches.

---

## 4. Memory

| Detector (active model set) | `int_free` | `psram_free` |
|---|---|---|
| MSRMNP 160×120 | 260–262 KB | 12.3 MB |
| YuNet 256×192 | 246–247 KB | 12.0 MB |
| ESPDet224 224×224 | 207 KB | 12.2 MB |
| ESPDet416 416×416 | 207 KB | 11.4 MB |

- **No leak.** Within every phase the free-RAM figures are flat across hundreds of frames over the
  ~8-minute run — they move only when models are swapped, then hold steady.
- **Internal RAM is the scarce resource**, not PSRAM: it drops from ~262 KB (MSRMNP) to **207 KB**
  (ESPDet) while PSRAM stays ≥ 11.4 MB of ~12 MB on-heap. ESPDet416's 416² activations cost the most
  PSRAM (lowest free, 11.4 MB) — footprint scales with model size/input exactly as expected.
- Recognizer choice (MFN vs MBF) barely moves internal free (≈246 vs ≈262 KB, dominated by the detector).

---

## 5. Theory validation scorecard

| Claim under test | Predicted | Observed | Status |
|---|---|---|---|
| Detector cost ordering | MSRMNP < YuNet < ESPDet224 < ESPDet416 | 44 < 92 < 112 < 380 ms | ✅ |
| MFN:MBF latency ratio | ~1:2.0 (datasheet 96:191) | 165:332 = 1:2.01 | ✅ |
| ~2× over bare-`run` datasheet | end-to-end alignment overhead | MFN 96→165, MBF 191→332 | ✅ |
| FPS is display/capture-bound | flat vs detector cost | 7–9 fps across 9× det range | ✅ |
| YuNet 256×192 ≈ ½ of 320² | half the pixels | ~92 ms vs ~190 ms | ✅ |
| Recognition discriminative @0.50 | genuine ≫ 0.50 | 0.64–0.71 mean (good enroll) | ✅ |
| ESPDet emits no keypoints → no reco | recognition impossible | zero genuine verifies under ESPDet | ✅ |
| No memory leak over long run | flat free-RAM | flat across ~8 min | ✅ |
| Prior genuine sims (MSRMNP) | MFN ~0.85 / MBF ~0.70 | MFN ~0.71 / MBF ~0.71 | ◑ MBF matches; MFN lower (session variance) |

---

## 6. Recommendations

1. **Harden enrollment** — capture multiple frames and reject low-sharpness/off-angle enroll frames
   (the #1 accuracy risk seen here). Optionally store 2–3 templates per identity.
2. **Re-enroll on detector switch**, or namespace the face DB per **detector×recognizer** pair (today
   it is per-recognizer only) — the ~0.05 cross-detector sim shows alignment is not interchangeable.
3. **Production combo:** MSRMNP + MFN gives the best genuine separation (~0.71) at the lowest cost
   (det ~44 + rec ~165 ms). YuNet + MBF is the most robust at range (~0.68, 98 % accept) if you need
   YuNet's longer detection reach, at ~2× the latency.
4. **Confirm the cross-detector enrollment hypothesis** with a controlled A/B (enroll under YuNet,
   verify under MSRMNP without re-enroll, and vice-versa) before relying on §3.2 finding 3.
5. FPS is display-bound — if higher FPS is needed, optimise the core-0 display flush / capture
   cadence (and the intermittent ~70 ms `draw` spike), not the detector.

---

## 7. Raw caveats

- One subject, one session → **TAR only, no FAR** (no impostor identity was enrolled).
- Means are eyeball aggregates of per-frame samples, not computed statistics.
- ESPDet224 has a single `det_ms` sample (one BENCH line before the switch to 416).
- `rec_ms` persists between recognitions, so it repeats across BENCH rows until the next verify —
  distributions above use the values at actual recognition events.
- Run terminated on a host serial disconnect; the device was healthy at cut-off.
