#!/usr/bin/env python3
"""Generate a 2D stripe LUT for the GRADE tab (fx 284 look 6).

GLES2 has no 3D textures, so the LUT is a horizontal strip of 16x16 blue
slices: red runs across a tile, green down it, blue selects the tile.

    256x16 = 16 slices, 512x16 = 32 slices (any 16*N x 16, N = 2..64)

Drop the PNG at
    /sdcard/Android/data/com.vdavid003.sm64port/files/luts/grade.png
and send `adb shell am broadcast ... --es cmd fx --ei k 284 --ei v 6`.
Selecting look 6 again re-reads the file.

Usage:
    scripts/grade-lut.py out.png            # demo "cool chrome" LUT
    scripts/grade-lut.py out.png --identity # identity, for validation only

Invalid sizes are refused (the renderer would reject them too); to exercise the
rejection path, make e.g. a 64x32 PNG with any image tool and push it.
"""
import argparse
import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.exit("Pillow is required: python3 -m pip install pillow")


def demo(r, g, b):
    """Visible but sane grade: channel rotation (proves the axis mapping) with
    a mild lift so shadow detail is not crushed."""
    return (
        min(1.0, 0.90 * b + 0.10 * r + 0.02),
        min(1.0, 0.90 * r + 0.10 * g + 0.02),
        min(1.0, 0.90 * g + 0.10 * b + 0.04),
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--identity", action="store_true")
    ap.add_argument("--size", default="256x16", help="16*N x 16 (default 256x16)")
    args = ap.parse_args()

    w, h = (int(x) for x in args.size.lower().split("x"))
    if h != 16 or w % 16 or not (32 <= w <= 1024):
        sys.exit("size must be 16*N x 16 with N in 2..64, got %dx%d" % (w, h))

    img = Image.new("RGB", (w, h))
    px = img.load()
    slices = w // 16
    for t in range(slices):
        b = t / max(slices - 1, 1)
        for y in range(16):
            g = y / 15.0
            for x in range(16):
                r = x / 15.0
                rgb = (r, g, b) if args.identity else demo(r, g, b)
                px[t * 16 + x, y] = tuple(int(round(c * 255)) for c in rgb)
    img.save(args.out)
    print("wrote %s (%dx%d, %d slices)" % (args.out, w, h, slices))


if __name__ == "__main__":
    main()
