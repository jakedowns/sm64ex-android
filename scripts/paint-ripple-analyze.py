#!/usr/bin/env python3
"""Frame-by-frame analysis of a painting-ripple recording.

    scripts/paint-ripple-analyze.py CLIP.mp4 --out DIR [--start N] [--strip 8]
                                             [--crop WxH]

Extracts every decoded frame with ffmpeg, then reports the mean absolute
difference between consecutive frames over the centre crop (the painting fills
it). A ripple that only advances on 30 Hz sim ticks gives an alternating
series: ~0 on every second frame at 60 Hz, a jump on the others. A ripple that
advances on every presented frame gives a roughly constant series.

Writes DIR/frames/*.png, DIR/diffs.tsv and DIR/strip.png (consecutive frames,
cropped and labelled), and prints a one-line summary suitable for a report.
"""
import argparse
import os
import shutil
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

FFMPEG_CANDIDATES = [
    os.environ.get("FFMPEG", ""),
    "/media/share/tmp/tools/ffmpeg/ffmpeg",
    "/usr/bin/ffmpeg",
    shutil.which("ffmpeg") or "",
]


def ffmpeg_path():
    for p in FFMPEG_CANDIDATES:
        if p and os.path.exists(p):
            return p
    return None


def font(size):
    for name in ("DejaVuSans.ttf", "DejaVuSansMono.ttf"):
        for base in ("/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/"):
            path = os.path.join(base, name)
            if os.path.exists(path):
                return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def extract(clip, out):
    frames = os.path.join(out, "frames")
    os.makedirs(frames, exist_ok=True)
    for f in os.listdir(frames):
        if f.endswith(".png"):
            os.remove(os.path.join(frames, f))
    ff = ffmpeg_path()
    if ff is None:
        sys.exit("ffmpeg not found (set FFMPEG=/path/to/ffmpeg)")
    subprocess.run(
        [ff, "-v", "error", "-y", "-i", clip, "-vsync", "0",
         os.path.join(frames, "%04d.png")],
        check=True,
    )
    return sorted(os.path.join(frames, f) for f in os.listdir(frames) if f.endswith(".png"))


def centre_crop(im, crop):
    w, h = im.size
    if crop:
        cw, ch = crop
    else:
        cw, ch = int(w * 0.5), int(h * 0.5)
    cw = min(cw, w)
    ch = min(ch, h)
    x = (w - cw) // 2
    y = (h - ch) // 2
    return im.crop((x, y, x + cw, y + ch)), (x, y, cw, ch)


def mad(a, b):
    """Mean absolute difference of two equally sized RGB images (0..255)."""
    pa, pb = a.load(), b.load()
    w, h = a.size
    total = 0
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            ca, cb = pa[x, y], pb[x, y]
            total += abs(ca[0] - cb[0]) + abs(ca[1] - cb[1]) + abs(ca[2] - cb[2])
    n = (w // 2 + w % 2) * (h // 2 + h % 2) * 3
    return total / float(max(n, 1))


def build_strip(paths, out, start, count, box, scale=1.0, caption_h=20):
    tiles = []
    for p in paths[start:start + count]:
        im = Image.open(p).convert("RGB")
        im = im.crop((box[0], box[1], box[0] + box[2], box[1] + box[3]))
        if scale != 1.0:
            im = im.resize((int(im.width * scale), int(im.height * scale)), Image.LANCZOS)
        tiles.append((os.path.basename(p), im))
    if not tiles:
        return None
    f = font(caption_h - 6)
    tw = sum(t.width for _, t in tiles) + 6 * (len(tiles) + 1)
    th = max(t.height for _, t in tiles) + caption_h + 8
    sheet = Image.new("RGB", (tw, th), (24, 24, 24))
    d = ImageDraw.Draw(sheet)
    x = 6
    for name, im in tiles:
        sheet.paste(im, (x, caption_h))
        d.text((x + 2, 3), name.replace(".png", ""), font=f, fill=(230, 230, 230))
        x += im.width + 6
    sheet.save(out)
    return out


def ffprobe_fps(clip):
    ff = ffmpeg_path()
    if ff is None:
        return 0.0
    probe = os.path.join(os.path.dirname(ff), "ffprobe")
    if not os.path.exists(probe):
        return 0.0
    try:
        out = subprocess.run(
            [probe, "-v", "error", "-select_streams", "v:0", "-show_entries",
             "stream=avg_frame_rate", "-of", "csv=p=0", clip],
            check=True, capture_output=True, text=True).stdout.strip()
        num, den = out.split("/")
        return float(num) / float(den) if float(den) else 0.0
    except Exception:
        return 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("clip")
    ap.add_argument("--out", required=True)
    ap.add_argument("--start", type=int, default=None,
                    help="first frame of the strip (default: the busiest window)")
    ap.add_argument("--strip", type=int, default=8)
    ap.add_argument("--crop", default=None, help="WxH of the analysis/centre crop")
    ap.add_argument("--scale", type=float, default=1.0)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    crop = None
    if args.crop:
        cw, ch = args.crop.lower().split("x")
        crop = (int(cw), int(ch))

    paths = extract(args.clip, args.out)
    if len(paths) < 2:
        sys.exit("no frames decoded")

    small = []
    box = None
    for p in paths:
        im = Image.open(p).convert("RGB")
        c, box = centre_crop(im, crop)
        small.append(c)

    diffs = [0.0]
    for i in range(1, len(small)):
        diffs.append(mad(small[i - 1], small[i]))

    with open(os.path.join(args.out, "diffs.tsv"), "w", encoding="utf-8") as fh:
        fh.write("frame\tmad\n")
        for i, d in enumerate(diffs):
            fh.write("%d\t%.4f\n" % (i, d))

    vals = diffs[1:]
    vals_sorted = sorted(vals)
    med = vals_sorted[len(vals_sorted) // 2] if vals_sorted else 0.0
    lo = min(vals) if vals else 0.0
    hi = max(vals) if vals else 0.0
    stalls = sum(1 for v in vals if med > 0.05 and v < med * 0.35)
    # Alternation score: how much the even-index diffs differ from the odd ones.
    even = [v for i, v in enumerate(vals) if i % 2 == 0]
    odd = [v for i, v in enumerate(vals) if i % 2 == 1]
    alt = 0.0
    if even and odd:
        me = sum(even) / len(even)
        mo = sum(odd) / len(odd)
        alt = abs(me - mo) / max(me, mo, 1e-6)

    start = args.start
    if start is None:
        window = 4
        best, best_s = 0, -1.0
        for s in range(1, max(2, len(diffs) - window)):
            score = sum(diffs[s:s + window])
            if score > best_s and min(diffs[s:s + window]) > 0.02:
                best_s, best = score, s - 1
        start = max(0, best)
    strip = build_strip(paths, os.path.join(args.out, "strip.png"), start, args.strip,
                        box, args.scale)

    print("frames=%d fps=%.1f mad median=%.3f min=%.3f max=%.3f stall_frames=%d/%d "
          "alternation=%.3f strip=%s start=%d"
          % (len(paths), ffprobe_fps(args.clip), med, lo, hi, stalls, len(vals), alt,
             os.path.basename(strip) if strip else "-", start))
    return 0


if __name__ == "__main__":
    sys.exit(main())
