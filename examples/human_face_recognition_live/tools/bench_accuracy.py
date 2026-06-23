#!/usr/bin/env python3
"""
bench_accuracy.py - offline accuracy analyzer for the ESP32-P4 face-recognition demo.

The firmware logs every recognition event as a `VERIFY,` line with the RAW top-1 cosine
score (RECO_QUERY_THR=-1 forces a raw match regardless of confidence) plus the runner-up
and, since the B2 change, the probe quality covariates:

    VERIFY: MFN,id=1,sim=0.7479,2nd=3/0.3200,margin=0.4279,thr=0.62,mgn=0.06,ACCEPT,db=15,\
            tw=128,fr=0.86,sh=14.2,ds=0.80,sat=0.00

That stream is everything needed to compute TAR / FAR / EER / ROC and to sweep the
(accept_thr, margin) operating point OFFLINE - the firmware was built to feed exactly this,
the analysis half just never existed. This script is that half.

GROUND TRUTH is supplied per capture session (no on-device input path required yet):
  * a capture where one ENROLLED person id=T stands in front of the camera -> genuine, true id = T
  * a capture where a NON-enrolled person stands                            -> impostor, true id = 0

USAGE
  # one genuine session (enrolled subject id=1 stood in front of the camera):
  python tools/bench_accuracy.py genuine_p1.log --genuine-id 1

  # one impostor session (an un-enrolled person):
  python tools/bench_accuracy.py impostor.log --impostor

  # many sessions at once via a manifest (csv: logfile,true_id   ; true_id 0 = impostor):
  python tools/bench_accuracy.py --manifest sessions.csv

  # write a per-record CSV and (if matplotlib is present) an ROC png:
  python tools/bench_accuracy.py *.log --manifest sessions.csv --csv out.csv --plot roc.png

Metrics (1:N open-set identification):
  genuine trial (true id T>0): TAR = ACCEPT and predicted id == T ; mis-ID = ACCEPT but id != T
  impostor trial (true id 0) : FAR = any ACCEPT
  decision at (thr,mgn): ACCEPT iff sim>=thr AND margin>=mgn AND id>0
  margin = sim - second_sim when a runner-up exists, else +inf (single-person DB auto-passes,
  matching the firmware).
"""
import argparse
import csv as csvmod
import math
import re
import sys
from collections import defaultdict

ANSI = re.compile(r"\x1b\[[0-9;]*m")
VERIFY = re.compile(r"\bVERIFY:\s*(.+?)\s*$")
TS = re.compile(r"\((\d+)\)")


def parse_log(path):
    """Yield parsed VERIFY records from one log file."""
    out = []
    with open(path, "r", errors="replace") as f:
        for raw in f:
            line = ANSI.sub("", raw.rstrip("\n"))
            m = VERIFY.search(line)
            if not m:
                continue
            msg = m.group(1)
            if msg.startswith("columns:"):
                continue
            parts = [p.strip() for p in msg.split(",")]
            if not parts:
                continue
            r = {"recognizer": parts[0], "decision": None, "src": path}
            tsm = TS.search(line[: m.start()])
            r["ts_ms"] = int(tsm.group(1)) if tsm else None
            for tok in parts[1:]:
                if tok in ("ACCEPT", "REJECT"):
                    r["decision"] = tok
                elif "=" in tok:
                    k, v = tok.split("=", 1)
                    r[k] = v
            try:
                r["id"] = int(r["id"])
                r["sim"] = float(r["sim"])
                sid, ssim = r.get("2nd", "-1/-1").split("/")
                r["second_id"] = int(sid)
                r["second_sim"] = float(ssim)
                r["db"] = int(r.get("db", 0))
                r["tw"] = int(float(r["tw"])) if "tw" in r else None
                for k in ("fr", "sh", "ds", "sat"):
                    r[k] = float(r[k]) if k in r else None
            except (KeyError, ValueError):
                continue
            if r["decision"] is None:
                continue
            out.append(r)
    return out


def eff_margin(r):
    """Margin the firmware actually gates on: +inf when there is no runner-up identity."""
    if r["second_id"] is not None and r["second_id"] >= 0:
        return r["sim"] - r["second_sim"]
    return math.inf


def accept_at(r, thr, mgn):
    return r["id"] > 0 and r["sim"] >= thr and eff_margin(r) >= mgn


def metrics(records, thr, mgn):
    """Return dict of TAR/FAR/FRR/misID and the raw counts at one operating point."""
    g = imp = ta = misid = fa = 0
    for r in records:
        acc = accept_at(r, thr, mgn)
        if r["true_id"] > 0:  # genuine trial
            g += 1
            if acc and r["id"] == r["true_id"]:
                ta += 1
            elif acc:
                misid += 1
        else:  # impostor trial (true_id == 0)
            imp += 1
            if acc:
                fa += 1
    return {
        "n_genuine": g, "n_impostor": imp,
        "TAR": ta / g if g else float("nan"),
        "FRR": 1 - ta / g if g else float("nan"),
        "misID": misid / g if g else float("nan"),
        "FAR": fa / imp if imp else float("nan"),
        "ta": ta, "misid": misid, "fa": fa,
    }


def sweep(records, mgn, lo=0.20, hi=0.95, step=0.01):
    """1-D accept-threshold sweep at a fixed margin -> list of (thr, TAR, FAR, FRR).

    Thresholds are generated as round(lo + i*step) (NOT by accumulating += step) so the
    threshold actually applied equals the one printed - otherwise float drift makes a sim
    that exactly equals a 'round' threshold fall on the wrong side of the >= compare.
    """
    rows = []
    n = int(round((hi - lo) / step)) + 1
    for i in range(n):
        t = round(lo + i * step, 4)
        m = metrics(records, t, mgn)
        rows.append((t, m["TAR"], m["FAR"], m["FRR"]))
    return rows


def find_eer(rows):
    """EER = threshold where FAR ~= FRR (needs both genuine and impostor trials)."""
    best = None
    for thr, tar, far, frr in rows:
        if math.isnan(far) or math.isnan(frr):
            return None
        d = abs(far - frr)
        if best is None or d < best[0]:
            best = (d, thr, (far + frr) / 2.0)
    return best  # (gap, thr, eer)


def suggest(rows, far_target):
    """Lowest-FRR threshold whose FAR <= target (most permissive that still meets FAR budget)."""
    cand = [r for r in rows if not math.isnan(r[2]) and r[2] <= far_target]
    if not cand:
        return None
    return min(cand, key=lambda r: (r[3] if not math.isnan(r[3]) else 1.0))


def pct(x):
    return "  n/a " if x is None or (isinstance(x, float) and math.isnan(x)) else f"{100*x:6.2f}%"


def bin_report(records, thr, mgn):
    """TAR/FAR sliced by the probe covariates, to attribute failures to condition."""
    def tw_bin(v):
        if v is None: return "tw=?"
        return "tw<60" if v < 60 else "tw 60-90" if v < 90 else "tw 90-150" if v < 150 else "tw>=150"
    def fr_bin(v):
        if v is None: return "fr=?"
        return "fr<0.6" if v < 0.6 else "fr 0.6-0.8" if v < 0.8 else "fr>=0.8"
    def sat_bin(v):
        if v is None: return "sat=?"
        return "sat<0.05" if v < 0.05 else "sat 0.05-0.15" if v < 0.15 else "sat>=0.15"

    for name, key, fn in (("FACE SIZE (tw)", "tw", tw_bin),
                          ("FRONTALITY (fr)", "fr", fr_bin),
                          ("EXPOSURE (sat)", "sat", sat_bin)):
        groups = defaultdict(list)
        for r in records:
            groups[fn(r.get(key))].append(r)
        print(f"\n  by {name}:")
        print(f"    {'bin':<14}{'n_gen':>6}{'n_imp':>6}{'TAR':>9}{'FAR':>9}")
        for k in sorted(groups):
            mm = metrics(groups[k], thr, mgn)
            print(f"    {k:<14}{mm['n_genuine']:>6}{mm['n_impostor']:>6}{pct(mm['TAR']):>9}{pct(mm['FAR']):>9}")


def main():
    ap = argparse.ArgumentParser(description="Offline TAR/FAR/EER/ROC analyzer for VERIFY logs.")
    ap.add_argument("logs", nargs="*", help="captured serial log file(s)")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--genuine-id", type=int, help="all positional logs = genuine trials for this enrolled id")
    g.add_argument("--impostor", action="store_true", help="all positional logs = impostor trials (true id 0)")
    ap.add_argument("--manifest", help="csv 'logfile,true_id' (true_id 0 = impostor); combine many sessions")
    ap.add_argument("--far-target", type=float, default=0.01, help="FAR budget for the suggested threshold (default 0.01)")
    ap.add_argument("--margin", type=float, default=None, help="margin to sweep thr at (default: value read from log, else 0.06)")
    ap.add_argument("--csv", help="write per-record CSV here")
    ap.add_argument("--plot", help="write ROC png here (needs matplotlib)")
    args = ap.parse_args()

    sessions = []  # (path, true_id)
    if args.manifest:
        with open(args.manifest) as f:
            for row in csvmod.reader(f):
                if not row or row[0].strip().startswith("#"):
                    continue
                sessions.append((row[0].strip(), int(row[1])))
    for p in args.logs:
        if args.genuine_id is not None:
            sessions.append((p, args.genuine_id))
        elif args.impostor:
            sessions.append((p, 0))
        else:
            sessions.append((p, None))  # unlabeled

    if not sessions:
        ap.error("no logs given. Provide log file(s) with --genuine-id/--impostor, or a --manifest.")

    records, unlabeled = [], 0
    for path, true_id in sessions:
        try:
            recs = parse_log(path)
        except OSError as e:
            print(f"!! cannot read {path}: {e}", file=sys.stderr)
            continue
        for r in recs:
            r["true_id"] = true_id
        if true_id is None:
            unlabeled += len(recs)
        records.extend([r for r in recs if r["true_id"] is not None])
        print(f"  {path}: {len(recs)} VERIFY events, true_id={true_id}")

    if not records:
        print("\nNo LABELED VERIFY records found. Add --genuine-id / --impostor / --manifest so genuine "
              "vs impostor can be separated (the log only carries the PREDICTED id).", file=sys.stderr)
        if unlabeled:
            print(f"({unlabeled} unlabeled events were parsed but cannot be scored.)", file=sys.stderr)
        sys.exit(2)

    # operating point as configured on-device (read from the log; fall back to 0.62/0.06)
    on_thr = float(records[0].get("thr", 0.62))
    on_mgn = float(records[0].get("mgn", 0.06))
    mgn = args.margin if args.margin is not None else on_mgn

    n_g = sum(1 for r in records if r["true_id"] > 0)
    n_i = sum(1 for r in records if r["true_id"] == 0)
    print(f"\n{'='*64}\nLABELED EVENTS: {len(records)}  (genuine={n_g}, impostor={n_i})")
    print(f"recognizer(s): {sorted(set(r['recognizer'] for r in records))}   "
          f"DB size seen: {sorted(set(r['db'] for r in records))}")

    m = metrics(records, on_thr, mgn)
    print(f"\nON-DEVICE OPERATING POINT  thr={on_thr:.2f}  margin={mgn:.2f}")
    print(f"  TAR (true accept) : {pct(m['TAR'])}   ({m['ta']}/{m['n_genuine']})")
    print(f"  FRR (false reject): {pct(m['FRR'])}")
    print(f"  mis-ID (accept wrong person): {pct(m['misID'])}   ({m['misid']}/{m['n_genuine']})  <- dangerous for payroll")
    print(f"  FAR (false accept): {pct(m['FAR'])}   ({m['fa']}/{m['n_impostor']})")

    rows = sweep(records, mgn)
    eer = find_eer(rows)
    if eer:
        print(f"\nEER (at margin={mgn:.2f}): {pct(eer[2])} near thr={eer[1]:.2f}")
    else:
        print(f"\nEER: needs BOTH genuine and impostor sessions (have genuine={n_g}, impostor={n_i}).")
    sug = suggest(rows, args.far_target)
    if sug:
        print(f"Suggested thr for FAR<={args.far_target:.0%}: thr={sug[0]:.2f} -> TAR={pct(sug[1])} FAR={pct(sug[2])}")

    print("\nTHRESHOLD SWEEP (margin fixed):")
    print(f"  {'thr':>5}{'TAR':>9}{'FAR':>9}{'FRR':>9}")
    for thr, tar, far, frr in rows:
        if abs((thr * 100) % 5) < 1e-6:  # print every 0.05 to keep it readable
            print(f"  {thr:>5.2f}{pct(tar):>9}{pct(far):>9}{pct(frr):>9}")

    bin_report(records, on_thr, mgn)

    if args.csv:
        cols = ["src", "ts_ms", "recognizer", "true_id", "id", "sim", "second_id", "second_sim",
                "db", "tw", "fr", "sh", "ds", "sat", "decision"]
        with open(args.csv, "w", newline="") as f:
            w = csvmod.DictWriter(f, fieldnames=cols, extrasaction="ignore")
            w.writeheader()
            w.writerows(records)
        print(f"\nwrote per-record CSV -> {args.csv}")

    if args.plot:
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
            fars = [r[2] for r in rows if not math.isnan(r[2]) and not math.isnan(r[1])]
            tars = [r[1] for r in rows if not math.isnan(r[2]) and not math.isnan(r[1])]
            if fars:
                plt.figure(figsize=(5, 5))
                plt.plot(fars, tars, "-o", ms=3)
                plt.xlabel("FAR"); plt.ylabel("TAR"); plt.title(f"ROC (margin={mgn:.2f})")
                plt.grid(True, alpha=0.3); plt.xlim(0, 1); plt.ylim(0, 1)
                plt.savefig(args.plot, dpi=120, bbox_inches="tight")
                print(f"wrote ROC -> {args.plot}")
            else:
                print("ROC needs impostor trials to have an FAR axis.", file=sys.stderr)
        except ImportError:
            print("matplotlib not installed; skipping --plot (metrics above are unaffected).", file=sys.stderr)


if __name__ == "__main__":
    main()
