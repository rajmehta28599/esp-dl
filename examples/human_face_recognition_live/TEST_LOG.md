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
