#!/usr/bin/env python3
"""Generate a minimal, unambiguous portal test level.

Room: 1200 x 600 x 1200 units.  Six coloured walls, four coloured floor
quadrants, and a freestanding magenta portal wall at z=-300.  No textures, no
platforms, no objects, no stars.
"""
from pathlib import Path

OUT = Path(__file__).resolve().parents[1] / "app/jni/src/levels/ptctest"
OUT.mkdir(parents=True, exist_ok=True)

X0, X1 = -600, 600
Y0, Y1 = 0, 600
Z0, Z1 = -600, 600
# Freestanding portal wall (front face at z=-280, normal +Z)
PX0, PX1 = -300, 300
PY0, PY1 = 0, 400
PZ0, PZ1 = -320, -280

COL = {
    "floor_r": (220, 60, 60), "floor_g": (60, 200, 80),
    "floor_b": (60, 110, 230), "floor_y": (230, 205, 70),
    "ceil": (90, 90, 100),
    "wall_x0": (200, 70, 70), "wall_x1": (70, 200, 80),
    "wall_z0": (70, 120, 220), "wall_z1": (220, 190, 70),
    "portal": (240, 60, 240), "portal_top": (255, 255, 255),
    "pat_w": (225, 225, 225), "pat_b": (35, 35, 35),
    "pat_r": (220, 50, 50), "pat_g": (50, 200, 70),
}


def v(x, y, z, rgb):
    r, g, b = rgb
    return f"    {{{{{{{x:5d}, {y:5d}, {z:5d}}}, 0, {{0, 0}}, {{0x{r:02X}, 0x{g:02X}, 0x{b:02X}, 0xFF}}}}}},"


def quad(a, b, c, d):
    return [a, b, c, d]


def box_faces(x0, y0, z0, x1, y1, z1):
    return [
        ("top",    quad((x1, y1, z0), (x0, y1, z0), (x0, y1, z1), (x1, y1, z1))),
        ("bottom", quad((x1, y0, z0), (x1, y0, z1), (x0, y0, z1), (x0, y0, z0))),
        ("front",  quad((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0))),
        ("back",   quad((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1))),
        ("left",   quad((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0))),
        ("right",  quad((x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1))),
    ]


room_faces = [
    ("floor_r", quad((0, 0, 0), (X0, 0, 0), (X0, 0, Z0), (0, 0, Z0))),
    ("floor_g", quad((X1, 0, 0), (0, 0, 0), (0, 0, Z0), (X1, 0, Z0))),
    ("floor_b", quad((0, 0, Z1), (X0, 0, Z1), (X0, 0, 0), (0, 0, 0))),
    ("floor_y", quad((X1, 0, Z1), (0, 0, Z1), (0, 0, 0), (X1, 0, 0))),
    ("ceil", quad((X1, Y1, Z0), (X0, Y1, Z0), (X0, Y1, Z1), (X1, Y1, Z1))),
    ("wall_x0", quad((X0, Y0, Z0), (X0, Y0, Z1), (X0, Y1, Z1), (X0, Y1, Z0))),
    ("wall_x1", quad((X1, Y0, Z1), (X1, Y0, Z0), (X1, Y1, Z0), (X1, Y1, Z1))),
    ("wall_z0", quad((X1, Y0, Z0), (X0, Y0, Z0), (X0, Y1, Z0), (X1, Y1, Z0))),
    ("wall_z1", quad((X0, Y0, Z1), (X1, Y0, Z1), (X1, Y1, Z1), (X0, Y1, Z1))),
]
portal_faces = []
for name, pts in box_faces(PX0, PY0, PZ0, PX1, PY1, PZ1):
    key = "portal_top" if name in ("top", "bottom") else "portal"
    portal_faces.append((key, pts))

# High-contrast checker plane for relay/parallax analysis.  It sits at z=0,
# between the two portals, and faces +Z (toward portal B's POV).
pattern_faces = []
for iy in range(8):
    for ix in range(8):
        x0 = -400 + ix * 100
        x1 = x0 + 100
        y0 = 100 + iy * 50
        y1 = y0 + 50
        if ix == 4:
            key = "pat_r"
        elif iy == 4:
            key = "pat_g"
        else:
            key = "pat_w" if (ix + iy) % 2 == 0 else "pat_b"
        pattern_faces.append((key, quad((x1, y0, 0), (x0, y0, 0), (x0, y1, 0), (x1, y1, 0))))

all_faces = room_faces + portal_faces + pattern_faces
faces = [pts for _, pts in all_faces]

# ---------------- header ----------------
header = """#ifndef PTCTEST_HEADER_H
#define PTCTEST_HEADER_H

#include "types.h"

extern const GeoLayout ptctest_geo[];
extern const Gfx ptctest_dl[];
extern const Collision ptctest_collision[];
extern const LevelScript level_ptctest_entry[];

#define PTCTEST_MARIO_X 0
#define PTCTEST_MARIO_Y 0
#define PTCTEST_MARIO_Z 0
#define PTCTEST_MARIO_YAW 0x8000
#define PTCTEST_PORTAL_A_X 0
#define PTCTEST_PORTAL_A_Y 200
#define PTCTEST_PORTAL_A_Z -280
#define PTCTEST_PORTAL_B_X 0
#define PTCTEST_PORTAL_B_Y 200
#define PTCTEST_PORTAL_B_Z 599

#endif
"""
(OUT / "header.h").write_text(header)

# ---------------- geometry ----------------
lines = [
    "#include <ultra64.h>",
    "#include \"sm64.h\"",
    "#include \"surface_terrains.h\"",
    "#include \"make_const_nonconst.h\"",
    "#include \"levels/ptctest/header.h\"",
    "",
]
lines.append("static const Vtx ptctest_vtx[] = {")
for key, pts in all_faces:
    for p in pts:
        lines.append(v(*p, COL[key]))
lines.append("};")
lines += ["", "const Gfx ptctest_dl[] = {",
          "    gsDPPipeSync(),",
          "    gsDPSetCombineMode(G_CC_SHADE, G_CC_SHADE),",
          "    gsSPClearGeometryMode(G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR),",
          "    gsSPSetGeometryMode(G_SHADE | G_CULL_BACK),",
          "    gsSPTexture(0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF),",
          "    gsDPSetRenderMode(G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2),"]

for i in range(len(all_faces)):
    b = i * 4
    lines += [f"    gsSPVertex(ptctest_vtx + {b}, 4, 0),",
              "    gsSP2Triangles(0, 1, 2, 0x0, 0, 2, 3, 0x0),"]
lines += ["    gsSPEndDisplayList(),", "};", ""]

# ---------------- collision ----------------
col_faces = box_faces(X0, Y0, Z0, X1, Y1, Z1) + box_faces(PX0, PY0, PZ0, PX1, PY1, PZ1)
lines.append("const Collision ptctest_collision[] = {")
lines.append("    COL_INIT(),")
lines.append(f"    COL_VERTEX_INIT({len(col_faces) * 4}),")
for _name, pts in col_faces:
    for x, y, z in pts:
        lines.append(f"    COL_VERTEX({x}, {y}, {z}),")
lines.append(f"    COL_TRI_INIT(SURFACE_DEFAULT, {len(col_faces) * 2}),")
for i in range(len(col_faces)):
    b = i * 4
    lines.append(f"    COL_TRI({b}, {b + 1}, {b + 2}),")
    lines.append(f"    COL_TRI({b}, {b + 2}, {b + 3}),")
lines.append("    COL_TRI_STOP(),")
lines.append("    COL_END(),")
lines.append("};")
(OUT / "leveldata.c").write_text("\n".join(lines) + "\n")

# ---------------- geo ----------------
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
#include "levels/ptctest/header.h"

const GeoLayout ptctest_geo[] = {
   GEO_NODE_SCREEN_AREA(10, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2),
   GEO_OPEN_NODE(),
      GEO_ZBUFFER(0),
      GEO_OPEN_NODE(),
         GEO_NODE_ORTHO(100),
         GEO_OPEN_NODE(),
            GEO_BACKGROUND_COLOR(0x0000),
         GEO_CLOSE_NODE(),
      GEO_CLOSE_NODE(),
      GEO_ZBUFFER(1),
      GEO_OPEN_NODE(),
         GEO_CAMERA_FRUSTUM_WITH_FUNC(45, 100, 20000, geo_camera_fov),
         GEO_OPEN_NODE(),
            GEO_CAMERA(1, 0, 2000, 6000, 0, 0, 0, geo_camera_main),
            GEO_OPEN_NODE(),
               GEO_DISPLAY_LIST(LAYER_OPAQUE, ptctest_dl),
               GEO_RENDER_OBJ(),
            GEO_CLOSE_NODE(),
         GEO_CLOSE_NODE(),
      GEO_CLOSE_NODE(),
   GEO_CLOSE_NODE(),
   GEO_END(),
};
"""
(OUT / "geo.c").write_text(geo)

# ---------------- script ----------------
script = """#include <ultra64.h>
#include "sm64.h"
#include "behavior_data.h"
#include "model_ids.h"
#include "seq_ids.h"
#include "segment_symbols.h"
#include "level_commands.h"
#include "game/level_update.h"
#include "levels/scripts.h"
#include "levels/ptctest/header.h"
#include "make_const_nonconst.h"

const LevelScript level_ptctest_entry[] = {
    INIT_LEVEL(),
    LOAD_RAW(/*seg*/ 0x13, _behaviorSegmentRomStart, _behaviorSegmentRomEnd),
    ALLOC_LEVEL_POOL(),
    MARIO(/*model*/ MODEL_MARIO, /*behParam*/ 0x00000001, /*beh*/ bhvMario),

    AREA(/*index*/ 1, ptctest_geo),
        TERRAIN(/*terrainData*/ ptctest_collision),
        SET_BACKGROUND_MUSIC(/*settingsPreset*/ 0x0000, /*seq*/ SEQ_LEVEL_INSIDE_CASTLE),
        TERRAIN_TYPE(/*terrainType*/ TERRAIN_STONE),
    END_AREA(),

    FREE_LEVEL_POOL(),
    MARIO_POS(/*area*/ 1, /*yaw*/ PTCTEST_MARIO_YAW,
              /*pos*/ PTCTEST_MARIO_X, PTCTEST_MARIO_Y, PTCTEST_MARIO_Z),
    CALL(/*arg*/ 0, /*func*/ lvl_init_or_update),
    CALL_LOOP(/*arg*/ 1, /*func*/ lvl_init_or_update),
    CLEAR_LEVEL(),
    SLEEP_BEFORE_EXIT(/*frames*/ 1),
    EXIT(),
};
"""
(OUT / "script.c").write_text(script)
print(f"wrote {OUT} ({len(all_faces)} quads, {len(col_faces)} collision boxes)")
