#!/usr/bin/env python3
"""Frame-by-frame portal-crossing seam analyser.

Reads the PTC status dumps captured by scripts/pseam-capture.sh (one line per
simulation tick) and reports, for every tick:

  * Mario's body centre projected to screen pixels (from cam pos/focus + FOV)
  * his apparent height in pixels (feet -> head)
  * the horizontal angle between his facing and the camera->Mario direction
    (0 = camera behind him, 180 = camera in front)
  * camera distance / action / animation frame / teleport counter

The crossing tick is the first tick whose `tele=` counter differs from the
previous line.  The seam metrics are the frame-to-frame screen jump across that
tick plus the apparent-height and facing-angle changes.

Usage:
  scripts/portal-seam-test.py --dump /tmp/opencode/pseam/dump-ww.txt
  scripts/portal-seam-test.py --dump ... --table
"""
import argparse
import math
import re
import sys

TOK = re.compile(r"([a-zA-Z0-9_]+)=(\"[^\"]*\"|\S+)")


def parse_dump(path):
    rows = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.strip()
            if "PTC level=" not in line:
                continue
            # strip the logcat timestamp prefix up to "PTC level="
            idx = line.find("PTC level=")
            body = line[idx:]
            toks = dict(TOK.findall(body))
            try:
                r = {
                    "level": int(toks["level"]),
                    "stage": toks.get("phase", "0"),
                    "tele": int(toks["tele"]),
                    "remap": int(toks.get("remap", "0")),
                    "action": toks["action"],
                    "anim": int(toks.get("anim", "-1")),
                    "mario": [float(v) for v in toks["mario"].split(",")],
                    "cam": [float(v) for v in toks["cam"].split(",")],
                    "focus": [float(v) for v in toks["focus"].split(",")],
                    "faceY": float(toks["face"].split(",")[1]),
                    "mh": float(toks.get("mh", "160")),
                    "cmode": int(toks.get("cmode", "0")),
                    "walk": toks.get("walk", ""),
                }
            except (KeyError, ValueError):
                continue
            rows.append(r)
    return rows


def vsub(a, b):
    return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]


def vdot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def vcross(a, b):
    return [a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]]


def vnorm(a):
    n = math.sqrt(vdot(a, a))
    return [a[0] / n, a[1] / n, a[2] / n] if n > 1e-9 else [0.0, 0.0, 0.0]


def project(cam, focus, p, tan_v, aspect, w, h):
    fwd = vnorm(vsub(focus, cam))
    up = [0.0, 1.0, 0.0]
    right = vnorm(vcross(fwd, up))
    up2 = vcross(right, fwd)
    d = vsub(p, cam)
    z = vdot(d, fwd)
    if z <= 1.0:
        return None
    x = vdot(d, right)
    y = vdot(d, up2)
    tan_h = tan_v * aspect
    ndc_x = (x / z) / tan_h
    ndc_y = (y / z) / tan_v
    return ((ndc_x * 0.5 + 0.5) * w, (0.5 - ndc_y * 0.5) * h, z)


def facing_vs_camera(cam, mario, faceY):
    """0 = facing away from the camera (seen from behind), 180 = facing it."""
    to_cam = vsub(cam, mario)
    # engine yaw: x = sin(yaw), z = cos(yaw)
    fx = math.sin(faceY * math.pi / 32768.0)
    fz = math.cos(faceY * math.pi / 32768.0)
    dot = (fx * to_cam[0] + fz * to_cam[2])
    n = math.sqrt(vdot(to_cam, to_cam)) or 1.0
    dot /= n
    dot = max(-1.0, min(1.0, dot))
    return math.degrees(math.acos(dot))


def analyse(rows, tan_v, aspect, w, h, table=False):
    if not rows:
        print("no dump rows")
        return 2
    print(f"frames={len(rows)} level={rows[0]['level']} cmode={rows[0]['cmode']} "
          f"fov_v={2*math.degrees(math.atan(tan_v)):.1f}deg aspect={aspect:.3f} "
          f"canvas={w}x{h}")

    per = []
    for r in rows:
        p = r["mario"]
        feet = project(r["cam"], r["focus"], [p[0], p[1], p[2]], tan_v, aspect, w, h)
        head = project(r["cam"], r["focus"],
                       [p[0], p[1] + r["mh"] - 6.0, p[2]], tan_v, aspect, w, h)
        ctr = project(r["cam"], r["focus"],
                      [p[0], p[1] + r["mh"] * 0.5, p[2]], tan_v, aspect, w, h)
        s = dict(r)
        s["ctr"] = ctr
        s["feet"] = feet
        s["head"] = head
        s["dist"] = math.sqrt(vdot(vsub(p, r["cam"]), vsub(p, r["cam"])))
        s["face_ang"] = facing_vs_camera(r["cam"], p, r["faceY"])
        if ctr and head and feet:
            s["pxh"] = math.hypot(head[0] - feet[0], head[1] - feet[1])
        else:
            s["pxh"] = None
        per.append(s)

    if table:
        print(f"{'f':>3} {'tele':>4} {'action':>10} {'anim':>5} "
              f"{'screen':>15} {'pxh':>6} {'dist':>6} {'face':>6} {'remap':>5}")
        for i, s in enumerate(per):
            if s["ctr"]:
                scr = f"{s['ctr'][0]:7.1f},{s['ctr'][1]:6.1f}"
            else:
                scr = "   offscreen"
            print(f"{i:3d} {s['tele']:4d} {s['action']:>10} {s['anim']:5d} "
                  f"{scr:>15} {(s['pxh'] or 0):6.1f} {s['dist']:6.1f} "
                  f"{s['face_ang']:6.1f} {s['remap']:5d}")

    # crossing frames
    cross = [i for i in range(1, len(per)) if per[i]["tele"] != per[i - 1]["tele"]]
    if not cross:
        print("RESULT: no teleport found in this dump")
        return 2

    failures = 0
    for ci in cross:
        a, b = per[ci - 1], per[ci]
        print(f"\n--- crossing at frame {ci} "
              f"(tele {a['tele']} -> {b['tele']}, src/dst {a['action']} -> {b['action']})")
        print(f"  mario  {a['mario']} -> {b['mario']}")
        print(f"  cam    {a['cam']} -> {b['cam']}")
        print(f"  focus  {a['focus']} -> {b['focus']}")
        print(f"  frame  tele={a['tele']}->{b['tele']} remap={a['remap']}->{b['remap']} "
              f"action {a['action']}->{b['action']} anim {a['anim']}->{b['anim']} "
              f"face {a['faceY']:.0f}->{b['faceY']:.0f}")
        if a["ctr"] and b["ctr"]:
            dx = b["ctr"][0] - a["ctr"][0]
            dy = b["ctr"][1] - a["ctr"][1]
            j = math.hypot(dx, dy)
            print(f"  SCREEN centre {a['ctr'][0]:.1f},{a['ctr'][1]:.1f} -> "
                  f"{b['ctr'][0]:.1f},{b['ctr'][1]:.1f}  jump={j:.2f}px "
                  f"(dx={dx:+.2f} dy={dy:+.2f})")
            ok = j <= 12.0
            print(f"  CHECK screen jump <= 12px: {'PASS' if ok else 'FAIL'} ({j:.2f})")
            failures += 0 if ok else 1
        else:
            print("  FAIL: Mario off-screen on one side of the crossing")
            failures += 1
        if a["pxh"] and b["pxh"]:
            ratio = b["pxh"] / a["pxh"]
            print(f"  apparent height {a['pxh']:.1f}px -> {b['pxh']:.1f}px "
                  f"ratio={ratio:.4f}")
            ok = 0.97 <= ratio <= 1.03
            print(f"  CHECK scale within 3%: {'PASS' if ok else 'FAIL'}")
            failures += 0 if ok else 1
        dfa = abs(((b["face_ang"] - a["face_ang"] + 180.0) % 360.0) - 180.0)
        print(f"  facing-vs-camera {a['face_ang']:.1f}deg -> {b['face_ang']:.1f}deg "
              f"(delta {dfa:.1f}deg)")
        ok = dfa <= 10.0
        print(f"  CHECK orientation delta <= 10deg: {'PASS' if ok else 'FAIL'}")
        failures += 0 if ok else 1
        print(f"  distance {a['dist']:.1f} -> {b['dist']:.1f}")

    # no teleport-sized jump right around the crossing (a hard landing later in
    # the fall is normal camera follow, so only the handoff window is checked)
    worst = (0.0, -1)
    for i in range(1, len(per)):
        if per[i]["ctr"] and per[i - 1]["ctr"]:
            j = math.hypot(per[i]["ctr"][0] - per[i - 1]["ctr"][0],
                           per[i]["ctr"][1] - per[i - 1]["ctr"][1])
            if j > worst[0]:
                worst = (j, i)
    print(f"\nworst frame-to-frame centre jump (whole clip): "
          f"{worst[0]:.2f}px at frame {worst[1]}")
    win = (0.0, -1)
    for ci in cross:
        for i in range(max(1, ci - 2), min(len(per), ci + 9)):
            if per[i]["ctr"] and per[i - 1]["ctr"]:
                j = math.hypot(per[i]["ctr"][0] - per[i - 1]["ctr"][0],
                               per[i]["ctr"][1] - per[i - 1]["ctr"][1])
                if j > win[0]:
                    win = (j, i)
    print(f"worst jump within +-2/+8 frames of the crossing: "
          f"{win[0]:.2f}px at frame {win[1]}")
    ok = win[0] <= 40.0
    print(f"CHECK no teleport-sized jump (>40px) at the handoff: "
          f"{'PASS' if ok else 'FAIL'}")
    failures += 0 if ok else 1

    print(f"\nRESULT: {'PASS' if failures == 0 else 'FAIL'} ({len(cross)} crossing(s), "
          f"{failures} failed check(s))")
    return 0 if failures == 0 else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", required=True)
    ap.add_argument("--fov", type=float, default=45.0, help="vertical FOV degrees")
    ap.add_argument("--aspect", type=float, default=16.0 / 9.0)
    ap.add_argument("--width", type=float, default=3200.0)
    ap.add_argument("--height", type=float, default=1800.0)
    ap.add_argument("--table", action="store_true")
    args = ap.parse_args()
    tan_v = math.tan(math.radians(args.fov) * 0.5)
    rows = parse_dump(args.dump)
    return analyse(rows, tan_v, args.aspect, args.width, args.height, args.table)


if __name__ == "__main__":
    sys.exit(main())
