#!/usr/bin/env python3
"""Build a labelled contact sheet from a blur-sweep artifact directory.

Usage: contact-sheet.py <dir> <out.png> [columns]

Reads results.txt (name, label, ms, fps, velmean, args) and tiles
<name>.png one per sweep step with a caption bar showing the sweep label,
frame time, fps and the mean velocity of the captured frame.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont


def parse_results(path):
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 4:
                continue
            row = {"name": parts[0], "label": parts[1]}
            for p in parts[2:]:
                if "=" in p:
                    k, v = p.split("=", 1)
                    row[k] = v
            rows.append(row)
    return rows


def font(size):
    for name in ("DejaVuSans.ttf", "DejaVuSansMono.ttf"):
        for base in ("/usr/share/fonts/truetype/dejavu/", "/usr/share/fonts/"):
            path = os.path.join(base, name)
            if os.path.exists(path):
                return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, out = sys.argv[1], sys.argv[2]
    cols = int(sys.argv[3]) if len(sys.argv) > 3 else 5

    results = os.path.join(src, "results.txt")
    rows = parse_results(results) if os.path.exists(results) else []
    if not rows:
        print("no results.txt rows")
        return 1

    tiles = []
    for row in rows:
        png = os.path.join(src, row["name"] + ".png")
        if not os.path.exists(png):
            continue
        tiles.append((row, Image.open(png).convert("RGB")))
    if not tiles:
        print("no stills found")
        return 1

    tw = 336
    f_label = font(15)
    f_info = font(13)
    bar_h = 40
    pad = 6

    # Scale each still to the tile width, keep aspect.
    scaled = []
    for row, im in tiles:
        w, h = im.size
        th = max(1, int(round(h * (tw / float(w)))))
        scaled.append((row, im.resize((tw, th), Image.LANCZOS)))

    th_max = max(im.size[1] for _, im in scaled)
    rows_n = (len(scaled) + cols - 1) // cols
    sheet_w = cols * tw + (cols + 1) * pad
    sheet_h = rows_n * (th_max + bar_h) + (rows_n + 1) * pad
    sheet = Image.new("RGB", (sheet_w, sheet_h), (18, 20, 24))
    draw = ImageDraw.Draw(sheet)

    for i, (row, im) in enumerate(scaled):
        r, c = divmod(i, cols)
        x = pad + c * (tw + pad)
        y = pad + r * (th_max + bar_h + pad)
        sheet.paste(im, (x, y))
        draw.rectangle([x, y + th_max, x + tw, y + th_max + bar_h],
                       fill=(28, 31, 37))
        label = row.get("label", row["name"])
        info = "ms=%s  fps=%s  motion=%s" % (
            row.get("ms", "?"), row.get("fps", "?"), row.get("motion", "?"))
        draw.text((x + 6, y + th_max + 3), label, fill=(235, 238, 242), font=f_label)
        draw.text((x + 6, y + th_max + 21), info, fill=(150, 200, 255), font=f_info)

    sheet.save(out)
    print("%s (%dx%d, %d tiles)" % (out, sheet_w, sheet_h, len(scaled)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
