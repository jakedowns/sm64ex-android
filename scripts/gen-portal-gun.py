#!/usr/bin/env python3
"""Generate the held portal-gun mesh/display list from the CC0 STL.

Source model: "Portal Gun" by 3DESIGNER27 (Tim Gubski), CC0 1.0.
  https://www.thingiverse.com/thing:3022786
  https://archive.org/download/thingiverse-3022786/Portal_Gun_3022786.zip

The 47k-triangle STL is decimated by vertex clustering into an N64-friendly
low-poly mesh, then emitted as a static Vtx array + Gfx display list.  The mesh
is re-oriented so local +Z is the barrel, +Y is up, +X is right.

Run from the repository root:

    python3 scripts/gen-portal-gun.py [path-to-Portal_Gun.stl]

Default source path: /tmp/opencode/portal-gun-model/files/Portal_Gun.stl
Output: app/jni/src/src/game/ptc_gun_model.inc
"""
import os
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "app/jni/src/src/game/ptc_gun_model.inc"
DEFAULT_STL = Path("/tmp/opencode/portal-gun-model/files/Portal_Gun.stl")

CELL = 1.5          # clustering grid in STL units
SCALE = 1.15        # STL units -> Mario units
FORWARD_SHIFT = -8.0 # push the barrel forward from the hand
UP_SHIFT = 0.0
MAX_BATCH = 32


def load_stl(path):
    data = path.read_bytes()
    n = struct.unpack_from("<I", data, 80)[0]
    if len(data) != 84 + 50 * n:
        raise SystemExit("not a binary STL")
    verts = []
    vidx = {}
    tris = []
    for i in range(n):
        off = 84 + 50 * i + 12
        t = []
        for k in range(3):
            v = struct.unpack_from("<3f", data, off + 12 * k)
            key = (round(v[0], 4), round(v[1], 4), round(v[2], 4))
            if key not in vidx:
                vidx[key] = len(verts)
                verts.append(key)
            t.append(vidx[key])
        tris.append(tuple(t))
    return verts, tris


def cluster(verts, tris, cell):
    mn = [min(v[a] for v in verts) for a in range(3)]
    cells = {}
    sums = {}

    def cidx(v):
        k = (int((v[0] - mn[0]) / cell), int((v[1] - mn[1]) / cell), int((v[2] - mn[2]) / cell))
        if k not in cells:
            cells[k] = len(cells)
            sums[k] = [0.0, 0.0, 0.0, 0]
        s = sums[k]
        s[0] += v[0]; s[1] += v[1]; s[2] += v[2]; s[3] += 1
        return cells[k]

    idx = [cidx(v) for v in verts]
    out_verts = []
    for k, i in cells.items():
        s = sums[k]
        out_verts.append((s[0] / s[3], s[1] / s[3], s[2] / s[3]))
    seen = set()
    out_tris = []
    for t in tris:
        a, b, c = idx[t[0]], idx[t[1]], idx[t[2]]
        if a == b or b == c or a == c:
            continue
        key = (a, b, c)
        if key in seen:
            continue
        seen.add(key)
        out_tris.append(key)

    # Vertex clustering can bridge separate shells into long spike triangles.
    # Drop only clearly bad ones so flat faces survive.
    max_edge2 = (3.2 * cell) * (3.2 * cell)
    kept = []
    for a, b, c in out_tris:
        def d2(i, j):
            dx = out_verts[i][0] - out_verts[j][0]
            dy = out_verts[i][1] - out_verts[j][1]
            dz = out_verts[i][2] - out_verts[j][2]
            return dx * dx + dy * dy + dz * dz
        if max(d2(a, b), d2(b, c), d2(c, a)) > max_edge2:
            continue
        ux = out_verts[b][0] - out_verts[a][0]
        uy = out_verts[b][1] - out_verts[a][1]
        uz = out_verts[b][2] - out_verts[a][2]
        vx = out_verts[c][0] - out_verts[a][0]
        vy = out_verts[c][1] - out_verts[a][1]
        vz = out_verts[c][2] - out_verts[a][2]
        cx = uy * vz - uz * vy
        cy = uz * vx - ux * vz
        cz = ux * vy - uy * vx
        if 0.5 * (cx * cx + cy * cy + cz * cz) ** 0.5 < 0.35:
            continue
        kept.append((a, b, c))
    return out_verts, kept


def transform(v):
    # STL: +Y is the gun axis, +Z is up.  Local: +Z forward, +Y up, +X right.
    x = v[0] * SCALE
    y = v[2] * SCALE
    z = v[1] * SCALE
    return x, y, z


def color(v):
    x, y, z = v
    if z < -14.0:
        return (0x36, 0x36, 0x42)       # rear grip
    if z > 14.0:
        return (0x86, 0xB4, 0xFF)       # front barrel
    if abs(x) > 9.0:
        return (0xD8, 0xD8, 0xE8)       # side shells
    return (0x88, 0x88, 0xA0)           # body


def build_batches(verts, tris):
    batches = []
    cur_ids = []
    cur_map = {}
    cur_tris = []

    def flush():
        if cur_ids:
            batches.append((list(cur_ids), list(cur_tris)))
        cur_ids.clear(); cur_map.clear(); cur_tris.clear()

    for t in tris:
        new = [v for v in t if v not in cur_map]
        if len(cur_ids) + len(new) > MAX_BATCH:
            flush()
        local = []
        for v in t:
            if v not in cur_map:
                cur_map[v] = len(cur_ids)
                cur_ids.append(v)
            local.append(cur_map[v])
        cur_tris.append(tuple(local))
    flush()
    return batches


def main():
    stl = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_STL
    if not stl.is_file():
        raise SystemExit(f"missing STL: {stl}\nDownload it first (see script header).")
    verts, tris = load_stl(stl)
    verts, tris = cluster(verts, tris, CELL)

    # recentre in local space
    mn = [min(v[a] for v in verts) for a in range(3)]
    mx = [max(v[a] for v in verts) for a in range(3)]
    off = [(mn[a] + mx[a]) * 0.5 for a in range(3)]
    off[2] += FORWARD_SHIFT
    off[1] += UP_SHIFT
    verts = [tuple(round(v[a] - off[a]) for a in range(3)) for v in verts]
    # local x/y/z after transform
    verts = [transform(v) for v in verts]
    verts = [(round(x), round(y), round(z)) for (x, y, z) in verts]

    batches = build_batches(verts, tris)

    lines = []
    lines.append("/* Generated by scripts/gen-portal-gun.py -- do not edit. */")
    lines.append("/* Portal gun by 3DESIGNER27 (Tim Gubski), CC0 1.0. */")
    lines.append("")
    lines.append("static const Vtx ptc_gun_vtx[] = {")
    base = 0
    for ids, _ in batches:
        for vi in ids:
            x, y, z = verts[vi]
            r, g, b = color(verts[vi])
            lines.append(f"    {{{{{{{x:4d},{y:4d},{z:4d}}}, 0, {{0, 0}}, {{0x{r:02X}, 0x{g:02X}, 0x{b:02X}, 0xFF}}}}}},")
    lines.append("};")
    lines.append("")
    lines.append("static const Gfx ptc_gun_dl[] = {")
    lines.append("    gsDPPipeSync(),")
    lines.append("    gsDPSetCombineMode(G_CC_SHADE, G_CC_SHADE),")
    lines.append("    gsSPSetGeometryMode(G_SHADE | G_ZBUFFER),")
    lines.append("    gsSPClearGeometryMode(G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR | G_CULL_BACK),")
    lines.append("    gsSPTexture(0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF),")
    lines.append("    gsDPSetRenderMode(G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2),")
    base = 0
    for ids, ts in batches:
        lines.append(f"    gsSPVertex(ptc_gun_vtx + {base}, {len(ids)}, 0),")
        i = 0
        while i < len(ts):
            a, b, c = ts[i]
            if i + 1 < len(ts):
                d, e, f = ts[i + 1]
                lines.append(f"    gsSP2Triangles({a},{b},{c},0x0, {d},{e},{f},0x0),")
                i += 2
            else:
                lines.append(f"    gsSP1Triangle({a},{b},{c},0x0),")
                i += 1
        base += len(ids)
    lines.append("    gsDPPipeSync(),")
    lines.append("    gsDPSetRenderMode(G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2),")
    lines.append("    gsSPEndDisplayList(),")
    lines.append("};")
    lines.append("")

    OUT.write_text("\n".join(lines) + "\n")
    print(f"{stl.name}: {len(verts)} verts, {len(tris)} tris, {len(batches)} batches -> {OUT}")


if __name__ == "__main__":
    main()
