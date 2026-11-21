#!/usr/bin/env python3
"""Generate a minimal, unambiguous portal test level.

Room: 4000 x 1600 x 4000 units, so the chase camera always stays inside it.
Four coloured floor quadrants give orientation; a magenta portal mount panel
sits at z=-300 and a second at z=+600; a high-contrast checker chart stands at
z=-800 facing +Z.  No textures, no platforms, no objects, no stars.

The portrait is deliberate: the room is much bigger than the portals so the
camera never ends up outside the walls (which used to show a flat wall colour
and looked like a broken relay).
"""
from pathlib import Path

OUT = Path(__file__).resolve().parents[1] / "app/jni/src/levels/ptctest"
OUT.mkdir(parents=True, exist_ok=True)

X0, X1 = -2000, 2000
Y0, Y1 = 0, 1600
Z0, Z1 = -2000, 2000
# Portal mount panels (thin quads, no collision needed).
PA_X0, PA_X1 = -360, 360
PA_Y0, PA_Y1 = 0, 560
PA_Z = -300          # portal A panel, faces +Z (towards Mario)
PB_Z = 600           # portal B panel, faces -Z (into the room)

COL = {
    "floor_r": (220, 60, 60), "floor_g": (60, 200, 80),
    "floor_b": (60, 110, 230), "floor_y": (230, 205, 70),
    "ceil": (90, 90, 100),
    "wall_x0": (140, 90, 90), "wall_x1": (90, 140, 95),
    "wall_z0": (85, 105, 150), "wall_z1": (150, 135, 85),
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
    # Floor quadrants use the +Y top-face winding.
    ("floor_r", quad((0, 0, Z0), (X0, 0, Z0), (X0, 0, 0), (0, 0, 0))),
    ("floor_g", quad((X1, 0, Z0), (0, 0, Z0), (0, 0, 0), (X1, 0, 0))),
    ("floor_b", quad((0, 0, 0), (X0, 0, 0), (X0, 0, Z1), (0, 0, Z1))),
    ("floor_y", quad((X1, 0, 0), (0, 0, 0), (0, 0, Z1), (X1, 0, Z1))),
    ("ceil", quad((X1, Y1, Z0), (X1, Y1, Z1), (X0, Y1, Z1), (X0, Y1, Z0))),
    # Wall winding faces *inwards*: single-sided quads are culled from behind.
    ("wall_x0", quad((X0, Y0, Z1), (X0, Y0, Z0), (X0, Y1, Z0), (X0, Y1, Z1))),
    ("wall_x1", quad((X1, Y0, Z0), (X1, Y0, Z1), (X1, Y1, Z1), (X1, Y1, Z0))),
    ("wall_z0", quad((X0, Y0, Z0), (X1, Y0, Z0), (X1, Y1, Z0), (X0, Y1, Z0))),
    ("wall_z1", quad((X1, Y0, Z1), (X0, Y0, Z1), (X0, Y1, Z1), (X1, Y1, Z1))),
]
# No visible mount panel: the portal needs invisible collision to raycast
# against, but a solid panel behind the source portal would also fill the
# middle of the relay view (the destination camera sits on the same side of
# the wall), making the decal look flat magenta.
portal_faces = []

# High-contrast chart between portal B and the relay camera.  The relay camera
# sits at the mirror of the chase camera (z ~ +1843 for this layout), so a panel
# at z=+1500 fills its whole view; the chase camera at z~951 cannot see it at
# all because it is behind it.  Every checker pixel on screen therefore comes
# from the portal relay.
pat_faces = []
# Sized for the relay camera, which sits ~343 units in front of it, so the
# grid fills that view; the portal decal then samples several cells and never
# a single flat colour.
PAT_X0, PAT_X1 = -150, 150
PAT_Y0, PAT_Y1 = -20, 340
PAT_Z = 1500
COLS, ROWS = 24, 24
cw = (PAT_X1 - PAT_X0) // COLS
ch = (PAT_Y1 - PAT_Y0) // ROWS
for iy in range(ROWS):
    for ix in range(COLS):
        x0 = PAT_X0 + ix * cw
        x1 = x0 + cw
        y0 = PAT_Y0 + iy * ch
        y1 = y0 + ch
        if ix == COLS // 2:
            key = "pat_r"       # vertical marker for left/right
        elif iy == ROWS // 2:
            key = "pat_g"       # horizontal marker for up/down
        else:
            key = "pat_w" if (ix + iy) % 2 == 0 else "pat_b"
        # Fixed-z quad in (x0,y0) -> (x1,y0) order faces +Z, towards the relay
        # camera that sits behind the portals.
        portal_faces.append((key, quad((x0, y0, PAT_Z), (x1, y0, PAT_Z),
                                       (x1, y1, PAT_Z), (x0, y1, PAT_Z))))
pattern_faces = []

# Visuals are generated at runtime by src/game/ptc_scene.c (knobs over adb),
# so the ROM only carries this level's collision floor and portal mounts.
all_faces = []
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
if not all_faces:
    lines.append("    /* unused: the test scene is generated at runtime */")
    lines.append("    {{{0, 0, 0}, 0, {0, 0}, {0, 0, 0, 0}}},")
for key, pts in all_faces:
    for p in pts:
        lines.append(v(*p, COL[key]))
lines.append("};")
# Leave RDP state alone: disabling texturing here (G_OFF / G_CC_SHADE) leaked
# into Mario's model display lists and rendered him untextured.
lines += ["", "const Gfx ptctest_dl[] = {", "    gsDPPipeSync(),"]

for i in range(len(all_faces)):
    b = i * 4
    lines += [f"    gsSPVertex(ptctest_vtx + {b}, 4, 0),",
              "    gsSP2Triangles(0, 1, 2, 0x0, 0, 2, 3, 0x0),"]

lines += ["    gsSPEndDisplayList(),", "};", ""]

# ---------------- collision ----------------
# Walls match the runtime scene defaults (half extent 1000, height 800) so the
# room is actually enclosed and the chase camera has surfaces to collide with.
WX0, WX1 = -1000, 1000
WZ0, WZ1 = -1000, 1000
WY1 = 800
WT = 40
col_faces = [
    # One solid +Y floor quad; a closed box's bottom face is not a floor.
    ("floor", quad((X1, Y0, Z0), (X0, Y0, Z0), (X0, Y0, Z1), (X1, Y0, Z1))),
    # Four enclosing walls (thin boxes) so Mario cannot walk out of the room.
    ("wall_x0", quad((WX0, 0, WZ1), (WX0, 0, WZ0), (WX0, WY1, WZ0), (WX0, WY1, WZ1))),
    ("wall_x1", quad((WX1, 0, WZ0), (WX1, 0, WZ1), (WX1, WY1, WZ1), (WX1, WY1, WZ0))),
    ("wall_z0", quad((WX0, 0, WZ0), (WX1, 0, WZ0), (WX1, WY1, WZ0), (WX0, WY1, WZ0))),
    ("wall_z1", quad((WX1, 0, WZ1), (WX0, 0, WZ1), (WX0, WY1, WZ1), (WX1, WY1, WZ1))),
    # Two thin mounts side by side on the *same* wall, ~180 apart centre to
    # centre (portal half-width is 67.5, so this leaves a visible gap).
] + box_faces(-180, PA_Y0, PA_Z - 20, 0, PA_Y1, PA_Z) \
  + box_faces(0, PA_Y0, PA_Z - 20, 180, PA_Y1, PA_Z)
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
#include "actors/common0.h"
#include "make_const_nonconst.h"

const LevelScript level_ptctest_entry[] = {
    INIT_LEVEL(),
    LOAD_RAW(/*seg*/ 0x13, _behaviorSegmentRomStart, _behaviorSegmentRomEnd),
    ALLOC_LEVEL_POOL(),
    /* The Goomba demo needs this model; loading it explicitly avoids relying
     * on a cross-segment JUMP_LINK into script_func_global_1. */
    LOAD_MODEL_FROM_GEO(MODEL_GOOMBA, goomba_geo),
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
