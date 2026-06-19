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
