#!/usr/bin/env python3
"""Analyse a screenshot of the Portal Test Chamber relay.

The test camera is fixed in front of portal A, so the portal ellipse is near
the centre of the frame.  A working relay shows the room/checkerboard through
the portal; a missing relay shows the flat blue portal fan.

Usage:
  python3 scripts/portal-relay-test.py L.png [--pair R.png] [--json]
  python3 scripts/portal-relay-test.py L.png --roi 0.30,0.25,0.70,0.75

Exit status: 0 if structured texture is detected, 1 otherwise.
"""
import argparse
import json
import math
import sys

try:
    from PIL import Image, ImageChops, ImageStat
except Exception as exc:  # pragma: no cover
    print("Pillow is required: python3 -m pip install pillow", file=sys.stderr)
    raise SystemExit(2)


def load_rgb(path):
    return Image.open(path).convert("RGB")


def roi_bounds(w, h, roi):
    if roi is None:
        return int(w * 0.25), int(h * 0.20), int(w * 0.75), int(h * 0.80)
    if isinstance(roi, str):
        roi = [float(v) for v in roi.split(",")]
    return (int(w * roi[0]), int(h * roi[1]), int(w * roi[2]), int(h * roi[3]))


def analyse(path, roi=None, solid_green=False):
    im = load_rgb(path)
    w, h = im.size
    x0, y0, x1, y1 = roi_bounds(w, h, roi)
    crop = im.crop((x0, y0, x1, y1))
    # Keep the pure-Python scan bounded; the pattern is low-frequency.
    crop.thumbnail((512, 512))
    cw, ch = crop.size
    px = list(crop.getdata())
    n = len(px)
    mean = tuple(sum(p[i] for p in px) / n for i in range(3))
    gray = [0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2] for p in px]
    gmean = sum(gray) / n
    std = math.sqrt(sum((v - gmean) ** 2 for v in gray) / n)

    edge_x = edge_y = 0
    gx_sum = gy_sum = 0.0
    for y in range(ch):
        row = y * cw
        for x in range(cw - 1):
            d = abs(gray[row + x] - gray[row + x + 1])
            gx_sum += d
            if d > 12.0:
                edge_x += 1
    for y in range(ch - 1):
        row = y * cw
        for x in range(cw):
            d = abs(gray[row + x] - gray[row + cw + x])
            gy_sum += d
            if d > 12.0:
                edge_y += 1
    edge_density = 0.5 * (edge_x + edge_y) / max(1, cw * (ch - 1) + (cw - 1) * ch)
    ratio = gx_sum / max(gy_sum, 1e-3)

    if solid_green:
        green = sum(1 for p in px if p[1] > 150 and p[0] < 130 and p[2] < 130)
        blue = sum(1 for p in px if p[2] > 150 and p[0] < 120 and p[1] < 150)
        gf = green / max(1, n)
        bf = blue / max(1, n)
        ok = gf > 0.005
        return {
            "path": path,
            "pass": bool(ok),
            "roi": [x0, y0, x1, y1],
            "green_fraction": round(gf, 4),
            "blue_fraction": round(bf, 4),
            "verdict": ("green relay decal visible (geometry/UVs work)" if ok else
                        ("blue flat portal: decal missing" if bf > gf else
                         "no green decal: relay overlay/geometry missing")),
        }

    flat_blue = math.sqrt(sum((mean[i] - c) ** 2 for i, c in enumerate((48, 96, 255))))
    flat = edge_density < 0.004 and std < 6.0
    structured = (not flat) and edge_density > 0.012 and std > 8.0
    return {
        "path": path,
        "pass": bool(structured),
        "roi": [x0, y0, x1, y1],
        "mean_rgb": [round(float(v), 1) for v in mean],
        "std": round(std, 2),
        "edge_density": round(edge_density, 4),
        "edge_ratio_hv": round(ratio, 2),
        "flat_blue_distance": round(flat_blue, 1),
        "verdict": ("structured relay texture" if structured else
                    ("flat portal colour: relay texture missing" if flat else
                     "too little texture: relay texture missing or occluded")),
    }


def pair_diff(path_a, path_b):
    a, b = load_rgb(path_a), load_rgb(path_b)
    if a.size != b.size:
        return None
    d = ImageChops.difference(a, b).convert("L")
    d.thumbnail((256, 256))
    return round(ImageStat.Stat(d).mean[0], 3)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--pair", help="second eye screenshot for comparison")
    ap.add_argument("--roi", help="x0,y0,x1,y1 as fractions (default centre)")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--solid-green", action="store_true", help="stage-1 solid relay colour")
    args = ap.parse_args()

    result = analyse(args.image, args.roi, args.solid_green)
    if args.pair:
        result["pair_mean_abs_diff"] = pair_diff(args.image, args.pair)

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        for k, v in result.items():
            print(f"{k}: {v}")
    raise SystemExit(0 if result.get("pass") else 1)


if __name__ == "__main__":
    main()
