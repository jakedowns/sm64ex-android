#!/usr/bin/env python3
"""Host harness for the wall-run stick-shaped jump-off + release window.

Runs the PTC `wrjumpN` rig on the tablet and asserts the exit vector for the
four stick cases, plus the release-then-jump window.  Everything that touches
adb happens inside one `scripts/agent-lock.sh device` command: unlock, restart
the app, load level 38, then for each case start `ptc wrjumpN`, poll `cmd dump`
and save a screenshot.

Cases (see ptc.c `ptc_wrj_*`):
  0 neutral            stick centred
  1 away               stick +X, away from the west annex wall
  2 along              stick +Z, along the wall
  3 into               stick -X, into the wall (must clamp outward)
  4 release + 2 ticks  jump inside the release window
  5 release + late     jump after the window (must be ignored)

Two dial checks follow: `fx 320 0` makes case 1 ignore the stick (old fixed
vector), and `fx 323 12` widens the window (case 4 still fires, case 5 still
does not).

Usage:
  scripts/wallrun-stick-test.py [--install] [--samples N]
                               [--out DIR] [--lock-timeout SECONDS]
Exit status: 0 pass, 1 fail, 75 device lock busy (retry), 2 usage error.
"""
import argparse
import os
import re
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCK = os.path.join(ROOT, "scripts", "agent-lock.sh")
PKG = "com.vdavid003.sm64port"
ACTIVITY = PKG + "/.sm64portActivity"
BROADCAST = "am broadcast -n %s/.DebugReceiver -a %s.CMD" % (PKG, PKG)

ACT_WALLRUN = "0x0300089a"
ACT_WALL_KICK_AIR = "0x03000886"

WR_RE = re.compile(
    r"\bwr=(\d+),(\d+),(\d+),(\d+),(\d+),(-?[\d.]+),(-?[\d.]+)")
ACTION_RE = re.compile(r"\baction=(0x[0-9a-fA-F]+)")
VEL_RE = re.compile(r"\bvel=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)")
MARIO_RE = re.compile(r"\bmario=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)")

RUNS = [
    # (ptc action suffix, output name, poll count, fx overrides before the run)
    ("0", "0_neutral", 10, []),
    ("1", "1_away", 10, []),
    ("2", "2_along", 10, []),
    ("3", "3_into", 10, []),
    ("4", "4_release_2ticks", 12, []),
    ("5", "5_release_late", 12, []),
    # dial checks
    ("1", "6_away_blend0", 10, [(320, 0)]),
    ("4", "7_rel_2ticks_w12", 14, [(320, 65), (323, 12)]),
    ("5", "8_rel_late_w12", 16, [(323, 12)]),
]


def inner_script(out, samples, install=False):
    install_cmd = ""
    if install:
        apk = os.path.join(ROOT, "app/build/outputs/apk/debug/app-debug.apk")
        install_cmd = "adb install -r %s | tail -1 || true" % shlex.quote(apk)
    runs = "\n".join(
        '{set_fx}\nrun_case {cs} {name} {n}'
        .format(
            set_fx="\n".join(
                "adb shell $B --es cmd fx --ei k {k} --ei v {v} >/dev/null 2>&1"
                .format(k=k, v=v) for k, v in fx),
            cs=cs, name=shlex.quote(name), n=n)
        for cs, name, n, fx in RUNS)
    return """set -u
export LD_LIBRARY_PATH=/home/ubuntu/.local/lib/android-tools:${{LD_LIBRARY_PATH:-}}
B="{broadcast}"
OUT={out}
rm -f "$OUT"/*.tsv "$OUT"/*.png "$OUT"/runlog.txt

/media/share/lp2-test/scripts/unlock.sh >/dev/null 2>&1 || true
{install}
adb shell am force-stop {pkg} >/dev/null 2>&1 || true
adb shell am start -n {activity} >/dev/null 2>&1 || true
sleep 10
adb shell $B --es cmd level --ei n 38 >/dev/null 2>&1 || true
sleep 8

run_case() {{
  CS=$1; NAME=$2; N=$3
  adb logcat -c >/dev/null 2>&1 || true
  adb shell $B --es cmd ptc --es action "wrjump$CS" >/dev/null 2>&1 || true
  : > "$OUT/$NAME.tsv"
  i=0
  while [ "$i" -lt "$N" ]; do
    adb shell $B --es cmd dump >/dev/null 2>&1 || true
    sleep 0.28
    LINE=$(adb logcat -d -s SM64Leia:I 2>/dev/null | grep 'PTC level=' | tail -n 1)
    [ -z "$LINE" ] && LINE='(missing)'
    printf '%s\\t%s\\n' "$i" "$LINE" >> "$OUT/$NAME.tsv"
    i=$((i + 1))
  done
  adb exec-out screencap -p > "$OUT/$NAME.png" 2>/dev/null || true
}}

{runs}

# restore the defaults for the next run
adb shell $B --es cmd fx --ei k 323 --ei v 5 >/dev/null 2>&1 || true
adb shell $B --es cmd fx --ei k 320 --ei v 65 >/dev/null 2>&1 || true
adb shell $B --es cmd dump >/dev/null 2>&1 || true
sleep 0.4
adb logcat -d -s SM64Leia:I 2>/dev/null | grep 'PTC level=' | tail -n 1 > "$OUT/final-dump.txt" || true
""".format(broadcast=BROADCAST, out=shlex.quote(out), pkg=PKG,
           activity=ACTIVITY, install=install_cmd or "true",
           runs=runs, samples=samples)


def parse_run(name, path):
    rows = []
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            for raw in fh:
                raw = raw.rstrip("\n")
                if not raw:
                    continue
                parts = raw.split("\t", 1)
                if len(parts) != 2:
                    continue
                line = parts[1]
                row = {"line": line, "missing": line == "(missing)"}
                m = WR_RE.search(line)
                if m:
                    row["rel_left"] = int(m.group(1))
                    row["rel_win"] = int(m.group(2))
                    row["last_jump"] = int(m.group(3))
                    row["blend"] = int(m.group(4))
                    row["init"] = int(m.group(5))
                    row["arm"] = int(m.group(6))
                    row["wrvx"] = float(m.group(7))
                    row["wrvz"] = float(m.group(8))
                m = ACTION_RE.search(line)
                if m:
                    row["action"] = m.group(1).lower()
                m = VEL_RE.search(line)
                if m:
                    row["vel"] = tuple(float(m.group(i)) for i in (1, 2, 3))
                m = MARIO_RE.search(line)
                if m:
                    row["mario"] = tuple(float(m.group(i)) for i in (1, 2, 3))
                rows.append(row)
    except OSError:
        pass
    return rows


def best_row(rows):
    """Prefer the last row that has a wr= vector; fall back on the last row."""
    with_wr = [r for r in rows if "wrvx" in r]
    if with_wr:
        return with_wr[-1]
    return rows[-1] if rows else None


def speed(r):
    return (r["wrvx"] ** 2 + r["wrvz"] ** 2) ** 0.5


def checks(name, rows):
    r = best_row(rows)
    if r is None or "wrvx" not in r:
        return [("status line with wr=", False, "no dump")]
    out = []
    vx, vz = r["wrvx"], r["wrvz"]
    sp = speed(r)
    obs = "v=(%.1f,%.1f) |v|=%.1f jump=%d init=%d arm=%d action=%s" % (
        vx, vz, sp, r["last_jump"], r["init"], r["arm"],
        r.get("action", "-"))
    jump_case = name not in ("5_release_late", "8_rel_late_w12")
    out.append(("wr= present", True, obs))
    out.append(("wall run started", r["init"] >= 1, "init=%d" % r["init"]))
    if jump_case:
        out.append(("exit speed 45-80", 45.0 <= sp <= 80.0, "|v|=%.1f" % sp))

    if name == "0_neutral":
        out.append(("neutral: outward +X 6-20", 6.0 <= vx <= 20.0, "vx=%.1f" % vx))
        out.append(("neutral: tangent +Z 45-70", 45.0 <= vz <= 70.0, "vz=%.1f" % vz))
        out.append(("jump source = wallrun", r["last_jump"] == 1,
                    "jump=%d" % r["last_jump"]))
    elif name == "1_away":
        out.append(("away: strong +X", vx > 42.0, "vx=%.1f" % vx))
        out.append(("away: little tangent", abs(vz) < 30.0, "vz=%.1f" % vz))
        out.append(("jump source = wallrun", r["last_jump"] == 1,
                    "jump=%d" % r["last_jump"]))
    elif name == "2_along":
        out.append(("along: tangent +Z", vz > 45.0, "vz=%.1f" % vz))
        out.append(("along: small outward", 0.0 <= vx <= 22.0, "vx=%.1f" % vx))
        out.append(("jump source = wallrun", r["last_jump"] == 1,
                    "jump=%d" % r["last_jump"]))
    elif name == "3_into":
        out.append(("into: clamped outward (vx>=4)", vx >= 4.0, "vx=%.1f" % vx))
        out.append(("into: keeps tangent", vz > 42.0, "vz=%.1f" % vz))
        out.append(("jump source = wallrun", r["last_jump"] == 1,
                    "jump=%d" % r["last_jump"]))
    elif name in ("4_release_2ticks", "7_rel_2ticks_w12"):
        out.append(("release window armed", r["arm"] >= 1, "arm=%d" % r["arm"]))
        out.append(("jump source = release window", r["last_jump"] == 2,
                    "jump=%d" % r["last_jump"]))
        saw = any(x.get("action") == ACT_WALL_KICK_AIR for x in rows)
        out.append(("action seen = WALL_KICK_AIR (info)", True,
                    "seen=%s last=%s" % (saw, r.get("action", "-"))))
        out.append(("release exit keeps tangent", vz > 42.0, "vz=%.1f" % vz))
        if name == "7_rel_2ticks_w12":
            out.append(("window dial = 12", r["rel_win"] == 12,
                        "win=%d" % r["rel_win"]))
    elif name in ("5_release_late", "8_rel_late_w12"):
        out.append(("release window armed", r["arm"] >= 1, "arm=%d" % r["arm"]))
        out.append(("late press not a wall jump", r["last_jump"] != 2,
                    "jump=%d" % r["last_jump"]))
        out.append(("release vector not applied (info)", True,
                    "v=(%.1f,%.1f)" % (vx, vz)))
        if name == "8_rel_late_w12":
            out.append(("window dial = 12", r["rel_win"] == 12,
                        "win=%d" % r["rel_win"]))
    elif name == "6_away_blend0":
        out.append(("blend 0 ignores the stick", 6.0 <= vx <= 20.0,
                    "vx=%.1f" % vx))
        out.append(("blend 0 = neutral vector", 45.0 <= vz <= 70.0,
                    "vz=%.1f" % vz))
        out.append(("jump source = wallrun", r["last_jump"] == 1,
                    "jump=%d" % r["last_jump"]))
    return out


def print_table(out, results):
    print("  %-18s %-8s %-7s %-5s %-5s %-5s %-6s %-8s %s"
          % ("case", "v(wr)", "|v|", "jmp", "win", "init", "arm",
             "action", "vel (on device)"))
    for name, rows in results:
        r = best_row(rows)
        if r is None or "wrvx" not in r:
            print("  %-18s %s" % (name, "(no wr= dump)"))
            continue
        vel = "-"
        if "vel" in r:
            vel = "%.1f,%.1f,%.1f" % r["vel"]
        print("  %-18s %-8s %-7.1f %-5d %-5d %-5d %-6d %-8s %s"
              % (name, "%.1f,%.1f" % (r["wrvx"], r["wrvz"]), speed(r),
                 r["last_jump"], r["rel_win"], r["init"], r["arm"],
                 r.get("action", "-"), vel))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--install", action="store_true",
                    help="install app/build/outputs/apk/debug/app-debug.apk first")
    ap.add_argument("--samples", type=int, default=10,
                    help="unused compatibility flag (per-run counts are fixed)")
    ap.add_argument("--out", default="/media/share/tmp/artifacts/sm64/wallrun-stick",
                    help="output directory for dumps and PNGs")
    ap.add_argument("--lock-timeout", type=int, default=900,
                    help="device lock timeout in seconds (default 900)")
    args = ap.parse_args(argv)

    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    reason = "wallrun stick jump-off"
    script = inner_script(out, args.samples, args.install)
    cmd = [LOCK, "device", "--timeout", str(args.lock_timeout),
           "--reason", reason, "--", "bash", "-lc", script]

    print("WALL-RUN STICK TEST  out=%s" % out)
    print("device: scripts/agent-lock.sh device --reason %s -- bash -lc <harness>"
          % shlex.quote(reason))
    print("(unlock, restart app, level 38, 6 stick/cooldown cases + 3 dial checks)")
    sys.stdout.flush()

    try:
        proc = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True)
    except OSError as exc:
        print("ERROR: cannot run %s: %s" % (LOCK, exc), file=sys.stderr)
        return 2

    if proc.returncode == 75:
        print("\nLOCK TIMEOUT (exit 75): the device lock is held by another")
        print("experiment. Nothing was changed. Re-run after it finishes.")
        return 75
    if proc.returncode != 0:
        print("ERROR: device command failed (exit %d)" % proc.returncode,
              file=sys.stderr)
        print(proc.stdout[-3000:], file=sys.stderr)
        print(proc.stderr[-3000:], file=sys.stderr)
        return 1

    results = []
    for _cs, name, _n, _fx in RUNS:
        results.append((name, parse_run(name, os.path.join(out, name + ".tsv"))))

    print()
    print_table(out, results)
    print()

    failed = 0
    total = 0
    for name, rows in results:
        for label, ok, observed in checks(name, rows):
            total += 1
            if not ok:
                failed += 1
            print("%s  %-36s %-18s observed: %s"
                  % ("PASS" if ok else "FAIL", label, name, observed))
    print()
    shots = sorted(n for n in os.listdir(out) if n.endswith(".png"))
    print("screenshots=%d %s" % (len(shots), " ".join(shots)))
    print("dumps in %s" % out)
    if failed:
        print("RESULT: FAIL (%d/%d checks)" % (total - failed, total))
        return 1
    print("RESULT: PASS (%d/%d checks)" % (total, total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
