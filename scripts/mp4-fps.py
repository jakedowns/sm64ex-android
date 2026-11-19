#!/usr/bin/env python3
"""Print the average video frame rate of an MP4 without ffmpeg/gst plugins.

Reads the movie/track timescale and the H.264 sample timing table (`stts`).
This is deliberately small and dependency-free so the recording script can
verify that the delivered file really is ~60 FPS.
"""
import struct
import sys


def boxes(data, start, end):
    p = start
    while p + 8 <= end:
        size = struct.unpack(">I", data[p:p + 4])[0]
        typ = data[p + 4:p + 8].decode("latin1")
        hdr = 8
        if size == 1:
            size = struct.unpack(">Q", data[p + 8:p + 16])[0]
            hdr = 16
        elif size == 0:
            size = end - p
        if size < hdr:
            return
        yield typ, p + hdr, p + size
        p += size


def child(data, box, name):
    return next(((t, s, e) for t, s, e in boxes(data, box[1], box[2]) if t == name), None)


def children(data, box, name):
    return [(t, s, e) for t, s, e in boxes(data, box[1], box[2]) if t == name]


def main(path):
    data = open(path, "rb").read()
    moov = child(data, (None, 0, len(data)), "moov")
    if moov is None:
        raise SystemExit("no moov box")
    for trak in children(data, moov, "trak"):
        mdia = child(data, trak, "mdia")
        mdhd = child(data, mdia, "mdhd")
        hdlr = child(data, mdia, "hdlr")
        handler = data[hdlr[1] + 8:hdlr[1] + 12].decode("latin1")
        if handler != "vide":
            continue
        off = mdhd[1] + 4
        if data[mdhd[1]] == 1:
            timescale, duration = struct.unpack(">IQ", data[off + 16:off + 28])
        else:
            timescale, duration = struct.unpack(">II", data[off + 8:off + 16])
        minf = child(data, mdia, "minf")
        stbl = child(data, minf, "stbl")
        stts = child(data, stbl, "stts")
        count = struct.unpack(">I", data[stts[1] + 4:stts[1] + 8])[0]
        p = stts[1] + 8
        samples = ticks = 0
        for _ in range(count):
            n, delta = struct.unpack(">II", data[p:p + 8])
            p += 8
            samples += n
            ticks += n * delta
        fps = samples / (ticks / timescale) if ticks else 0.0
        stsd = child(data, stbl, "stsd")
        sp = stsd[1] + 8
        w, h = struct.unpack(">HH", data[sp + 32:sp + 36])
        print(f"codec={data[sp + 4:sp + 8].decode('latin1')} size={w}x{h} "
              f"duration={duration / timescale:.3f}s samples={samples} avg_fps={fps:.2f}")
        return fps
    raise SystemExit("no video track")


if __name__ == "__main__":
    main(sys.argv[1])
