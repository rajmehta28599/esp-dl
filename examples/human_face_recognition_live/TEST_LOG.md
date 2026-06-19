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

## Test 002 — (pending) Phase 1 accuracy measurement
**Build:** Phase 1 + empty-DB skip · **Goal:** measure genuine/impostor sims after a real enrollment.
**Procedure:** fix DET+REC (start MSRMNP+MFN), tap ENROLL + hold ~2.5 s, then verify; enroll a 2nd
person for an FAR check. Capture `ENROLL,` and `VERIFY,` lines.
**Result:** _to be filled from the next monitor log._
**Compare vs baseline:** genuine sim vs 0.64–0.71; check FAR (person1 ≠ person2).
**Decision/next:** _tbd._

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
