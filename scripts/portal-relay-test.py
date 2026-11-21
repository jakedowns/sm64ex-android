#!/usr/bin/env python3
"""Analyse a screenshot of the Portal Test Chamber relay.

The test camera is fixed in front of portal A, so the portal ellipse is near
the centre of the frame.  A working relay shows the room/checkerboard through
the portal; a missing relay shows the flat blue portal fan.

Usage:
  python3 scripts/portal-relay-test.py L.png --pattern --pair R.png [--json]
  python3 scripts/portal-relay-test.py L.png --roi 0.36,0.30,0.64,0.72

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


def analyse(path, roi=None, solid_green=False, pattern_mode=False):
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

    if pattern_mode:
        # Find the densest cluster of neutral high-contrast pixels: the relay
        # checker is black/white while HUD sprites and the room are saturated
        # colours, so this isolates the portal decal without a hand-tuned ROI.
        mask = set(i for i, g in enumerate(gray)
                   if (g > 200 or g < 55)
                   and (max(px[i]) - min(px[i])) < 28)
        sx = (x1 - x0) / max(1, cw)
        sy = (y1 - y0) / max(1, ch)
        BW = 16
        bw_n = (cw + BW - 1) // BW
        bh_n = (ch + BW - 1) // BW
        counts = [[0] * bw_n for _ in range(bh_n)]
        for i in mask:
            counts[(i // cw) // BW][(i % cw) // BW] += 1
        best = (0, 0, 0)
        for by in range(bh_n):
            for bx in range(bw_n):
                c = counts[by][bx]
                if c > best[0]:
                    best = (c, bx, by)
        if best[0] > 0:
            thresh = max(4, best[0] // 4)
            keep = set()
            stack = [(best[1], best[2])]
            seen = set(stack)
            while stack:
                bx, by = stack.pop()
                if counts[by][bx] < thresh:
                    continue
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        nx, ny = bx + dx, by + dy
                        if 0 <= nx < bw_n and 0 <= ny < bh_n and (nx, ny) not in seen:
                            seen.add((nx, ny))
                            stack.append((nx, ny))
            keep = seen
            pts = [i for i in mask
                   if (i // cw) // BW < bh_n and (i % cw) // BW < bw_n
                   and ((i // cw) // BW, (i % cw) // BW) in keep]
        else:
            pts = []
        if pts:
            xs = [i % cw for i in pts]
            ys = [i // cw for i in pts]
            bx0, bx1 = min(xs), max(xs)
            by0, by1 = min(ys), max(ys)
            w_px = (bx1 - bx0 + 1) * sx
            h_px = (by1 - by0 + 1) * sy
            aspect = h_px / max(1.0, w_px)
            cx = x0 + (bx0 + bx1 + 1) * 0.5 * sx
            cy = y0 + (by0 + by1 + 1) * 0.5 * sy
            fill = len(pts) / max(1.0, (bx1 - bx0 + 1) * (by1 - by0 + 1))
        else:
            w_px = h_px = aspect = cx = cy = fill = 0.0
        return {
            "path": path,
            "pass": bool(w_px > 20 and h_px > 20 and aspect > 1.15 and fill > 0.20),
            "roi": [x0, y0, x1, y1],
            "pattern_bbox": [int(w_px), int(h_px)],
            "pattern_aspect": round(aspect, 2),
            "pattern_center": [int(cx), int(cy)],
            "pattern_fill": round(fill, 2),
            "edge_density": round(edge_density, 4),
            "verdict": ("relay pattern visible (tall oval)" if w_px > 20 and h_px > 20 and aspect > 1.15 else
                        ("relay pattern squeezed/circular" if w_px > 20 and h_px > 20 else
                         "no relay pattern: portal surface is flat")),
        }

    if solid_green:
        green_pts = [i for i, p in enumerate(px) if p[1] > 150 and p[0] < 130 and p[2] < 130]
        blue = sum(1 for p in px if p[2] > 150 and p[0] < 120 and p[1] < 150)
        gf = len(green_pts) / max(1, n)
        bf = blue / max(1, n)
        ok = gf > 0.005
        # The green region is the decal itself, so its box gives the on-screen
        # aspect directly (the portal ellipse is 1.5-1.8x taller than wide).
        sx = (x1 - x0) / max(1, cw)
        sy = (y1 - y0) / max(1, ch)
        if green_pts:
            gxs = [i % cw for i in green_pts]
            gys = [i // cw for i in green_pts]
            bw = (max(gxs) - min(gxs) + 1) * sx
            bh = (max(gys) - min(gys) + 1) * sy
            aspect = bh / max(1.0, bw)
            cx = x0 + (min(gxs) + max(gxs) + 1) * 0.5 * sx
            cy = y0 + (min(gys) + max(gys) + 1) * 0.5 * sy
        else:
            bw = bh = aspect = cx = cy = 0.0
        return {
            "path": path,
            "pass": bool(ok),
            "roi": [x0, y0, x1, y1],
            "green_fraction": round(gf, 4),
            "blue_fraction": round(bf, 4),
            "decal_bbox": [int(bw), int(bh)],
            "decal_aspect": round(aspect, 2),
            "decal_center": [int(cx), int(cy)],
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
    ap.add_argument("--solid-green", action="store_true", help="old solid relay colour")
    ap.add_argument("--pattern", action="store_true", help="checker/parallax pattern mode")
    args = ap.parse_args()

    result = analyse(args.image, args.roi, args.solid_green, args.pattern)
    if args.pair:
        result["pair_mean_abs_diff"] = pair_diff(args.image, args.pair)
        if args.pattern:
            def column_profile(path):
                im = load_rgb(path)
                w, h = im.size
                x0, y0, x1, y1 = roi_bounds(w, h, args.roi)
                c = im.crop((x0, y0, x1, y1)).convert("L")
                c.thumbnail((320, 320))
                cw, ch = c.size
                px = list(c.getdata())
                return [sum(px[y * cw + x] for y in range(ch)) / max(1, ch)
                        for x in range(cw)]
            a = column_profile(args.image)
            b = column_profile(args.pair)
            best_shift, best_score = 0, 1e30
            for sh in range(-40, 41):
                tot = cnt = 0
                for x in range(10, len(a) - 10):
                    bx = x + sh
                    if 0 <= bx < len(b):
                        tot += abs(a[x] - b[bx])
                        cnt += 1
                if cnt and tot / cnt < best_score:
                    best_score = tot / cnt
                    best_shift = sh
            result["lr_shift_px"] = best_shift
            # Direct parallax measure: how far the pattern's box centre moved
            # between the two eyes (the profile shift above is a cross-check).
            other = analyse(args.pair, args.roi, False, True)
            if other.get("pattern_center") and result.get("pattern_center"):
                result["lr_dx_px"] = other["pattern_center"][0] - result["pattern_center"][0]
                result["lr_dy_px"] = other["pattern_center"][1] - result["pattern_center"][1]
                result["lr_parallax"] = abs(result["lr_dx_px"]) >= 3
            else:
                result["lr_parallax"] = abs(best_shift) >= 3

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        for k, v in result.items():
            print(f"{k}: {v}")
    raise SystemExit(0 if result.get("pass") else 1)


if __name__ == "__main__":
    main()
