/**
 * Runtime Portal Test Chamber scene + debug knob table.
 *
 * The empty test level (LEVEL_PTCTEST) bakes only its collision floor into the
 * ROM.  Everything visible - the room box, the coloured floor quadrants and the
 * checker backdrop the relay camera sees - is generated here from runtime
 * parameters, so a test does not need a rebuild to change room size, chart
 * placement or checker density.
 *
 * Knobs are set over adb:
 *   adb shell $B --es cmd ptc --es action "knob scene.chart_z 1200"
 *   adb shell $B --es cmd ptc --es action reload
 * `reload` re-reads /sdcard/Android/data/com.vdavid003.sm64port/files/ptc.cfg
 * (or /data/local/tmp/ptc.cfg), one "name value" pair per line.
 */

#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "sm64.h"
#include "ptc_scene.h"

#include "game/area.h"
#include "game/game_init.h"
#include "game/level_update.h"
#include "game/rendering_graph_node.h"
#include "game/memory.h"
#include "level_table.h"

/* --------------------------------------------------------------- knobs */

float g_ptc_decal_offset = 12.0f;
int   g_ptc_decal_depth = 1;
float g_ptc_select_maxdeg = 75.0f;
int   g_ptc_select_facing = 1;

extern int g_portal_debug_color;      /* solid relay colour */
extern int gfx_portal_debug_blit;     /* full-screen relay FBO */
extern int g_portal_debug_align;      /* magenta source fan */
extern int gfx_stereo_same_both;      /* 0 off, 1 = L, 2 = R */

typedef struct {
    const char *name;
    int is_int;
    void *ptr;
} PtcKnob;

static int s_scene_on = 1;
static float s_room = 1000.0f;      /* half extent in X and Z */
static float s_room_h = 800.0f;
static float s_chart_z = 1500.0f;
static float s_chart_w = 300.0f;
static float s_chart_h = 360.0f;
static float s_chart_x = 270.0f;     /* chart centre X (relay camera axis) */
static float s_chart_y = 160.0f;     /* chart centre height */
static int s_chart_cols = 16;
static int s_chart_rows = 16;
/* Diagnostic cubes: one in front of each portal, so each relay window should
 * show the *other* portal's cube (A -> red, B -> blue). */
static int s_cubes_on = 1;

/* 600 quads covers the room plus a 24x24 chart. */
#define PTC_SCENE_MAX_QUADS 1260
static Vtx s_vtx[PTC_SCENE_MAX_QUADS * 4];
static int s_quad_count;
static int s_dirty = 1;
static int s_build_failed;

static const PtcKnob s_knobs[] = {
    { "scene.on",          1, &s_scene_on },
    { "scene.room",        0, &s_room },
    { "scene.height",      0, &s_room_h },
    { "scene.chart_z",     0, &s_chart_z },
    { "scene.chart_w",     0, &s_chart_w },
    { "scene.chart_h",     0, &s_chart_h },
    { "scene.chart_x",     0, &s_chart_x },
    { "scene.chart_y",     0, &s_chart_y },
    { "scene.chart_cols",  1, &s_chart_cols },
    { "scene.chart_rows",  1, &s_chart_rows },
    { "scene.cubes",       1, &s_cubes_on },
    { "decal.offset",      0, &g_ptc_decal_offset },
    { "decal.depth",       1, &g_ptc_decal_depth },
    { "relay.solid",       1, &g_portal_debug_color },
    { "relay.blit",        1, &gfx_portal_debug_blit },
    { "fan.src",           1, &g_portal_debug_align },
    { "select.maxdeg",     0, &g_ptc_select_maxdeg },
    { "select.facing",     1, &g_ptc_select_facing },
    { "eye",               1, &gfx_stereo_same_both },
};

void ptc_scene_invalidate(void) {
    s_dirty = 1;
}

static void scene_mark_dirty(void) {
    s_dirty = 1;
}

int ptc_knob_set(const char *name, float value) {
    if (name == NULL) {
        return 0;
    }
    for (unsigned i = 0; i < sizeof(s_knobs) / sizeof(s_knobs[0]); i++) {
        if (strcmp(name, s_knobs[i].name) != 0) {
            continue;
        }
        if (s_knobs[i].is_int) {
            *(int *)s_knobs[i].ptr = (int)(value < 0.0f ? value - 0.5f : value + 0.5f);
        } else {
            *(float *)s_knobs[i].ptr = value;
        }
        scene_mark_dirty();
        return 1;
    }
    return 0;
}

int ptc_knob_get(const char *name, float *value) {
    for (unsigned i = 0; i < sizeof(s_knobs) / sizeof(s_knobs[0]); i++) {
        if (strcmp(name, s_knobs[i].name) == 0) {
            if (value != NULL) {
                *value = s_knobs[i].is_int ? (float)*(int *)s_knobs[i].ptr
                                           : *(float *)s_knobs[i].ptr;
            }
            return 1;
        }
    }
    return 0;
}

void ptc_knob_reload(void) {
    static const char *const paths[] = {
        "/sdcard/Android/data/com.vdavid003.sm64port/files/ptc.cfg",
        "/data/local/tmp/ptc.cfg",
    };
    FILE *f = NULL;
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]) && f == NULL; i++) {
        f = fopen(paths[i], "r");
    }
    if (f == NULL) {
        return;
    }
    char line[160];
    while (fgets(line, sizeof(line), f) != NULL) {
        char *hash = strchr(line, '#');
        if (hash != NULL) *hash = '\0';
        char name[48];
        float value;
        if (sscanf(line, "%47s %f", name, &value) == 2) {
            ptc_knob_set(name, value);
        }
    }
    fclose(f);
}

void ptc_knob_dump(char *buf, int len) {
    if (buf == NULL || len <= 0) {
        return;
    }
    snprintf(buf, len,
             "knobs scene=%d room=%.0f h=%.0f chart=%.0f/%.0fx%.0f@%.0f,%.0f %dx%d "
             "decal.off=%.1f depth=%d sel=%.0f/%d solid=%d blit=%d fan=%d cubes=%d",
             s_scene_on, s_room, s_room_h, s_chart_z, s_chart_w, s_chart_h,
             s_chart_x, s_chart_y, s_chart_cols, s_chart_rows, g_ptc_decal_offset,
             g_ptc_decal_depth, g_ptc_select_maxdeg, g_ptc_select_facing,
             g_portal_debug_color, gfx_portal_debug_blit, g_portal_debug_align,
             s_cubes_on);
}

int ptc_scene_enabled(void) {
    return s_scene_on;
}

void ptc_scene_toggle(void) {
    s_scene_on = !s_scene_on;
    s_dirty = 1;
}

static void scene_quad(int *n, float x0, float y0, float z0,
                       float x1, float y1, float z1,
                       float x2, float y2, float z2,
                       float x3, float y3, float z3,
                       u8 r, u8 g, u8 b) {
    if (*n + 4 > PTC_SCENE_MAX_QUADS * 4) {
        s_build_failed = 1;
        return;
    }
    const float xs[4] = { x0, x1, x2, x3 };
    const float ys[4] = { y0, y1, y2, y3 };
    const float zs[4] = { z0, z1, z2, z3 };
    for (int i = 0; i < 4; i++) {
        Vtx *v = &s_vtx[(*n)++];
        v->v.ob[0] = (s16)xs[i];
        v->v.ob[1] = (s16)ys[i];
        v->v.ob[2] = (s16)zs[i];
        v->v.flag = 0;
        v->v.tc[0] = 0;
        v->v.tc[1] = 0;
        v->v.cn[0] = r;
        v->v.cn[1] = g;
        v->v.cn[2] = b;
        v->v.cn[3] = 0xFF;
    }
}

/* Axis-aligned box with outward faces; `half` is the half extent (1 "unit"
 * here is 100 world units, i.e. one Mario-height-ish block). */
static void scene_box(int *n, float cx, float cy, float cz, float half,
                      u8 r, u8 g, u8 b) {
    float x0 = cx - half, x1 = cx + half;
    float y0 = cy - half, y1 = cy + half;
    float z0 = cz - half, z1 = cz + half;
    /* top / bottom */
    scene_quad(n, x1, y1, z0, x0, y1, z0, x0, y1, z1, x1, y1, z1, r, g, b);
    scene_quad(n, x1, y0, z1, x0, y0, z1, x0, y0, z0, x1, y0, z0, r, g, b);
    /* +Z / -Z */
    scene_quad(n, x0, y0, z1, x1, y0, z1, x1, y1, z1, x0, y1, z1, r, g, b);
    scene_quad(n, x1, y0, z0, x0, y0, z0, x0, y1, z0, x1, y1, z0, r, g, b);
    /* +X / -X */
    scene_quad(n, x1, y0, z1, x1, y0, z0, x1, y1, z0, x1, y1, z1, r, g, b);
    scene_quad(n, x0, y0, z0, x0, y0, z1, x0, y1, z1, x0, y1, z0, r, g, b);
}

static void scene_build(void) {
    int n = 0;
    s_build_failed = 0;

    const float X = s_room;
    const float Z = s_room;
    const float H = s_room_h;

    /* Floor quadrants (top faces, +Y). */
    scene_quad(&n, 0, 0, -Z,  -X, 0, -Z,  -X, 0, 0,   0, 0, 0,   220, 60, 60);
    scene_quad(&n, X, 0, -Z,   0, 0, -Z,   0, 0, 0,   X, 0, 0,    60, 200, 80);
    scene_quad(&n, 0, 0, 0,   -X, 0, 0,   -X, 0, Z,   0, 0, Z,    60, 110, 230);
    scene_quad(&n, X, 0, 0,    0, 0, 0,    0, 0, Z,   X, 0, Z,   230, 205, 70);

    /* Ceiling (faces down). */
    scene_quad(&n, X, H, -Z,  X, H, Z,  -X, H, Z,  -X, H, -Z,   90, 90, 100);

    /* Walls, wound to face inwards. */
    scene_quad(&n, -X, 0, Z,  -X, 0, -Z,  -X, H, -Z,  -X, H, Z,   140, 90, 90);
    scene_quad(&n, X, 0, -Z,  X, 0, Z,   X, H, Z,   X, H, -Z,    90, 140, 95);
    scene_quad(&n, -X, 0, -Z,  X, 0, -Z,  X, H, -Z,  -X, H, -Z,  85, 105, 150);
    scene_quad(&n, X, 0, Z,  -X, 0, Z,  -X, H, Z,  X, H, Z,     150, 135, 85);

    /* Checker backdrop.  It sits between portal B and the relay camera, so it
     * fills the destination view while staying behind the chase camera.  The
     * quads face +Z, towards the relay camera. */
    int cols = s_chart_cols < 1 ? 1 : (s_chart_cols > 24 ? 24 : s_chart_cols);
    int rows = s_chart_rows < 1 ? 1 : (s_chart_rows > 24 ? 24 : s_chart_rows);
    float cw = s_chart_w / (float)cols;
    float ch = s_chart_h / (float)rows;
    float cx0 = s_chart_x - s_chart_w * 0.5f;
    float cy0 = s_chart_y - s_chart_h * 0.5f;
    for (int iy = 0; iy < rows; iy++) {
        for (int ix = 0; ix < cols; ix++) {
            float x0 = cx0 + ix * cw;
            float x1 = x0 + cw;
            float y0 = cy0 + iy * ch;
            float y1 = y0 + ch;
            u8 r, g, b;
            if (ix == cols / 2) {
                r = 220; g = 50; b = 50;          /* vertical marker */
            } else if (iy == rows / 2) {
                r = 50; g = 200; b = 70;          /* horizontal marker */
            } else if (((ix + iy) & 1) == 0) {
                r = 225; g = 225; b = 225;
            } else {
                r = 35; g = 35; b = 35;
            }
            scene_quad(&n, x0, y0, s_chart_z, x1, y0, s_chart_z,
                       x1, y1, s_chart_z, x0, y1, s_chart_z, r, g, b);
            /* Back face too: with same-surface portals the relay camera can
             * end up on either side of the backdrop. */
            scene_quad(&n, x1, y0, s_chart_z, x0, y0, s_chart_z,
                       x0, y1, s_chart_z, x1, y1, s_chart_z, r, g, b);
        }
    }

    /* Diagnostic cubes 150 units in front of each portal mount. */
    if (s_cubes_on) {
        /* Offset sideways so each cube sits in front of its portal without
         * covering the oval: the window stays visible. */
        scene_box(&n, -190.0f, 140.0f, -150.0f, 50.0f, 40, 80, 220);  /* blue @ A */
        scene_box(&n, 190.0f, 140.0f, -150.0f, 50.0f, 220, 40, 40);   /* red  @ B */
    }

    s_quad_count = n / 4;
    s_dirty = 0;
}

void ptc_scene_render(void) {
    if (!s_scene_on || gCurrLevelNum != LEVEL_PTCTEST) {
        return;
    }
    if (s_dirty) {
        scene_build();
    }
    if (s_quad_count <= 0 || s_build_failed) {
        return;
    }

    Gfx *h = gDisplayListHead;
    /* G_ZBUFFER is essential: the scene is appended after the object graph, so
     * without it the room paints over Mario and the actors. */
    gSPSetGeometryMode(h++, G_ZBUFFER);
    gSPClearGeometryMode(h++, G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR);
    gSPSetGeometryMode(h++, G_SHADE | G_CULL_BACK);
    gSPTexture(h++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_OFF);
    gDPSetCombineMode(h++, G_CC_SHADE, G_CC_SHADE);
    gDPSetRenderMode(h++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);

    /* The fork keeps at most 64 loaded vertices, so upload 16 quads at a time. */
    int i = 0;
    while (i < s_quad_count) {
        int q = s_quad_count - i;
        if (q > 16) q = 16;
        gSPVertex(h++, s_vtx + i * 4, q * 4, 0);
        for (int k = 0; k < q; k++) {
            int b = k * 4;
            gSP2Triangles(h++, b, b + 1, b + 2, 0, b, b + 2, b + 3, 0);
        }
        i += q;
    }
    /* Restore texturing for whatever draws next (the portal fan, HUD, and the
     * next pass).  Leaving G_TX_RENDERTILE off made Mario's model render with
     * flat vertex colours. */
    gSPTexture(h++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
    gSPSetGeometryMode(h++, G_LIGHTING);
    gDisplayListHead = h;
}
