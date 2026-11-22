#!/usr/bin/env python3
"""Host-side harness for the NINJA CHAIN (see NINJA-CHAIN.md).

Runs one chain section on the tablet, polls the PTC dump, and asserts the
documented acceptance criteria.  Everything that touches adb happens inside a
single `scripts/agent-lock.sh device` command: unlock, restart the app, load
level 38, start the section, poll `cmd dump` every ~0.6 s, save up to three
screenshots.

Usage:
  scripts/ninja-chain-test.py [--section chainbox|chainwall|chaingrapple|
                              chainjet|chainportal|chaingoomba|chain]
                             [--seconds N] [--out DIR]
  scripts/ninja-chain-test.py --fast [--ticks N] [--section ...] [--install]

Two modes:

  real time (default)  Poll `cmd dump` for --seconds, save checkpoints.  This
                       is the only mode that produces screenshots of the run.
  --fast               Load level 38, start the section, then run `cmd runticks
                       --ei n <ticks>` and read the per-tick trace with `cmd
                       trace`.  Asserts the same flags/stars/crossings from the
                       trace and prints the speedup vs 30 Hz real time.  One
                       screenshot is taken afterwards (normal rendering) to
                       prove the game still presents frames.

Defaults: section=chain, seconds=30, ticks=300, out=/tmp/opencode/ninja-test
Exit status: 0 pass, 1 fail, 75 device lock busy (retry), 2 usage error.

Dump tokens parsed (app/jni/src/src/game/ptc.c, ptc_format_status):
  chain=<phase>,<timer>,<flags hex>,<goomba crossings>,<cubes>
  cmode=  star=  star2=  action=  mario=  tele=

Trace lines (app/jni/src/src/game/ptc_trace.c):
  t=<timer> ph=<phase> fl=0x<flags> act=0x<action> pos=<x,y,z>
  st=<star0><star1> gx=<crossings> cm=<cam mode>
"""
import argparse
import os
import re
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCK = os.path.join(ROOT, "scripts", "agent-lock.sh")
PKG = "com.vdavid003.sm64port"
ACTIVITY = PKG + "/.sm64portActivity"
BROADCAST = "am broadcast -n %s/.DebugReceiver -a %s.CMD" % (PKG, PKG)

SECTIONS = ["chainbox", "chainwall", "chaingrapple", "chainjet",
            "chainportal", "chaingoomba", "chain"]

# Chain flag bits (ptc.c CH_FLAG_*).
F_BALL, F_PUNCH, F_KICK, F_SLAM = 0x001, 0x002, 0x004, 0x008
F_JETPACK, F_WALLRUN, F_GRAPPLE, F_PORTAL = 0x010, 0x020, 0x040, 0x080
F_STAR1, F_STAR2, F_GOOMBA = 0x100, 0x200, 0x400

ACT_WALLRUN = 0x0300089A

CHAIN_RE = re.compile(r"chain=(\d+),(\d+),(0x[0-9a-fA-F]+),(\d+),(\d+)")
CMODE_RE = re.compile(r"\bcmode=(-?\d+)")
STAR_RE = re.compile(r"\bstar=(-?\d+)")
STAR2_RE = re.compile(r"\bstar2=(-?\d+)")
ACTION_RE = re.compile(r"\baction=(0x[0-9a-fA-F]+)")
MARIO_RE = re.compile(r"\bmario=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)")
TELE_RE = re.compile(r"\btele=(-?\d+)")

TRACE_RE = re.compile(
    r"t=(\d+)\s+ph=(-?\d+)\s+fl=(0x[0-9a-fA-F]+)\s+act=(0x[0-9a-fA-F]+)"
    r"\s+pos=(-?\d+),(-?\d+),(-?\d+)\s+st=(\d)(\d)\s+gx=(-?\d+)\s+cm=(-?\d+)")
RUN_DONE_RE = re.compile(r"PTC runticks done n=(\d+) ms=(\d+)")

MISSING = "(no PTC level= line)"


def inner_script(section, seconds, out, install=False):
    """One self-contained bash script; every adb call lives in here."""
    sec_q = shlex.quote(section)
    out_q = shlex.quote(out)
    install_cmd = ""
    if install:
        apk = Path(__file__).resolve().parents[1] / "app/build/outputs/apk/debug/app-debug.apk"
        install_cmd = f"adb install -r {shlex.quote(str(apk))} | tail -1 || true"
    return """set -u
export LD_LIBRARY_PATH=/home/ubuntu/.local/lib/android-tools:${{LD_LIBRARY_PATH:-}}
B="{broadcast}"
OUT={out}
SEC={sec}
TOTAL={total}
LOG="$OUT/samples.tsv"
: > "$LOG"
rm -f "$OUT"/"$SEC"-25.png "$OUT"/"$SEC"-60.png "$OUT"/"$SEC"-95.png

# unlock
/media/share/lp2-test/scripts/unlock.sh >/dev/null 2>&1 || true
{install}
# restart the app so the PTC state (and teleport counter) is clean
adb shell am force-stop {pkg} >/dev/null 2>&1 || true
adb shell am start -n {activity} >/dev/null 2>&1 || true
sleep 10

# level 38 = Portal Test Chamber
adb shell $B --es cmd level --ei n 38 >/dev/null 2>&1 || true
sleep 8

# start the section
adb shell $B --es cmd ptc --es action "$SEC" >/dev/null 2>&1 || true

START=$(date +%s)
SHOT25=0; SHOT60=0; SHOT95=0
IDX=0
shot() {{  # shot <pct>
  adb exec-out screencap -p > "$OUT/$SEC-$1.png" 2>/dev/null || true
}}

while :; do
  NOW=$(date +%s)
  EL=$((NOW - START))
  [ "$EL" -ge "$TOTAL" ] && break

  adb logcat -c >/dev/null 2>&1 || true
  adb shell $B --es cmd dump >/dev/null 2>&1 || true
  sleep 0.5
  LINE=$(adb logcat -d -s SM64Leia:I 2>/dev/null | grep 'PTC level=' | tail -n 1)
  [ -z "$LINE" ] && LINE='{missing}'
  printf '%s\\t%s\\t%s\\n' "$IDX" "$EL" "$LINE" >> "$LOG"

  AFTER=$(date +%s)
  PCT=$(( (AFTER - START) * 100 / TOTAL ))
  if [ "$SHOT25" -eq 0 ] && [ "$PCT" -ge 25 ]; then shot 25; SHOT25=1; fi
  if [ "$SHOT60" -eq 0 ] && [ "$PCT" -ge 60 ]; then shot 60; SHOT60=1; fi
  if [ "$SHOT95" -eq 0 ] && [ "$PCT" -ge 95 ]; then shot 95; SHOT95=1; fi
  IDX=$((IDX + 1))
done

# the 95% checkpoint is effectively "end of window": take it if the loop
# finished before crossing the threshold.
if [ "$SHOT95" -eq 0 ]; then shot 95; fi
printf '%s %s %s\\n' "$IDX" "$SHOT25$SHOT60$SHOT95" "$TOTAL" > "$OUT/status.txt"
""".format(broadcast=BROADCAST, out=out_q, sec=sec_q, total=int(seconds),
           pkg=PKG, activity=ACTIVITY, missing=MISSING,
           install=install_cmd or "true")


def inner_fast_script(section, ticks, out, install=False):
    """Fast-forward script: start the section normally, then runticks + trace.

    Everything still happens inside one agent-lock device session.  The batch
    is timed on the device (trace footer, logged as `PTC runticks done`) and
    around the adb round trip; one screenshot afterwards proves the normal
    render path still works.
    """
    sec_q = shlex.quote(section)
    out_q = shlex.quote(out)
    install_cmd = ""
    if install:
        apk = Path(__file__).resolve().parents[1] / "app/build/outputs/apk/debug/app-debug.apk"
        install_cmd = f"adb install -r {shlex.quote(str(apk))} | tail -1 || true"
    return """set -u
export LD_LIBRARY_PATH=/home/ubuntu/.local/lib/android-tools:${{LD_LIBRARY_PATH:-}}
B="{broadcast}"
OUT={out}
SEC={sec}
TICKS={ticks}
rm -f "$OUT"/"$SEC"-fast.png "$OUT/trace.txt" "$OUT/fast-timing.txt" "$OUT/fast-dump.txt"

# unlock
/media/share/lp2-test/scripts/unlock.sh >/dev/null 2>&1 || true
{install}
# restart the app so the PTC state (and teleport counter) is clean
adb shell am force-stop {pkg} >/dev/null 2>&1 || true
adb shell am start -n {activity} >/dev/null 2>&1 || true
sleep 10

# level 38 = Portal Test Chamber
adb shell $B --es cmd level --ei n 38 >/dev/null 2>&1 || true
sleep 8

# start the section normally so camera/placement/chain setup are unchanged
adb shell $B --es cmd ptc --es action "$SEC" >/dev/null 2>&1 || true
sleep 1

adb logcat -c >/dev/null 2>&1 || true
T0=$(date +%s%N)
adb shell $B --es cmd runticks --ei n "$TICKS" >/dev/null 2>&1 || true

# wait for the game thread to finish the batch (logged from pc_main.c)
DONE=0
for i in $(seq 1 200); do
  sleep 0.1
  if adb logcat -d -s SM64Leia:I 2>/dev/null | grep -q 'PTC runticks done'; then
    DONE=1
    break
  fi
done
T1=$(date +%s%N)

# fetch the trace (one log record per line)
adb shell $B --es cmd trace >/dev/null 2>&1 || true
for i in $(seq 1 100); do
  sleep 0.1
  if adb logcat -d -s SM64Leia:I 2>/dev/null | grep -q 'PTC trace end'; then
    break
  fi
done
adb logcat -d -s SM64Leia:I 2>/dev/null \\
  | grep -E 'PTC runticks done|t=[0-9]+ ph=|PTC trace end' > "$OUT/trace.txt" || true

# normal rendering must still work after the batch: one screenshot + dump
sleep 0.5
adb exec-out screencap -p > "$OUT/$SEC-fast.png" 2>/dev/null || true
adb logcat -c >/dev/null 2>&1 || true
adb shell $B --es cmd dump >/dev/null 2>&1 || true
sleep 0.6
adb logcat -d -s SM64Leia:I 2>/dev/null | grep 'PTC level=' | tail -n 1 \\
  > "$OUT/fast-dump.txt" || true

printf '%s %s %s\\n' "$DONE" "$T0" "$T1" > "$OUT/fast-timing.txt"
""".format(broadcast=BROADCAST, out=out_q, sec=sec_q, ticks=int(ticks),
           pkg=PKG, activity=ACTIVITY, install=install_cmd or "true")


def parse_trace_line(raw):
    """Parse one trace line; returns None for headers/footers/log prefixes."""
    m = TRACE_RE.search(raw)
    if not m:
        return None
    return {
        "timer": int(m.group(1)),
        "phase": int(m.group(2)),
        "flags": int(m.group(3), 16),
        "action": m.group(4).lower(),
        "mario": (int(m.group(5)), int(m.group(6)), int(m.group(7))),
        "star": int(m.group(8)),
        "star2": int(m.group(9)),
        "crossings": int(m.group(10)),
        "cmode": int(m.group(11)),
        "line": raw.strip(),
    }


def trace_summary(samples):
    """Fold the per-tick trace into the same aggregate the dump path uses."""
    agg = {"flags": 0, "crossings": 0, "tele": 0, "star": 0, "star2": 0,
           "cmode": None, "actions": set()}
    for s in samples:
        agg["flags"] |= s["flags"]
        agg["crossings"] = max(agg["crossings"], s["crossings"])
        agg["star"] = max(agg["star"], s["star"])
        agg["star2"] = max(agg["star2"], s["star2"])
        agg["cmode"] = s["cmode"]
        agg["actions"].add(s["action"])
    return agg


def parse_sample(idx, elapsed, line):
    s = {"idx": int(idx), "t": int(elapsed), "line": line, "missing": line == MISSING}
    m = CHAIN_RE.search(line)
    if m:
        s["phase"] = int(m.group(1))
        s["timer"] = int(m.group(2))
        s["flags"] = int(m.group(3), 16)
        s["crossings"] = int(m.group(4))
        s["cubes"] = int(m.group(5))
    for key, rx in (("cmode", CMODE_RE), ("star", STAR_RE), ("star2", STAR2_RE),
                    ("tele", TELE_RE)):
        mm = rx.search(line)
        if mm:
            s[key] = int(mm.group(1))
    mm = ACTION_RE.search(line)
    if mm:
        s["action"] = mm.group(1).lower()
    mm = MARIO_RE.search(line)
    if mm:
        s["mario"] = tuple(float(mm.group(i)) for i in (1, 2, 3))
    return s


def summary(samples):
    agg = {"flags": 0, "crossings": 0, "tele": 0, "star": 0, "star2": 0,
           "cmode": None, "actions": set()}
    for s in samples:
        if s.get("missing"):
            continue
        agg["flags"] |= s.get("flags", 0)
        agg["crossings"] = max(agg["crossings"], s.get("crossings", 0))
        agg["tele"] = max(agg["tele"], s.get("tele", 0))
        agg["star"] = max(agg["star"], s.get("star", 0))
        agg["star2"] = max(agg["star2"], s.get("star2", 0))
        if "cmode" in s:
            agg["cmode"] = s["cmode"]
        if "action" in s:
            agg["actions"].add(s["action"])
    return agg


def checks(section, samples, agg, trace_mode=False):
    """Return [(label, ok, observed)] for the section's acceptance criteria.

    trace_mode uses the runticks trace, which has no teleport counter; the
    dump-only `tele > 0` check is skipped there.
    """
    if not samples or all(s.get("missing") for s in samples):
        return [("no samples collected", False, "samples=0")]

    f = agg["flags"]
    out = []
    if section == "chainbox":
        out.append(("flags & 0xF == 0xF", (f & 0xF) == 0xF, "flags=0x%03x" % f))
    elif section == "chainwall":
        out.append(("wall-run: flag 0x20 or action 0x%08X" % ACT_WALLRUN,
                    bool(f & F_WALLRUN) or ACT_WALLRUN_HEX in agg["actions"],
                    "flags=0x%03x action=%s" % (f, ",".join(sorted(agg["actions"])) or "-")))
    elif section == "chaingrapple":
        out.append(("flags & 0x40 (grapple)", bool(f & F_GRAPPLE), "flags=0x%03x" % f))
        out.append(("flags & 0x100 (star1)", bool(f & F_STAR1), "flags=0x%03x" % f))
    elif section == "chainjet":
        out.append(("flags & 0x10 (jetpack)", bool(f & F_JETPACK), "flags=0x%03x" % f))
        out.append(("flags & 0x80 (portal)", bool(f & F_PORTAL), "flags=0x%03x" % f))
        out.append(("flags & 0x200 (star2)", bool(f & F_STAR2), "flags=0x%03x" % f))
    elif section == "chainportal":
        out.append(("flags & 0x80 (portal)", bool(f & F_PORTAL), "flags=0x%03x" % f))
        if not trace_mode:
            out.append(("tele > 0", agg["tele"] > 0, "tele=%d" % agg["tele"]))
    elif section == "chaingoomba":
        out.append(("goomba crossings > 0", agg["crossings"] > 0,
                    "crossings=%d" % agg["crossings"]))
    elif section == "chain":
        out.append(("flags & 0x7FF == 0x7FF", (f & 0x7FF) == 0x7FF, "flags=0x%03x" % f))
        out.append(("star = 1", agg["star"] == 1, "star=%d" % agg["star"]))
        out.append(("star2 = 1", agg["star2"] == 1, "star2=%d" % agg["star2"]))
        out.append(("goomba crossings > 0", agg["crossings"] > 0,
                    "crossings=%d" % agg["crossings"]))
        out.append(("cmode = 3", agg["cmode"] == 3, "cmode=%s" % agg["cmode"]))
    return out


ACT_WALLRUN_HEX = "0x%08x" % ACT_WALLRUN


def print_table(samples):
    print("  %-3s %-5s %-6s %-6s %-8s %-5s %-4s %-4s %-5s %-9s %-5s %-12s %s"
          % ("#", "t(s)", "phase", "timer", "flags", "cmode", "star", "s2",
             "tele", "cross", "cubes", "action", "mario"))
    for s in samples:
        if s.get("missing"):
            print("  %-3d %-5d %-6s %-6s %-8s %-5s %-4s %-4s %-5s %-9s %-5s %-12s %s"
                  % (s["idx"], s["t"], "-", "-", "-", "-", "-", "-", "-", "-",
                     "-", "-", "(no dump)"))
            continue
        mario = "-"
        if "mario" in s:
            mario = "%.0f,%.0f,%.0f" % s["mario"]
        print("  %-3d %-5d %-6s %-6s 0x%03x    %-5s %-4s %-4s %-5s %-9s %-5s %-12s %s"
              % (s["idx"], s["t"], s.get("phase", "-"), s.get("timer", "-"),
                 s.get("flags", 0), s.get("cmode", "-"), s.get("star", "-"),
                 s.get("star2", "-"), s.get("tele", "-"), s.get("crossings", "-"),
                 s.get("cubes", "-"), s.get("action", "-"), mario))


def print_trace(samples, limit=4):
    print("  %-8s %-4s %-7s %-12s %-24s %-4s %-4s %-3s"
          % ("t", "ph", "flags", "action", "pos", "st", "gx", "cm"))
    show = samples if len(samples) <= 2 * limit else (
        samples[:limit] + [None] + samples[-limit:])
    for s in show:
        if s is None:
            print("  ...")
            continue
        print("  %-8d %-4d 0x%03x    %-12s %-24s %d%d   %-3d %-3d"
              % (s["timer"], s["phase"], s["flags"], s["action"],
                 "%d,%d,%d" % s["mario"], s["star"], s["star2"],
                 s["crossings"], s["cmode"]))


def image_mean_luma(path):
    """Mean luminance 0..255, or None when Pillow is unavailable."""
    try:
        from PIL import Image, ImageStat
    except ImportError:
        return None
    try:
        with Image.open(path) as im:
            stat = ImageStat.Stat(im.convert("L"))
        return stat.mean[0]
    except Exception:
        return None


def run_fast(args, out):
    reason = "ninja chain fast %s (%d ticks)" % (args.section, args.ticks)
    script = inner_fast_script(args.section, args.ticks, out, args.install)
    cmd = [LOCK, "device", "--reason", reason, "--", "bash", "-lc", script]

    print("NINJA CHAIN FAST-FORWARD  section=%s ticks=%d"
          % (args.section, args.ticks))
    print("out=%s" % out)
    print("device: scripts/agent-lock.sh device --reason %s -- bash -lc <harness>"
          % shlex.quote(reason))
    print("(unlock, restart app, level 38, action %s, runticks %d, cmd trace)"
          % (args.section, args.ticks))
    sys.stdout.flush()

    try:
        proc = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True)
    except OSError as exc:
        print("ERROR: cannot run %s: %s" % (LOCK, exc), file=sys.stderr)
        return 2

    if proc.returncode == 75:
        print("\nLOCK TIMEOUT (exit 75): the device lock is held by another")
        print("experiment on this host. Nothing was run and nothing was changed.")
        print("RETRY: wait for the other test to finish, then re-run the same")
        print("command. Do not bypass or delete the lock.")
        return 75
    if proc.returncode != 0:
        print("ERROR: device command failed (exit %d)" % proc.returncode,
              file=sys.stderr)
        if proc.stderr.strip():
            print(proc.stderr.strip(), file=sys.stderr)
        return 1

    def read_text(name):
        path = os.path.join(out, name)
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as fh:
                return fh.read()
        except OSError:
            return ""

    raw = read_text("trace.txt")
    lines = [ln for ln in raw.splitlines() if ln.strip()]
    samples = [s for s in (parse_trace_line(ln) for ln in lines) if s]

    done = None
    for ln in lines:
        m = RUN_DONE_RE.search(ln)
        if m:
            done = (int(m.group(1)), int(m.group(2)))
    timing = read_text("fast-timing.txt").split()
    host_ms = None
    if len(timing) == 3:
        try:
            host_ms = (int(timing[2]) - int(timing[1])) / 1e6
        except ValueError:
            host_ms = None

    shot = os.path.join(out, args.section + "-fast.png")
    shot_size = os.path.getsize(shot) if os.path.exists(shot) else 0
    luma = image_mean_luma(shot) if shot_size else None
    dump = read_text("fast-dump.txt").strip()

    print()
    if not samples:
        print("  (no trace lines -- did `cmd runticks`/`cmd trace` exist in the APK?)")
    else:
        print_trace(samples)
    print()
    print("trace lines=%d  screenshot=%s (%d bytes, mean luma=%s)"
          % (len(samples), os.path.basename(shot) if shot_size else "-",
             shot_size, "n/a" if luma is None else "%.0f" % luma))
    if dump:
        print("dump after fast-forward: %s" % dump)
    else:
        print("dump after fast-forward: (missing)")

    real_ms = args.ticks * 1000.0 / 30.0
    if done is not None:
        n, sim_ms = done
        speedup = real_ms / sim_ms if sim_ms > 0 else float("inf")
        print("timing: %d ticks sim_ms=%d (device)  real30_ms=%.0f  speedup=%.1fx"
              % (n, sim_ms, real_ms, speedup))
    else:
        print("timing: no `PTC runticks done` log line; real30_ms=%.0f"
              % real_ms)
    if host_ms is not None:
        print("timing: adb round trip (broadcast -> done log)=%.0f ms" % host_ms)

    agg = trace_summary(samples) if samples else {
        "flags": 0, "crossings": 0, "tele": 0, "star": 0, "star2": 0,
        "cmode": None, "actions": set()}
    results = checks(args.section, samples, agg, trace_mode=True)
    results.append(("cmd dump still answers", bool(dump), "yes" if dump else "missing"))
    results.append(("rendered frame after ff", shot_size > 0,
                    "%d bytes" % shot_size))
    if luma is not None:
        results.append(("screenshot not black", luma > 2.0, "mean=%.0f" % luma))

    failed = [r for r in results if not r[1]]
    print()
    for label, ok, observed in results:
        print("%s  %-34s observed: %s" % ("PASS" if ok else "FAIL", label, observed))
    print()
    if failed:
        print("RESULT: FAIL %s (%d/%d checks)"
              % (args.section, len(results) - len(failed), len(results)))
        return 1
    print("RESULT: PASS %s (%d/%d checks)"
          % (args.section, len(results), len(results)))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Run one NINJA CHAIN section on the tablet and check it.")
    ap.add_argument("--section", choices=SECTIONS, default="chain",
                    help="section to start (default: chain)")
    ap.add_argument("--seconds", type=float, default=30.0,
                    help="polling window in seconds (default: 30)")
    ap.add_argument("--install", action="store_true",
                    help="install app/build/outputs/apk/debug/app-debug.apk first")
    ap.add_argument("--out", default="/tmp/opencode/ninja-test",
                    help="output directory (default: /tmp/opencode/ninja-test)")
    ap.add_argument("--fast", action="store_true",
                    help="use the in-engine runticks fast-forward + trace")
    ap.add_argument("--ticks", type=int, default=300,
                    help="runticks tick count for --fast (default: 300, max 2400)")
    args = ap.parse_args(argv)

    if args.seconds < 1:
        ap.error("--seconds must be >= 1")
    if args.fast and not (1 <= args.ticks <= 2400):
        ap.error("--ticks must be 1..2400")
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)
    for name in ("samples.tsv", "status.txt", "trace.txt",
                 "fast-timing.txt", "fast-dump.txt"):
        try:
            os.remove(os.path.join(out, name))
        except OSError:
            pass

    if args.fast:
        return run_fast(args, out)

    reason = "ninja chain %s" % args.section
    script = inner_script(args.section, args.seconds, out, args.install)
    cmd = [LOCK, "device", "--reason", reason, "--", "bash", "-lc", script]

    print("NINJA CHAIN TEST  section=%s seconds=%g" % (args.section, args.seconds))
    print("out=%s" % out)
    print("device: scripts/agent-lock.sh device --reason %s -- bash -lc <harness>"
          % shlex.quote(reason))
    print("(unlock, restart app, level 38, action %s, poll dump every ~0.6 s)"
          % args.section)
    sys.stdout.flush()

    try:
        proc = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True)
    except OSError as exc:
        print("ERROR: cannot run %s: %s" % (LOCK, exc), file=sys.stderr)
        return 2

    if proc.returncode == 75:
        print("\nLOCK TIMEOUT (exit 75): the device lock is held by another")
        print("experiment on this host. Nothing was run and nothing was changed.")
        print("RETRY: wait for the other test to finish, then re-run the same")
        print("command. Do not bypass or delete the lock.")
        return 75
    if proc.returncode != 0:
        print("ERROR: device command failed (exit %d)" % proc.returncode,
              file=sys.stderr)
        if proc.stderr.strip():
            print(proc.stderr.strip(), file=sys.stderr)
        return 1

    samples = []
    log_path = os.path.join(out, "samples.tsv")
    try:
        with open(log_path, "r", encoding="utf-8", errors="replace") as fh:
            for raw in fh:
                raw = raw.rstrip("\n")
                if not raw:
                    continue
                parts = raw.split("\t", 2)
                if len(parts) != 3:
                    continue
                samples.append(parse_sample(parts[0], parts[1], parts[2]))
    except OSError as exc:
        print("ERROR: no samples file (%s)" % exc, file=sys.stderr)
        return 1

    shots = sorted(n for n in os.listdir(out)
                   if n.startswith(args.section + "-") and n.endswith(".png"))
    shots = [n for n in shots if os.path.getsize(os.path.join(out, n)) > 0]

    print()
    if not samples:
        print("  (no samples collected)")
    else:
        print_table(samples)
    print()
    print("samples=%d  screenshots=%d %s"
          % (len(samples), len(shots), " ".join(shots)))

    agg = summary(samples)
    results = checks(args.section, samples, agg)
    failed = [r for r in results if not r[1]]
    for label, ok, observed in results:
        print("%s  %-34s observed: %s" % ("PASS" if ok else "FAIL", label, observed))
    print()
    if failed:
        print("RESULT: FAIL %s (%d/%d checks)" %
              (args.section, len(results) - len(failed), len(results)))
        return 1
    print("RESULT: PASS %s (%d/%d checks)" % (args.section, len(results), len(results)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
