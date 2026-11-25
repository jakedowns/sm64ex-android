#!/usr/bin/env python3
"""Assemble the HFR held-object stills into a labelled frame strip.

    scripts/hfr-scale-strip.py OUT.png --crop X,Y,W,H shot1.png shot2.png ...

Each input is cropped to the same rectangle (so the object is directly
comparable), optionally scaled, and laid out left to right with its filename as
a caption. Used for the before/after strip referenced by HFR-HELD-OBJECTS.md.
"""
import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFont

CAPTION_H = 22


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--crop", default=None,
                    help="x,y,w,h in source pixels (default: full frame)")
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--gap", type=int, default=8)
    ap.add_argument("images", nargs="+")
    args = ap.parse_args()

    crop = None
    if args.crop:
        crop = tuple(int(v) for v in args.crop.split(","))
        if len(crop) != 4:
            print("--crop needs x,y,w,h", file=sys.stderr)
            return 2

    tiles = []
    for path in args.images:
        im = Image.open(path).convert("RGB")
        if crop:
            # --crop is x,y,w,h; PIL wants a (left, upper, right, lower) box.
            x, y, cw, ch = crop
            im = im.crop((x, y, x + cw, y + ch))
        if args.scale != 1.0:
            im = im.resize((max(1, int(im.width * args.scale)),
                            max(1, int(im.height * args.scale))), Image.LANCZOS)
        tiles.append((os.path.basename(path), im))

    w = sum(t.width for _, t in tiles) + args.gap * (len(tiles) - 1)
    h = max(t.height for _, t in tiles) + CAPTION_H
    out = Image.new("RGB", (w, h), (24, 24, 24))
    draw = ImageDraw.Draw(out)
    try:
        font = ImageFont.truetype("DejaVuSans.ttf", 14)
    except OSError:
        font = ImageFont.load_default()

    x = 0
    for name, t in tiles:
        out.paste(t, (x, CAPTION_H))
        draw.text((x + 4, 3), name, fill=(235, 235, 235), font=font)
        x += t.width + args.gap
    out.save(args.out)
    print(f"wrote {args.out} ({out.width}x{out.height})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
