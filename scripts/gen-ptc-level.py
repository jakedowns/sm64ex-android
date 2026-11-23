#!/usr/bin/env python3
"""Generate the Portal Test Chamber graybox level.

The level is deliberately tiny and fully procedural: one room, five boxes of
different heights, two portal target panels and a checkerboard wall texture.
Keeping the geometry in a generator makes the measured coordinates and the
collision/render data impossible to disagree with each other.

Run from the repository root:

    python3 scripts/gen-ptc-level.py

The generated files are normal C sources; they do not depend on this script at
build time.
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "app/jni/src/levels/ptc"

# ---- world-space layout (N64 units) -------------------------------------
# The z1 wall carries test portal B, so it stays where the portal test expects
# it; the other three walls move out to make room for the annexes.
ROOM = dict(x0=-2600, x1=3200, y0=0, y1=2800, z0=-3200, z1=1250)

# name: (x0, z0, x1, z1, top_y, top_rgb, side_rgb)
PLATFORMS = [
    ("P1", -1050, -650, -750, -350, 180, (95, 205, 120), (55, 115, 70)),
    ("P2",   750, -650, 1050, -350, 360, (105, 155, 235), (60, 90, 140)),
    ("P3", -1050,  150, -750,  450, 560, (235, 215, 100), (140, 125, 60)),
    ("P4",   750,  150, 1050,  450, 680, (225, 110, 225), (130, 65, 130)),
    ("P5",  -600,  600,  600, 1200, 800, (235, 100, 90), (140, 60, 55)),
    # A freestanding portal wall near Mario.  Its -Z face is the blue target.
    ("P6",  -750, -400, -450, -300, 260, (150, 150, 160), (90, 90, 100)),
    # Momentum test: high drop platform, launch ramp target and second star.
    ("P7",  -300, -1500, 300, -1100, 1100, (120, 150, 210), (70, 90, 140)),
    # Green "reset" pad: landing on it rebuilds the Box3D pyramid.
    ("P8",  1150,   650, 1350,   850,   40, (110, 250, 140), (55, 150, 85)),
    # --- WALL-RUN ANNEX (west wing) -------------------------------------
    # Corridor runs along +Z: interior x -2340..-2160, z -600..1200.  The west
    # face (x = -2340) is the run surface.
    ("WR_W", -2400, -600, -2340, 1200, 420, (170, 175, 190), (95, 100, 115)),
    ("WR_E", -2160, -600, -2100, 1200, 420, (170, 175, 190), (95, 100, 115)),
    # --- JETPACK ANNEX (east wing) --------------------------------------
    # Walls stop level with the landing pad: a taller rim trapped Mario on top.
    ("JP_W",  2600, -400,  2660,    0, 1200, (150, 165, 205), (80, 90, 120)),
    ("JP_E",  2940, -400,  3000,    0, 1200, (150, 165, 205), (80, 90, 120)),
    ("JP_S",  2600, -460,  3000, -400, 1200, (150, 165, 205), (80, 90, 120)),
    ("JP_N",  2600,    0,  3000,   60, 1200, (150, 165, 205), (80, 90, 120)),
    ("JP_PAD",2650, -350,  2950,  -50, 1200, (120, 240, 150), (60, 140, 85)),
    ("P9",  -300,   150, 300,   560,  950, (230, 190, 90), (140, 110, 45)),
    # --- NINJA COURSE (south wing) --------------------------------------
    # Jetpack launch pad: the first flight lifts Mario to the wall-run height.
    ("L1",  -1100, -1600, -600, -1400, 450, (110, 200, 230), (60, 110, 130)),
    # Wall-run wall (west face x=-360) and its catch platform.
    ("N1",  -360, -2350, -300, -1400, 700, (150, 150, 165), (80, 80, 95)),
    ("N2",  -950, -2700, -360, -2350, 330, (225, 150, 90), (130, 85, 50)),
    # Tall grapple pillar rising out of the big catch platform.
    ("N3",  -150, -2850,  -90, -2790, 950, (200, 120, 220), (110, 60, 130)),
    ("N4",  -200, -3200, 1000, -2600, 200, (90, 200, 200), (50, 115, 120)),
    # Jetpack target and its portal tower.
    ("N5",  1200, -3150, 2100, -2600, 900, (220, 200, 90), (125, 110, 45)),
    ("N7",  1900, -3000, 2100, -2800, 1700, (150, 190, 150), (80, 110, 85)),
    # Final tower reached through the ceiling portal, plus the goomba gate.
    ("N6", -1500, -3200, -300, -2600, 1500, (170, 170, 210), (95, 95, 130)),
    ("N8",  -450, -3100, -390, -2900, 2100, (190, 150, 120), (105, 80, 60)),
]

# 45-degree launch ramp: surface y = -z - 50, x in [-250,250], z in [-850,-550].
RAMP = dict(x0=-250, x1=250, z0=-850, z1=-550, y0=800, y1=500)

PORTAL_A = (-600, 100, -400)      # hitscan target on the near portal wall (-Z face)
PORTAL_B = (0, 900, 1199)         # hitscan target on the back wall
PORTAL_A_NORMAL = (0, 0, -1)      # faces Mario (-Z side)
PORTAL_B_NORMAL = (0, 0, -1)      # into the room
STAR = (180, 870, 900)
STAR2 = (0, 1000, 480)
MARIO_START = (-600, 0, -950)
MOM_MARIO_START = (0, 1100, -1250)
MOM_PORTAL_A = (0, 0, -1042)      # floor portal under the measured P7 drop line
MOM_PORTAL_B = (0, 650, -700)     # 45-degree ramp portal
MOM_PORTAL_B_NORMAL = (0, 0.70710678, 0.70710678)
WALK_YAW = 0                      # face +Z (towards the near portal wall)
STAR_YAW = 0x5A82                # diagonally right/back towards the star

# World-space labels.  (name, text, x centre, y centre, z, width, height)
LABELS = [
    ("p1", "P1",   -900, 100,  -651, 120, 60),
    ("p2", "P2",    900, 270,  -651, 120, 60),
    ("p3", "P3",   -900, 470,   149, 120, 60),
    ("p4", "P4",    900, 590,   149, 120, 60),
    ("p5", "P5",      0, 680,   599, 120, 60),
    ("a",  "A",    -600, 250,  -401, 100, 50),
    ("b",  "B",       0, 1050, 1199, 100, 50),
    ("p7", "P7",      0, 1000, -1501, 120, 60),
    ("p9", "P9",      0,  850,   149, 120, 60),
    ("n2", "N2",   -650,  560, -2351, 100, 50),
    ("n3", "N3",   -120, 1040, -2791, 100, 50),
    ("n5", "N5",   1650,  990, -2601, 100, 50),
    ("n6", "N6",   -900, 1590, -2601, 100, 50),
]

TEXTURE_SCALE = 8                 # 8 texels per world unit = 1 unit per 8px check


def v(x, y, z):
    return (int(x), int(y), int(z))


def face(points, normal):
    """Return the points wound so cross(p1-p0,p2-p0) points along normal."""
    p0, p1, p2, p3 = points
    ux, uy, uz = p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]
    vx, vy, vz = p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]
    cx, cy, cz = (uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx)
    if cx * normal[0] + cy * normal[1] + cz * normal[2] < 0:
        # Reverse the whole loop; swapping p1/p2 (the old code) makes a
        # self-intersecting bow-tie and only part of the quad renders.
        points = points[::-1]
    return points


def subdiv_quad(pts, max_cell=256.0):
    """Split a quad (p0,p1,p2,p3 loop) into a grid of same-winding quads."""
    import math as _m
    p0, p1, p2, p3 = pts

    def d(a, b):
        return _m.sqrt((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2)

    nu = max(1, int(_m.ceil(d(p0, p1) / max_cell)))
    nv = max(1, int(_m.ceil(d(p0, p3) / max_cell)))

    def lerp(a, b, t):
        return (a[0] + (b[0] - a[0]) * t,
                a[1] + (b[1] - a[1]) * t,
                a[2] + (b[2] - a[2]) * t)

    def at(u, v):
        return lerp(lerp(p0, p1, u), lerp(p3, p2, u), v)

    out = []
    for iv in range(nv):
        v0 = iv / nv
        v1 = (iv + 1) / nv
        for iu in range(nu):
            u0 = iu / nu
            u1 = (iu + 1) / nu
            out.append([at(u0, v0), at(u1, v0), at(u1, v1), at(u0, v1)])
    return out


def ramp_faces():
    """45-degree solid ramp: surface y = -z - 50, with vertical ends/sides."""
    x0, x1 = RAMP["x0"], RAMP["x1"]
    z0, z1 = RAMP["z0"], RAMP["z1"]
    y0, y1 = RAMP["y0"], RAMP["y1"]
    n = (0.0, 0.70710678, 0.70710678)
    return [
        ("ramp_top", face([v(x1, y0, z0), v(x0, y0, z0), v(x0, y1, z1), v(x1, y1, z1)], n), n),
        ("ramp_front", face([v(x1, 0, z1), v(x0, 0, z1), v(x0, y1, z1), v(x1, y1, z1)], (0, 0, 1)), (0, 0, 1)),
        ("ramp_back", face([v(x0, 0, z0), v(x1, 0, z0), v(x1, y0, z0), v(x0, y0, z0)], (0, 0, -1)), (0, 0, -1)),
        ("ramp_left", face([v(x0, 0, z0), v(x0, y0, z0), v(x0, y1, z1), v(x0, 0, z1)], (-1, 0, 0)), (-1, 0, 0)),
        ("ramp_right", face([v(x1, 0, z1), v(x1, y1, z1), v(x1, y0, z0), v(x1, 0, z0)], (1, 0, 0)), (1, 0, 0)),
    ]


def box_faces(x0, y0, z0, x1, y1, z1):
    """Outward-wound faces of an axis-aligned box."""
    return [
        ("top",    face([v(x1, y1, z0), v(x0, y1, z0), v(x0, y1, z1), v(x1, y1, z1)], (0, 1, 0)), (0, 1, 0)),
        ("bottom", face([v(x1, y0, z0), v(x1, y0, z1), v(x0, y0, z1), v(x0, y0, z0)], (0, -1, 0)), (0, -1, 0)),
        ("front",  face([v(x1, y0, z0), v(x0, y0, z0), v(x0, y1, z0), v(x1, y1, z0)], (0, 0, -1)), (0, 0, -1)),
        ("back",   face([v(x0, y0, z1), v(x1, y0, z1), v(x1, y1, z1), v(x0, y1, z1)], (0, 0, 1)), (0, 0, 1)),
        ("left",   face([v(x0, y0, z0), v(x0, y0, z1), v(x0, y1, z1), v(x0, y1, z0)], (-1, 0, 0)), (-1, 0, 0)),
        ("right",  face([v(x1, y0, z1), v(x1, y0, z0), v(x1, y1, z0), v(x1, y1, z1)], (1, 0, 0)), (1, 0, 0)),
    ]


_room_quads = [
    ("floor",   face([v(ROOM["x1"], ROOM["y0"], ROOM["z0"]), v(ROOM["x0"], ROOM["y0"], ROOM["z0"]),
                      v(ROOM["x0"], ROOM["y0"], ROOM["z1"]), v(ROOM["x1"], ROOM["y0"], ROOM["z1"])], (0, 1, 0)), (0, 1, 0)),
    ("ceiling", face([v(ROOM["x1"], ROOM["y1"], ROOM["z0"]), v(ROOM["x1"], ROOM["y1"], ROOM["z1"]),
                      v(ROOM["x0"], ROOM["y1"], ROOM["z1"]), v(ROOM["x0"], ROOM["y1"], ROOM["z0"])], (0, -1, 0)), (0, -1, 0)),
    ("front",   face([v(ROOM["x0"], ROOM["y0"], ROOM["z0"]), v(ROOM["x1"], ROOM["y0"], ROOM["z0"]),
                      v(ROOM["x1"], ROOM["y1"], ROOM["z0"]), v(ROOM["x0"], ROOM["y1"], ROOM["z0"])], (0, 0, 1)), (0, 0, 1)),
    ("back",    face([v(ROOM["x1"], ROOM["y0"], ROOM["z1"]), v(ROOM["x0"], ROOM["y0"], ROOM["z1"]),
                      v(ROOM["x0"], ROOM["y1"], ROOM["z1"]), v(ROOM["x1"], ROOM["y1"], ROOM["z1"])], (0, 0, -1)), (0, 0, -1)),
    ("left",    face([v(ROOM["x0"], ROOM["y0"], ROOM["z0"]), v(ROOM["x0"], ROOM["y0"], ROOM["z1"]),
                      v(ROOM["x0"], ROOM["y1"], ROOM["z1"]), v(ROOM["x0"], ROOM["y1"], ROOM["z0"])], (1, 0, 0)), (1, 0, 0)),
    ("right",   face([v(ROOM["x1"], ROOM["y0"], ROOM["z0"]), v(ROOM["x1"], ROOM["y1"], ROOM["z0"]),
                      v(ROOM["x1"], ROOM["y1"], ROOM["z1"]), v(ROOM["x1"], ROOM["y0"], ROOM["z1"])], (-1, 0, 0)), (-1, 0, 0)),
]

# Split the room into ~256-unit cells: the checker tiles cleanly instead of
# being stretched across the whole wall, and Box3D/collision gets many more
# triangles to work with.
room_faces = []
for _name, _pts, _n in _room_quads:
    for _q in subdiv_quad(_pts):
        room_faces.append((_name, _q, _n))

# Checkerboard texture: 32x32 RGBA16, 8px blocks => one 8px check = one unit.
# Low contrast + bilinear filtering keeps distant wall checks from turning into
# a black/white moire pattern.
checks = []
for y in range(32):
    for x in range(32):
        checks.append(0xA529 if ((x // 8) + (y // 8)) % 2 == 0 else 0x6319)


def vtx(x, y, z, s, t, rgb):
    r, g, b = rgb
    return f"    {{{{{{{x:5d}, {y:5d}, {z:5d}}}, 0, {{{s:6d}, {t:6d}}}, {{0x{r:02X}, 0x{g:02X}, 0x{b:02X}, 0xFF}}}}}},"


def emit_collision():
    lines = []
    lines.append("const Collision ptc_collision[] = {")
    lines.append("    COL_INIT(),")
    faces = list(room_faces)
    for name, x0, z0, x1, z1, top, _top_rgb, _side_rgb in PLATFORMS:
        base = -120 if name == "P6" else 0
        faces += box_faces(x0, base, z0, x1, top, z1)
    faces += ramp_faces()
    lines.append(f"    COL_VERTEX_INIT({len(faces) * 4}),")
    for _name, pts, _n in faces:
        for x, y, z in pts:
            # Subdivided faces interpolate to floats; collision wants integers.
            lines.append(f"    COL_VERTEX({int(round(x))}, {int(round(y))}, {int(round(z))}),")
    # Room surfaces participate in camera collision so the chase camera cannot
    # leave through the back wall after the portal exit.  Platform boxes are
    # excluded so the low debug camera is not shoved around by them.
    room_tris = len(room_faces) * 2
    plat_tris = (len(faces) - len(room_faces)) * 2
    lines.append(f"    COL_TRI_INIT(SURFACE_DEFAULT, {room_tris}),")
    for i in range(len(room_faces)):
        b = i * 4
        lines.append(f"    COL_TRI({b}, {b + 1}, {b + 2}),")
        lines.append(f"    COL_TRI({b}, {b + 2}, {b + 3}),")
    lines.append("    COL_TRI_STOP(),")
    lines.append(f"    COL_TRI_INIT(SURFACE_NO_CAM_COLLISION, {plat_tris}),")
    for i in range(len(room_faces), len(faces)):
        b = i * 4
        lines.append(f"    COL_TRI({b}, {b + 1}, {b + 2}),")
        lines.append(f"    COL_TRI({b}, {b + 2}, {b + 3}),")
    lines.append("    COL_TRI_STOP(),")
    lines.append("    COL_END(),")
    lines.append("};")
    return "\n".join(lines)


def emit_walls():
    walls = [
        ("front", (ROOM["x0"], ROOM["y0"], ROOM["z0"]), (ROOM["x1"], ROOM["y0"], ROOM["z0"]),
         (ROOM["x1"], ROOM["y1"], ROOM["z0"]), (ROOM["x0"], ROOM["y1"], ROOM["z0"]),
         (0, 0, 1), lambda p: ((p[0] - ROOM["x0"]) * TEXTURE_SCALE, p[1] * TEXTURE_SCALE)),
        ("back", (ROOM["x1"], ROOM["y0"], ROOM["z1"]), (ROOM["x0"], ROOM["y0"], ROOM["z1"]),
         (ROOM["x0"], ROOM["y1"], ROOM["z1"]), (ROOM["x1"], ROOM["y1"], ROOM["z1"]),
         (0, 0, -1), lambda p: ((p[0] - ROOM["x0"]) * TEXTURE_SCALE, p[1] * TEXTURE_SCALE)),
        ("left", (ROOM["x0"], ROOM["y0"], ROOM["z0"]), (ROOM["x0"], ROOM["y0"], ROOM["z1"]),
         (ROOM["x0"], ROOM["y1"], ROOM["z1"]), (ROOM["x0"], ROOM["y1"], ROOM["z0"]),
         (1, 0, 0), lambda p: ((p[2] - ROOM["z0"]) * TEXTURE_SCALE, p[1] * TEXTURE_SCALE)),
        ("right", (ROOM["x1"], ROOM["y0"], ROOM["z1"]), (ROOM["x1"], ROOM["y0"], ROOM["z0"]),
         (ROOM["x1"], ROOM["y1"], ROOM["z0"]), (ROOM["x1"], ROOM["y1"], ROOM["z1"]),
         (-1, 0, 0), lambda p: ((p[2] - ROOM["z0"]) * TEXTURE_SCALE, p[1] * TEXTURE_SCALE)),
    ]
    lines = [
        "ALIGNED8 static const u8 ptc_checker_tex[2048] = {",
    ]
    # N64 RGBA16 data is big-endian; emit bytes, not host-endian u16 values.
    check_bytes = []
    for c in checks:
        check_bytes.append((c >> 8) & 0xFF)
        check_bytes.append(c & 0xFF)
    for i in range(0, len(check_bytes), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in check_bytes[i:i+16]) + ",")
    lines += [
        "};",
        "",
        "static const Vtx ptc_vtx_walls[] = {",
    ]
    # N = +Z (portal B wall, stays neutral), S = -Z green, W = -X blue, E = +X red.
    wall_tint = {
        "front": (130, 230, 140),   # south
        "back":  (255, 255, 255),   # north (portal B)
        "left":  (140, 175, 255),   # west
        "right": (255, 150, 140),   # east
    }
    wall_ranges = []
    vcount = 0
    for name, p0, p1, p2, p3, normal, uv in walls:
        pts = face([p0, p1, p2, p3], normal)
        tint = wall_tint.get(name, (255, 255, 255))
        start = vcount
        # Subdivide so the checker tiles at a consistent size across the room
        # instead of being stretched over one giant quad.
        # Keep each checker cell's UV span below the 1024-texel s16 wrap so
        # the texture tiles instead of stretching back to 0 at a cell corner.
        for cell in subdiv_quad(pts, 120.0):
            for p in [(int(round(v[0])), int(round(v[1])), int(round(v[2]))) for v in cell]:
                s, t = uv(p)
                # 32-texel checker: wrap in 10.5 fixed point so the bigger room
                # does not overflow the s16 texture coordinates.
                s %= 1024
                t %= 1024
                lines.append(vtx(*p, s, t, tint))
                vcount += 1
        wall_ranges.append((start, vcount - start))

    wall_setup = [
        "    gsDPPipeSync(),",
        "    gsDPSetCombineMode(G_CC_MODULATERGB, G_CC_MODULATERGB),",
        "    gsSPClearGeometryMode(G_LIGHTING),",
        "    gsSPSetGeometryMode(G_SHADE | G_CULL_BACK),",
        "    gsDPSetTextureFilter(G_TF_BILERP),",
        "    gsDPSetTexturePersp(G_TP_PERSP),",
        "    gsDPSetTextureLUT(G_TT_NONE),",
        "    gsDPSetTextureImage(G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, ptc_checker_tex),",
        "    gsDPLoadSync(),",
        "    gsDPLoadBlock(G_TX_LOADTILE, 0, 0, 32 * 32 - 1, CALC_DXT(32, G_IM_SIZ_16b_BYTES)),",
        "    gsDPPipeSync(),",
        "    gsDPSetTile(G_IM_FMT_RGBA, G_IM_SIZ_16b, 8, 0, G_TX_RENDERTILE, 0,",
        "                G_TX_WRAP | G_TX_NOMIRROR, 5, G_TX_NOLOD,",
        "                G_TX_WRAP | G_TX_NOMIRROR, 5, G_TX_NOLOD),",
        "    gsDPSetTileSize(0, 0, 0, (32 - 1) << G_TEXTURE_IMAGE_FRAC, (32 - 1) << G_TEXTURE_IMAGE_FRAC),",
        "    gsSPTexture(0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON),",
    ]
    wall_restore = [
        "    gsSPTexture(0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF),",
        "    gsDPPipeSync(),",
        "    gsDPSetRenderMode(G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2),",
        "    gsDPSetCombineMode(G_CC_MODULATERGB, G_CC_MODULATERGB),",
        "    gsSPSetGeometryMode(G_LIGHTING),",
        "    gsSPEndDisplayList(),",
        "};",
    ]

    # One wall per display list so the occluder fade can target only the wall
    # that is actually between the camera and Mario.
    # Mirror panel on the west wall: a pale, semi-reflective quad that the
    # mirror-Mario node draws himself in front of (SM64 castle trick).
    mirror_start = vcount
    mir_pts = face([
        (3190, 100, -200),
        (3190, 100, 600),
        (3190, 900, 600),
        (3190, 900, -200),
    ], (-1, 0, 0))
    for cell in subdiv_quad(mir_pts):
        for p in [(int(round(v[0])), int(round(v[1])), int(round(v[2]))) for v in cell]:
            s_, t_ = ((p[2] + 200) * 4) % 1024, ((p[1] - 100) * 4) % 1024
            lines.append(vtx(*p, s_, t_, (170, 180, 205)))
            vcount += 1
    lines += ["};", ""]
    lines.append(f"#define PTC_MIRROR_VTX_START {mirror_start}")
    lines.append(f"#define PTC_MIRROR_VTX_COUNT {vcount - mirror_start}")
    lines.append("#define PTC_MIRROR_X (3190.0f)")
    lines.append("")

    for i, (vstart, vlen) in enumerate(wall_ranges):
        # Convention: load up to 32 vertices (8 quads) per gSPVertex and draw
        # the matching triangles, instead of one 4-vertex load per cell.
        tris = []
        for base in range(0, vlen, 32):
            n = min(32, vlen - base)
            tris.append(f"    gsSPVertex(ptc_vtx_walls + {vstart + base}, {n}, 0),")
            for q in range(n // 4):
                b = q * 4
                tris.append(f"    gsSP2Triangles({b}, {b + 1}, {b + 2}, 0x0, "
                            f"{b}, {b + 2}, {b + 3}, 0x0),")
        lines += [
            f"const Gfx ptc_dl_wall_{i}[] = {{",
            *wall_setup,
            "    gsDPSetRenderMode(G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2),",
            *tris,
            *wall_restore,
            "",
            f"const Gfx ptc_dl_wall_{i}_fade[] = {{",
            *wall_setup,
            "    gsDPSetRenderMode(G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2),",
            "    gsDPSetPrimColor(0, 0, 255, 255, 255, 90),",
            *tris,
            *wall_restore,
            "",
        ]

    # Mirror surface DL (same batched vertex loads as the walls).
    mtris = []
    mcount = vcount - mirror_start
    for base in range(0, mcount, 32):
        n = min(32, mcount - base)
        mtris.append(f"    gsSPVertex(ptc_vtx_walls + {mirror_start + base}, {n}, 0),")
        for q in range(n // 4):
            b = q * 4
            mtris.append(f"    gsSP2Triangles({b}, {b + 1}, {b + 2}, 0x0, "
                         f"{b}, {b + 2}, {b + 3}, 0x0),")
    lines += [
        "const Gfx ptc_dl_mirror[] = {",
        *wall_setup,
        "    gsDPSetRenderMode(G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2),",
        *mtris,
        *wall_restore,
        "",
    ]
    return "\n".join(lines)


def label_texture(text):
    """32x16 pixel-art cell scaled 2x, opaque dark blue background."""
    img = Image.new("RGBA", (32, 16), (16, 26, 56, 255))
    d = ImageDraw.Draw(img)
    font = ImageFont.load_default()
    bbox = d.textbbox((0, 0), text, font=font)
    tw = bbox[2] - bbox[0]
    th = bbox[3] - bbox[1]
    d.text(((32 - tw) // 2 - bbox[0], (16 - th) // 2 - bbox[1]), text,
           fill=(255, 245, 170, 255), font=font)
    img = img.resize((64, 32), Image.NEAREST)
    px = img.load()
    out = []
    for y in range(32):
        for x in range(64):
            r, g, b, a = px[x, y]
            c = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (1 if a else 0)
            out.append((c >> 8) & 0xFF)
            out.append(c & 0xFF)
    return out


def emit_labels():
    lines = []
    for name, text, _cx, _cy, _z, _w, _h in LABELS:
        data = label_texture(text)
        lines.append(f"ALIGNED8 static const u8 ptc_label_tex_{name}[4096] = {{")
        for i in range(0, len(data), 16):
            lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i+16]) + ",")
        lines.append("};")
        lines.append("")
    lines += [
        "const Gfx ptc_dl_labels[] = {",
        "    gsDPPipeSync(),",
        "    gsDPSetCombineMode(G_CC_MODULATERGB, G_CC_MODULATERGB),",
        "    gsSPClearGeometryMode(G_LIGHTING),",
        "    gsSPSetGeometryMode(G_SHADE | G_CULL_BACK),",
        "    gsDPSetTextureFilter(G_TF_POINT),",
        "    gsDPSetTexturePersp(G_TP_PERSP),",
        "    gsDPSetTextureLUT(G_TT_NONE),",
    ]
    for name, _text, cx, cy, z, w, h in LABELS:
        hw = w // 2
        hh = h // 2
        # Front face is normal -Z (towards the entry camera), same winding as
        # the platform front faces.
        lines += [
            f"    gsDPSetTextureImage(G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, ptc_label_tex_{name}),",
            "    gsDPLoadSync(),",
            "    gsDPLoadBlock(G_TX_LOADTILE, 0, 0, 64 * 32 - 1, CALC_DXT(64, G_IM_SIZ_16b_BYTES)),",
            "    gsDPPipeSync(),",
            "    gsDPSetTile(G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 0, G_TX_RENDERTILE, 0,",
            "                G_TX_CLAMP, 6, G_TX_NOLOD, G_TX_CLAMP, 5, G_TX_NOLOD),",
            "    gsDPSetTileSize(0, 0, 0, (64 - 1) << G_TEXTURE_IMAGE_FRAC, (32 - 1) << G_TEXTURE_IMAGE_FRAC),",
            "    gsSPTexture(0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON),",
            "    gsSPVertex(ptc_vtx_label_" + name + ", 4, 0),",
            "    gsSP2Triangles(0, 1, 2, 0x0, 0, 2, 3, 0x0),",
        ]
    lines += [
        "    gsSPTexture(0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF),",
        "    gsDPPipeSync(),",
        "    gsDPSetCombineMode(G_CC_MODULATERGB, G_CC_MODULATERGB),",
        "    gsSPSetGeometryMode(G_LIGHTING),",
        "    gsSPEndDisplayList(),",
        "};",
    ]
    return "\n".join(lines)


def emit_label_vertices():
    lines = []
    for name, _text, cx, cy, z, w, h in LABELS:
        hw, hh = w // 2, h // 2
        x0, x1 = cx - hw, cx + hw
        y0, y1 = cy - hh, cy + hh
        # right,bottom -> left,bottom -> left,top -> right,top  (normal -Z)
        pts = [v(x1, y0, z), v(x0, y0, z), v(x0, y1, z), v(x1, y1, z)]
        lines.append(f"static const Vtx ptc_vtx_label_{name}[] = {{")
        uvs = [(0, (32 - 1) << 5), ((64 - 1) << 5, (32 - 1) << 5), ((64 - 1) << 5, 0), (0, 0)]
        for pt, (ss, tt) in zip(pts, uvs):
            lines.append(vtx(*pt, ss, tt, (255, 255, 255)))
        lines += ["};", ""]
    return "\n".join(lines)


def emit_solid():
    lines = []
    # Room floor + ceiling.
    # Solid floor/ceiling visuals stay one quad each (the subdivided version is
    # only needed for collision and for the textured walls).
    for label, pts, _n in (_room_quads[0], _room_quads[1]):
        lines.append(f"static const Vtx ptc_vtx_room_{label}[] = {{")
        rgb = (112, 112, 112) if label == "floor" else (70, 70, 70)
        for p in pts:
            p = (int(round(p[0])), int(round(p[1])), int(round(p[2])))
            lines.append(vtx(*p, 0, 0, rgb))
        lines += ["};", ""]

    # Platform boxes: four explicit vertices per face so every triangle's
    # winding is consistent with the outward normal.
    for name, x0, z0, x1, z1, top, top_rgb, side_rgb in PLATFORMS:
        lines.append(f"static const Vtx ptc_vtx_{name.lower()}[] = {{")
        base = -120 if name == "P6" else 0
        for fname, pts, _n in box_faces(x0, base, z0, x1, top, z1):
            rgb = top_rgb if fname == "top" else side_rgb
            for p in pts:
                t = (p[1] - base) / max(1, top - base)
                shade = 0.60 + 0.40 * t
                c = tuple(min(255, int(v * shade)) for v in rgb)
                lines.append(vtx(*p, 0, 0, c))
        lines += ["};", ""]

    # Ramp wedge: four vertices per face, wound by ramp_faces().
    lines.append("static const Vtx ptc_vtx_ramp[] = {")
    for fname, pts, _n in ramp_faces():
        rgb = (190, 170, 120) if fname == "ramp_top" else (100, 90, 65)
        for p in pts:
            t = (p[1] - 500) / 300.0
            shade = 0.60 + 0.40 * max(0.0, min(1.0, t))
            c = tuple(min(255, int(v * shade)) for v in rgb)
            lines.append(vtx(*p, 0, 0, c))
    lines += ["};", ""]

    # Portal target panels, one unit in front of each wall.
    panels = [
        ("a", PORTAL_A, PORTAL_A_NORMAL, (28, 48, 130)),
        ("b", PORTAL_B, PORTAL_B_NORMAL, (150, 60, 18)),
        ("ma", MOM_PORTAL_A, (0, 1, 0), (30, 60, 140)),
        ("mb", MOM_PORTAL_B, MOM_PORTAL_B_NORMAL, (160, 70, 25)),
    ]
    for name, target, normal, rgb in panels:
        # Build a surface basis and place a square panel one unit out.
        up = (0.0, 1.0, 0.0)
        tx = up[1] * normal[2] - up[2] * normal[1]
        ty = up[2] * normal[0] - up[0] * normal[2]
        tz = up[0] * normal[1] - up[1] * normal[0]
        if tx * tx + ty * ty + tz * tz < 0.01:
            tx = 1.0 * normal[1] - 0.0 * normal[2]
            ty = 0.0 * normal[2] - 0.0 * normal[0]
            tz = 0.0 * normal[0] - 1.0 * normal[1]
        tl = (tx * tx + ty * ty + tz * tz) ** 0.5
        tan = (tx / tl, ty / tl, tz / tl)
        bx = normal[1] * tan[2] - normal[2] * tan[1]
        by = normal[2] * tan[0] - normal[0] * tan[2]
        bz = normal[0] * tan[1] - normal[1] * tan[0]
        cx, cy, cz = target
        cx += normal[0] * 1.0
        cy += normal[1] * 1.0
        cz += normal[2] * 1.0
        def corner(sx, sy):
            return v(cx + tan[0] * 120 * sx + bx * 120 * sy,
                     cy + tan[1] * 120 * sx + by * 120 * sy,
                     cz + tan[2] * 120 * sx + bz * 120 * sy)
        pts = face([corner(1, -1), corner(-1, -1), corner(-1, 1), corner(1, 1)], normal)
        lines.append(f"static const Vtx ptc_vtx_panel_{name}[] = {{")
        for p in pts:
            lines.append(vtx(*p, 0, 0, rgb))
        lines += ["};", ""]

    def emit_dl(name, body, fade=False):
        nonlocal lines
        lines.append(f"const Gfx {name}[] = {{")
        lines += [
            "    gsDPPipeSync(),",
            "    gsDPSetCombineMode(G_CC_PRIMITIVE, G_CC_PRIMITIVE)," if fade
                else "    gsDPSetCombineMode(G_CC_SHADE, G_CC_SHADE),",
            "    gsSPClearGeometryMode(G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR),",
            "    gsSPSetGeometryMode(G_SHADE | G_CULL_BACK),",
            "    gsSPTexture(0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF),",
            "    gsDPSetRenderMode(G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2)," if fade
                else "    gsDPSetRenderMode(G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2),",
        ]
        lines += body
        lines += [
            "    gsDPPipeSync(),",
            "    gsDPSetRenderMode(G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2),",
            "    gsDPSetCombineMode(G_CC_MODULATERGB, G_CC_MODULATERGB),",
            "    gsSPSetGeometryMode(G_LIGHTING),",
            "    gsSPEndDisplayList(),",
            "};",
            "",
        ]

    # Room floor/ceiling never occlude the camera-to-Mario segment in a way
    # that needs fading, so they stay one opaque list.
    room_body = [
        "    gsSPVertex(ptc_vtx_room_floor, 4, 0),",
        "    gsSP2Triangles(0, 1, 2, 0x0, 0, 2, 3, 0x0),",
        "    gsSPVertex(ptc_vtx_room_ceiling, 4, 0),",
        "    gsSP2Triangles(0, 1, 2, 0x0, 0, 2, 3, 0x0),",
    ]
    emit_dl("ptc_dl_room", room_body)

    # Six faces per platform, four verts each: one opaque and one faded list
    # per platform so only the blocking platform fades.
    for name, *_ in PLATFORMS:
        body = [f"    gsSPVertex(ptc_vtx_{name.lower()}, 24, 0),"]
        for f in range(6):
            b = f * 4
            body.append(f"    gsSP2Triangles({b}, {b + 1}, {b + 2}, 0x0, {b}, {b + 2}, {b + 3}, 0x0),")
        emit_dl(f"ptc_dl_{name.lower()}", body)
        emit_dl(f"ptc_dl_{name.lower()}_fade", body, fade=True)

    # Ramp wedge: five faces, four verts each.
    ramp_body = ["    gsSPVertex(ptc_vtx_ramp, 20, 0),"]
    for f in range(5):
        b = f * 4
        ramp_body.append(f"    gsSP2Triangles({b}, {b + 1}, {b + 2}, 0x0, {b}, {b + 2}, {b + 3}, 0x0),")
    emit_dl("ptc_dl_ramp", ramp_body)
    emit_dl("ptc_dl_ramp_fade", ramp_body, fade=True)

    # Portal target panels are flat decals on the walls; keep them opaque.
    panel_body = []
    for name in ("a", "b", "ma", "mb"):
        panel_body += [
            f"    gsSPVertex(ptc_vtx_panel_{name}, 4, 0),",
            "    gsSP2Triangles(0, 1, 2, 0x0, 0, 2, 3, 0x0),",
        ]
    emit_dl("ptc_dl_panels", panel_body)
    return "\n".join(lines)


def main():
    OUT.mkdir(parents=True, exist_ok=True)

    header = f"""#ifndef PTC_LEVEL_H
#define PTC_LEVEL_H

/* Generated by scripts/gen-ptc-level.py.  All values are world-space N64
 * units (Mario is 160 units tall).  Keep the test assertions in ptc_test.c in
 * sync with these values. */

#define PTC_ROOM_X0 {ROOM['x0']}
#define PTC_ROOM_X1 {ROOM['x1']}
#define PTC_ROOM_Y0 {ROOM['y0']}
#define PTC_ROOM_Y1 {ROOM['y1']}
#define PTC_ROOM_Z0 {ROOM['z0']}
#define PTC_ROOM_Z1 {ROOM['z1']}

/* Platform tops (measured against Mario's jump limits). */
#define PTC_P1_TOP 180
#define PTC_P2_TOP 360
#define PTC_P3_TOP 560
#define PTC_P4_TOP 680
#define PTC_P5_TOP 800

#define PTC_PORTAL_A_X {PORTAL_A[0]}
#define PTC_PORTAL_A_Y {PORTAL_A[1]}
#define PTC_PORTAL_A_Z {PORTAL_A[2]}
#define PTC_PORTAL_B_X {PORTAL_B[0]}
#define PTC_PORTAL_B_Y {PORTAL_B[1]}
#define PTC_PORTAL_B_Z {PORTAL_B[2]}

#define PTC_STAR_X {STAR[0]}
#define PTC_STAR_Y {STAR[1]}
#define PTC_STAR_Z {STAR[2]}

#define PTC_STAR2_X {STAR2[0]}
#define PTC_STAR2_Y {STAR2[1]}
#define PTC_STAR2_Z {STAR2[2]}

#define PTC_MOM_START_X {MOM_MARIO_START[0]}
#define PTC_MOM_START_Y {MOM_MARIO_START[1]}
#define PTC_MOM_START_Z {MOM_MARIO_START[2]}
#define PTC_MOM_PORTAL_A_X {MOM_PORTAL_A[0]}
#define PTC_MOM_PORTAL_A_Y {MOM_PORTAL_A[1]}
#define PTC_MOM_PORTAL_A_Z {MOM_PORTAL_A[2]}
#define PTC_MOM_PORTAL_B_X {MOM_PORTAL_B[0]}
#define PTC_MOM_PORTAL_B_Y {MOM_PORTAL_B[1]}
#define PTC_MOM_PORTAL_B_Z {MOM_PORTAL_B[2]}
#define PTC_MOM_PORTAL_B_NX {MOM_PORTAL_B_NORMAL[0]}
#define PTC_MOM_PORTAL_B_NY {MOM_PORTAL_B_NORMAL[1]}
#define PTC_MOM_PORTAL_B_NZ {MOM_PORTAL_B_NORMAL[2]}
#define PTC_RAMP_X0 {RAMP['x0']}
#define PTC_RAMP_X1 {RAMP['x1']}
#define PTC_RAMP_Z0 {RAMP['z0']}
#define PTC_RAMP_Z1 {RAMP['z1']}
#define PTC_RAMP_Y0 {RAMP['y0']}
#define PTC_RAMP_Y1 {RAMP['y1']}
#define PTC_P7_TOP 1100
#define PTC_P9_TOP 950
#define PTC_P9_X0 -300
#define PTC_P9_X1 300
#define PTC_P9_Z0 150
#define PTC_P9_Z1 560

#define PTC_MIRROR_X (3190.0f)
#define PTC_MARIO_START_X {MARIO_START[0]}
#define PTC_MARIO_START_Y {MARIO_START[1]}
#define PTC_MARIO_START_Z {MARIO_START[2]}
#define PTC_WALK_YAW {WALK_YAW}
#define PTC_STAR_YAW {STAR_YAW}

#endif
"""
    (OUT / "ptc_level.h").write_text(header)

    leveldata = """#include <ultra64.h>
#include "sm64.h"
#include "surface_terrains.h"
#include "make_const_nonconst.h"

#include "levels/ptc/ptc_level.h"

/* 1-unit checkerboard walls (8x8 texels, 4x4 checks, 4 texels per unit). */
""" + emit_walls() + "\n\n" + emit_solid() + "\n\n" + emit_label_vertices() + emit_labels() + "\n\n" + emit_collision() + "\n"
    (OUT / "leveldata.c").write_text(leveldata)

    header_h = """#ifndef PTC_HEADER_H
#define PTC_HEADER_H

#include "types.h"

extern const GeoLayout ptc_geo[];
extern const Gfx ptc_dl_room[];
extern const Gfx ptc_dl_panels[];
extern const Gfx ptc_dl_labels[];
extern const Gfx ptc_dl_wall_0[];
extern const Gfx ptc_dl_wall_0_fade[];
extern const Gfx ptc_dl_wall_1[];
extern const Gfx ptc_dl_wall_1_fade[];
extern const Gfx ptc_dl_wall_2[];
extern const Gfx ptc_dl_wall_2_fade[];
extern const Gfx ptc_dl_wall_3[];
extern const Gfx ptc_dl_wall_3_fade[];
extern const Gfx ptc_dl_p1[];
extern const Gfx ptc_dl_p1_fade[];
extern const Gfx ptc_dl_p2[];
extern const Gfx ptc_dl_p2_fade[];
extern const Gfx ptc_dl_p3[];
extern const Gfx ptc_dl_p3_fade[];
extern const Gfx ptc_dl_p4[];
extern const Gfx ptc_dl_p4_fade[];
extern const Gfx ptc_dl_p5[];
extern const Gfx ptc_dl_p5_fade[];
extern const Gfx ptc_dl_p6[];
extern const Gfx ptc_dl_p6_fade[];
extern const Gfx ptc_dl_p7[];
extern const Gfx ptc_dl_p7_fade[];
extern const Gfx ptc_dl_p8[];
extern const Gfx ptc_dl_p8_fade[];
extern const Gfx ptc_dl_wr_w[];
extern const Gfx ptc_dl_wr_w_fade[];
extern const Gfx ptc_dl_wr_e[];
extern const Gfx ptc_dl_wr_e_fade[];
extern const Gfx ptc_dl_jp_w[];
extern const Gfx ptc_dl_jp_w_fade[];
extern const Gfx ptc_dl_jp_e[];
extern const Gfx ptc_dl_jp_e_fade[];
extern const Gfx ptc_dl_jp_s[];
extern const Gfx ptc_dl_jp_s_fade[];
extern const Gfx ptc_dl_jp_n[];
extern const Gfx ptc_dl_jp_n_fade[];
extern const Gfx ptc_dl_jp_pad[];
extern const Gfx ptc_dl_jp_pad_fade[];
extern const Gfx ptc_dl_mirror[];
extern const Gfx ptc_dl_p9[];
extern const Gfx ptc_dl_p9_fade[];
extern const Gfx ptc_dl_ramp[];
extern const Gfx ptc_dl_ramp_fade[];
extern const Collision ptc_collision[];
extern const LevelScript level_ptc_entry[];

#endif
"""
    (OUT / "header.h").write_text(header_h)

    geo = """#include <ultra64.h>
#include "sm64.h"
#include "geo_commands.h"

#include "game/level_geo.h"
#include "game/geo_misc.h"
#include "game/camera.h"
#include "game/moving_texture.h"
#include "game/screen_transition.h"
#include "game/paintings.h"

#include "make_const_nonconst.h"
#include "levels/ptc/header.h"

extern Gfx *ptc_geo_obj(s32 callContext, struct GraphNode *node, UNUSED void *context);
extern Gfx *ptc_geo_mirror_mario(s32 callContext, struct GraphNode *node, UNUSED void *context);

const GeoLayout ptc_geo[] = {
   GEO_NODE_SCREEN_AREA(10, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2),
   GEO_OPEN_NODE(),
      GEO_ZBUFFER(0),
      GEO_OPEN_NODE(),
         GEO_NODE_ORTHO(100),
         GEO_OPEN_NODE(),
            GEO_BACKGROUND_COLOR(0x0001),
         GEO_CLOSE_NODE(),
      GEO_CLOSE_NODE(),
      GEO_ZBUFFER(1),
      GEO_OPEN_NODE(),
         GEO_CAMERA_FRUSTUM_WITH_FUNC(45, 100, 20000, geo_camera_fov),
         GEO_OPEN_NODE(),
            GEO_CAMERA(1, 0, 2000, 6000, 0, 0, 0, geo_camera_main),
            GEO_OPEN_NODE(),
               GEO_DISPLAY_LIST(LAYER_OPAQUE, ptc_dl_room),
{ASM_NODES}
               GEO_DISPLAY_LIST(LAYER_OPAQUE, ptc_dl_panels),
               GEO_DISPLAY_LIST(LAYER_OPAQUE, ptc_dl_labels),
               GEO_RENDER_OBJ(),
            GEO_CLOSE_NODE(),
         GEO_CLOSE_NODE(),
      GEO_CLOSE_NODE(),
   GEO_CLOSE_NODE(),
   GEO_END(),
};
"""
    asm = "".join(f"               GEO_ASM({i}, ptc_geo_obj),\n"
                  for i in range(len(PLATFORMS) + 1 + 4))
    asm += ("            GEO_ASM(21, ptc_geo_obj),\n"
            "            GEO_ASM(22, ptc_geo_mirror_mario),\n")
    (OUT / "geo.c").write_text(geo.replace("{ASM_NODES}", asm))

    script = """#include <ultra64.h>
#include "sm64.h"
#include "behavior_data.h"
#include "model_ids.h"
#include "seq_ids.h"
#include "segment_symbols.h"
#include "level_commands.h"

#include "game/level_update.h"

#include "levels/scripts.h"
#include "levels/ptc/header.h"
#include "levels/ptc/ptc_level.h"
#include "actors/common0.h"

#include "make_const_nonconst.h"

const LevelScript level_ptc_entry[] = {
    INIT_LEVEL(),
    LOAD_MIO0(/*seg*/ 0x07, _ptc_segment_7SegmentRomStart, _ptc_segment_7SegmentRomEnd),
    LOAD_RAW(/*seg*/ 0x13, _behaviorSegmentRomStart, _behaviorSegmentRomEnd),
    ALLOC_LEVEL_POOL(),
    /* The goomba-lemming finale needs this model. */
    LOAD_MODEL_FROM_GEO(MODEL_GOOMBA, goomba_geo),
    MARIO(/*model*/ MODEL_MARIO, /*behParam*/ 0x00000001, /*beh*/ bhvMario),

    AREA(/*index*/ 1, ptc_geo),
        OBJECT(/*model*/ MODEL_STAR, /*pos*/ PTC_STAR_X, PTC_STAR_Y, PTC_STAR_Z,
               /*angle*/ 0, 0, 0, /*behParam*/ 0x00000000, /*beh*/ bhvPtcDebugStar),
        OBJECT(/*model*/ MODEL_STAR, /*pos*/ PTC_STAR2_X, PTC_STAR2_Y, PTC_STAR2_Z,
               /*angle*/ 0, 0, 0, /*behParam*/ 0x01000000, /*beh*/ bhvPtcDebugStar),
        TERRAIN(/*terrainData*/ ptc_collision),
        SET_BACKGROUND_MUSIC(/*settingsPreset*/ 0x0000, /*seq*/ SEQ_LEVEL_INSIDE_CASTLE),
        TERRAIN_TYPE(/*terrainType*/ TERRAIN_STONE),
    END_AREA(),

    FREE_LEVEL_POOL(),
    MARIO_POS(/*area*/ 1, /*yaw*/ PTC_WALK_YAW, /*pos*/ PTC_MARIO_START_X, PTC_MARIO_START_Y,
              PTC_MARIO_START_Z),
    CALL(/*arg*/ 0, /*func*/ lvl_init_or_update),
    CALL_LOOP(/*arg*/ 1, /*func*/ lvl_init_or_update),
    CLEAR_LEVEL(),
    SLEEP_BEFORE_EXIT(/*frames*/ 1),
    EXIT(),
};
"""
    (OUT / "script.c").write_text(script)

    print(f"wrote {OUT}/ptc_level.h")
    print(f"wrote {OUT}/leveldata.c")
    print(f"wrote {OUT}/header.h")
    print(f"wrote {OUT}/geo.c")
    print(f"wrote {OUT}/script.c")


if __name__ == "__main__":
    main()
