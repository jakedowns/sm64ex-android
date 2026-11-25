#!/usr/bin/env python3
"""Parse SM64HFR telemetry from `scripts/hfr-bowser-capture.sh` into a per-frame
CSV and report the held-object scale stability.

    scripts/hfr-scale-check.py DIR_OR_LOG [DIR_OR_LOG ...]

Each phase log is expected to contain lines like:

  SM64HFR f=123 tick=456 a=0.250 fps=2 interp=1 slow=1 held=1 entries=37
          beh=8032... act=0 hs=1 anim=12 gs=1.000,1.000,1.000
          prev=1.0000,1.0000,1.0000 cur=1.0000,1.0000,1.0000
          out=1.0000,0.9808,1.0000 mean=0.9936 dev=0.0192 pvmode=0 gap=1 peq=0
          pos=0,50,-800

`prev`/`cur` are the taking part in the blend. `out` is the matrix that was
actually presented. `dev` is max_j |out_j / lerp(prev_j, cur_j) - 1|: the
artifact in one number (0 = perfect scale interpolation).

Outputs <phase>.csv next to each log and a summary table, then a verdict.
"""
import csv
import os
import re
import statistics
import sys

NUM = r"[-+]?\d+(?:\.\d+)?"
RE_KV = re.compile(r"(\w+)=(" + NUM + r"(?:," + NUM + r")*)")
RE_BEH = re.compile(r"beh=([0-9a-fA-F]+)")

FIELDS_INT = {"f", "tick", "fps", "interp", "slow", "held", "entries", "act",
              "hs", "anim", "pvmode", "gap", "peq", "idx"}
FIELDS_VEC = {"gs", "prev", "cur", "out", "pos", "cT", "pT"}
FIELDS_FLOAT = {"a", "mean", "dev", "hdev"}


def parse_line(line):
    if "SM64HFR" not in line:
        return None
    body = line.split("SM64HFR", 1)[1]
    rec = {}
    for key, val in RE_KV.findall(body):
        if key in FIELDS_INT:
            rec[key] = int(val)
        elif key in FIELDS_FLOAT:
            rec[key] = float(val)
        elif key in FIELDS_VEC:
            rec[key] = [float(x) for x in val.split(",")]
    m = RE_BEH.search(body)
    if m:
        rec["beh"] = m.group(1)
    if "f" not in rec or "out" not in rec:
        return None
    return rec


def phase_name(path):
    return os.path.splitext(os.path.basename(path))[0]


def analyse(path):
    rows = []
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            rec = parse_line(line)
            if rec:
                rows.append(rec)
    if not rows:
        print(f"  {phase_name(path)}: no SM64HFR frames")
        return None

    csv_path = os.path.splitext(path)[0] + ".csv"
    with open(csv_path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["frame", "tick", "alpha", "fps", "interp", "slow", "held",
                    "entries", "behavior", "action", "held_state", "anim",
                    "gs0", "gs1", "gs2", "prev0", "prev1", "prev2",
                    "cur0", "cur1", "cur2", "out0", "out1", "out2",
                    "mean", "dev", "hdev", "blend", "gap", "prev_eq_cur", "idx",
                    "cur_t0", "cur_t1", "cur_t2", "prev_t0", "prev_t1", "prev_t2",
                    "pos0", "pos1", "pos2"])
        for r in rows:
            w.writerow([
                r.get("f"), r.get("tick"), r.get("a"), r.get("fps"),
                r.get("interp"), r.get("slow"), r.get("held"), r.get("entries"),
                r.get("beh"), r.get("act"), r.get("hs"), r.get("anim"),
                *r.get("gs", [None] * 3), *r.get("prev", [None] * 3),
                *r.get("cur", [None] * 3), *r.get("out", [None] * 3),
                r.get("mean"), r.get("dev"), r.get("hdev"), r.get("pvmode"),
                r.get("gap"), r.get("peq"), r.get("idx"),
                *r.get("cT", [None] * 3), *r.get("pT", [None] * 3),
                *r.get("pos", [None] * 3),
            ])

    means = [r["mean"] for r in rows]
    devs = [r["dev"] for r in rows]
    hdevs = [r.get("hdev", 0.0) for r in rows]
    deltas = [abs(b - a) for a, b in zip(means, means[1:])]

    # Worst per-frame scale delta within one tick phase (alpha bucket), so the
    # 30 Hz step at a tick boundary is not mistaken for the intra-tick pulse.
    by_alpha = {}
    for r in rows:
        by_alpha.setdefault(round(r.get("a", 1.0), 3), []).append(r["mean"])
    intra = 0.0
    for vals in by_alpha.values():
        if len(vals) > 1:
            intra = max(intra, max(vals) - min(vals))

    return {
        "name": phase_name(path),
        "frames": len(rows),
        "blend": rows[0].get("pvmode", -1),
        "fps": rows[0].get("fps", -1),
        "interp": rows[0].get("interp", -1),
        "alpha_mean": statistics.fmean(r.get("a", 0.0) for r in rows),
        "mean_min": min(means),
        "mean_max": max(means),
        "mean_span": max(means) - min(means),
        "dev_max": max(devs),
        "hdev_max": max(hdevs),
        "delta_max": max(deltas) if deltas else 0.0,
        "intra_span": intra,
        "jumps": sum(1 for d in devs if d > 0.005),
        "peq": sum(1 for r in rows if r.get("peq") == 1),
        "gap_bad": sum(1 for r in rows if r.get("gap", 1) not in (0, 1)),
        "csv": csv_path,
    }


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    results = []
    for path in argv[1:]:
        if os.path.isdir(path):
            for name in sorted(os.listdir(path)):
                if name.endswith(".log"):
                    res = analyse(os.path.join(path, name))
                    if res:
                        results.append(res)
        else:
            res = analyse(path)
            if res:
                results.append(res)
    if not results:
        print("no telemetry found")
        return 1

    print(f"{'phase':22s} {'frames':>6s} {'fps':>3s} {'int':>3s} {'blend':>5s} "
          f"{'mean span':>9s} {'dev max':>8s} {'hdev max':>8s} {'dmean max':>9s} {'jumps':>5s}")
    for r in results:
        print(f"{r['name']:22s} {r['frames']:6d} {r['fps']:3d} {r['interp']:3d} "
              f"{r['blend']:5d} {r['mean_span']:9.4f} {r['dev_max']:8.4f} "
              f"{r['hdev_max']:8.4f} {r['delta_max']:9.4f} {r['jumps']:5d}")

    print()
    legacy = [r for r in results if r["blend"] == 0 and r["interp"] == 1]
    fixed = [r for r in results if r["blend"] == 1 and r["interp"] == 1]
    hfroff = [r for r in results if r["interp"] == 0]

    if hfroff:
        r = hfroff[0]
        print(f"baseline HFR off : mean scale {r['mean_min']:.4f}..{r['mean_max']:.4f}, "
              f"dev {r['dev_max']:.4f}  -> expect no pulse")
    for r in legacy:
        print(f"legacy linear    : mean scale {r['mean_min']:.4f}..{r['mean_max']:.4f} "
              f"(span {r['mean_span']*100:.2f}%), body distortion {r['hdev_max']*100:.2f}%, "
              f"blend dev {r['dev_max']*100:.2f}%, {r['jumps']}/{r['frames']} over 0.5%")
    for r in fixed:
        print(f"slerp/recompose  : mean scale {r['mean_min']:.4f}..{r['mean_max']:.4f} "
              f"(span {r['mean_span']*100:.2f}%), body distortion {r['hdev_max']*100:.2f}%, "
              f"blend dev {r['dev_max']*100:.2f}%, {r['jumps']}/{r['frames']} over 0.5%")

    print()
    if legacy and legacy[0]["hdev_max"] > 0.01:
        print("VERDICT: presented held scale pulses "
              f"({legacy[0]['hdev_max']*100:.2f}% body distortion at worst) — bug reproduced.")
    elif legacy:
        print("VERDICT: no scale pulse measured in the legacy phase.")
    if fixed:
        worst = max(r["hdev_max"] for r in fixed)
        if worst < 0.005:
            print(f"VERDICT: fix holds the scale steady (max deviation {worst*100:.3f}%).")
        else:
            print(f"VERDICT: fix still deviates by {worst*100:.3f}% — investigate.")
    for r in results:
        print(f"  csv: {r['csv']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
