# PPA-era Benchmark Report & Finalization Plan — ESP32-P4 Face Attendance

_2026-06-22. Supersedes BENCHMARK_REPORT.md (that was the `54e7a36` LVGL/~9 fps era). Device target =
a compact, close-range, mobile-like attendance unit (dev platform: CrowPanel 7" P4, 1024×600). Build =
full UI on the non-blocking **PPA** display engine (`USE_PPA_DISPLAY=1`, 30 fps), YuNet resolutions
runtime-switchable via the DET button._

---

## 1. Detector / resolution benchmark (live sweep on the PPA path)

Same build, full UI, MFN recognizer, Med crop, **display = PPA (decoupled from the AI)**. Headline:
**every detector holds fps = 30 with NO watchdog storm** — detection runs on core 1, so its cost is core-1
load + recognition latency, never dropped frames or core-0 starvation. (The 384/512 "overload" seen in the
earlier study was purely the old LVGL display path saturating core 0; the PPA path removes it.)

| Detector | input | det ms | core1 % | genuine sim | cross `2nd` | enroll ease | verdict |
|---|---|---|---|---|---|---|---|
| MSRMNP | 160×120 | ~18 | 30–65 | 0.76–0.90 | — | easy | fast native HFD baseline |
| **YuNet128** | 128×96 | **~15** | 27–35 | 0.82–0.90 (≤350 mm) | rej 0.21–0.30 | **HARD** (tw<50) | lightest, but enroll-hard + noisier |
| **YuNet256** | 256×192 | ~60 | 50–89 | **0.91–0.95** (to 600 mm) | 0.17–0.39 | easy (tw~58) | ★ **WINNER — sweet spot** |
| YuNet384 | 384×288 | ~150 | 57–92 | 0.82–0.94 | 0.18–0.44 | very easy | heavier, no quality gain |
| YuNet512 | 512×384 | ~290 | 78–96 | 0.71–0.95 | 0.28–0.44 | very easy | heaviest, no quality gain |
| ESPDet224 | 224×224 | ~67 | 52–55 | — (no landmarks) | — | — | detect-only |

All rows: **fps = 30, load0 ~6–30 %, no TWDT storm.**

**Recommendation: YuNet 256×192.** Cleanest recognition (genuine 0.91–0.95, strong margins, recognized to
600 mm), snappy detection (~60 ms ≈ 6 recognitions/s), and **enrolls easily** (tensor face width tw≈58 at
arm's length). **128** is fastest (det ~15 ms) but its small tensor face (tw 31–46 at normal distance) falls
under the enroll-quality gate (`Q_MIN_TENSOR_FACE_W=50`) → enrollment needs the face very close, and
recognition is noisier. **384/512** run fine on PPA but cost 2.5–5× the detection time for **no
recognition-quality gain** over 256, with sluggish cadence (512 ≈ 2.5/s vs 256 ≈ 6/s).

## 2. System speed benchmark (the journey)

| Stage | FPS | Lever |
|---|---|---|
| LVGL keeper (`v3.3.5-37`) | ~9 | (baseline — synchronous full-screen flush on core 0) |
| det-throttle + mirror-off | ~9 | freed core 1 / killed a CPU pass (still display-capped) |
| PPA blocking | 20 | hardware blit, but blocking core 0 |
| **PPA non-blocking (current)** | **30** | overlap blit with capture → core 0 freed; correct color |

30 fps = the SC2336 sensor cap. The PPA path freed core 0, so the full UI + any detector now runs without
the watchdog storm the LVGL full-UI path hit.

## 3. Tests to finalize the flow (validation matrix)

✅ done · 🟡 partial · ⬜ not started

| # | Test | Measure | Pass bar | Status |
|---|---|---|---|---|
| T1 | Recognition accuracy | TAR/FAR over 5–10 people, multiple sessions (YuNet256+MFN, thr 0.62/mgn 0.06) | TAR ≥95 %, FAR ≈0 | 🟡 spot-checks clean; needs formal N-person run |
| T2 | Operating range | min/max distance recognition holds | ~250 mm → ~1 m | ✅ recognition to ~1.2 m (Test 022, enroll/recog gates split); enroll stays tight |
| T3 | Lighting robustness | low/normal/bright/backlit + multi-template enroll | works except harsh backlight | ⬜ |
| T4 | Multi-person in frame | correct largest-face pick, no cross-accept | right id, others rejected | 🟡 2-face frames seen |
| T5 | Soak / stability | 1–4 h; punch card shown 100s of times | no leak/hang/desync/tear | 🟡 minutes clean |
| T6 | Punch flow integrity | debounce, card pause-handoff, id/time correct | 1 punch/visit, clean card | ✅ verified |
| T7 | Enrollment quality | multi-template, gate behavior, 128 enroll-hard case | 3–5 good templates/person | 🟡 |
| T8 | Anti-spoof / liveness | photo + video attack | rejects 2D replays | ⬜ heuristic only |
| T9 | Data persistence | employee + punch survive reboot | 0 lost punches | ⬜ RAM/serial only today |
| T10 | Real time + backend | UTC via NTP (C6), punch upload | correct UTC, synced | ⬜ |
| T11 | Power / thermal | sustained-30 fps current + temp | within battery/thermal budget | ⬜ |
| T12 | Target-display port | rebuild on chosen panel (4.3" etc.) | fps + layout OK | ⬜ depends on display choice |

## 4. Use-case flow (attendance punch)

1. **Idle** — live 30 fps preview; status line shows detector / fps / DB count.
2. **Approach** — user at ~250 mm–1 m; face detected (YuNet) + aligned (5 landmarks).
3. **Recognize** — embedding vs DB; accept if `sim ≥ 0.62` AND `margin ≥ 0.06` over runner-up.
4. **Punch** — on fresh accept (debounced ≥5 s/person): record `{empID, empCode, name, punch_unix}`; show
   the green PUNCH card (id · sim · time · distance) ~4 s (PPA pauses → LVGL owns screen), then resume.
5. **No match** — stay live; optional "not recognized" / enroll prompt.
6. **Enroll** (admin) — tap ENROLL; capture 3–5 quality templates (varied pose/light); save to the DB.
7. **Sync** (future, C6) — punches upload to backend with real UTC; OTA.

## 5. Recommended production lock

- **Display:** PPA non-blocking, full UI (30 fps, no storm).
- **Detector:** YuNet **256×192**.
- **Recognizer:** MFN (fast + clean); MBF if a deployment wants the widest margins.
- **Thresholds:** accept 0.62 / margin 0.06 (low-light sites may drop accept to ~0.55).
- **Enrollment:** 3–5 templates/person across lighting; keep the enroll gate tight (no tiny/far enroll faces).
- **Range:** widen *recognition* to ~1 m; keep *enroll* close (task C).

## 6. Remaining work (priority)

1. ~~Range to ~1 m (task C)~~ ✅ **DONE (Test 022)** — enroll/recog gates split; recognition to ~1.2 m, enroll tight.
2. **Employee + punch store** — persist `{empID(u32), name[30], empCode(u32), reg_unix, punch_unix}`; grow the
   storage partition (1 MB → ~6 MB).
3. **Accuracy + lighting formalization** (T1/T3) — multi-template enroll, N-person TAR/FAR.
4. **Soak + power/thermal** (T5/T11) on target hardware.
5. **C6 connectivity** — NTP UTC + backend push + OTA.
6. **Liveness** — real anti-spoof model.
7. **Target-display port** — once the panel (4.3"/other) is chosen.
