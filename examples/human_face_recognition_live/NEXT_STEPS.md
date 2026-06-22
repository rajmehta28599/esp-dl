# Next Steps — roadmap (2026-06-22, after the 30 fps PPA milestone)

The **speed track is DONE**: 9 → 30 fps (3.3×) + correct color via the non-blocking PPA display path
(behind `USE_PPA_DISPLAY`, keeper `v3.3.5-37` untouched). See `TEST_LOG.md` for history (Tests 008–018),
`IMPROVEMENT_PLAN.md` for the original phases. This file is the forward work list.

---

## A. YuNet detector-resolution study  ← USER'S NEXT TASK

**Goal:** find the best YuNet **input resolution** (current = 256×192).

**How detector resolution relates to recognition (the crux):** the recognizer warps a **112×112 aligned
face from the camera crop** using YuNet's 5 landmarks — it does **not** read the detector's input image.
So detector res affects recognition only via (1) landmark precision and (2) whether the face is detected.
Recognition quality is really set by **how many source pixels the face has** = distance + crop tightness.

- Lower detector res → little effect on a **close** kiosk face; hurts **small/far** faces (missed or
  imprecise landmarks → misalignment → worse recognition).
- A **small face in frame** (far, or wide crop) → few source pixels → blurry 112×112 → degraded
  recognition. Mitigate with a **tighter range/crop mode** or a closer subject — not a bigger detector.

**NEW context:** PPA freed core 1 (~70% idle at 30 fps), so detector latency no longer caps FPS — a heavier
detector (320/416) is now affordable, which flips the old tradeoff.

**Valid resolutions must have BOTH dims ÷32** (strides 8/16/32 → clean grids cols=W/s, rows=H/s; the
C++ decode assumes exact division). So 4:3 candidates are only: 128×96, 256×192, **384×288**, 512×384.
(320×240, 192×144, 416×320 are INVALID — 240/144/320 aren't ÷32 → broken grid.) Flash is ~constant
(~152 KB) at every res — same weights, only the input tensor shape changes.

| YuNet input (grid @s32) | pixels | det ms (PPA-era est) | range | note |
|---|---|---|---|---|
| 128×96 (4×3) | 12k | ~14 | short | far faces missed; coarse landmarks |
| **256×192 (current, 8×6)** | 49k | **~57** | medium | current kiosk balance |
| **384×288 (12×9)** | 110k | ~128 | long | bigger: more range + sharper landmarks ← **testing now** |
| 512×384 (16×12) | 196k | ~228 | longest | heaviest; only if range demands it |

**Protocol per resolution:** re-export YuNet at the new input via `quantize_yunet_*.py` (work dir
`D:\Payroll_FaceDetection\Petpooja_ESP32P4\yunet_port\`) → `EMBED_FILES` swap in main/CMakeLists +
`YUNET_W/YUNET_H` in `yunet_detect.cpp` → flash → measure: **det_ms · max detect distance · landmark sanity
(DBG box+kps) · recognition genuine/cross sims at near/mid/far · flash size**. Pick the knee = enough range
for the kiosk without needless latency/flash. Log each as a TEST_LOG entry vs the previous.

---

## B. Full forward work list

| # | Item | What | Why | Effort | Priority |
|---|---|---|---|---|---|
| 1 | **Commit 30 fps milestone** | git-commit the PPA engine (behind `USE_PPA_DISPLAY`) | save the big working result before more change | XS | NOW |
| 2 | **PPA productionize** | full UI chrome (buttons/stats/punch card) OVER the PPA camera = *overlay-coexistence* + re-add detection boxes (per-buffer `esp_cache_msync` before each submit) + optional `num_fbs=2` for tearing | ship 30 fps with the REAL UI, not the minimal spike | M–L | P1 |
| 3 | **YuNet resolution study** | section A | best detector range/accuracy/speed (now that PPA freed the latency budget) | M | P1 (user's next) |
| 4 | **Recognition robustness** | multi-template enroll across lighting; consider accept thr 0.62→0.55 for low light | accuracy in real low/high-light conditions (#1 lever, > model choice) | S | P2 |
| 5 | **Employee + punch store** | `person_db` += empCode(u32)/reg_unix; append-only punch log (~12 B/punch); enlarge storage partition 1 MB → ~6 MB | the payroll data gap — punches currently vanish on reboot (live card + serial only) | M | P2 |
| 6 | **C6 connectivity** | WiFi → real NTP UTC + backend push + OTA; needs C6 firmware upgrade to esp_hosted ≥2.9.4 | real timestamps (faked from build epoch today) + backend sync (IMPROVEMENT_PLAN Phase 4) | L | P3 |
| 7 | **Distance calibration** | calibrate `DIST_K_IPD/BOX` per lens (placeholders today) | accurate "BACK/CLOSER" distance guide | S | P3 |
| 8 | **Liveness / anti-spoof** | source/integrate a real liveness model (heuristic texture/motion only today) | secure attendance (defeat photo/video spoof) | L | P4 |
| 9 | **>30 fps (optional)** | switch SC2336 to a 720p@60 / 640×480@50 sensor mode | only if >30 wanted; DSI panel caps ~56 Hz regardless | M | opt |

**Production model combo (from the lighting analysis):** YuNet + MBF (biggest cross-lighting margins) or
YuNet + MFN (½ latency); thr 0.62 / margin 0.06; multi-template enroll across lighting; AE/AWB + glare
warning on; a constant fill light beats any software tweak for low light.
