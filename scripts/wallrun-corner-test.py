#!/usr/bin/env python3
"""90-degree corner wall-run smoke test (see TESTING.md).

Runs the PTC `cornerN` rig (ptc.c) eight times, one per jump timing, and
measures whether Mario can wall-run along wall A, jump at an inner 90-degree
corner and continue the run on the perpendicular wall B.

The rig is deterministic by construction: `ptc cornerN` freezes Mario on the
start pose in real time, and the run only advances inside `cmd runticks`, i.e.
inside the engine's own fast-forward loop (the same path the ninja chain uses).
A batch therefore produces the same ticks every time, and `cmd trace` returns
one `WRC ...` line per tick with position, action and the wall normal.  The
`cmd dump` `wrc=` field carries the rig's summary; this script cross-checks the
two and reports the timing window.

This script REFUSES to touch the tablet unless it is already running inside the
device lock (AGENT_DEVICE_LOCK_HELD=1).  It installs nothing.

Usage:
  AGENT_TABLET_MUTE=1 scripts/agent-lock.sh device --timeout 600 \\
      --reason "wallrun corner" -- scripts/wallrun-corner-test.py

  # retune the press-distance window without a rebuild (fx keys 410/411):
  ... -- scripts/wallrun-corner-test.py --perfect 1500 --step 80

Options: --ticks N (batch length, default 150), --out DIR, --skip-clip,
--dry-run.  Exit: 0 pass, 1 fail, 2 usage / not under the device lock.

Wall-run geometry (fixed in the chamber, no new geometry):
  wall A  room west wall   x = -2600, normal +X, run direction -Z
  wall B  room south wall  z = -3200, normal +Z
  corner  (-2600, -3200), inner corner
  start   (-2574, 900, -700), vel (-14, 0, -42), stick 0x8000 (-Z)

Timings (press distance before the corner): d = perfect + (3 - i) * step, so
index 0 is the earliest jump and index 7 the latest.  Defaults 1480 / 100:
  0 1780  1 1680  2 1580  3 1480  4 1380  5 1280  6 1180  7 1080

Outcomes (ptc.c WRC_OUT_*): 1 corner-pass, 2 fell, 3 stuck, 4 bonk.
"""
import argparse
import os
import re
import subprocess
import sys
import time
from datetime import datetime

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PKG = "com.vdavid003.sm64port"
ACTIVITY = PKG + "/.sm64portActivity"
BROADCAST = "am broadcast -n %s/.DebugReceiver -a %s.CMD" % (PKG, PKG)
UNLOCK = "/media/share/lp2-test/scripts/unlock.sh"

CASES = 8
PERFECT = 3
ACT_WALLRUN = 0x0300089A
ACT_FLAG_AIR = 1 << 11

OUTCOME = {0: "none", 1: "corner-pass", 2: "fell", 3: "stuck", 4: "bonk"}

# adb on this host is a prebuilt that needs its own lib directory.
ADB_ENV = dict(os.environ)
ADB_ENV["LD_LIBRARY_PATH"] = (("/home/ubuntu/.local/lib/android-tools:")
                              + os.environ.get("LD_LIBRARY_PATH", ""))

# wrc=<case>,<stage>,<outcome>,<re>,<done>,<adv>,<ymin>,<corner>,<djump>,<jy>,
#     <maxstall>,<bonk>
WRC_RE = re.compile(
    r"\bwrc=(\d+),(\d+),(\d+),(-?\d+),(\d+),(-?[\d.]+),(-?[\d.]+),(\d+),"
    r"(-?[\d.]+),(-?\d+),(\d+),(\d+)")
TRACE_RE = re.compile(
    r"WRC t=(-?\d+) ph=(\d+) a=0x([0-9a-fA-F]+) p=(-?\d+),(-?\d+),(-?\d+) "
    r"v=(-?\d+),(-?\d+) n=(-?\d+),(-?\d+) rel=(-?\d+)")
# wr=<rel_left>,<window>,<last_jump>,<runs_started>,<windows_armed>,<lvx>,<lvz>
WR_RE = re.compile(
    r"\bwr=(\d+),(\d+),(\d+),(\d+),(\d+),(-?[\d.]+),(-?[\d.]+)")
FPS_RE = re.compile(r"\bfps=([\d.]+)")
ACTION_RE = re.compile(r"\baction=(0x[0-9a-fA-F]+)")
MARIO_RE = re.compile(r"\bmario=(-?[\d.]+),(-?[\d.]+),(-?[\d.]+)")
LEVEL_RE = re.compile(r"\blevel=(-?\d+)")
LVL_RE = re.compile(r"\blvl=(-?\d+)")


class Fail(Exception):
    pass


# ------------------------------------------------------------------ adb glue

def adb(args, timeout=40):
    return subprocess.run(["adb"] + args, capture_output=True, timeout=timeout,
                          text=True, env=ADB_ENV)


def adb_shell(cmd, timeout=40):
    return adb(["shell", cmd], timeout=timeout)


def broadcast(cmd, extra=None, timeout=40):
    line = "%s --es cmd %s" % (BROADCAST, cmd)
    if extra:
        line += " " + extra
    return adb_shell(line, timeout=timeout)


def logcat_clear():
    adb(["logcat", "-c"], timeout=20)


def logcat_text():
    cp = adb(["logcat", "-d", "-s", "SM64Leia:I"], timeout=40)
    return cp.stdout or ""


def dump_line(timeout=4.0):
    """One `cmd dump` round trip; returns the PTC status line or ''."""
    logcat_clear()
    broadcast("dump")
    deadline = time.time() + timeout
    while time.time() < deadline:
        time.sleep(0.25)
        for raw in logcat_text().splitlines():
            if "PTC level=" in raw:
                return raw
    return ""


def wait_level(level, seconds):
    deadline = time.time() + seconds
    line = ""
    while time.time() < deadline:
        line = dump_line()
        if line:
            m = LEVEL_RE.search(line)
            lvl = LVL_RE.search(line)
            if (m and int(m.group(1)) == level
                    and (not lvl or int(lvl.group(1)) >= 1)):
                return line
        time.sleep(0.5)
    raise Fail("level %d did not become ready (%s)"
               % (level, line or "no PTC status line"))


def wait_for_log(marker, seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        if marker in logcat_text():
            return True
        time.sleep(0.1)
    return False


def screencap(path):
    with open(path, "wb") as fh:
        cp = subprocess.run(["adb", "exec-out", "screencap", "-p"],
                            stdout=fh, stderr=subprocess.DEVNULL, timeout=60,
                            env=ADB_ENV)
    return cp.returncode == 0 and os.path.getsize(path) > 0


# ------------------------------------------------------------------- parsing

def parse_dump(line):
    out = {"raw": line}
    m = WRC_RE.search(line)
    if m:
        out.update({
            "case": int(m.group(1)), "stage": int(m.group(2)),
            "outcome": int(m.group(3)), "re": int(m.group(4)),
            "done": int(m.group(5)), "adv": float(m.group(6)),
            "ymin": float(m.group(7)), "corner": int(m.group(8)),
            "djump": float(m.group(9)), "jy": int(m.group(10)),
            "maxstall": int(m.group(11)), "bonk": int(m.group(12)),
        })
    m = WR_RE.search(line)
    if m:
        out.update({
            "rel_left": int(m.group(1)), "rel_win": int(m.group(2)),
            "last_jump": int(m.group(3)), "runs": int(m.group(4)),
            "arms": int(m.group(5)),
        })
    m = FPS_RE.search(line)
    if m:
        out["fps"] = float(m.group(1))
    m = ACTION_RE.search(line)
    if m:
        out["action"] = int(m.group(1), 16)
    m = MARIO_RE.search(line)
    if m:
        out["mario"] = tuple(float(m.group(i)) for i in (1, 2, 3))
    return out


def parse_trace(text):
    rows = []
    for raw in text.splitlines():
        m = TRACE_RE.search(raw)
        if not m:
            continue
        rows.append({
            "t": int(m.group(1)), "stage": int(m.group(2)),
            "action": int(m.group(3), 16),
            "pos": (int(m.group(4)), int(m.group(5)), int(m.group(6))),
            "vel": (int(m.group(7)), int(m.group(8))),
            "n": (int(m.group(9)), int(m.group(10))),
            "rel": int(m.group(11)),
        })
    return rows


def trace_checks(rows):
    """Split into (always-required, pass-only) checks + derived values."""
    derived = {"re": -1, "floor_before_b": 0, "maxstep": 0.0, "stall": 0,
               "run_speed": 0.0, "jump_t": None, "re_t": None,
               "min_y": 0}
    glob = []
    entry = []

    if not rows:
        return [("trace lines present", False, "none")], [], derived

    # Wall A run before the press: ACT_WALLRUN with normal +X (100).
    a_rows = [r for r in rows
              if r["action"] == ACT_WALLRUN and r["stage"] <= 1
              and r["n"][0] >= 70]
    glob.append(("wall-run started on wall A (normal +X)", bool(a_rows),
                 "trace lines=%d" % len(a_rows)))

    jump_rows = [r for r in rows if r["stage"] == 2]
    if jump_rows:
        derived["jump_t"] = jump_rows[0]["t"]
    glob.append(("jump pressed (rig reached the jump stage)",
                 bool(jump_rows) or any(r["stage"] >= 3 for r in rows),
                 "t=%s" % derived["jump_t"]))

    if a_rows:
        speed = (a_rows[-1]["vel"][0] ** 2 + a_rows[-1]["vel"][1] ** 2) ** 0.5
        derived["run_speed"] = speed
        glob.append(("entry run speed 40-60", 40.0 <= speed <= 60.0,
                     "|v|=%.1f" % speed))

    # Floor contact after the jump.
    floor_rows = [r for r in rows
                  if r["rel"] >= 0 and not (r["action"] & ACT_FLAG_AIR)]

    # Wall B run: ACT_WALLRUN with normal ~+Z (0,100).
    b_rows = [r for r in rows
              if r["action"] == ACT_WALLRUN and abs(r["n"][0]) < 70]
    if b_rows:
        derived["re"] = b_rows[0]["rel"]
        derived["re_t"] = b_rows[0]["t"]
        derived["floor_before_b"] = len(
            [r for r in floor_rows if r["t"] <= b_rows[0]["t"]])
    entry.append(("re-entered ACT_WALLRUN on wall B (normal +Z)", bool(b_rows),
                  "rel=%s" % (derived["re"] if b_rows else "never")))
    entry.append(("no floor contact before that re-entry",
                  bool(b_rows) and derived["floor_before_b"] == 0,
                  "floor ticks before=%d" % derived["floor_before_b"]))

    # Continuity between the jump and the re-entry: no teleport, no stall.
    tracked = [r for r in rows if r["rel"] >= 0]
    if b_rows and b_rows[0]["t"] >= 0:
        tracked = [r for r in tracked if r["t"] <= b_rows[0]["t"]]
    maxstep = 0.0
    stall = worst = 0
    for prev, cur in zip(tracked, tracked[1:]):
        dx = cur["pos"][0] - prev["pos"][0]
        dy = cur["pos"][1] - prev["pos"][1]
        dz = cur["pos"][2] - prev["pos"][2]
        step = (dx * dx + dy * dy + dz * dz) ** 0.5
        maxstep = max(maxstep, step)
        if step < 3.0:
            stall += 1
            worst = max(worst, stall)
        else:
            stall = 0
    derived["maxstep"] = maxstep
    derived["stall"] = worst
    glob.append(("no teleport across the corner (max step < 130u)",
                 maxstep < 130.0, "max=%.1f" % maxstep))
    # A pinned run at the corner is a legitimate measurement (outcome
    # `stuck`), so the stall threshold only applies to the nominal pass.
    entry.append(("no stall of >= 6 ticks below 3u/tick", worst < 6,
                  "worst=%d" % worst))
    if rows:
        derived["min_y"] = min(r["pos"][1] for r in rows)
        glob.append(("never dropped through the floor (y > -2)",
                     derived["min_y"] > -2, "min y=%d" % derived["min_y"]))
    return glob, entry, derived


# -------------------------------------------------------------------- runner

def run_case(cs, ticks, out, tag, do_still=True):
    """One deterministic batch: arm the rig, fast-forward, read trace + dump."""
    logcat_clear()
    broadcast("ptc", "--es action corner%d" % cs)
    time.sleep(0.4)          # the rig holds the start pose until the batch
    broadcast("runticks", "--ei n %d" % ticks)
    if not wait_for_log("PTC runticks done", 40.0):
        raise Fail("case %d: runticks batch did not finish" % cs)
    trace_text = ""
    logcat_clear()
    broadcast("trace")
    wait_for_log("PTC trace end", 20.0)
    trace_text = logcat_text()
    with open(os.path.join(out, "trace-%s.txt" % tag), "w",
              encoding="utf-8") as fh:
        fh.write(trace_text)
    dump = dump_line()
    if not dump:
        raise Fail("case %d: no PTC status line after the batch" % cs)
    with open(os.path.join(out, "dump-%s.txt" % tag), "w",
              encoding="utf-8") as fh:
        fh.write(dump + "\n")
    if do_still:
        screencap(os.path.join(out, "%s.png" % tag))
    return parse_dump(dump), parse_trace(trace_text), trace_text


def scan_fatal(text, hits):
    for raw in text.splitlines():
        if "FATAL EXCEPTION" in raw or "ANR in " in raw:
            hits.append(raw.strip())


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--perfect", type=int, default=None,
                    help="nominal press distance (fx 410); default 1480")
    ap.add_argument("--step", type=int, default=None,
                    help="spacing of the eight timings (fx 411); default 100")
    ap.add_argument("--ticks", type=int, default=150,
                    help="runticks batch length per case (default 150)")
    ap.add_argument("--out", default=None,
                    help="artifact directory (default "
                         "/media/share/tmp/artifacts/sm64/wallrun-corner/<stamp>)")
    ap.add_argument("--skip-clip", action="store_true",
                    help="do not screenrecord the passing case")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the plan without touching the tablet")
    args = ap.parse_args(argv)

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    out = os.path.abspath(
        args.out or ("/media/share/tmp/artifacts/sm64/wallrun-corner/%s"
                     % stamp))

    perfect = args.perfect if args.perfect is not None else 1480
    step = args.step if args.step is not None else 100
    dists = [perfect + (PERFECT - i) * step for i in range(CASES)]

    print("WALL-RUN 90-DEGREE CORNER TEST")
    print("  geometry : wall A x=-2600 (normal +X, run -Z), wall B z=-3200 "
          "(normal +Z)")
    print("             inner corner (-2600,-3200), long walls, no new "
          "geometry")
    print("  start    : (-2574, 900, -700) vel (-14,0,-42) stick -Z (0x8000)")
    print("  timings  : %s" % " ".join("%d:%d" % (i, d)
                                       for i, d in enumerate(dists)))
    print("  method   : %d-tick runticks batch per timing, 1 still each, "
          "trace cross-check" % args.ticks)
    print("  artifacts: %s" % out)
    if args.dry_run:
        print("  (dry run: the tablet was not touched)")
        return 0

    if os.environ.get("AGENT_DEVICE_LOCK_HELD") != "1":
        print("\nREFUSING: this harness must run inside the device lock.",
              file=sys.stderr)
        print("  AGENT_TABLET_MUTE=1 scripts/agent-lock.sh device --timeout 600 \\",
              file=sys.stderr)
        print("      --reason \"wallrun corner\" -- "
              "scripts/wallrun-corner-test.py", file=sys.stderr)
        print("It never takes the lock itself: an unlocked run would collide "
              "with another agent on the shared tablet.", file=sys.stderr)
        return 2

    os.makedirs(out, exist_ok=True)
    t_start = time.time()
    old_timeout = ""
    fatal = []

    try:
        dev = adb(["devices"], timeout=20)
        if dev.returncode != 0 or "\tdevice" not in (dev.stdout or ""):
            print("\nERROR: no adb device. The tablet sleeps and drops Wi-Fi adb;",
                  file=sys.stderr)
            print("wake it (or ask a human) and re-run. adb said: %s"
                  % ((dev.stdout or dev.stderr or "").strip()[:200],),
                  file=sys.stderr)
            return 1
        if os.path.exists(UNLOCK):
            subprocess.run([UNLOCK], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=30)
        cp = adb_shell("settings get system screen_off_timeout")
        if cp.returncode == 0:
            old_timeout = (cp.stdout or "").strip()
        adb_shell("settings put system screen_off_timeout 600000")

        adb_shell("am force-stop %s" % PKG)
        adb_shell("am start -n %s" % ACTIVITY)
        time.sleep(9)
        broadcast("level", "--ei n 38")
        wait_level(38, 45)

        if args.perfect is not None:
            broadcast("fx", "--ei k 410 --ei v %d" % args.perfect)
        if args.step is not None:
            broadcast("fx", "--ei k 411 --ei v %d" % args.step)
        if args.perfect is not None or args.step is not None:
            time.sleep(0.4)

        results = []
        for cs in range(CASES):
            dump, rows, trace_text = run_case(cs, args.ticks, out,
                                              "%02d_%d" % (cs, dists[cs]))
            scan_fatal(trace_text, fatal)
            checks, entry, derived = trace_checks(rows)
            results.append({"case": cs, "dist": dists[cs], "dump": dump,
                            "rows": rows, "checks": checks, "entry": entry,
                            "derived": derived})
            print("  case %d d=%-5d outcome=%-11s re=%-3d adv=%-4.0f "
                  "fps=%s ticks=%d"
                  % (cs, dists[cs],
                     OUTCOME.get(dump.get("outcome", 0), "?"),
                     dump.get("re", -1), dump.get("adv", 0.0),
                     dump.get("fps", "-"), len(rows)))
            sys.stdout.flush()

        # Determinism: the nominal case twice must produce the same summary.
        nominal = results[PERFECT]
        repeat_dump, repeat_rows, _ = run_case(PERFECT, args.ticks, out,
                                               "repeat_%d" % dists[PERFECT],
                                               do_still=False)
        same = all(repeat_dump.get(k) == nominal["dump"].get(k)
                   for k in ("re", "adv", "djump", "ymin", "outcome"))
        print("  repeat  case %d: %s (re %s/%s adv %s/%s)"
              % (PERFECT, "identical" if same else "SHIFTED",
                 nominal["dump"].get("re"), repeat_dump.get("re"),
                 nominal["dump"].get("adv"), repeat_dump.get("adv")))

        # Visual evidence follows the case that actually passed: the nominal
        # timing normally, or the first passing timing if the window shifted.
        pass_cases = [r for r in results if r["dump"].get("outcome") == 1]
        focus = (PERFECT if nominal["dump"].get("outcome") == 1 else
                 (pass_cases[0]["case"] if pass_cases else PERFECT))
        focus_row = results[focus]

        # Mid-corner still at that case's measured re-entry tick.
        mid_still = ""
        re_t = focus_row["derived"].get("re_t")
        if re_t is not None and re_t > 0:
            logcat_clear()
            broadcast("ptc", "--es action corner%d" % focus)
            time.sleep(0.4)
            broadcast("runticks", "--ei n %d" % (re_t + 3))
            if wait_for_log("PTC runticks done", 40.0):
                still = os.path.join(out, "corner-pass-mid.png")
                if screencap(still):
                    mid_still = still

        # Real-time clip of the passing case (the only non-deterministic run;
        # the table above never depends on it).
        clip = ""
        if not args.skip_clip:
            remote = "/sdcard/wallrun-corner.mp4"
            adb_shell("rm -f %s" % remote)
            rec = subprocess.Popen(
                ["adb", "shell",
                 "screenrecord --size 1280x720 --bit-rate 8000000 "
                 "--time-limit 8 %s" % remote],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                env=ADB_ENV)
            time.sleep(1.2)
            logcat_clear()
            broadcast("ptc", "--es action cornerlive%d" % focus)
            time.sleep(6.5)
            rec.wait(timeout=25)
            clip = os.path.join(out, "corner-pass.mp4")
            subprocess.run(["adb", "pull", remote, clip],
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=60, env=ADB_ENV)
            adb_shell("rm -f %s" % remote)
            scan_fatal(logcat_text(), fatal)

        # Leave the camera/FOV as they were; the stills are already saved.
        broadcast("ptc", "--es action corneroff")
        time.sleep(0.3)
        health_line = dump_line()
    except Fail as exc:
        print("\nERROR: %s" % exc, file=sys.stderr)
        return 1
    except subprocess.TimeoutExpired as exc:
        print("\nERROR: adb timeout: %s" % exc, file=sys.stderr)
        return 1
    finally:
        if old_timeout:
            try:
                adb_shell("settings put system screen_off_timeout %s"
                          % old_timeout, timeout=15)
            except Exception:
                pass

    # ------------------------------------------------------------- reporting
    print()
    print("  %-3s %-5s %-11s %-4s %-5s %-6s %-7s %-6s %-4s %s"
          % ("#", "dist", "outcome", "re", "adv", "ymin", "corner", "stall",
             "bonk", "fps"))
    for r in results:
        d = r["dump"]
        print("  %-3d %-5d %-11s %-4d %-5.0f %-6.0f %-7d %-6d %-4d %s"
              % (r["case"], r["dist"],
                 OUTCOME.get(d.get("outcome", 0), "?"),
                 d.get("re", -1), d.get("adv", 0.0), d.get("ymin", 0.0),
                 d.get("corner", 0), d.get("maxstall", 0),
                 d.get("bonk", 0), d.get("fps", "-")))

    checks = []
    for r in results:
        d = r["dump"]
        for label, ok, obs in r["checks"]:
            checks.append(("case %d d=%d: %s" % (r["case"], r["dist"], label),
                           ok, obs))
        checks.append(("case %d d=%d: rig finished and reported this case"
                       % (r["case"], r["dist"]),
                       d.get("done") == 1 and d.get("case") == r["case"],
                       "done=%s case=%s" % (d.get("done"), d.get("case"))))
        checks.append(("case %d d=%d: dump summary agrees with the trace"
                       % (r["case"], r["dist"]),
                       d.get("re", -2) == r["derived"]["re"],
                       "dump re=%s trace re=%s"
                       % (d.get("re"), r["derived"]["re"])))
        checks.append(("case %d d=%d: wr= shows the jump-off from ACT_WALLRUN"
                       % (r["case"], r["dist"]),
                       d.get("last_jump") == 1 and d.get("runs", 0) >= 1,
                       "wr last_jump=%s runs=%s"
                       % (d.get("last_jump"), d.get("runs"))))
        checks.append(("case %d d=%d: dump fps sane (engine still stepping)"
                       % (r["case"], r["dist"]),
                       5.0 <= d.get("fps", 0.0) <= 240.0,
                       "fps=%s" % d.get("fps")))

    # Pass-only criteria for the nominal (designed "perfect") timing.
    for label, ok, obs in nominal["entry"]:
        checks.append(("nominal d=%d: %s" % (dists[PERFECT], label), ok, obs))
    nominal_ok = (nominal["dump"].get("outcome") == 1
                  and nominal["derived"]["re"] >= 0)
    checks.append(("nominal perfect timing (index %d, d=%d) corner-passes"
                   % (PERFECT, dists[PERFECT]), nominal_ok,
                   OUTCOME.get(nominal["dump"].get("outcome", 0), "?")))
    checks.append(("nominal: wr= run counter shows a second run started",
                   nominal["dump"].get("runs", 0) >= 2,
                   "wr runs=%s" % nominal["dump"].get("runs")))
    checks.append(("no FATAL/ANR in the run", not fatal,
                   "%d line(s)%s"
                   % (len(fatal),
                      (": " + fatal[0][:110]) if fatal else "")))
    hd = parse_dump(health_line) if health_line else {}
    checks.append(("app still responsive afterwards (sane fps)",
                   bool(health_line) and 5.0 <= hd.get("fps", 0.0) <= 240.0,
                   "fps=%s" % hd.get("fps", "no dump")))
    checks.append(("repeat run identical (deterministic rig)", same,
                   "case %d" % PERFECT))

    passed = [r for r in results if r["dump"].get("outcome") == 1]
    print()
    if passed:
        lo = min(r["dist"] for r in passed)
        hi = max(r["dist"] for r in passed)
        res = [r["derived"]["re"] for r in passed
               if r["derived"]["re"] >= 0]
        idx = [r["case"] for r in passed]
        print("  window  : index %s, press distance %d..%d units, re-entry "
              "%s ticks after the jump"
              % (",".join(str(i) for i in idx), lo, hi,
                 ("%d..%d" % (min(res), max(res))) if res else "-"))
        speed = nominal["derived"].get("run_speed", 0.0)
        if speed > 1.0:
            span = (hi - lo) / speed
            print("            %.1f units at |v|=%.1f = %.1f ticks "
                  "(%.0f ms at 30 Hz) of press timing"
                  % (hi - lo, speed, span, span * 1000.0 / 30.0))
    else:
        print("  window  : none - no timing produced a corner pass")
        print("            retune with --perfect/--step (fx 410/411), then "
              "update PTC_WRC_PERFECT_DEF")

    print()
    failed = 0
    for label, ok, obs in checks:
        if not ok:
            failed += 1
        print("%s  %-62s observed: %s"
              % ("PASS" if ok else "FAIL", label, obs))
    print()
    print("stills: %s" % " ".join(sorted(n for n in os.listdir(out)
                                         if n.endswith(".png"))))
    if mid_still:
        print("mid-corner still: %s" % mid_still)
    if clip and os.path.exists(clip) and os.path.getsize(clip) > 0:
        print("clip: %s (%d bytes)" % (clip, os.path.getsize(clip)))
    print("wall time: %.0f s" % (time.time() - t_start))
    print("artifacts: %s" % out)
    print("notes: a 'fell'/'bonk'/'stuck' row is a measurement, not a crash; "
          "only the nominal row and the global checks decide PASS/FAIL")

    if failed:
        print("RESULT: FAIL (%d/%d checks)" % (len(checks) - failed,
                                               len(checks)))
        return 1
    print("RESULT: PASS (%d/%d checks)" % (len(checks), len(checks)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
