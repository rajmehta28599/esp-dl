# tools/ — host-side benchmark analysis

## `bench_accuracy.py` — TAR / FAR / EER / ROC from the device `VERIFY` log

The firmware logs every recognition event as a `VERIFY,` line carrying the **raw** top-1 cosine
score (the on-device `RECO_QUERY_THR=-1` forces a raw match), the runner-up, and the probe
quality covariates (`tw,fr,sh,ds,sat`). This script turns a captured log into real accuracy
numbers and sweeps the `(accept_thr, margin)` operating point — the analysis half the firmware
was built to feed.

### 1. Capture a labeled session

Flash the firmware, then capture the serial output to a file **per known subject**. Easiest is the
monitor's built-in logging (add `--log-file` to your monitor launch, or press `Ctrl+T` then `Ctrl+L`
inside the monitor to toggle a log file):

```
idf.py -p COM12 monitor --log-file genuine_p1.log
```

Protocol (1:N open-set identification):
1. **Enroll** the test people (tap ENROLL, hold still ~2.5 s each).
2. **Genuine session** — one *enrolled* person stands in front of the camera; capture to e.g.
   `genuine_p1.log`. That whole file's events are genuine trials for that person's id.
3. **Impostor session** — a person who is **not** enrolled stands in front; capture to
   `impostor.log`. Those events are impostor trials (true id `0`).
4. Repeat per person / per condition (distance, lighting, angle) for richer bins.

### 2. Analyze

```bash
# single genuine session (enrolled id = 1):
python tools/bench_accuracy.py genuine_p1.log --genuine-id 1

# single impostor session:
python tools/bench_accuracy.py impostor.log --impostor

# many sessions at once — manifest is a csv of  logfile,true_id  (true_id 0 = impostor):
python tools/bench_accuracy.py --manifest sessions.csv --csv records.csv --plot roc.png
```

`sessions.csv`:
```
# logfile,true_id   (0 = impostor / not enrolled)
genuine_p1.log,1
genuine_p2.log,2
impostor.log,0
```

### 3. Read the output

- **ON-DEVICE OPERATING POINT** — TAR / FRR / **mis-ID** / FAR at the firmware's current
  `thr/margin`. `mis-ID` (accepted the *wrong* enrolled person) is the dangerous one for payroll.
- **EER** and **suggested thr** (lowest-FRR threshold meeting a FAR budget, `--far-target`).
- **THRESHOLD SWEEP** — re-decides every event at each threshold so you can pick the operating
  point from data instead of the current hard-coded `0.62 / 0.06`.
- **Per-condition bins** (face size `tw`, frontality `fr`, exposure `sat`) — shows *where* accuracy
  drops (e.g. small/far faces, off-angle, dark), which drives the AI gate work.

Notes: stdlib-only (no numpy); `--plot` needs `matplotlib` but every metric prints without it.
Ground truth is per-session because the log carries the *predicted* id, not the true subject — a
future on-device `gt` marker can make it per-line.
