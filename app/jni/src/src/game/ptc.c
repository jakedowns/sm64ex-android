/**
 * Portal Test Chamber: deterministic graybox scenario, debug star and the
 * headless regression driver.
 *
 * This file intentionally contains no rendering tricks and no alternate
 * physics.  The test drives the normal Mario state machine with scripted stick
 * input and only teleports/places objects through the same portal code a
 * player uses.  Everything observable is also printed in one parseable line
 * (ptc_format_status) for the adb test runner.
 */

#include <ultra64.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "sm64.h"
#include "ptc.h"
#include "levels/ptc/ptc_level.h"

#include "behavior_data.h"
#include "model_ids.h"
#include "game/area.h"
#include "game/camera.h"
#include "game/game_init.h"
#include "level_table.h"
#include "game/level_update.h"
#include "audio/external.h"
#include "game/mario.h"
#include "game/memory.h"
#include "game/object_helpers.h"
#include "game/object_list_processor.h"
#include "game/macro_special_objects.h"
#include "game/portal.h"
#include "pc/controller/controller_api.h"
#include "game/print.h"
#include "game/save_file.h"
#include "game/interaction.h"
#include "engine/math_util.h"
#include "engine/surface_collision.h"
#include "engine/graph_node.h"
#include "levels/ptc/header.h"
#include "boxphys.h"
#include "ptc_scene.h"
#include "movekit.h"

extern int custom_camera_freecam_state(int which);
extern int gfx_splat_active;
extern int gfx_splat_target;
extern int gfx_splat_shape;
extern int gfx_splat_density;

/* Per-object occluder fade.  Bit N = fade object N (0..8 platforms, 9 ramp,
 * 10..13 room walls), set by ptc_occlusion_update(). */
u32 g_ptc_fade_mask = 0;
/* Debug switch for demonstrating the occluder fade. */
static int s_fade_enable = 1;

/* Animated 0..1 fade amount per object (0 = opaque, 1 = fully faded). */
static f32 s_ptc_fade[40];

/* Debug calibration quad: a camera-facing world-space square, drawn to check
 * that 3D geometry keeps square pixels at any aspect setting. */
int g_ptc_aspect_calib = 0;
void ptc_aspect_calib_toggle(void) {
    g_ptc_aspect_calib = !g_ptc_aspect_calib;
}

/* 0-9 base platforms, 10-16 the wall-run / jetpack annex boxes.  Every entry
 * must be listed here *and* have a GEO_ASM node, or the box keeps its collision
 * while never being drawn (invisible geometry Mario bumps into). */
static const Gfx *const s_ptc_solid_dl[17] = {
    ptc_dl_p1, ptc_dl_p2, ptc_dl_p3, ptc_dl_p4,
    ptc_dl_p5, ptc_dl_p6, ptc_dl_p7, ptc_dl_p8, ptc_dl_p9,
    ptc_dl_wr_w, ptc_dl_wr_e,
    ptc_dl_jp_w, ptc_dl_jp_e, ptc_dl_jp_s, ptc_dl_jp_n, ptc_dl_jp_pad,
    ptc_dl_ramp,
};
/* Per-platform pre-list: a writable prim-colour command (alpha is updated
 * each frame to animate the fade) followed by the translucent display list. */
static Gfx s_ptc_fade_pre[17][3] = {
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_p1_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_p2_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_p3_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_p4_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_p5_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_p6_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_p7_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_p8_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_p9_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_wr_w_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_wr_e_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_jp_w_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_jp_e_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_jp_s_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_jp_n_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_jp_pad_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_ramp_fade), gsSPEndDisplayList() },
};
/* Ninja-course platforms: drawn through the same fade system as the P boxes
 * so they can be ghosted when they sit between the camera and Mario. */
static const Gfx *const s_ptc_course_dl[9] = {
    ptc_dl_l1, ptc_dl_n1, ptc_dl_n2, ptc_dl_n3, ptc_dl_n4,
    ptc_dl_n5, ptc_dl_n7, ptc_dl_n6, ptc_dl_n8,
};
static Gfx s_ptc_course_fade_pre[9][3] = {
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_l1_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_n1_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_n2_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_n3_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_n4_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_n5_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_n7_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_n6_fade), gsSPEndDisplayList() },
    { gsDPSetPrimColor(0, 0, 190, 190, 200, 255), gsSPDisplayList(ptc_dl_n8_fade), gsSPEndDisplayList() },
};
static const Gfx *const s_ptc_wall_dl[4] = {
    ptc_dl_wall_0, ptc_dl_wall_1, ptc_dl_wall_2, ptc_dl_wall_3,
};
static const Gfx *const s_ptc_wall_fade[4] = {
    ptc_dl_wall_0_fade, ptc_dl_wall_1_fade,
    ptc_dl_wall_2_fade, ptc_dl_wall_3_fade,
};

/* Drawn instead of a wall when the no-pull-in camera has passed through it. */
static const Gfx s_ptc_empty_dl[] = {
    gsSPEndDisplayList(),
};

/* GEO_ASM entry point: pick the opaque or translucent variant for the object
 * identified by the node parameter. */
Gfx *ptc_geo_obj(s32 callContext, struct GraphNode *node, UNUSED void *context) {
    struct GraphNodeGenerated *gen = (struct GraphNodeGenerated *) node;
    int id = (int) gen->parameter;

    (void) callContext; (void) context;
    if (id >= 0 && id < 17) {
        if (s_ptc_fade[id] > 0.01f) {
            /* Never fully transparent: clamp the fade so occluding geometry
             * stays readable as ghosted blocks instead of disappearing. */
            f32 f = s_ptc_fade[id];
            if (f > 1.0f) f = 1.0f;
            u32 a = (u32)(255.0f - f * 175.0f);
            if (a < 70) a = 70;
            s_ptc_fade_pre[id][0].words.w1 =
                (s_ptc_fade_pre[id][0].words.w1 & 0xFFFFFF00u) | (a & 0xFF);
            return s_ptc_fade_pre[id];
        }
        return (Gfx *) s_ptc_solid_dl[id];
    }
    if (id >= 30 && id < 39) {
        int ci = id - 30;
        if (s_ptc_fade[20 + ci] > 0.01f) {
            f32 f = s_ptc_fade[20 + ci];
            u32 a;
            if (f > 1.0f) f = 1.0f;
            a = (u32)(255.0f - f * 175.0f);
            if (a < 70) a = 70;
            s_ptc_course_fade_pre[ci][0].words.w1 =
                (s_ptc_course_fade_pre[ci][0].words.w1 & 0xFFFFFF00u) | (a & 0xFF);
            return s_ptc_course_fade_pre[ci];
        }
        return (Gfx *) s_ptc_course_dl[ci];
    }
    if (id == 21) {
        return (Gfx *) ptc_dl_mirror;
    }
    id -= 17;
    if (id >= 0 && id < 4) {
        extern int gfx_cam_noclip;
        if (gfx_cam_noclip && (g_ptc_fade_mask & (1u << (10 + id)))) {
            /* The camera is through this wall: hide it completely. */
            return (Gfx *) s_ptc_empty_dl;
        }
        return (Gfx *)((g_ptc_fade_mask & (1u << (10 + id)))
                ? s_ptc_wall_fade[id] : s_ptc_wall_dl[id]);
    }
    return (Gfx *) ptc_dl_room;
}

/* ------------------------------------------------------------ mirror wall */

/* SM64's castle mirror is not a render-to-texture reflection: it draws a
 * mirrored copy of Mario (position flipped about the mirror plane, yaw negated,
 * X scale negated) in front of a plain mirror quad.  Same trick here, using the
 * generated plane so the two agree. */
static struct GraphNodeObject s_ptc_mirror_mario;

Gfx *ptc_geo_mirror_mario(s32 callContext, struct GraphNode *node, UNUSED void *context) {
    struct Object *mario = gMarioState != NULL ? gMarioState->marioObj : NULL;

    switch (callContext) {
        case GEO_CONTEXT_CREATE:
            init_graph_node_object(NULL, &s_ptc_mirror_mario, NULL,
                                   gVec3fZero, gVec3sZero, gVec3fOne);
            break;
        case GEO_CONTEXT_AREA_LOAD:
            geo_add_child(node, &s_ptc_mirror_mario.node);
            break;
        case GEO_CONTEXT_AREA_UNLOAD:
            geo_remove_child(&s_ptc_mirror_mario.node);
            break;
        case GEO_CONTEXT_RENDER:
            if (mario != NULL && mario->header.gfx.pos[0] < PTC_MIRROR_X) {
                s_ptc_mirror_mario.sharedChild = mario->header.gfx.sharedChild;
                s_ptc_mirror_mario.unk18 = mario->header.gfx.unk18;
                vec3s_copy(s_ptc_mirror_mario.angle, mario->header.gfx.angle);
                vec3f_copy(s_ptc_mirror_mario.pos, mario->header.gfx.pos);
                vec3f_copy(s_ptc_mirror_mario.scale, mario->header.gfx.scale);
                s_ptc_mirror_mario.unk38 = *(struct GraphNodeObject_sub *)
                        &mario->header.gfx.unk38.animID;
                s_ptc_mirror_mario.pos[0] = 2.0f * PTC_MIRROR_X - s_ptc_mirror_mario.pos[0];
                s_ptc_mirror_mario.angle[1] = -s_ptc_mirror_mario.angle[1];
                s_ptc_mirror_mario.scale[0] *= -1.0f;
                s_ptc_mirror_mario.node.flags |= GRAPH_RENDER_ACTIVE;
            } else {
                s_ptc_mirror_mario.node.flags &= ~GRAPH_RENDER_ACTIVE;
            }
            break;
    }
    return NULL;
}

/* Slab test for the camera -> Mario segment. */
static int ptc_ray_box(const Vec3f o, const Vec3f d,
                       f32 x0, f32 y0, f32 z0, f32 x1, f32 y1, f32 z1) {
    f32 tmin = 0.0f, tmax = 1.0f;
    const f32 lo[3] = { x0, y0, z0 };
    const f32 hi[3] = { x1, y1, z1 };
    for (int i = 0; i < 3; i++) {
        if (fabsf(d[i]) < 0.0001f) {
            if (o[i] < lo[i] || o[i] > hi[i]) return 0;
        } else {
            f32 t1 = (lo[i] - o[i]) / d[i];
            f32 t2 = (hi[i] - o[i]) / d[i];
            if (t1 > t2) { f32 t = t1; t1 = t2; t2 = t; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return 0;
        }
    }
    return tmin < 0.999f && tmax > 0.001f;
}

static int ptc_ray_plane(const Vec3f o, const Vec3f d, int axis, f32 plane,
                         f32 a0, f32 a1, f32 b0, f32 b1) {
    f32 t, u, v;
    if (fabsf(d[axis]) < 0.0001f) return 0;
    t = (plane - o[axis]) / d[axis];
    if (t <= 0.001f || t >= 0.999f) return 0;
    u = o[(axis + 1) % 3] + d[(axis + 1) % 3] * t;
    v = o[(axis + 2) % 3] + d[(axis + 2) % 3] * t;
    return u >= a0 && u <= a1 && v >= b0 && v <= b1;
}

/* Approach the per-object fade target so occlusion fades in and out. */
static void ptc_fade_update(void) {
    const f32 step = 0.09f;
    for (int i = 0; i < 29; i++) {
        f32 target = (g_ptc_fade_mask & (1u << i)) ? 1.0f : 0.0f;
        if (s_ptc_fade[i] < target) {
            s_ptc_fade[i] += step;
            if (s_ptc_fade[i] > target) s_ptc_fade[i] = target;
        } else if (s_ptc_fade[i] > target) {
            s_ptc_fade[i] -= step;
            if (s_ptc_fade[i] < target) s_ptc_fade[i] = target;
        }
    }
}

/* Fade only the PTC objects that actually cross the camera-to-Mario ray. */
static void ptc_occlusion_update(void) {
    static const f32 boxes[][6] = {
        { -1050, 0, -650, -750, 180, -350 },   /* 0 P1 */
        {   750, 0, -650, 1050, 360, -350 },   /* 1 P2 */
        { -1050, 0,  150, -750, 560,  450 },   /* 2 P3 */
        {   750, 0,  150, 1050, 680,  450 },   /* 3 P4 */
        {  -600, 0,  600,  600, 800, 1200 },   /* 4 P5 */
        {  -750, -120, -400, -450, 260, -300 },/* 5 P6 */
        {  -300, 0, -1500, 300, 1100, -1100 }, /* 6 P7 */
        { 1150, 0,  650, 1350,  40,  850 },    /* 7 P8 reset pad */
        {  -300, 0,  150, 300, 950,  560 },    /* 8 P9 */
        {  -250, 500, -850, 250, 800, -550 },  /* 9 ramp */
    };
    static const f32 course_boxes[][6] = {
        { -1100, 0, -1600,  -600, 450, -1400 }, /* L1 */
        {  -360, 0, -2350,  -300, 700, -1400 }, /* N1 */
        {  -950, 0, -2700,  -360, 330, -2350 }, /* N2 */
        {  -150, 0, -2850,   -90, 950, -2790 }, /* N3 */
        {  -200, 0, -3200,  1000, 200, -2600 }, /* N4 */
        {  1200, 0, -3150,  2100, 900, -2600 }, /* N5 */
        {  1900, 0, -3000,  2100, 1700, -2800 },/* N7 */
        {  -900, 0, -3200,  -300, 1500, -2700 },/* N6 */
        {  -450, 0, -3100,  -390, 2100, -2900 },/* N8 */
    };
    Vec3f o, d, target;

    g_ptc_fade_mask = 0;
    if (!s_fade_enable) {
        ptc_fade_update();
        return;
    }
    if (gCurrLevelNum != 38 || gCurrAreaIndex != 1 || gMarioState == NULL) {
        ptc_fade_update();
        return;
    }
    vec3f_copy(o, gLakituState.pos);
    vec3f_copy(target, gMarioState->pos);
    target[1] += 80.0f;
    d[0] = target[0] - o[0];
    d[1] = target[1] - o[1];
    d[2] = target[2] - o[2];

    for (unsigned i = 0; i < sizeof(boxes) / sizeof(boxes[0]); i++) {
        if (ptc_ray_box(o, d, boxes[i][0], boxes[i][1], boxes[i][2],
                        boxes[i][3], boxes[i][4], boxes[i][5])) {
            g_ptc_fade_mask |= 1u << i;
        }
    }
    for (unsigned i = 0; i < sizeof(course_boxes) / sizeof(course_boxes[0]); i++) {
        if (ptc_ray_box(o, d, course_boxes[i][0], course_boxes[i][1], course_boxes[i][2],
                        course_boxes[i][3], course_boxes[i][4], course_boxes[i][5])) {
            g_ptc_fade_mask |= 1u << (20 + i);
        }
    }
    /* Walls: 10 south/front (z=-3200), 11 north/back (z=1250),
     * 12 west/left (x=-2600), 13 east/right (x=3200).  Room extents from
     * scripts/gen-ptc-level.py; keep these in sync if the room changes. */
    if (ptc_ray_plane(o, d, 2, -3200.0f, -2600.0f, 3200.0f, 0.0f, 2800.0f)) {
        g_ptc_fade_mask |= 1u << 10;
    }
    if (ptc_ray_plane(o, d, 2, 1250.0f, -2600.0f, 3200.0f, 0.0f, 2800.0f)) {
        g_ptc_fade_mask |= 1u << 11;
    }
    if (ptc_ray_plane(o, d, 0, -2600.0f, -3200.0f, 1250.0f, 0.0f, 2800.0f)) {
        g_ptc_fade_mask |= 1u << 12;
    }
    if (ptc_ray_plane(o, d, 0, 3200.0f, -3200.0f, 1250.0f, 0.0f, 2800.0f)) {
        g_ptc_fade_mask |= 1u << 13;
    }
    ptc_fade_update();
}

#define PTC_SETUP_TICKS       60      /* visible pause after portal placement */
#define PTC_WALK_TIMEOUT      300
#define PTC_TELEPORT_TIMEOUT  90
#define PTC_STAR_TIMEOUT      300
#define PTC_STAR_PAUSE        45      /* let the star be visible after arrival */
#define PTC_STAR_HIDDEN_Y     (-10000.0f)

static int s_testActive;
static int s_phase;
static int s_phaseTimer;
static int s_failed;
static char s_failReason[96];

/* Move-annex smoke test state (declared early: the input override reads it). */
static int s_annexMode;      /* 0 none, 1 wall-run, 2 jetpack */
static int s_annexTimer;
static int s_annexSeen;
static float s_annexStartY;

static int s_shotA;
static int s_shotB;
static int s_portalsPlaced;
static int s_teleportSeen;
static int s_teleportSrc = -1;
static int s_teleportDst = -1;
static int s_onPlatform;
static int s_starCollected[2];
static int s_testMode;
/* Portal walk-through capture: 0 off, 1 A->B, 2 B->A.  Holds forward through
 * the crossing and keeps walking after the exit (unlike the traversal test,
 * which brakes so the star can be collected). */
static int s_portalWalk;
static int s_portalWalkTimer;
static int s_portalWalkDelay;
static int s_portalWalkYaw;
static int s_portalWalkTeleBase;
/* "Mario portal walk loop": endless A<->B figure loop through two adjacent
 * portals on the west wall (smoke test for the teleport path). */
static int s_mpw;             /* 0 off, 1 running */
static int s_mpwPasses;       /* teleports completed this run */
static int s_mpwPhase;        /* 0 settle, 1 walk in, 2 right-hand turn, 3 stalled */
static int s_mpwTimer;        /* ticks in the current phase */
static int s_mpwTarget;       /* portal to walk into next: 0 = A, 1 = B */
static int s_mpwTeleBase;
static int s_mpwTurnYaw;
/* Placement of the A/B pair for the loop.  The standalone smoke test uses the
 * west wall; the ninja run reuses the same loop on the south wall behind the
 * N6 star. */
static Vec3f s_mpwA, s_mpwB, s_mpwStart;
static s16 s_mpwEntryYaw = 0xC000;
static int s_mpwSettle = 90;
static int s_mpwLastSrc;
static int s_mpwLastDst;
static int s_wallTest;          // bitmask from the last walltest command
static int s_momDropped;
static f32 s_momPrevSpeed;
static f32 s_momEntrySpeed;
static f32 s_momExitSpeed;
static f32 s_momApex;
static f32 s_momExitY;
static f32 s_momExitVX;
static f32 s_momExitVY;
static f32 s_momExitVZ;
/* Portal-momentum telemetry: the mapped exit vector the portal installed, the
 * velocity Mario still had one physics tick later, and how well they agree. */
static f32 s_momMappedVX;
static f32 s_momMappedVY;
static f32 s_momMappedVZ;
static f32 s_momHoldSpeed;
static f32 s_momHoldCos;
static f32 s_momRatio;
static f32 s_momTurnDot;
static int s_realStarsBefore;
static int s_realStarsAfter;
static int s_saveStarsBefore;
static int s_saveStarsAfter;
static int s_inputHookCalls;
/* 10 s seamless light/camera orbit showcase. */
static int s_showOn;
static int s_showTimer;
static float s_showCenter[3];
static u32 s_showStartMs;
static int s_showFlipped;


static char s_pendingCommand[24];
static volatile int s_pendingCommandReady;
static void ptc_apply_command(const char *action);
static void ptc_goomba_update(void);
static void ptc_annex_update(void);
static void ptc_pov_apply(void);
static int  s_pov_shot;
static void ptc_chain_update(void);
static void ptc_chain_input(struct MarioState *m);
static void ptc_chain_walk_to(struct MarioState *m, float x, float z);
static void ptc_chain_stick(struct MarioState *m, s16 yaw, float mag);
static const char *ptc_chain_note(int ph);
static int s_chainFlags;
static u32 s_chainHudFlags;
static int s_chainHudSaved;
static int s_finaleSplatSaved[4];
static int s_chainGoombaCross;
static int s_cineOn;
static int s_cineInit;
static int s_cineSavedFov;
/* Green test note in the Android debug panel (gfx_sdl2.c). */
extern void gfx_set_debug_note(const char *s);
extern int gfx_cam_mode;
static int  s_chainMode;
static void ptc_finish_ok(void);
static void ptc_record_teleport(void);

static int s_calActive;
static int s_calStage;
static int s_calTimer;
static int s_calArmed;
static f32 s_calPeak;
static f32 s_calSingle;
static f32 s_calDouble;
static f32 s_calTriple;

static int ptc_in_level(void) {
    return gCurrLevelNum == LEVEL_PTC && gCurrAreaIndex == 1 && gMarioState != NULL;
}

static int ptc_save_star_count(void) {
    if (gCurrSaveFileNum < 1 || gCurrSaveFileNum > 4) {
        return 0;
    }
    return save_file_get_total_star_count(gCurrSaveFileNum - 1, COURSE_MIN - 1, COURSE_MAX - 1);
}

static void ptc_fail(const char *reason) {
    s_failed = 1;
    s_testActive = 0;
    s_phase = PTC_PHASE_FAIL;
    if (reason != NULL) {
        snprintf(s_failReason, sizeof(s_failReason), "%s", reason);
    }
}

/* ------------------------------------------------------------- debug star */

static struct Object *ptc_find_star(int index) {
    struct ObjectNode *head;
    struct ObjectNode *node;
    const BehaviorScript *want;

    if (gObjectLists == NULL) {
        return NULL;
    }
    head = &gObjectLists[OBJ_LIST_LEVEL];
    node = head->next;
    want = (const BehaviorScript *) segmented_to_virtual(bhvPtcDebugStar);

    while (node != head) {
        struct Object *obj = (struct Object *) node;
        if (obj->behavior == want && ((obj->oBehParams >> 24) & 0xFF) == index) {
            return obj;
        }
        node = node->next;
    }
    return NULL;
}

static void ptc_star_position(int index, f32 *x, f32 *y, f32 *z) {
    if (index == 1) {
        *x = PTC_STAR2_X;
        *y = PTC_STAR2_Y;
        *z = PTC_STAR2_Z;
    } else {
        *x = PTC_STAR_X;
        *y = PTC_STAR_Y;
        *z = PTC_STAR_Z;
    }
}

int ptc_star_collected_index(int index) {
    return index >= 0 && index < 2 ? s_starCollected[index] : 0;
}

/* Compact snapshot for the fast-forward trace (ptc_trace / runticks). */
void ptc_chain_state(int *phase, int *flags, int *crossings, int *star0, int *star1) {
    if (phase != NULL) *phase = s_chainMode;
    if (flags != NULL) *flags = (int) s_chainFlags;
    if (crossings != NULL) *crossings = s_chainGoombaCross;
    if (star0 != NULL) *star0 = s_starCollected[0];
    if (star1 != NULL) *star1 = s_starCollected[1];
}

int ptc_star_collected(void) {
    return s_starCollected[0];
}

int ptc_star2_collected(void) {
    return s_starCollected[1];
}

void ptc_star_mark_collected_index(int index) {
    if (index >= 0 && index < 2) {
        s_starCollected[index] = 1;
    }
}

void ptc_star_mark_collected(void) {
    ptc_star_mark_collected_index(0);
}

int ptc_star_visible_index(int index) {
    struct Object *star = ptc_find_star(index);
    return !ptc_star_collected_index(index) && star != NULL && star->oPosY > -1000.0f;
}

int ptc_star_visible(void) {
    return ptc_star_visible_index(0);
}

void ptc_star_reset(void) {
    int found[2] = { 0, 0 };

    s_starCollected[0] = 0;
    s_starCollected[1] = 0;

    if (gObjectLists != NULL) {
        struct ObjectNode *head = &gObjectLists[OBJ_LIST_LEVEL];
        struct ObjectNode *node = head->next;
        const BehaviorScript *want =
            (const BehaviorScript *) segmented_to_virtual(bhvPtcDebugStar);
        while (node != head) {
            struct Object *star = (struct Object *) node;
            if (star->behavior == want) {
                int idx = (star->oBehParams >> 24) & 0xFF;
                if (idx < 2) {
                    f32 x, y, z;
                    ptc_star_position(idx, &x, &y, &z);
                    found[idx] = 1;
                    star->oPosX = x;
                    star->oPosY = y;
                    star->oPosZ = z;
                    star->oInteractStatus = 0;
                    star->oInteractType = INTERACT_COIN; /* no save-file write */
                    star->oDamageOrCoinValue = 0;
                    star->activeFlags |= ACTIVE_FLAG_ACTIVE;
                    star->header.gfx.scale[0] = 1.0f;
                    star->header.gfx.scale[1] = 1.0f;
                    star->header.gfx.scale[2] = 1.0f;
                }
            }
            node = node->next;
        }
    }

    if (ptc_in_level()) {
        for (int idx = 0; idx < 2; idx++) {
            if (!found[idx]) {
                f32 x, y, z;
                struct Object *star;
                ptc_star_position(idx, &x, &y, &z);
                struct Object *parent = gMarioState != NULL && gMarioState->marioObj != NULL
                                      ? gMarioState->marioObj : &gMacroObjectDefaultParent;
                star = spawn_object_abs_with_rot(parent, 0, MODEL_STAR, bhvPtcDebugStar,
                                                 (s16) x, (s16) y, (s16) z, 0, 0, 0);
                if (star != NULL) {
                    star->oBehParams = (u32) idx << 24;
                }
            }
        }
    }
}

/* --------------------------------------------------------------- test setup */

static void ptc_reset_mario(void) {
    struct MarioState *m = gMarioState;
    struct Surface *floor = NULL;
    Vec3f p = { PTC_MARIO_START_X, PTC_MARIO_START_Y, PTC_MARIO_START_Z };

    if (m == NULL) {
        return;
    }

    m->pos[0] = p[0];
    m->pos[1] = p[1];
    m->pos[2] = p[2];
    m->vel[0] = m->vel[1] = m->vel[2] = 0.0f;
    m->forwardVel = 0.0f;
    m->slideVelX = m->slideVelZ = 0.0f;
    m->faceAngle[0] = 0;
    m->faceAngle[1] = PTC_WALK_YAW;
    m->faceAngle[2] = 0;
    m->intendedMag = 0.0f;
    m->intendedYaw = PTC_WALK_YAW;
    m->health = 0x880;
    m->hurtCounter = 0;
    m->healCounter = 0;
    m->flags &= ~(MARIO_WING_CAP | MARIO_METAL_CAP | MARIO_VANISH_CAP | MARIO_CAP_ON_HEAD);
    m->capTimer = 0;
    m->wall = NULL;
    m->ceil = NULL;
    m->peakHeight = p[1];
    m->floorHeight = find_floor(p[0], p[1] + 80.0f, p[2], &floor);
    m->floor = floor;
    m->floorAngle = 0;
    m->action = ACT_IDLE;
    m->prevAction = ACT_IDLE;
    m->actionState = 0;
    m->actionTimer = 0;
    m->actionArg = 0;
    m->input = 0;

    if (m->marioObj != NULL) {
        vec3f_copy(m->marioObj->header.gfx.pos, m->pos);
        m->marioObj->oPosX = m->pos[0];
        m->marioObj->oPosY = m->pos[1];
        m->marioObj->oPosZ = m->pos[2];
        m->marioObj->oMoveAngleYaw = PTC_WALK_YAW;
    }

    set_mario_action(m, ACT_IDLE, 0);
}

void ptc_reset(void) {
    {
        extern void custom_camera_set_focus_hint(int on, const Vec3f pos);
        extern void custom_camera_set_yaw_hint(int on, s16 yaw);
        extern void custom_camera_set_freelook(int on, const Vec3f pos, const Vec3f focus);
        extern int gfx_stereo_fov_pct;
        s_chainMode = 0;
        s_cineOn = 0;
        s_cineInit = 0;
        if (s_chainHudSaved) {
            gHudDisplay.flags = s_chainHudFlags;
            s_chainHudSaved = 0;
        }
        if (s_finaleSplatSaved[0] || s_finaleSplatSaved[3]) {
            gfx_splat_active = s_finaleSplatSaved[0];
            gfx_splat_target = s_finaleSplatSaved[1];
            gfx_splat_shape = s_finaleSplatSaved[2];
            gfx_splat_density = s_finaleSplatSaved[3];
        }
        custom_camera_set_freelook(0, NULL, NULL);
        gfx_stereo_fov_pct = 100;
        movekit_force_trigger(0);
        custom_camera_set_focus_hint(0, NULL);
        custom_camera_set_yaw_hint(0, 0);
    }
    s_testActive = 0;
    s_phase = PTC_PHASE_IDLE;
    s_phaseTimer = 0;
    s_failed = 0;
    s_failReason[0] = '\0';
    s_portalWalk = 0;
    s_mpw = 0;
    s_mpwPhase = 0;
    s_mpwTimer = 0;
    s_mpwPasses = 0;
    s_shotA = s_shotB = 0;
    s_portalsPlaced = 0;
    s_teleportSeen = 0;
    s_teleportSrc = -1;
    s_teleportDst = -1;
    s_onPlatform = 0;
    s_testMode = 0;
    s_momDropped = 0;
    s_momPrevSpeed = 0.0f;
    s_momEntrySpeed = 0.0f;
    s_momExitSpeed = 0.0f;
    s_momApex = 0.0f;
    s_momExitY = 0.0f;
    s_momExitVX = 0.0f;
    s_momExitVY = 0.0f;
    s_momExitVZ = 0.0f;
    s_momMappedVX = 0.0f;
    s_momMappedVY = 0.0f;
    s_momMappedVZ = 0.0f;
    s_momHoldSpeed = 0.0f;
    s_momHoldCos = 1.0f;
    s_momRatio = 1.0f;
    s_momTurnDot = 1.0f;

    portal_reset();
    portal_give_gun();
    ptc_reset_mario();
    ptc_star_reset();

    s_realStarsBefore = gMarioState != NULL ? gMarioState->numStars : 0;
    s_realStarsAfter = s_realStarsBefore;
    s_saveStarsBefore = ptc_save_star_count();
    s_saveStarsAfter = s_saveStarsBefore;
}

void ptc_place_portals(void) {
    portal_give_gun();
    Vec3f originA = { PTC_PORTAL_A_X, PTC_PORTAL_A_Y, -700.0f };
    Vec3f targetA = { PTC_PORTAL_A_X, PTC_PORTAL_A_Y, PTC_PORTAL_A_Z };
    Vec3f originB = { PTC_PORTAL_B_X, PTC_PORTAL_B_Y, 700.0f };
    Vec3f targetB = { PTC_PORTAL_B_X, PTC_PORTAL_B_Y, 1199.0f };

    s_shotA = portal_shoot_at(0, originA, targetA);
    s_shotB = portal_shoot_at(1, originB, targetB);
    s_portalsPlaced = (s_shotA && s_shotB) ? 2 : portal_count();
}

void ptc_start_test(void) {
    ptc_reset();
    s_testActive = 1;
    s_phase = PTC_PHASE_PLACE;
    s_phaseTimer = 0;
    s_calActive = 0;

    if (!ptc_in_level()) {
        ptc_fail("not in Portal Test Chamber (level 38 area 1)");
    }
}

void ptc_stop(void) {
    s_testActive = 0;
    s_calActive = 0;
    if (s_phase != PTC_PHASE_FAIL) {
        s_phase = PTC_PHASE_IDLE;
    }
}

/* ---------------------------------------------------- momentum launch test */

static void ptc_momentum_place_portals(void) {
    Vec3f aOrig = { PTC_MOM_PORTAL_A_X, 400.0f, PTC_MOM_PORTAL_A_Z };
    Vec3f aTarget = { PTC_MOM_PORTAL_A_X, PTC_MOM_PORTAL_A_Y, PTC_MOM_PORTAL_A_Z };
    Vec3f n = { PTC_MOM_PORTAL_B_NX, PTC_MOM_PORTAL_B_NY, PTC_MOM_PORTAL_B_NZ };
    Vec3f bOrig = { PTC_MOM_PORTAL_B_X + n[0] * 300.0f,
                    PTC_MOM_PORTAL_B_Y + n[1] * 300.0f,
                    PTC_MOM_PORTAL_B_Z + n[2] * 300.0f };
    Vec3f bTarget = { PTC_MOM_PORTAL_B_X, PTC_MOM_PORTAL_B_Y, PTC_MOM_PORTAL_B_Z };

    s_shotA = portal_shoot_at(0, aOrig, aTarget);
    s_shotB = portal_shoot_at(1, bOrig, bTarget);
    s_portalsPlaced = (s_shotA && s_shotB) ? 2 : portal_count();
}

static void ptc_momentum_place_mario(void) {
    struct MarioState *m = gMarioState;
    struct Surface *floor = NULL;

    if (m == NULL) {
        return;
    }
    m->pos[0] = PTC_MOM_START_X;
    m->pos[1] = PTC_MOM_START_Y;
    m->pos[2] = PTC_MOM_START_Z;
    m->vel[0] = m->vel[1] = m->vel[2] = 0.0f;
    m->forwardVel = 0.0f;
    m->faceAngle[0] = 0;
    m->faceAngle[1] = PTC_WALK_YAW;
    m->faceAngle[2] = 0;
    m->intendedMag = 0.0f;
    m->intendedYaw = PTC_WALK_YAW;
    m->floorHeight = find_floor(m->pos[0], m->pos[1] + 80.0f, m->pos[2], &floor);
    m->floor = floor;
    m->floorAngle = 0;
    m->wall = NULL;
    m->ceil = NULL;
    m->action = ACT_IDLE;
    m->prevAction = ACT_IDLE;
    m->actionState = 0;
    m->actionTimer = 0;
    m->actionArg = 0;
    m->input = 0;
    if (m->marioObj != NULL) {
        vec3f_copy(m->marioObj->header.gfx.pos, m->pos);
        m->marioObj->oPosX = m->pos[0];
        m->marioObj->oPosY = m->pos[1];
        m->marioObj->oPosZ = m->pos[2];
        m->marioObj->oMoveAngleYaw = PTC_WALK_YAW;
    }
    set_mario_action(m, ACT_IDLE, 0);
}

/* Fixed side-on shot for the momentum demo: the floor portal on the left, the
 * 45-degree exit ramp and the whole launch arc read in one frame.  Pinned via
 * the custom camera's freelook override, which portal.c leaves alone on the
 * crossing frame. */
static void ptc_momentum_camera_on(void) {
    extern void custom_camera_apply_mode(int mode);
    extern void custom_camera_set_freelook(int on, const Vec3f pos, const Vec3f focus);
    extern int gfx_stereo_fov_pct;
    Vec3f pos = { 2400.0f, 900.0f, -850.0f };
    Vec3f focus = { 0.0f, 520.0f, -900.0f };

    custom_camera_apply_mode(3);
    custom_camera_set_freelook(1, pos, focus);
    gfx_stereo_fov_pct = 140;
    /* Same choice as the trailer: the relay window goes black when the exit
     * faces open air, so draw the coloured portal fans for the demo. */
    portal_view_set(0);
}

static void ptc_momentum_camera_off(void) {
    extern void custom_camera_set_freelook(int on, const Vec3f pos, const Vec3f focus);
    custom_camera_set_freelook(0, NULL, NULL);
    portal_view_set(1);
}

void ptc_momentum_start(void) {
    ptc_reset();
    s_testMode = 1;
    s_testActive = 1;
    s_phase = PTC_PHASE_MOM_PLACE;
    s_phaseTimer = 0;
    s_momDropped = 0;
    s_momEntrySpeed = 0.0f;
    s_momExitSpeed = 0.0f;
    s_momApex = 0.0f;
    s_momPrevSpeed = 0.0f;
    if (!ptc_in_level()) {
        ptc_fail("not in Portal Test Chamber (level 38 area 1)");
        return;
    }
    ptc_momentum_place_mario();
    ptc_momentum_camera_on();
}

/* --------------------------------------------------- portal walk-through */

/* Deterministic capture rig for the seamless-crossing test (TESTING.md):
 * place the standard A/B pair, put Mario on a normal third-person chase camera
 * (custom mode 3) and hold the stick through the portal at walking speed.  The
 * exit direction becomes the new "forward" on the crossing tick, so he keeps
 * walking out of the destination instead of stopping on the threshold. */
static void ptc_portal_walk_camera(const Vec3f mpos, s16 yaw) {
    extern void custom_camera_apply_mode(int mode);
    extern void custom_camera_reset(void);
    extern float gfx_cam_dist, gfx_cam_height;
    /* Match the custom camera's default orbit (pitch 0x1800) so its first
     * update does not move the rig.  s_yaw is the look yaw: for a camera
     * behind Mario it equals his facing. */
    const s16 pitch = 0x1800;
    const s16 camYaw = yaw;
    f32 cp = coss(pitch);
    Vec3f focus, pos;

    gfx_cam_dist = 700.0f;
    gfx_cam_height = 110.0f;
    focus[0] = mpos[0];
    focus[1] = mpos[1] + gfx_cam_height;
    focus[2] = mpos[2];
    pos[0] = focus[0] - sins(camYaw) * cp * gfx_cam_dist;
    pos[1] = focus[1] + sins(pitch) * gfx_cam_dist;
    pos[2] = focus[2] - coss(camYaw) * cp * gfx_cam_dist;

    vec3f_copy(gLakituState.pos, pos);
    vec3f_copy(gLakituState.focus, focus);
    vec3f_copy(gLakituState.curPos, pos);
    vec3f_copy(gLakituState.curFocus, focus);
    vec3f_copy(gLakituState.goalPos, pos);
    vec3f_copy(gLakituState.goalFocus, focus);
    /* Drop any cinematic freelook from the chain so this chase rig can track
     * Mario through the loop. */
    {
        extern void custom_camera_set_freelook(int on, const Vec3f pos,
                                               const Vec3f focus);
        custom_camera_set_freelook(0, NULL, NULL);
    }
    custom_camera_apply_mode(3);
    custom_camera_reset();
}

/* 0 = west wall -> south wall, 1 = south wall -> west wall,
 * 2 = N7 tower wall -> N6 ceiling (the ninja-chain pair). */
static void ptc_portal_walk_start(int mode) {
    struct MarioState *m = gMarioState;
    Vec3f mpos;
    s16 yaw;
    int placed;

    ptc_reset();
    portal_give_gun();
    if (mode == 2) {
        Vec3f aOrig = { 1700.0f, 1050.0f, -2900.0f };
        Vec3f aTgt  = { 1900.0f, 1040.0f, -2900.0f };
        Vec3f bOrig = { -600.0f, 2200.0f, -2980.0f };
        Vec3f bTgt  = { -600.0f, 2799.0f, -2980.0f };
        placed = portal_shoot_at(0, aOrig, aTgt) & portal_shoot_at(1, bOrig, bTgt);
        mpos[0] = 1450.0f;
        mpos[1] = 900.0f;
        mpos[2] = -2900.0f;
        yaw = 0x4000;               /* east, into the N7 portal */
    } else {
        Vec3f aOrig = { -1900.0f, 140.0f, -2000.0f };
        Vec3f aTgt  = { -2599.0f, 140.0f, -2000.0f };
        Vec3f bOrig = { -1500.0f, 140.0f, -2600.0f };
        Vec3f bTgt  = { -1500.0f, 140.0f, -3199.0f };
        placed = portal_shoot_at(0, aOrig, aTgt) & portal_shoot_at(1, bOrig, bTgt);
        if (mode == 1) {
            mpos[0] = -1500.0f;
            mpos[1] = 0.0f;
            mpos[2] = -2900.0f;
            yaw = 0x8000;           /* south, into the south-wall portal */
        } else {
            mpos[0] = -2300.0f;
            mpos[1] = 0.0f;
            mpos[2] = -2000.0f;
            yaw = 0xC000;           /* west, into the west-wall portal */
        }
    }
    if (m == NULL) {
        return;
    }
    (void) placed;
    s_portalWalk = mode + 1;
    s_portalWalkTimer = 0;
    s_portalWalkDelay = 90;   /* settle the chase camera before walking */
    s_portalWalkYaw = yaw;
    s_portalWalkTeleBase = portal_teleport_count();

    m->pos[0] = mpos[0];
    m->pos[1] = mpos[1];
    m->pos[2] = mpos[2];
    m->vel[0] = m->vel[1] = m->vel[2] = 0.0f;
    m->forwardVel = 0.0f;
    m->faceAngle[0] = 0;
    m->faceAngle[1] = yaw;
    m->faceAngle[2] = 0;
    m->intendedYaw = yaw;
    m->intendedMag = 0.0f;
    set_mario_action(m, ACT_IDLE, 0);
    if (m->marioObj != NULL) {
        vec3f_copy(m->marioObj->header.gfx.pos, m->pos);
        m->marioObj->oMoveAngleYaw = yaw;
    }
    ptc_portal_walk_camera(mpos, yaw);
}

static void ptc_portal_walk_update(void) {
    struct MarioState *m = gMarioState;

    if (!s_portalWalk || m == NULL) {
        return;
    }
    if (s_portalWalkDelay > 0) {
        s_portalWalkDelay--;
        return;
    }
    s_portalWalkTimer++;
    if (portal_teleport_count() != s_portalWalkTeleBase) {
        s_portalWalkTeleBase = portal_teleport_count();
        /* Keep walking out of the destination, not back into it. */
        s_portalWalkYaw = m->faceAngle[1];
        s_portalWalkTimer = 0;
    }
    if (s_portalWalkTimer > 240) {
        s_portalWalk = 0;
    }
}

/* ------------------------------------------- mario portal walk loop (smoke) */

/* Endless figure loop through two portals placed side by side on the west
 * wall, both facing the room (+X):
 *
 *      room (x > -2600)          wall
 *        Mario --> A  [-2000]  |
 *                 B  [-1820]  |
 *        ^                    |
 *        +---- right turn ----+
 *
 * A and B are 180 units apart at y=140: Mario's centre of mass (80) sits in
 * the lower half of the ellipse while his feet stay on the floor, so he walks
 * through them normally.  Each pass is a real portal teleport (the normal
 * portal_update proximity path), never a position warp.
 *
 * adb:  adb shell $B --es cmd ptc --es action marioportalwalkloop
 *       adb shell $B --es cmd ptc --es action marioportalwalkstop
 * dump: mpw=<active>,<passes>,<phase>,<target>,<timer>,<src>,<dst>
 *       phases: 0 settle, 1 walk in, 2 turn right, 3 stalled/stopped */
#define PTCL_WALL_X       (-2599.0f)
#define PTCL_PORTAL_Y     140.0f
#define PTCL_A_Z          (-2000.0f)
#define PTCL_B_Z          (-1820.0f)     /* 180 units from A, same wall */
#define PTCL_START_X      (-2300.0f)
#define PTCL_SETTLE_TICKS 90             /* chase camera settles, then walk */
#define PTCL_STALL_TICKS  420            /* ~14 s with no teleport = stall */
#define PTCL_TURN_STEP    0x0C00         /* right-hand turn ramp per tick */

static void ptc_mpw_place_mario(void) {
    struct MarioState *m = gMarioState;

    if (m == NULL) {
        return;
    }
    m->pos[0] = s_mpwStart[0];
    m->pos[1] = s_mpwStart[1];
    m->pos[2] = s_mpwStart[2];
    m->vel[0] = m->vel[1] = m->vel[2] = 0.0f;
    m->forwardVel = 0.0f;
    m->faceAngle[0] = 0;
    m->faceAngle[1] = s_mpwEntryYaw;
    m->faceAngle[2] = 0;
    m->intendedYaw = s_mpwEntryYaw;
    m->intendedMag = 0.0f;
    set_mario_action(m, ACT_IDLE, 0);
    if (m->marioObj != NULL) {
        vec3f_copy(m->marioObj->header.gfx.pos, m->pos);
        m->marioObj->oMoveAngleYaw = s_mpwEntryYaw;
    }
    gCameraMovementFlags |= CAM_MOVE_INIT_CAMERA;
}

static void ptc_mpw_stop(void) {
    s_mpw = 0;
    s_mpwPhase = 0;
    s_mpwTimer = 0;
}

/* Places the deterministic A/B pair and starts the loop.  Calling it again
 * restarts from the same coordinates. */
/* Shared loop rig.  `resetTest` keeps the standalone smoke test behaving as
 * before; the ninja run passes 0 so the chain's stars/state survive. */
static void ptc_mpw_begin(const Vec3f aPos, const Vec3f bPos,
                          const Vec3f aOrig, const Vec3f bOrig,
                          const Vec3f start, s16 yaw, int resetTest) {
    Vec3f mpos;
    struct MarioState *m;

    if (!ptc_in_level()) {
        snprintf(s_failReason, sizeof(s_failReason),
                 "marioportalwalkloop: not in Portal Test Chamber");
        return;
    }
    if (resetTest) {
        ptc_reset();
    }
    portal_give_gun();

    /* Same hitscan placement the player uses; explicit coordinates. */
    s_shotA = portal_shoot_at(0, aOrig, aPos);
    s_shotB = portal_shoot_at(1, bOrig, bPos);

    vec3f_copy(s_mpwA, aPos);
    vec3f_copy(s_mpwB, bPos);
    vec3f_copy(s_mpwStart, start);
    s_mpwEntryYaw = yaw;

    m = gMarioState;
    if (m == NULL) {
        return;
    }

    s_mpw = 1;
    s_mpwPasses = 0;
    s_mpwPhase = 0;
    s_mpwTimer = 0;
    s_mpwTarget = 0;
    s_mpwTeleBase = portal_teleport_count();
    s_mpwTurnYaw = yaw;
    s_mpwLastSrc = -1;
    s_mpwLastDst = -1;
    s_mpwSettle = resetTest ? PTCL_SETTLE_TICKS : 20;

    ptc_mpw_place_mario();
    mpos[0] = m->pos[0];
    mpos[1] = m->pos[1];
    mpos[2] = m->pos[2];
    ptc_portal_walk_camera(mpos, yaw);
}

void ptc_mario_portal_walk_loop_start(void) {
    Vec3f aPos = { PTCL_WALL_X, PTCL_PORTAL_Y, PTCL_A_Z };
    Vec3f bPos = { PTCL_WALL_X, PTCL_PORTAL_Y, PTCL_B_Z };
    Vec3f aOrig = { PTCL_START_X, PTCL_PORTAL_Y, PTCL_A_Z };
    Vec3f bOrig = { PTCL_START_X, PTCL_PORTAL_Y, PTCL_B_Z };
    Vec3f start = { PTCL_START_X, 0.0f, PTCL_A_Z };

    ptc_mpw_begin(aPos, bPos, aOrig, bOrig, start, 0xC000, 1);
}

/* Streaming version of the same rig: no reset, so the in-place teleport count
 * and Mario's position are left alone. */
void ptc_mario_portal_walk_loop_stop(void) {
    ptc_mpw_stop();
}

/* Portal the loop is currently walking into (falls back to the constants if
 * placement ever fails). */
static void ptc_mpw_target_pos(Vec3f out) {
    Vec3f n;

    if (!portal_get(s_mpwTarget, out, n)) {
        if (s_mpwTarget == 0) {
            vec3f_copy(out, s_mpwA);
        } else {
            vec3f_copy(out, s_mpwB);
        }
    }
}

static void ptc_mpw_update(void) {
    struct MarioState *m = gMarioState;

    if (!s_mpw || m == NULL) {
        return;
    }
    s_mpwTimer++;

    if (s_mpwPhase == 0) {
        if (s_mpwTimer >= s_mpwSettle) {
            s_mpwPhase = 1;
            s_mpwTimer = 0;
        }
        return;
    }

    if (portal_teleport_count() != s_mpwTeleBase) {
        s_mpwTeleBase = portal_teleport_count();
        s_mpwPasses++;
        s_mpwLastSrc = portal_last_src();
        s_mpwLastDst = portal_last_dst();
        /* Walk back into the portal we just came out of. */
        s_mpwTarget = (s_mpwLastDst >= 0) ? s_mpwLastDst : (1 - s_mpwTarget);
        s_mpwPhase = 2;
        s_mpwTimer = 0;
        s_mpwTurnYaw = m->faceAngle[1];
        return;
    }

    if (s_mpwPhase == 1 && s_mpwTimer > PTCL_STALL_TICKS) {
        snprintf(s_failReason, sizeof(s_failReason),
                 "marioportalwalkloop stalled after %d passes", s_mpwPasses);
        s_mpwPhase = 3;
        s_mpw = 0;
    }
}

/* Stick for the loop; runs from ptc_mario_input_override() so the scripted
 * input wins over the controller in the same way as the portal-walk rigs. */
static void ptc_mpw_input(struct MarioState *m) {
    Vec3f pp;

    if (s_mpwPhase == 1) {
        ptc_mpw_target_pos(pp);
        ptc_chain_walk_to(m, pp[0], pp[2]);
        if ((m->action & ACT_GROUP_MASK) == ACT_GROUP_STATIONARY) {
            set_mario_action(m, ACT_WALKING, 0);
        }
        return;
    }

    if (s_mpwPhase == 2) {
        /* Turn right (yaw increases) until we face the target portal, then let
         * the walk phase home in on its centre. */
        s16 desired;
        int delta;
        int step;

        ptc_mpw_target_pos(pp);
        desired = (s16) atan2s(pp[2] - m->pos[2], pp[0] - m->pos[0]);
        delta = (int) (s16) (desired - (s16) s_mpwTurnYaw);
        if (delta == -0x8000) {
            delta = 0x8000;        /* 180 deg: the sketch turns right */
        }
        if (delta > PTCL_TURN_STEP) {
            step = PTCL_TURN_STEP;
        } else if (delta < -PTCL_TURN_STEP) {
            step = -PTCL_TURN_STEP;
        } else {
            step = delta;
        }
        s_mpwTurnYaw = (s16) (s_mpwTurnYaw + step);
        ptc_chain_stick(m, (s16) s_mpwTurnYaw, 10.0f);
        if ((m->action & ACT_GROUP_MASK) == ACT_GROUP_STATIONARY) {
            set_mario_action(m, ACT_WALKING, 0);
        }
        if ((delta > -0x800 && delta < 0x800) || s_mpwTimer > 45) {
            s_mpwPhase = 1;
            s_mpwTimer = 0;
        }
    }
}

/* ------------------------------------------------------------ calibration */

void ptc_calibrate_start(void) {
    ptc_reset();
    s_calActive = 1;
    s_calStage = 0;
    s_calTimer = 0;
    s_calArmed = 0;
    s_calPeak = 0.0f;
    s_phase = PTC_PHASE_CALIBRATE;
    s_phaseTimer = 0;

    if (gMarioState != NULL) {
        gMarioState->pos[0] = 0.0f;
        gMarioState->pos[1] = 0.0f;
        gMarioState->pos[2] = 0.0f;
        gMarioState->faceAngle[1] = 0;
    }
}

static void ptc_calibrate_update(void) {
    struct MarioState *m = gMarioState;
    u32 action;

    if (!s_calActive || m == NULL) {
        return;
    }

    if (s_calTimer == 0) {
        m->pos[0] = 0.0f;
        m->pos[1] = m->floorHeight;
        m->pos[2] = 0.0f;
        m->vel[0] = m->vel[1] = m->vel[2] = 0.0f;
        m->forwardVel = 32.0f;
        m->faceAngle[1] = 0;
        m->intendedYaw = 0;
        switch (s_calStage) {
            case 0: set_mario_action(m, ACT_JUMP, 0); break;
            case 1: set_mario_action(m, ACT_DOUBLE_JUMP, 0); break;
            default: set_mario_action(m, ACT_TRIPLE_JUMP, 0); break;
        }
        s_calPeak = m->pos[1];
        s_calArmed = 1;
    }

    s_calTimer++;
    if (m->pos[1] > s_calPeak) {
        s_calPeak = m->pos[1];
    }

    action = m->action;
    if (s_calArmed && s_calTimer > 12 && m->pos[1] <= m->floorHeight + 2.0f
        && (action & ACT_GROUP_MASK) == ACT_GROUP_STATIONARY) {
        switch (s_calStage) {
            case 0: s_calSingle = s_calPeak; break;
            case 1: s_calDouble = s_calPeak; break;
            default: s_calTriple = s_calPeak; break;
        }

        s_calStage++;
        s_calTimer = 0;
        s_calArmed = 0;
        if (s_calStage >= 3) {
            s_calActive = 0;
            s_phase = PTC_PHASE_IDLE;
        }
    } else if (s_calTimer > 240) {
        s_calActive = 0;
        s_phase = PTC_PHASE_IDLE;
        ptc_fail("calibration jump did not land");
    }
}

/* ---------------------------------------------------------------- stepping */

static int ptc_mario_on_upper_platform(void) {
    struct MarioState *m = gMarioState;

    if (m == NULL) {
        return 0;
    }
    return m->pos[2] > 600.0f && m->pos[1] >= PTC_P5_TOP - 4.0f
        && m->pos[1] <= PTC_P5_TOP + 16.0f && m->floorHeight >= PTC_P5_TOP - 4.0f;
}

static void ptc_momentum_update(void) {
    struct MarioState *m = gMarioState;

    if (!s_testActive || m == NULL) {
        return;
    }
    s_phaseTimer++;

    switch (s_phase) {
        case PTC_PHASE_MOM_PLACE:
            if ((s_phaseTimer % 10) == 1 || s_phaseTimer == 1) {
                ptc_momentum_place_portals();
            }
            if (s_portalsPlaced == 2 && s_phaseTimer >= 30) {
                ptc_momentum_place_mario();
                s_phase = PTC_PHASE_MOM_DROP;
                s_phaseTimer = 0;
            } else if (s_phaseTimer > 90) {
                ptc_fail("momentum portals could not be placed");
            }
            break;

        case PTC_PHASE_MOM_DROP:
            /* Walk off the high platform, then drop vertically into the floor
             * portal.  Freefall and ledge wall collisions keep injecting
             * horizontal velocity, and this fork rebuilds air velocity from
             * forwardVel every tick, so keep killing the horizontal component
             * for the whole drop instead of doing it only once.  The previous
             * tick's speed is the entrance speed. */
            ptc_record_teleport();
            if (s_teleportSeen) {
                /* Do NOT zero here: the teleport just installed the rotated
                 * launch velocity for the next phase. */
                Vec3f pEntry, pMapped;
                s_momEntrySpeed = s_momPrevSpeed;
                s_momExitSpeed = sqrtf(m->vel[0] * m->vel[0] + m->vel[1] * m->vel[1]
                                       + m->vel[2] * m->vel[2]);
                portal_last_velocities(pEntry, pMapped);
                s_momMappedVX = pMapped[0];
                s_momMappedVY = pMapped[1];
                s_momMappedVZ = pMapped[2];
                s_momRatio = portal_last_speed_ratio();
                s_momTurnDot = portal_last_turn_dot();
                s_momExitY = m->pos[1];
                s_momExitVX = m->vel[0];
                s_momExitVY = m->vel[1];
                s_momExitVZ = m->vel[2];
                s_momApex = m->pos[1];
                s_phase = PTC_PHASE_MOM_FLY;
                s_phaseTimer = 0;
                break;
            }
            if (m->action & ACT_FLAG_AIR) {
                m->vel[0] = 0.0f;
                m->vel[2] = 0.0f;
                m->forwardVel = 0.0f;
                s_momDropped = 1;
            }
            if (s_phaseTimer > 240) {
                ptc_fail("Mario did not enter the floor portal");
            } else {
                s_momPrevSpeed = sqrtf(m->vel[0] * m->vel[0] + m->vel[1] * m->vel[1]
                                       + m->vel[2] * m->vel[2]);
            }
            break;

        case PTC_PHASE_MOM_FLY:
            /* The teleport happens in portal_update(), after Mario's physics.
             * Sample one tick later: if the airborne state rebuilt the launch
             * from forwardVel/faceAngle correctly, the velocity should still
             * point along the mapped exit and keep (almost all of) its speed. */
            if (s_phaseTimer == 1) {
                float len = sqrtf(m->vel[0] * m->vel[0] + m->vel[1] * m->vel[1]
                                  + m->vel[2] * m->vel[2]);
                float mappedLen = sqrtf(s_momMappedVX * s_momMappedVX
                                      + s_momMappedVY * s_momMappedVY
                                      + s_momMappedVZ * s_momMappedVZ);
                s_momHoldSpeed = len;
                if (len > 1e-4f && mappedLen > 1e-4f) {
                    float dot = m->vel[0] * s_momMappedVX + m->vel[1] * s_momMappedVY
                              + m->vel[2] * s_momMappedVZ;
                    s_momHoldCos = dot / (len * mappedLen);
                }
            }
            if (m->pos[1] > s_momApex) {
                s_momApex = m->pos[1];
            }
            /* Visual-only cannon pose: same animation and pitch as
             * ACT_SHOT_FROM_CANNON, without changing the launch physics. */
            if (m->action & ACT_FLAG_AIR) {
                set_mario_animation(m, MARIO_ANIM_AIRBORNE_ON_STOMACH);
                m->faceAngle[0] = atan2s(m->forwardVel, m->vel[1]);
                m->marioObj->header.gfx.angle[0] = -m->faceAngle[0];
            }
            if (m->floorHeight >= PTC_P9_TOP - 5.0f
                && m->pos[1] <= PTC_P9_TOP + 14.0f
                && m->pos[0] > PTC_P9_X0 - 20.0f && m->pos[0] < PTC_P9_X1 + 20.0f
                && m->pos[2] > PTC_P9_Z0 - 20.0f && m->pos[2] < PTC_P9_Z1 + 20.0f) {
                s_onPlatform = 1;
                s_phase = PTC_PHASE_MOM_STAR;
                s_phaseTimer = 0;
                m->faceAngle[0] = 0;
                m->marioObj->header.gfx.angle[0] = 0;
            } else if (s_phaseTimer > 240 || (m->pos[1] < 150.0f
                       && m->floorHeight < PTC_P9_TOP - 100.0f)) {
                ptc_fail("Mario missed the momentum target platform");
            }
            break;

        case PTC_PHASE_MOM_STAR:
            if (ptc_star2_collected()) {
                if (s_momEntrySpeed < 55.0f) {
                    ptc_fail("entrance velocity was below the momentum threshold");
                } else if (s_momRatio < 0.999f || s_momRatio > 1.001f) {
                    ptc_fail("portal changed the speed of the crossing");
                } else if (s_momHoldCos < 0.99f) {
                    ptc_fail("air state lost the portal exit direction");
                } else if (s_momHoldSpeed < s_momEntrySpeed * 0.90f) {
                    ptc_fail("portal exit speed was lost on the next tick");
                } else {
                    ptc_momentum_camera_off();
                    ptc_finish_ok();
                }
            } else if (s_phaseTimer > 180) {
                ptc_fail("momentum star was not collected");
            }
            break;

        default:
            break;
    }
}

static void ptc_record_teleport(void) {
    if (portal_teleport_count() > 0) {
        s_teleportSeen = 1;
        s_teleportSrc = portal_last_src();
        s_teleportDst = portal_last_dst();
    }
}

static void ptc_finish_ok(void) {
    s_realStarsAfter = gMarioState != NULL ? gMarioState->numStars : 0;
    s_saveStarsAfter = ptc_save_star_count();
    if (s_realStarsAfter != s_realStarsBefore) {
        ptc_fail("Mario's real star count changed");
        return;
    }
    if (s_saveStarsAfter != s_saveStarsBefore) {
        ptc_fail("save-file star count changed");
        return;
    }
    s_testActive = 0;
    s_phase = PTC_PHASE_DONE;
}

static void ptc_show_hsv(float h, float sat, float v, float *r, float *g, float *b) {
    h = h - floorf(h);
    float i = floorf(h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1.0f - sat);
    float q = v * (1.0f - f * sat);
    float t = v * (1.0f - (1.0f - f) * sat);
    switch (((int) i) % 6) {
        case 0: *r = v; *g = t; *b = p; break;
        case 1: *r = q; *g = v; *b = p; break;
        case 2: *r = p; *g = v; *b = t; break;
        case 3: *r = p; *g = q; *b = v; break;
        case 4: *r = t; *g = p; *b = v; break;
        default:*r = v; *g = p; *b = q; break;
    }
}

/* 600 ticks = 20.0 s at 30 Hz, played back at 2x for a 10 s clip so the
 * ~17 fps screenrecord source yields ~34 unique frames per output second.
 * Camera and lights are linear in phase, so tick 600 is exactly tick 0. */
static void ptc_lightshow_update(void) {
    struct MarioState *m = gMarioState;
    extern void gfx_opengl_light_set(int index, float x, float y, float z,
                                     float r, float g, float b, float radius);
    extern void gfx_opengl_probe_config(float x, float y, float z, int spacing,
                                        int dim, int radius, int resolution, int vis);
    extern void custom_camera_set_freelook(int on, const Vec3f pos, const Vec3f focus);

    if (!s_showOn || m == NULL) {
        return;
    }
    {
        /* Wall-clock phase: the sim frame rate varies (30-40 fps), so a
         * tick-based period would not line up with the video.  20 s wall is
         * exactly one period at any frame rate; 2x playback makes it 10 s. */
        extern double gfx_wall_time_sec(void);
        u32 now = (u32) (gfx_wall_time_sec() * 1000.0);
        float sec;
        float ph;
        float camR = 900.0f, camH = 620.0f;
        float lr = 780.0f;
        if (s_showStartMs == 0) {
            s_showStartMs = now;
        }
        sec = (float)(now - s_showStartMs) * 0.001f;
        ph = (2.0f * 3.14159265f) * fmodf(sec, 20.0f) / 20.0f;
        Vec3f pos, focus;
        int i;

        pos[0] = s_showCenter[0] - sinf(ph) * camR;
        pos[1] = s_showCenter[1] + camH;
        pos[2] = s_showCenter[2] - cosf(ph) * camR;
        focus[0] = s_showCenter[0];
        focus[1] = s_showCenter[1] + 90.0f;
        focus[2] = s_showCenter[2];
        custom_camera_set_freelook(1, pos, focus);

        for (i = 0; i < 3; i++) {
            float ang = ph + (float) i * 2.0943951f;   /* 120 deg apart */
            float hue = fmodf(sec, 20.0f) / 20.0f + (float) i / 3.0f;
            float r, g, b;
            ptc_show_hsv(hue, 0.80f, 1.0f, &r, &g, &b);
            gfx_opengl_light_set(i,
                s_showCenter[0] + cosf(ang) * lr,
                s_showCenter[1] + 1000.0f,
                s_showCenter[2] + sinf(ang) * lr,
                r, g, b, 1900.0f);
        }
        if (!s_showFlipped && sec >= 10.0f) {
            s_showFlipped = 1;
            m->faceAngle[1] = 0x0000;
            set_mario_action(m, ACT_BACKFLIP, 0);
            m->vel[1] = 31.0f;
            m->forwardVel = 0.0f;
        }
        gfx_opengl_probe_config(s_showCenter[0], s_showCenter[1] + 240.0f,
                                s_showCenter[2], 700, 2, 30, 16, 1);
    }
    s_showTimer++;
}

/* ---------------------------------------------------------- blur showcase
 * 10 s wall-clock orbit around Mario with repeated slow-motion backflips.
 * The sim runs every 3rd tick (g_sim_slow_div) so the flip floats, while the
 * camera keeps orbiting in real time; the frozen ticks re-present the last
 * display list at 1/3, 2/3 of the step so the motion stays continuous. If
 * sub-frame interpolation is switched off the demo stays at normal speed
 * rather than falling back to stop-motion. Lights/probes/shadows are off so
 * the only moving thing in frame is Mario. */
static int s_blurShow;
static u32 s_blurStart;
static u32 s_blurLastFlip;
static Vec3f s_blurCenter;

static void ptc_blurshow_begin(void) {
    extern void custom_camera_apply_mode(int mode);
    extern void gfx_opengl_light_count(int count);
    extern void gfx_opengl_light_shadows(int on);
    extern void gfx_opengl_ssfx_set(int which, int value);
    extern int g_sim_slow_div;
    extern int g_sim_interp;
    struct MarioState *m = gMarioState;

    s_testMode = 0;
    s_showOn = 0;
    s_blurShow = 1;
    s_blurStart = 0;
    s_blurLastFlip = 0;
    if (m != NULL) {
        /* Open floor in the chamber, same spot the light show uses. */
        m->pos[0] = 0.0f;
        m->pos[1] = 0.0f;
        m->pos[2] = -1800.0f;
        m->vel[0] = m->vel[1] = m->vel[2] = 0.0f;
        m->forwardVel = 0.0f;
        m->faceAngle[1] = 0x0000;
        set_mario_action(m, ACT_IDLE, 0);
        if (m->marioObj != NULL) {
            vec3f_copy(m->marioObj->header.gfx.pos, m->pos);
        }
        s_blurCenter[0] = m->pos[0];
        s_blurCenter[1] = m->pos[1] + 80.0f;
        s_blurCenter[2] = m->pos[2];
    }
    g_sim_slow_div = g_sim_interp ? 3 : 1;
    custom_camera_apply_mode(3);   /* CUSTOM: the free-look camera module */
    gfx_opengl_light_count(0);
    gfx_opengl_light_shadows(0);
    gfx_opengl_ssfx_set(1, 0);     /* AO */
    gfx_opengl_ssfx_set(11, 0);    /* DOF */
    gfx_opengl_ssfx_set(21, 0);    /* SSGI */
    gfx_opengl_ssfx_set(70, 0);    /* SSRTX */
    gfx_opengl_ssfx_set(50, 0);    /* shadows */
    gfx_opengl_ssfx_set(110, 0);   /* probes */
    gfx_opengl_ssfx_set(250, 0);   /* crepuscular rays */
    gfx_set_debug_note("BLUR SHOW: 10s orbit, slow-mo backflips, lights off");
}

static void ptc_blurshow_stop(void) {
    extern void custom_camera_set_freelook(int on, const Vec3f pos, const Vec3f focus);
    extern int g_sim_slow_div;
    Vec3f p = { 0, 0, 0 };
    s_blurShow = 0;
    g_sim_slow_div = 1;
    custom_camera_set_freelook(0, p, p);
}

static void ptc_blurshow_update(void) {
    extern void custom_camera_set_freelook(int on, const Vec3f pos, const Vec3f focus);
    struct MarioState *m = gMarioState;
    u32 now;
    float sec, ph;
    Vec3f pos, focus;
    const float R = 650.0f, H = 420.0f;

    if (!s_blurShow || m == NULL) {
        return;
    }
    {
        extern double gfx_wall_time_sec(void);
        now = (u32) (gfx_wall_time_sec() * 1000.0);
    }
    if (s_blurStart == 0) {
        s_blurStart = now;
    }
    sec = (float)(now - s_blurStart) * 0.001f;
    ph = (2.0f * 3.14159265f) * fmodf(sec, 10.0f) / 10.0f;

    pos[0] = s_blurCenter[0] - sinf(ph) * R;
    pos[1] = s_blurCenter[1] + H;
    pos[2] = s_blurCenter[2] - cosf(ph) * R;
    focus[0] = m->pos[0];
    focus[1] = m->pos[1] + 90.0f;
    focus[2] = m->pos[2];
    custom_camera_set_freelook(1, pos, focus);

    /* Repeat the backflip whenever he is back on the ground. */
    if (s_blurLastFlip == 0 || (now - s_blurLastFlip) > 3200) {
        if (m->pos[1] <= s_blurCenter[1] + 6.0f && fabsf(m->vel[1]) < 4.0f) {
            m->faceAngle[1] = atan2s(pos[2] - m->pos[2], pos[0] - m->pos[0]);
            set_mario_action(m, ACT_BACKFLIP, 0);
            m->vel[1] = 30.0f;
            m->forwardVel = 0.0f;
            s_blurLastFlip = now;
        }
    }
}

void ptc_update(void) {
    struct MarioState *m = gMarioState;
    extern void portal_telemetry_tick(void);

    ptc_occlusion_update();
    portal_telemetry_tick();
    ptc_lightshow_update();
    ptc_blurshow_update();

    if (s_pendingCommandReady) {
        s_pendingCommandReady = 0;
        ptc_apply_command(s_pendingCommand);
    }

    if (s_pov_shot) {
        ptc_pov_apply();
    }

    ptc_chain_update();

    ptc_goomba_update();
    ptc_annex_update();
    ptc_portal_walk_update();
    ptc_mpw_update();

    if (s_calActive) {
        ptc_calibrate_update();
        return;
    }
    if (s_testMode == 1) {
        ptc_momentum_update();
        return;
    }
    if (!s_testActive || m == NULL) {
        return;
    }

    s_phaseTimer++;

    switch (s_phase) {
        case PTC_PHASE_PLACE:
            /* Retry the two deterministic hitscans for the first few ticks in
             * case the level's collision partition is still settling. */
            if ((s_phaseTimer % 10) == 1 || s_phaseTimer == 1) {
                ptc_place_portals();
            }
            if (s_portalsPlaced == 2 && s_phaseTimer >= PTC_SETUP_TICKS) {
                s_phase = PTC_PHASE_WALK;
                s_phaseTimer = 0;
            } else if (s_phaseTimer > 90) {
                ptc_fail("could not place both portals");
            }
            break;

        case PTC_PHASE_WALK:
            m->intendedMag = 32.0f;
            m->intendedYaw = PTC_WALK_YAW;
            m->faceAngle[1] = PTC_WALK_YAW;
            m->forwardVel = 32.0f;
            ptc_record_teleport();
            if (s_teleportSeen) {
                s_phase = PTC_PHASE_TELEPORT;
                s_phaseTimer = 0;
            } else if (s_phaseTimer > PTC_WALK_TIMEOUT) {
                ptc_fail("Mario did not enter the entry portal");
            }
            break;

        case PTC_PHASE_TELEPORT:
            ptc_record_teleport();
            if (s_teleportSrc != 0 || s_teleportDst != 1) {
                ptc_fail("wrong portal pair used");
            } else if (ptc_mario_on_upper_platform()) {
                s_onPlatform = 1;
                s_phase = PTC_PHASE_STAR;
                s_phaseTimer = 0;
            } else if (s_phaseTimer > PTC_TELEPORT_TIMEOUT) {
                ptc_fail("Mario did not arrive on the upper platform");
            }
            break;

        case PTC_PHASE_STAR:
            if (s_phaseTimer > PTC_STAR_PAUSE) {
                m->intendedMag = 32.0f;
                m->intendedYaw = PTC_STAR_YAW;
                m->faceAngle[1] = PTC_STAR_YAW;
                m->forwardVel = 32.0f;
            } else {
                m->intendedMag = 0.0f;
                m->forwardVel = 0.0f;
            }
            ptc_record_teleport();
            if (!s_onPlatform && ptc_mario_on_upper_platform()) {
                s_onPlatform = 1;
            }
            if (s_starCollected[0]) {
                ptc_finish_ok();
            } else if (s_phaseTimer > PTC_STAR_TIMEOUT) {
                ptc_fail("debug star was not collected");
            }
            break;

        default:
            break;
    }
}

/* Appended after CH_DONE so the existing dump mode numbers stay stable; kept
 * as a macro because the input override (defined earlier) needs it too. */
#define CH_PORTAL_LOOP 34

void ptc_mario_input_override(struct MarioState *m) {
    if (m == NULL) {
        return;
    }
    s_inputHookCalls++;

    if (s_chainMode != 0) {
        /* The portal-loop phase hands control to the smoke-test rig, which
         * owns both the stick and the camera for its duration. */
        if (s_chainMode == CH_PORTAL_LOOP && s_mpw) {
            ptc_mpw_input(m);
            return;
        }
        ptc_chain_input(m);
        return;
    }

    if (s_portalWalk != 0) {
        if (s_portalWalkDelay > 0) {
            return;
        }
        m->intendedMag = 32.0f;
        m->intendedYaw = (s16) s_portalWalkYaw;
        m->input |= INPUT_NONZERO_ANALOG;
        m->input &= ~INPUT_UNKNOWN_5;
        if ((m->action & ACT_GROUP_MASK) == ACT_GROUP_STATIONARY
            || m->action == ACT_DECELERATING) {
            set_mario_action(m, ACT_WALKING, 0);
        }
        return;
    }

    if (s_mpw != 0) {
        ptc_mpw_input(m);
        return;
    }

    if (s_annexMode == 1) {
        /* Hold "forward + into the wall" so the wall-run trigger sees a
         * non-zero stick and horizontal speed towards the surface. */
        m->intendedMag = 32.0f;
        m->intendedYaw = 0x0000;                 /* +Z, along the corridor */
        m->input |= INPUT_NONZERO_ANALOG;
        m->input &= ~INPUT_UNKNOWN_5;
        return;
    }
    if (s_annexMode == 2) {
        movekit_force_trigger(1);
        return;
    }
    if (s_annexMode == 3) {
        /* Grapple fires on the press edge, which ptc_annex_update pulses. */
        return;
    }

    if (s_calActive) {
        /* Keep A held for maximum jump height. */
        m->input |= INPUT_A_DOWN;
        return;
    }
    if (!s_testActive) {
        return;
    }

    if (s_testMode == 1) {
        /* This fork has air-strafe control: holding the stick while falling
         * accelerates Mario horizontally and carries him past the floor
         * portal.  Only walk while grounded; in the air let him drop straight.
         * After landing on P9, walk towards the second star. */
        int wantWalk = (s_phase == PTC_PHASE_MOM_DROP && !(m->action & ACT_FLAG_AIR))
                    || s_phase == PTC_PHASE_MOM_STAR;
        if (wantWalk) {
            m->intendedMag = 32.0f;
            m->intendedYaw = PTC_WALK_YAW;
            m->input |= INPUT_NONZERO_ANALOG;
            m->input &= ~INPUT_UNKNOWN_5;
            if ((m->action & ACT_GROUP_MASK) == ACT_GROUP_STATIONARY
                || m->action == ACT_DECELERATING) {
                set_mario_action(m, ACT_WALKING, 0);
            }
        } else if (s_phase == PTC_PHASE_MOM_DROP || s_phase == PTC_PHASE_MOM_FLY) {
            m->intendedMag = 0.0f;
            m->input &= ~INPUT_NONZERO_ANALOG;
        }
        return;
    }

    if (s_phase == PTC_PHASE_WALK
        || (s_phase == PTC_PHASE_STAR && s_phaseTimer > PTC_STAR_PAUSE)) {
        m->intendedMag = 32.0f;
        m->intendedYaw = (s_phase == PTC_PHASE_STAR) ? PTC_STAR_YAW : PTC_WALK_YAW;
        m->input |= INPUT_NONZERO_ANALOG;
        // update_mario_inputs() set this before the scripted stick was applied;
        // leaving it set makes act_walking() brake immediately.
        m->input &= ~INPUT_UNKNOWN_5;
        if ((m->action & ACT_GROUP_MASK) == ACT_GROUP_STATIONARY
            || m->action == ACT_DECELERATING) {
            set_mario_action(m, ACT_WALKING, 0);
        }
    } else if (s_phase == PTC_PHASE_PLACE || s_phase == PTC_PHASE_TELEPORT
               || (s_phase == PTC_PHASE_STAR && s_phaseTimer <= PTC_STAR_PAUSE)) {
        /* Stand still while the portals are placed, just after teleport and
         * during the star pause.  The teleport keeps the entry walk speed,
         * which otherwise carries Mario back into the exit portal (the
         * back-wall portal is only the exit push away). */
        m->intendedMag = 0.0f;
        m->input &= ~INPUT_NONZERO_ANALOG;
        m->forwardVel = 0.0f;
        if (!(m->action & ACT_FLAG_AIR)) {
            m->vel[0] = 0.0f;
            m->vel[2] = 0.0f;
            if ((m->action & ACT_GROUP_MASK) != ACT_GROUP_STATIONARY) {
                set_mario_action(m, ACT_IDLE, 0);
            }
        }
    }
}

/* --------------------------------------------------------------- on screen */

void ptc_hud_update(void) {
    /* World-space labels are baked into the level display lists.  The 2D text
     * pass is deliberately not used here: the test harness reads the PTC dump
     * and the labels stay in the recorded scene. */
}

/* ------------------------------------------------- goomba portal demo */

/* A deterministic Goomba that walks into portal A while a mirrored copy walks
 * out of portal B, so the crossing is seamless instead of a mid-air snap.
 * Both objects are placed from one logical position; once the entering copy is
 * fully behind portal A it is deleted and the exiting copy takes over. */
#define PTC_GOOMBA_SPEED   9.0f
#define PTC_GOOMBA_SIZE    70.0f
#define PTC_GOOMBA_START_Z 520.0f

static struct Object *s_goombaIn;
static struct Object *s_goombaOut;
static int s_goombaActive;
static float s_goombaZ;
static float s_goombaX;
static float s_goombaPlaneZ;     /* portal plane the loop crosses */
static float s_goombaTurnZ;      /* far turnaround line */
static int   s_goombaTowards;    /* +1 walking toward the plane, -1 back */
static int   s_goombaDir;        /* -1 = walking into the wall, +1 = walking out */
static int   s_goombaCrossings;

static void ptc_goomba_reset_state(void) {
    s_goombaIn = NULL;
    s_goombaOut = NULL;
    s_goombaActive = 0;
    s_goombaZ = PTC_GOOMBA_START_Z;
    s_goombaX = -90.0f;
    s_goombaDir = -1;
    s_goombaTowards = -1;
    s_goombaCrossings = 0;
}

static void ptc_goomba_place(struct Object *o, float x, float z, s16 yaw) {
    if (o == NULL) {
        return;
    }
    o->oPosX = x;
    o->oPosY = 0.0f;
    o->oPosZ = z;
    o->oHomeX = x;
    o->oHomeY = 0.0f;
    o->oHomeZ = z;
    o->oMoveAngleYaw = yaw;
    o->oFaceAngleYaw = yaw;
    o->oForwardVel = 0.0f;          /* position is driven here, not by AI */
    o->oVelY = 0.0f;
    o->oGravity = 0.0f;
    o->oFlags |= OBJ_FLAG_ACTIVE_FROM_AFAR;
    /* The object processor hides anything whose drawing distance is smaller
     * than the camera distance; a runtime-spawned actor never ran its init, so
     * keep both the distance and the render flag under our control. */
    o->oDrawingDistance = 12000.0f;
    o->header.gfx.node.flags |= GRAPH_RENDER_ACTIVE;
    o->header.gfx.node.flags &= ~GRAPH_RENDER_INVISIBLE;
    if (o->header.gfx.node.parent == NULL) {
        o->header.gfx.pos[0] = o->oPosX;
        o->header.gfx.pos[1] = o->oPosY;
        o->header.gfx.pos[2] = o->oPosZ;
    }
    o->header.gfx.angle[1] = yaw;
}

static void ptc_goomba_start(void) {
    /* Main chamber: both portals on the north (back) wall, Mario watching from
     * the room side, Goomba marching in and out of them forever. */
    Vec3f aOrig = { -90.0f, 140.0f, 500.0f };
    Vec3f aTarget = { -90.0f, 140.0f, 1249.0f };
    Vec3f bOrig = { 90.0f, 140.0f, 500.0f };
    Vec3f bTarget = { 90.0f, 140.0f, 1249.0f };
    Vec3f pa, pb, na, nb;

    ptc_goomba_reset_state();
    s_goombaX = -90.0f;          /* walk into portal A */
    portal_reset();
    portal_give_gun();
    portal_shoot_at(0, aOrig, aTarget);
    portal_shoot_at(1, bOrig, bTarget);
    portal_view_force(0);

    s_goombaPlaneZ = 1250.0f;
    if (portal_get(0, pa, na)) {
        s_goombaPlaneZ = pa[2];
    }
    /* Start on the room side of the plane and walk toward it. */
    s_goombaTowards = (s_goombaPlaneZ >= PTC_GOOMBA_START_Z) ? 1 : -1;
    s_goombaTurnZ = s_goombaPlaneZ - (float) s_goombaTowards * 650.0f;
    s_goombaZ = s_goombaTurnZ;
    s_goombaDir = s_goombaTowards;
    s_goombaIn = spawn_object_abs_with_rot(
        gMarioState != NULL ? gMarioState->marioObj : &gMacroObjectDefaultParent, 0,
        MODEL_GOOMBA, bhvGoomba, (s16) s_goombaX, 0, (s16) s_goombaZ, 0, 0x8000, 0);
    s_goombaActive = 1;

    if (gMarioState != NULL) {
        gMarioState->pos[0] = 260.0f;
        gMarioState->pos[1] = 0.0f;
        gMarioState->pos[2] = 700.0f;
        gMarioState->vel[0] = gMarioState->vel[1] = gMarioState->vel[2] = 0.0f;
        gMarioState->faceAngle[1] = 0x8000;
        set_mario_action(gMarioState, ACT_IDLE, 0);
        if (gMarioState->marioObj != NULL) {
            vec3f_copy(gMarioState->marioObj->header.gfx.pos, gMarioState->pos);
        }
    }
    g_portal_debug_color = 0;
    {
        extern int gfx_stereo_same_both;
        gfx_stereo_same_both = 0;
    }
    gCameraMovementFlags |= CAM_MOVE_INIT_CAMERA;
}

static void ptc_goomba_update(void) {
    struct MarioState *m = gMarioState;
    Vec3f pa, na, pb, nb;
    float planeZ = s_goombaPlaneZ;

    if (!s_goombaActive || s_goombaIn == NULL) {
        return;
    }
    (void) m;
    if (portal_get(0, pa, na) && portal_get(1, pb, nb)) {
        planeZ = pa[2];
        s_goombaPlaneZ = planeZ;
    }

    s_goombaZ += (float) s_goombaDir * PTC_GOOMBA_SPEED;

    if (s_goombaDir == s_goombaTowards
        && (planeZ - s_goombaZ) * (float) s_goombaTowards <= 0.0f) {
        /* Crossed the plane: appear at the other portal and walk back out.
         * For two mounts on one plane the map is x -> (Bx - (x - Ax)). */
        s_goombaZ = planeZ;
        if (portal_get(0, pa, na) && portal_get(1, pb, nb)) {
            s_goombaX = pb[0] - (s_goombaX - pa[0]);
        } else {
            s_goombaX = -s_goombaX;
        }
        s_goombaDir = -s_goombaTowards;
        s_goombaCrossings++;
    } else if (s_goombaDir == -s_goombaTowards
               && (s_goombaZ - s_goombaTurnZ) * (float) s_goombaDir <= 0.0f) {
        s_goombaZ = s_goombaTurnZ;
        s_goombaDir = s_goombaTowards;
    }

    ptc_goomba_place(s_goombaIn, s_goombaX, s_goombaZ,
                     s_goombaDir < 0 ? 0x8000 : 0x0000);
}

/* --------------------------------------------------- portal POV test */

/* Persistent, tick-reproducible "Mario seen through the window" rig.
 * Portals sit on the two walls of the SW corner: that is the only arrangement
 * where the relay camera looks into open space instead of the back of the
 * destination wall.  Mario parks in the destination space and the camera parks
 * on the viewed portal's axis.  Everything is re-applied every tick, so the
 * idle/sleep camera, the Lakitu smoothing and level resets cannot move the
 * framing between setup and screenshot. */
float g_pov_cam_dist = 600.0f;    /* camera distance from the viewed plane */
float g_pov_cam_height = 90.0f;   /* eye height: Mario's mid-body, not the portal centre */
float g_pov_mario_dist = 400.0f;  /* Mario distance from the destination plane */
int   g_pov_freeze = 1;           /* hold idle frame 0 (repeatable silhouette) */
int   g_pov_stereo = 0;           /* clean mono capture by default */
/* s_pov_shot: 0 off, 1 view A, 2 view B (declared near the top). */

static int ptc_pov_place(void) {
    /* Aim the portals at Mario's mid-body height (the mapping preserves height
     * relative to the portal centre, so this is what centres him in the
     * window when the test camera is level with the floor). */
    Vec3f aOrig = { -2400.0f, 90.0f, -3100.0f };
    Vec3f aTarget = { -2700.0f, 90.0f, -3100.0f };   /* west wall, normal +X */
    Vec3f bOrig = { -2500.0f, 90.0f, -3000.0f };
    Vec3f bTarget = { -2500.0f, 90.0f, -3300.0f };   /* south wall, normal +Z */
    Vec3f pa, na, pb, nb;

    ptc_goomba_reset_state();
    portal_reset();
    portal_give_gun();
    portal_shoot_at(0, aOrig, aTarget);
    portal_shoot_at(1, bOrig, bTarget);
    return portal_get(0, pa, na) && portal_get(1, pb, nb);
}

static void ptc_pov_apply(void) {
    Vec3f pa, na, pb, nb;
    Vec3f camPos, camFocus, marioPos;
    int view = (s_pov_shot == 2) ? 1 : 0;
    int other = 1 - view;
    extern void custom_camera_set_freelook(int on, const Vec3f pos, const Vec3f focus);
    extern void custom_camera_apply_mode(int mode);

    if (!portal_get(view, pa, na) || !portal_get(other, pb, nb)) {
        return;
    }

    /* Camera on the viewed portal's axis: x/z centred with its geometry,
     * pulled straight back along its normal, at Mario's mid-body height so the
     * relay image is centred on him instead of on the wall above him. */
    camPos[0] = pa[0] + na[0] * g_pov_cam_dist;
    camPos[1] = g_pov_cam_height;
    camPos[2] = pa[2] + na[2] * g_pov_cam_dist;
    camFocus[0] = pa[0];
    camFocus[1] = g_pov_cam_height;
    camFocus[2] = pa[2];

    /* Mario in the destination space, feet on the floor. */
    marioPos[0] = pb[0] + nb[0] * g_pov_mario_dist;
    marioPos[1] = 0.0f;
    marioPos[2] = pb[2] + nb[2] * g_pov_mario_dist;

    if (gMarioState != NULL) {
        gMarioState->pos[0] = marioPos[0];
        gMarioState->pos[1] = 0.0f;
        gMarioState->pos[2] = marioPos[2];
        gMarioState->vel[0] = gMarioState->vel[1] = gMarioState->vel[2] = 0.0f;
        /* Face away from the destination wall - i.e. straight into the relay
         * camera behind it - so the window shows his front, not his back. */
        gMarioState->faceAngle[1] = (s16) atan2s(-nb[2], -nb[0]);
        set_mario_action(gMarioState, ACT_IDLE, 0);
        if (gMarioState->marioObj != NULL) {
            vec3f_copy(gMarioState->marioObj->header.gfx.pos, marioPos);
            gMarioState->marioObj->header.gfx.angle[1] = gMarioState->faceAngle[1];
            if (g_pov_freeze) {
                gMarioState->marioObj->header.gfx.unk38.animFrame = 0;
            }
        }
    }

    custom_camera_apply_mode(3);          /* CUSTOM */
    custom_camera_set_freelook(1, camPos, camFocus);
    portal_view_force(view);
    g_portal_debug_color = 0;

    /* The 3D weave makes screenshots unreadable (and a mono shot is the honest
     * way to judge whether the model is complete).  Use the flat single-eye
     * path rather than disabling stereo, which would leave the composite
     * looking at one half of the canvas. */
    {
        extern int gfx_stereo_2d;
        extern int gfx_stereo_same_both;
        if (g_pov_stereo) {
            gfx_stereo_2d = 0;
            gfx_stereo_same_both = 0;
        } else {
            gfx_stereo_2d = 1;
            gfx_stereo_same_both = 1;
        }
    }
}

/* --------------------------------------------------- gun aim test */

/* Side-on view of Mario with a fixed reticle marker: the held gun must point
 * its barrel at the marker.  The override bypasses the camera-ray hitscan so a
 * spectator camera can see the aim in profile. */
static void ptc_gun_aim_test(void) {
    Vec3f target = { -560.0f, 240.0f, -700.0f };
    Vec3f camPos = { -220.0f, 260.0f, -1060.0f };
    Vec3f camFocus = { -600.0f, 120.0f, -860.0f };
    extern void custom_camera_set_freelook(int on, const Vec3f pos, const Vec3f focus);
    extern void custom_camera_apply_mode(int mode);
    extern int gfx_stereo_2d;
    extern int gfx_stereo_same_both;

    portal_reset();
    portal_give_gun();
    portal_aim_override(1, target);
    g_portal_debug_color = 0;

    if (gMarioState != NULL) {
        gMarioState->pos[0] = -600.0f;
        gMarioState->pos[1] = 0.0f;
        gMarioState->pos[2] = -900.0f;
        gMarioState->vel[0] = gMarioState->vel[1] = gMarioState->vel[2] = 0.0f;
        gMarioState->faceAngle[1] = 0x0000;      /* face +Z */
        set_mario_action(gMarioState, ACT_IDLE, 0);
        if (gMarioState->marioObj != NULL) {
            vec3f_copy(gMarioState->marioObj->header.gfx.pos, gMarioState->pos);
            gMarioState->marioObj->header.gfx.unk38.animFrame = 0;
        }
    }
    custom_camera_apply_mode(3);
    custom_camera_set_freelook(1, camPos, camFocus);
    gfx_stereo_2d = 1;
    gfx_stereo_same_both = 1;
}

static void ptc_gun_aim_end(void) {
    extern void custom_camera_set_freelook(int on, const Vec3f pos, const Vec3f focus);
    extern int gfx_stereo_2d;
    extern int gfx_stereo_same_both;

    portal_aim_override(0, NULL);
    custom_camera_set_freelook(0, NULL, NULL);
    gfx_stereo_2d = 0;
    gfx_stereo_same_both = 0;
}

/* ------------------------------------------------------- ninja chain */

/* One continuous, scripted run through every system in the chamber:
 * box3d ball/punch/kick/slam, jetpack, wall-run, grapple, a ceiling-portal
 * drop and the goomba lemming loop.  The driver owns Mario's input through
 * ptc_mario_input_override() and advances on world state (action, ground,
 * position, teleport count), never on a cut.  `ptc chainwall|chaingrapple|
 * chainjet|chainportal|chaingoomba` start at a single section for tuning. */

enum {
    CH_OFF = 0,
    CH_BOX_BALL,
    CH_BOX_BALL_WAIT,
    CH_BOX_WALK_PUNCH,
    CH_BOX_PUNCH,
    CH_BOX_WALK_KICK,
    CH_BOX_KICK,
    CH_BOX_WALK_SLAM,
    CH_BOX_SLAM_JUMP,
    CH_BOX_SLAM,
    CH_JET_L1,
    CH_RUN_WALL,
    CH_WALLRUN,
    CH_WALL_LAND,
    CH_GRAPPLE_AIM,
    CH_GRAPPLE_FIRE,
    CH_GRAPPLE_HOLD,
    CH_GRAPPLE_RELEASE,
    CH_GRAPPLE_TRAVEL,
    CH_GRAPPLE_AIM2,
    CH_GRAPPLE_FIRE2,
    CH_GRAPPLE_HOLD2,
    CH_GRAPPLE_RELEASE2,
    CH_LAND_N4,
    CH_STAR1,
    CH_JET_N5,
    CH_LAND_N5,
    CH_PORTAL_RUN,
    CH_PORTAL_FALL,
    CH_STAR2,
    CH_GOOMBA_SETUP,
    CH_GOOMBA_LOOP,
    CH_FINALE,
    CH_DONE
};

#define CH_FLAG_BALL      (1u << 0)
#define CH_FLAG_PUNCH     (1u << 1)
#define CH_FLAG_KICK      (1u << 2)
#define CH_FLAG_SLAM      (1u << 3)
#define CH_FLAG_JETPACK   (1u << 4)
#define CH_FLAG_WALLRUN   (1u << 5)
#define CH_FLAG_GRAPPLE   (1u << 6)
#define CH_FLAG_PORTAL    (1u << 7)
#define CH_FLAG_STAR1     (1u << 8)
#define CH_FLAG_STAR2     (1u << 9)
#define CH_FLAG_GOOMBA    (1u << 10)

#define CH_BOX_X        (-450.0f)
#define CH_BOX_TOWER_Z  (-1150.0f)
#define CH_BOX_B_Z      (-1080.0f)   /* front of the 30-cube pyramid */
#define CH_BOX_C_Z      (-1090.0f)
#define CH_BOX_D_Z      (-1100.0f)
#define CH_BOX_PYRAMID_Z (-1250.0f)
#define CH_L1_X         (-875.0f)
#define CH_L1_Z         (-1500.0f)
#define CH_STAR1_X      (300.0f)
#define CH_STAR1_Y      (320.0f)
#define CH_STAR1_Z      (-2950.0f)
#define CH_STAR2_X      (-750.0f)
#define CH_STAR2_Y      (1620.0f)
#define CH_STAR2_Z      (-3100.0f)

static int s_chainTimer;
static int s_chainFlags;
static float s_chainTargetX, s_chainTargetZ;
static u32 s_chainHudFlags;
static int s_chainHudSaved;
static int s_finaleSplatSaved[4];
static int s_chainTeleBase;
static int s_chainJumped;
static int s_chainLoop;      /* chainloop action: restart after the finale */
/* Second grapple stage: anchor computed above Mario after the first release,
 * plus the start pose of the second swing so it begins without a snap. */
static Vec3f s_chainAnchorB;
static Vec3f s_chainSwingPrev;
static float s_chainSwingAng0;
static float s_chainSwingR0;
#define CH_GOOMBA_N 1
static int s_chainGoombaState[CH_GOOMBA_N];
static int s_chainGoombaTimer[CH_GOOMBA_N];
static int s_chainGoombaPortalPlaced;
static int s_chainGoombaLoops;
static int s_chainGoombaShifted;
static int s_chainGoombaSquash;
static int s_chainGoombaCross;
static float s_chainGoombaX[CH_GOOMBA_N], s_chainGoombaY[CH_GOOMBA_N];
static float s_chainGoombaZ[CH_GOOMBA_N], s_chainGoombaVY[CH_GOOMBA_N];
static struct Object *s_chainGoomba[CH_GOOMBA_N];

static int ptc_chain_grounded(struct MarioState *m) {
    return m != NULL && !(m->action & ACT_FLAG_AIR);
}

static void ptc_chain_star_place(int idx, float x, float y, float z) {
    struct Object *star = ptc_find_star(idx);
    if (star == NULL && ptc_in_level()) {
        struct Object *parent = gMarioState != NULL && gMarioState->marioObj != NULL
                              ? gMarioState->marioObj : &gMacroObjectDefaultParent;
        star = spawn_object_abs_with_rot(parent, 0, MODEL_STAR, bhvPtcDebugStar,
                                         (s16) x, (s16) y, (s16) z, 0, 0, 0);
        if (star != NULL) {
            star->oBehParams = (u32) idx << 24;
        }
    }
    if (star != NULL) {
        star->oPosX = x;
        star->oPosY = y;
        star->oPosZ = z;
        star->oInteractStatus = 0;
        star->oInteractType = INTERACT_COIN;
        star->oDamageOrCoinValue = 0;
        star->activeFlags |= ACTIVE_FLAG_ACTIVE;
        star->header.gfx.scale[0] = 1.0f;
        star->header.gfx.scale[1] = 1.0f;
        star->header.gfx.scale[2] = 1.0f;
    }
    if (idx >= 0 && idx < 2) {
        s_starCollected[idx] = 0;
    }
}

static void ptc_chain_camera_setup(void) {
    extern void custom_camera_apply_mode(int mode);
    extern float gfx_cam_dist, gfx_cam_height;
    extern int gfx_stereo_2d, gfx_stereo_same_both;

    custom_camera_apply_mode(3);
    gfx_cam_dist = 700.0f;
    gfx_cam_height = 110.0f;
    gCameraMovementFlags |= CAM_MOVE_INIT_CAMERA;
    /* Flat capture: the Leia weave makes stills unreadable. */
    gfx_stereo_2d = 1;
    gfx_stereo_same_both = 1;
}

static void ptc_chain_spawn_boxes(void) {
    boxphys_reset();
    /* A full 4-layer Box3D pyramid is the melee hero beat; the ball blaster
     * gets its own smaller tower so it does not destroy the pyramid first. */
    boxphys_test_pyramid(CH_BOX_X, 0.0f, CH_BOX_PYRAMID_Z);
    boxphys_test_spawn(-700.0f, 60.0f, -1150.0f);   /* lone ball target */
}

static void ptc_chain_shoot_player_portals(void) {
    Vec3f aOrig = { 1700.0f, 1050.0f, -2900.0f };
    Vec3f aTgt  = { 1900.0f, 1040.0f, -2900.0f };
    Vec3f bOrig = { -600.0f, 2200.0f, -2980.0f };
    Vec3f bTgt  = { -600.0f, 2799.0f, -2980.0f };

    portal_shoot_at(0, aOrig, aTgt);
    portal_shoot_at(1, bOrig, bTgt);
}

static void ptc_chain_shoot_goomba_portals(void) {
    /* Floor portal in front of Mario, ceiling portal above it: the goomba
     * drops in and comes out of the ceiling for the fall-damage finish. */
    Vec3f aOrig = { -600.0f, 1620.0f, -3020.0f };
    Vec3f aTgt  = { -600.0f, 1500.0f, -3020.0f };
    Vec3f bOrig = { -600.0f, 2680.0f, -3020.0f };
    Vec3f bTgt  = { -600.0f, 2800.0f, -3020.0f };
    /* portal 1 = orange floor, portal 0 = blue ceiling */
    portal_shoot_at(1, aOrig, aTgt);
    portal_shoot_at(0, bOrig, bTgt);
}

/* BAM: move the blue exit one portal width sideways. */
static void ptc_chain_shift_goomba_portal(void) {
    Vec3f bOrig = { -600.0f, 2680.0f, -2880.0f };
    Vec3f bTgt  = { -600.0f, 2800.0f, -2880.0f };
    portal_shoot_at(0, bOrig, bTgt);   /* blue ceiling */
}

static void ptc_chain_setup(void) {
    struct MarioState *m = gMarioState;

    s_chainMode = CH_BOX_BALL;
    s_chainTimer = 0;
    s_chainFlags = 0;
    s_cineOn = 1;
    s_cineInit = 0;
    {
        extern int gfx_stereo_fov_pct;
        s_cineSavedFov = gfx_stereo_fov_pct;
    }
    s_chainJumped = 0;
    /* The relay window is black where the ceiling pair faces open air, which
     * reads as a hole in the trailer; draw the flat portal fan instead. */
    portal_view_set(0);
    movekit_force_retract(0);
    s_chainHudFlags = gHudDisplay.flags;
    s_chainHudSaved = 1;
    s_finaleSplatSaved[0] = gfx_splat_active;
    s_finaleSplatSaved[1] = gfx_splat_target;
    s_finaleSplatSaved[2] = gfx_splat_shape;
    s_finaleSplatSaved[3] = gfx_splat_density;
    gHudDisplay.flags &= ~(HUD_DISPLAY_FLAG_CAMERA_AND_POWER
                           | HUD_DISPLAY_FLAG_EMPHASIZE_POWER);
    for (int gi = 0; gi < CH_GOOMBA_N; gi++) {
        s_chainGoombaState[gi] = 0;
        s_chainGoomba[gi] = NULL;
    }
    s_chainGoombaCross = 0;

    {
        extern void custom_camera_set_yaw_hint(int on, s16 yaw);
        custom_camera_set_yaw_hint(0, 0);
    }
    portal_reset();
    portal_give_gun();
    movekit_set_flag(0, 1);
    movekit_set_flag(1, 1);
    movekit_set_flag(2, 0);
    movekit_force_trigger(0);

    {
        char nb[160];
        snprintf(nb, sizeof(nb), "CUSTOM CAM | %s", ptc_chain_note(s_chainMode));
        gfx_set_debug_note(nb);
    }
    {
        extern void custom_camera_set_freelook(int on, const Vec3f pos, const Vec3f focus);
        extern void custom_camera_reset(void);
        custom_camera_set_freelook(0, NULL, NULL);
        custom_camera_reset();
    }
    ptc_chain_star_place(0, CH_STAR1_X, CH_STAR1_Y, CH_STAR1_Z);
    ptc_chain_star_place(1, CH_STAR2_X, CH_STAR2_Y, CH_STAR2_Z);
    ptc_chain_spawn_boxes();
    ptc_chain_shoot_player_portals();
    ptc_chain_camera_setup();

    if (m != NULL) {
        m->pos[0] = CH_BOX_X;
        m->pos[1] = 0.0f;
        m->pos[2] = -1000.0f;
        m->vel[0] = m->vel[1] = m->vel[2] = 0.0f;
        m->forwardVel = 0.0f;
        m->faceAngle[1] = 0x8000;      /* south, down the course */
        m->intendedMag = 0.0f;
        m->intendedYaw = 0x8000;
        set_mario_action(m, ACT_IDLE, 0);
        if (m->marioObj != NULL) {
            vec3f_copy(m->marioObj->header.gfx.pos, m->pos);
            m->marioObj->header.gfx.unk38.animFrame = 0;
        }
    }
    s_chainTeleBase = portal_teleport_count();
}

static void ptc_chain_place_ground(float x, float y, float z, s16 yaw) {
    struct MarioState *m = gMarioState;
    if (m == NULL) {
        return;
    }
    m->pos[0] = x;
    m->pos[1] = y;
    m->pos[2] = z;
    m->vel[0] = m->vel[1] = m->vel[2] = 0.0f;
    m->forwardVel = 0.0f;
    m->faceAngle[1] = yaw;
    set_mario_action(m, ACT_IDLE, 0);
    if (m->marioObj != NULL) {
        vec3f_copy(m->marioObj->header.gfx.pos, m->pos);
    }
    gCameraMovementFlags |= CAM_MOVE_INIT_CAMERA;
}

static void ptc_chain_place(float x, float y, float z, s16 yaw) {
    struct MarioState *m = gMarioState;
    if (m == NULL) {
        return;
    }
    m->pos[0] = x;
    m->pos[1] = y;
    m->pos[2] = z;
    m->vel[0] = m->vel[1] = m->vel[2] = 0.0f;
    m->forwardVel = 0.0f;
    m->faceAngle[1] = yaw;
    set_mario_action(m, ACT_FREEFALL, 0);
    if (m->marioObj != NULL) {
        vec3f_copy(m->marioObj->header.gfx.pos, m->pos);
    }
    gCameraMovementFlags |= CAM_MOVE_INIT_CAMERA;
}

/* Per-tick scripted stick: walk towards a world point. */
static void ptc_chain_walk_to(struct MarioState *m, float x, float z) {
    float dx, dz, len;
    if (m == NULL) {
        return;
    }
    dx = x - m->pos[0];
    dz = z - m->pos[2];
    len = sqrtf(dx * dx + dz * dz);
    if (len > 1.0f) {
        m->intendedYaw = (s16) atan2s(dz, dx);   /* SM64 yaw: atan2s(z, x) */
    }
    m->intendedMag = 32.0f;
    m->input |= INPUT_NONZERO_ANALOG;
    m->input &= ~INPUT_UNKNOWN_5;
}

static void ptc_chain_stick(struct MarioState *m, s16 yaw, float mag) {
    if (m == NULL) {
        return;
    }
    m->intendedYaw = yaw;
    m->intendedMag = mag;
    m->input |= INPUT_NONZERO_ANALOG;
    m->input &= ~INPUT_UNKNOWN_5;
}

static void ptc_chain_hint(int on, float x, float y, float z) {
    extern void custom_camera_set_focus_hint(int on, const Vec3f pos);
    Vec3f p = { x, y, z };
    custom_camera_set_focus_hint(on, p);
}

static const char *ptc_chain_note(int ph) {
    extern void gfx_set_debug_note(const char *s);
extern int gfx_cam_mode;
    switch (ph) {
        case CH_BOX_BALL:      return "NINJA 1/8 box3d: ball blast, punch, kick, ground pound";
        case CH_BOX_WALK_PUNCH:return "NINJA 1/8: walk to the punch cube";
        case CH_BOX_PUNCH:     return "NINJA 1/8: PUNCH the cube";
        case CH_BOX_WALK_KICK: return "NINJA 1/8: line up the kick cube";
        case CH_BOX_KICK:      return "NINJA 1/8: JUMP KICK the cube";
        case CH_BOX_WALK_SLAM: return "NINJA 1/8: walk to the slam cube";
        case CH_BOX_SLAM_JUMP:
        case CH_BOX_SLAM:      return "NINJA 1/8: GROUND POUND the pyramid";
        case CH_JET_L1:        return "NINJA 2/8 jetpack: fly up to the L1 pad";
        case CH_RUN_WALL:      return "NINJA 3/8 wallrun: run east off L1, jump into the wall";
        case CH_WALLRUN:       return "NINJA 3/8 wallrun: 52 ticks along the wall";
        case CH_WALL_LAND:     return "NINJA 3/8 wallrun: land on N2";
        case CH_GRAPPLE_AIM:
        case CH_GRAPPLE_FIRE:
        case CH_GRAPPLE_HOLD:  return "NINJA 4/8 grapple: hook the ceiling and swing";
        case CH_GRAPPLE_RELEASE: return "NINJA 4/8 grapple: release into the gap";
        case CH_GRAPPLE_TRAVEL:  return "NINJA 4/8 grapple: carry to the second hook";
        case CH_GRAPPLE_AIM2:
        case CH_GRAPPLE_FIRE2:   return "NINJA 4/8 grapple: re-grab";
        case CH_GRAPPLE_HOLD2:   return "NINJA 4/8 grapple: SWING 2";
        case CH_GRAPPLE_RELEASE2:return "NINJA 4/8 grapple: release 2 over N4";
        case CH_LAND_N4:         return "NINJA 4/8 grapple: land on N4";
        case CH_STAR1:         return "NINJA 4/8: collect the N4 star";
        case CH_JET_N5:        return "NINJA 5/8 jetpack: climb east to N5";
        case CH_LAND_N5:       return "NINJA 5/8 jetpack: land on N5";
        case CH_PORTAL_RUN:    return "NINJA 6/8 portals: run into the N7 portal";
        case CH_PORTAL_FALL:   return "NINJA 6/8 portals: drop out of the ceiling onto N6";
        case CH_STAR2:         return "NINJA 6/8: collect the N6 star";
        case CH_PORTAL_LOOP:   return "NINJA 6/8 portals: walk the A/B loop x4";
        case CH_GOOMBA_SETUP:
        case CH_GOOMBA_LOOP:   return "NINJA 7/8: GOOMBA GRIEFING";
        case CH_FINALE:        return "NINJA: SPLAT 360";
        case CH_DONE:          return "NINJA: run complete";
        default:               return "NINJA: ready";
    }
}

static void ptc_chain_advance(int next) {
    char buf[160];
    s_chainMode = next;
    s_chainTimer = 0;
    if (next == CH_RUN_WALL) {
        s_chainJumped = 0;
        movekit_set_flag(0, 1);   /* wall-run back on for the wall section */
    }
    snprintf(buf, sizeof(buf), "CUSTOM CAM | %s", ptc_chain_note(next));
    gfx_set_debug_note(buf);
}

/* Goomba lemming: walks into the wall portal, drops out of the ceiling one. */
static void ptc_chain_goomba_place(struct Object *o, float x, float y, float z, s16 yaw) {
    if (o == NULL) {
        return;
    }
    o->oPosX = x;
    o->oPosY = y;
    o->oPosZ = z;
    o->oHomeX = x;
    o->oHomeY = y;
    o->oHomeZ = z;
    o->oForwardVel = 0.0f;
    o->oVelX = o->oVelY = o->oVelZ = 0.0f;
    o->oGravity = 0.0f;
    o->oMoveAngleYaw = yaw;
    o->oFaceAngleYaw = yaw;
    o->oDrawingDistance = 12000.0f;
    o->header.gfx.node.flags |= GRAPH_RENDER_ACTIVE;
    o->header.gfx.node.flags &= ~GRAPH_RENDER_INVISIBLE;
    vec3f_copy(o->header.gfx.pos, (Vec3f){ x, y, z });
    o->header.gfx.angle[1] = yaw;
}

static void ptc_chain_goomba_tick(void) {
    for (int gi = 0; gi < CH_GOOMBA_N; gi++) {
        struct Object *o = s_chainGoomba[gi];
        if (o == NULL) {
            continue;
        }
        if (s_chainGoombaState[gi] == 0) {
            /* Waddle at Mario; portals are shot just before it arrives. */
            s_chainGoombaZ[gi] -= 4.0f;
            if (!s_chainGoombaPortalPlaced && s_chainGoombaZ[gi] < -2900.0f) {
                ptc_chain_shoot_goomba_portals();
                s_chainGoombaPortalPlaced = 1;
            }
            if (s_chainGoombaZ[gi] < -3005.0f) {
                s_chainGoombaState[gi] = 1;
                s_chainGoombaX[gi] = -600.0f;
                s_chainGoombaZ[gi] = -3020.0f;
                s_chainGoombaY[gi] = 2790.0f;
                s_chainGoombaVY[gi] = -6.0f;
                s_chainGoombaLoops = 0;
                s_chainGoombaCross++;
            }
        } else if (s_chainGoombaState[gi] == 1) {
            /* Accelerating portal loop: ceiling -> floor -> ceiling. */
            s_chainGoombaVY[gi] -= 3.2f;
            if (s_chainGoombaVY[gi] < -70.0f) s_chainGoombaVY[gi] = -70.0f;
            s_chainGoombaY[gi] += s_chainGoombaVY[gi];
            if (s_chainGoombaY[gi] <= 1500.0f) {
                s_chainGoombaY[gi] = 1500.0f;
                s_chainGoombaLoops++;
                if (s_chainGoombaLoops < 8) {
                    /* back out of the ceiling, carrying the extra speed */
                    s_chainGoombaY[gi] = 2790.0f;
                    s_chainGoombaZ[gi] = -3020.0f;
                    s_chainGoombaCross++;
                } else {
                    /* BAM: exit is now beside the orange portal, not above it */
                    if (!s_chainGoombaShifted) {
                        ptc_chain_shift_goomba_portal();
                        s_chainGoombaShifted = 1;
                    }
                    s_chainGoombaState[gi] = 3;
                    s_chainGoombaY[gi] = 2790.0f;
                    s_chainGoombaZ[gi] = -2880.0f;
                    s_chainGoombaVY[gi] = -70.0f;
                    s_chainGoombaCross++;
                }
            }
        } else if (s_chainGoombaState[gi] == 3) {
            /* Bullet: out of the shifted portal onto the hard ground. */
            s_chainGoombaY[gi] -= 70.0f;
            if (s_chainGoombaY[gi] <= 1500.0f) {
                s_chainGoombaY[gi] = 1500.0f;
                s_chainGoombaState[gi] = 4;
                s_chainGoombaSquash = 0;
                play_sound(SOUND_ACTION_TERRAIN_HEAVY_LANDING, gDefaultSoundArgs);
            }
        } else if (s_chainGoombaState[gi] == 4) {
            /* Splat: flatten, hold for a beat, then fade out. */
            s_chainGoombaSquash++;
            if (s_chainGoombaSquash < 8) {
                o->header.gfx.scale[0] = 1.9f;
                o->header.gfx.scale[1] = 0.30f;
                o->header.gfx.scale[2] = 1.9f;
            } else if (s_chainGoombaSquash < 18) {
                o->header.gfx.scale[1] = 0.30f + 0.05f * (float)(s_chainGoombaSquash - 8);
            } else {
                o->header.gfx.node.flags &= ~GRAPH_RENDER_ACTIVE;
                s_chainGoombaState[gi] = 5;
            }
        } else {
            continue;
        }
        ptc_chain_goomba_place(o, s_chainGoombaX[gi], s_chainGoombaY[gi],
                               s_chainGoombaZ[gi], 0xC000);   /* face -Z */
    }
}

static void ptc_chain_goomba_start(void) {
    extern float gfx_cam_dist, gfx_cam_height;
    extern void custom_camera_set_yaw_hint(int on, s16 yaw);
    gfx_cam_dist = 700.0f;
    gfx_cam_height = 180.0f;
    custom_camera_set_yaw_hint(1, 0x8000);
    s_chainGoombaPortalPlaced = 0;
    s_chainGoombaLoops = 0;
    s_chainGoombaShifted = 0;
    s_chainGoombaSquash = 0;
    for (int gi = 0; gi < CH_GOOMBA_N; gi++) {
        s_chainGoombaState[gi] = 0;
        s_chainGoombaTimer[gi] = 0;
        s_chainGoombaX[gi] = -600.0f;
        s_chainGoombaY[gi] = 1500.0f;
        s_chainGoombaZ[gi] = -2720.0f;
        s_chainGoombaVY[gi] = 0.0f;
        if (s_chainGoomba[gi] == NULL) {
            struct Object *parent = gMarioState != NULL && gMarioState->marioObj != NULL
                                  ? gMarioState->marioObj : &gMacroObjectDefaultParent;
            s_chainGoomba[gi] = spawn_object_abs_with_rot(parent, 0, MODEL_GOOMBA, bhvStaticObject,
                                                          (s16) s_chainGoombaX[gi],
                                                          (s16) s_chainGoombaY[gi],
                                                          (s16) s_chainGoombaZ[gi], 0, 0xC000, 0);
        }
        ptc_chain_goomba_place(s_chainGoomba[gi], s_chainGoombaX[gi], s_chainGoombaY[gi],
                               s_chainGoombaZ[gi], 0xC000);
    }
    ptc_chain_hint(1, -600.0f, 1900.0f, -3020.0f);
}

/* ------------------------------------------------- section captions */

static const char *ptc_chain_caption(int ph) {
    switch (ph) {
        case CH_BOX_BALL:
        case CH_BOX_BALL_WAIT:   return "BOX3D PYRAMID";
        case CH_BOX_WALK_SLAM:
        case CH_BOX_SLAM_JUMP:
        case CH_BOX_SLAM:        return "GROUND POUND";
        case CH_BOX_WALK_PUNCH:
        case CH_BOX_PUNCH:       return "PUNCH";
        case CH_BOX_WALK_KICK:
        case CH_BOX_KICK:        return "KICK";
        case CH_JET_L1:
        case CH_JET_N5:          return "ROCKET";
        case CH_RUN_WALL:
        case CH_WALLRUN:
        case CH_WALL_LAND:       return "WALL RUN";
        case CH_GRAPPLE_AIM:
        case CH_GRAPPLE_FIRE:
        case CH_GRAPPLE_HOLD:
        case CH_GRAPPLE_RELEASE: return "CEILING GRAPPLE";
        case CH_GRAPPLE_TRAVEL:  return "RELEASE";
        case CH_GRAPPLE_AIM2:
        case CH_GRAPPLE_FIRE2:   return "RE-GRAB";
        case CH_GRAPPLE_HOLD2:   return "SWING 2";
        case CH_GRAPPLE_RELEASE2:return "RELEASE 2";
        case CH_LAND_N4:
        case CH_STAR1:
        case CH_STAR2:           return "STAR";
        case CH_PORTAL_RUN:
        case CH_PORTAL_FALL:     return "PORTALS";
        case CH_PORTAL_LOOP:     return "PORTAL LOOP";
        case CH_GOOMBA_SETUP:
        case CH_GOOMBA_LOOP:     return "GOOMBA GRIEFING";
        case CH_FINALE:          return "SPLAT 360";
        default:                 return NULL;
    }
}

/* Called once per rendered frame from render_hud(); uses the game's own
 * colourful HUD lettering so captions read as an in-game UI element. */
void ptc_chain_hud_text(void) {
    extern void print_caption(s32 x, s32 y, const char *str);
    extern void print_quick_dev_marker(const char *str);
    extern int gfx_opengl_denoise_state(int which);
    extern int gfx_rate_state(int which);
    extern int gfx_quick_dev_enabled(void);
    extern int gfx_quick_dev_level(void);
    const char *cap;

    /* QUICK DEV marker: bottom-left, so a screenshot states how it was taken. */
    {
        char qd[24];
        if (gfx_quick_dev_enabled()) {
            snprintf(qd, sizeof(qd), "QUICK DEV L%d", gfx_quick_dev_level());
            print_quick_dev_marker(qd);
        } else {
            print_quick_dev_marker(NULL);
        }
    }
    /* Denoiser/rate-limit comparison captions: read live state every frame so
     * the labels always match the settings being shown. */
    if (gfx_opengl_denoise_state(7)) {
        char line[128];
        int path = gfx_opengl_denoise_state(0);
        int don = gfx_opengl_denoise_state(1);
        extern void print_caption2_small(s32 x, s32 y, const char *str);
        extern int gfx_opengl_ssfx_get(int which);
        if (gfx_rate_state(1) > 0) {
            snprintf(line, sizeof(line), "PATH %s | DN %s | I%d P%d F%d B%d R%d | %dFPS%s",
                     path ? "ALT" : "MAIN", don ? "ON" : "OFF",
                     gfx_opengl_denoise_state(2), gfx_opengl_denoise_state(3),
                     gfx_opengl_denoise_state(5), gfx_opengl_denoise_state(6),
                     gfx_opengl_denoise_state(8),
                     gfx_rate_state(1), gfx_rate_state(2) ? " ADAPT" : "");
        } else {
            snprintf(line, sizeof(line), "PATH %s | DN %s | I%d P%d F%d B%d R%d | MODE FPS%s",
                     path ? "ALT" : "MAIN", don ? "ON" : "OFF",
                     gfx_opengl_denoise_state(2), gfx_opengl_denoise_state(3),
                     gfx_opengl_denoise_state(5), gfx_opengl_denoise_state(6),
                     gfx_opengl_denoise_state(8),
                     gfx_rate_state(2) ? " ADAPT" : "");
        }
        if (gfx_opengl_ssfx_get(100)) {
            char mb[64];
            snprintf(mb, sizeof(mb), " | MB s%d x%d r%d",
                     gfx_opengl_ssfx_get(101), gfx_opengl_ssfx_get(142),
                     gfx_opengl_ssfx_get(144));
            strncat(line, mb, sizeof(line) - strlen(line) - 1);
        }
        if (gfx_opengl_ssfx_get(31) == 9) {
            strncat(line, " | VIEW VECTORS", sizeof(line) - strlen(line) - 1);
        }
        {
            extern int g_sim_slow_div;
            extern int g_sim_interp;
            if (g_sim_slow_div > 1) {
                char sm[48];
                snprintf(sm, sizeof(sm), " | SIM 1/%d%s", g_sim_slow_div,
                         g_sim_interp ? "" : " NOINTERP");
                strncat(line, sm, sizeof(line) - strlen(line) - 1);
            }
        }
        print_caption2_small(SCREEN_WIDTH / 2, 34, line);
    }
    /* Momentum demo: label the drop and the launch even though it is not part
     * of the cinematic chain (this is the playable proof that portals preserve
     * velocity). */
    if (s_showOn) {
        print_caption(SCREEN_WIDTH / 2, 168, "LIGHT LOOP");
        return;
    }
    if (s_testMode == 1 && s_testActive
        && s_phase >= PTC_PHASE_MOM_DROP && s_phase <= PTC_PHASE_MOM_FLY) {
        print_caption(SCREEN_WIDTH / 2, 168,
                      s_phase == PTC_PHASE_MOM_FLY ? "MOMENTUM" : "PORTAL LAUNCH");
        return;
    }
    if (s_chainMode == CH_OFF || !s_cineOn) {
        return;
    }
    cap = ptc_chain_caption(s_chainMode);
    /* Grapple phases are called out individually. */
    if (s_chainMode == CH_GOOMBA_LOOP) {
        static char gbuf[24];
        if (s_chainGoombaState[0] == 0) {
            cap = "GOOMBA GRIEFING";
        } else if (s_chainGoombaState[0] == 1) {
            snprintf(gbuf, sizeof(gbuf), "LOOP %d/8", s_chainGoombaLoops + 1);
            cap = gbuf;
        } else if (s_chainGoombaState[0] == 3) {
            cap = "SHIFT! BULLET";
        } else if (s_chainGoombaState[0] >= 4) {
            cap = "SPLAT";
        }
    }
    if (s_chainMode == CH_PORTAL_LOOP) {
        static char pbuf[24];
        if (s_mpwPhase == 2) {
            snprintf(pbuf, sizeof(pbuf), "PASS %d/4", s_mpwPasses + 1);
            cap = pbuf;
        } else {
            cap = "PORTAL LOOP";
        }
    }
    if (s_chainMode == CH_GRAPPLE_HOLD) {
        cap = (s_chainTimer > 35) ? "RETRACT" : "SWING";
    } else if (s_chainMode == CH_GRAPPLE_RELEASE) {
        cap = "RELEASE";
    } else if (s_chainMode == CH_GRAPPLE_HOLD2) {
        cap = "SWING 2";
    } else if (s_chainMode == CH_GRAPPLE_RELEASE2) {
        cap = "RELEASE 2";
    } else if (s_chainMode == CH_GRAPPLE_AIM2 || s_chainMode == CH_GRAPPLE_FIRE2) {
        cap = "RE-GRAB";
    }
    if (cap != NULL) {
        print_caption(SCREEN_WIDTH / 2, 168, cap);
    }
}

/* ------------------------------------------------ cinematic camera track */

/* The trailer camera: one continuous, eased path.  Each chain phase asks for a
 * shot (position / look-at / FOV); the path glides between them, so the run is
 * a single take with no cuts.  Freelook is used so the framer is not fighting
 * the gameplay chase camera; WALL NOCLIP keeps walls out of the way. */
static int s_cineOn;
static int s_cineInit;
static int s_cineSavedFov = 100;
static Vec3f s_cinePos;
static Vec3f s_cineFocus;
static float s_cineFov = 120.0f;

static void chain_cam_ease(Vec3f dst, Vec3f src, float k) {
    dst[0] += (src[0] - dst[0]) * k;
    dst[1] += (src[1] - dst[1]) * k;
    dst[2] += (src[2] - dst[2]) * k;
}

static void ptc_chain_camera(struct MarioState *m) {
    extern void custom_camera_set_freelook(int on, const Vec3f pos, const Vec3f focus);
    extern int gfx_stereo_fov_pct;
    Vec3f pos, focus, mpos;
    float fov, k;
    int ph = s_chainMode;

    if (!s_cineOn || m == NULL) {
        return;
    }
    if (ph == CH_PORTAL_LOOP) {
        return;   /* the smoke-test rig owns the camera while it runs */
    }
    vec3f_copy(mpos, m->pos);
    mpos[1] += 70.0f;

    k = 0.12f;
    fov = 130.0f;
    pos[0] = -150.0f; pos[1] = 230.0f; pos[2] = -820.0f;
    focus[0] = CH_BOX_X; focus[1] = 120.0f; focus[2] = -1280.0f;

    switch (ph) {
        case CH_BOX_WALK_PUNCH:
        case CH_BOX_PUNCH:
            /* Close tracking shot so the punch and the cube it launches read. */
            pos[0] = mpos[0] + 330.0f; pos[1] = mpos[1] + 230.0f; pos[2] = mpos[2] + 300.0f;
            vec3f_copy(focus, mpos);
            fov = 130.0f; k = 0.20f;
            break;
        case CH_BOX_WALK_KICK:
        case CH_BOX_KICK:
            /* Mirror side for the kick, same close read. */
            pos[0] = mpos[0] - 330.0f; pos[1] = mpos[1] + 230.0f; pos[2] = mpos[2] + 300.0f;
            vec3f_copy(focus, mpos);
            fov = 130.0f; k = 0.20f;
            break;
        case CH_BOX_WALK_SLAM:
        case CH_BOX_SLAM_JUMP:
        case CH_BOX_SLAM:
            /* Wide enough to hold Mario on the pyramid top and the debris. */
            pos[0] = -450.0f; pos[1] = 700.0f; pos[2] = -250.0f;
            focus[0] = CH_BOX_X; focus[1] = 300.0f; focus[2] = CH_BOX_PYRAMID_Z;
            fov = 160.0f; k = 0.16f;
            break;

        case CH_JET_L1:
            pos[0] = -120.0f; pos[1] = 260.0f; pos[2] = -1900.0f;
            vec3f_copy(focus, mpos);
            fov = 145.0f; k = 0.12f;
            break;

        case CH_RUN_WALL:
        case CH_WALLRUN:
        case CH_WALL_LAND:
            /* West of the run surface, above N2 (the old y=300 camera was
             * inside that platform now that it is drawn). */
            pos[0] = -620.0f; pos[1] = 540.0f; pos[2] = mpos[2] + 300.0f;
            vec3f_copy(focus, mpos);
            focus[1] = mpos[1] - 10.0f;
            fov = 150.0f; k = 0.22f;
            break;

        case CH_GRAPPLE_AIM:
        case CH_GRAPPLE_FIRE:
            /* North of the gap: N3 now blocks the east/west sight lines. */
            pos[0] = -150.0f; pos[1] = 1150.0f; pos[2] = -2050.0f;
            focus[0] = -450.0f; focus[1] = 900.0f; focus[2] = -2800.0f;
            fov = 165.0f; k = 0.14f;
            break;
        case CH_GRAPPLE_HOLD:
        case CH_GRAPPLE_RELEASE:
        case CH_GRAPPLE_TRAVEL:
            /* Wide shot from the north: the whole rope and the swing arc,
             * with N3 behind Mario instead of in front of him. */
            pos[0] = 0.0f; pos[1] = 1300.0f; pos[2] = -1900.0f;
            focus[0] = -250.0f; focus[1] = 800.0f; focus[2] = -2700.0f;
            fov = 180.0f; k = 0.14f;
            break;
        case CH_GRAPPLE_AIM2:
        case CH_GRAPPLE_FIRE2:
        case CH_GRAPPLE_HOLD2:
        case CH_GRAPPLE_RELEASE2: {
            /* Track the second hook: frame the midpoint between Mario and the
             * anchor so the rope, the hook and the arc all read.  Before the
             * anchor exists (TRAVEL) assume it is directly above. */
            float ax = s_chainAnchorB[0];
            float ay = s_chainAnchorB[1];
            if (ay < 1.0f) {
                ax = mpos[0];
                ay = mpos[1] + 1200.0f;
            }
            pos[0] = mpos[0] - 300.0f;
            pos[1] = mpos[1] + 280.0f;
            pos[2] = mpos[2] + 950.0f;
            focus[0] = (mpos[0] + ax) * 0.5f;
            focus[1] = (mpos[1] + ay) * 0.5f - 80.0f;
            focus[2] = mpos[2];
            fov = 160.0f; k = 0.08f;
            break;
        }
        case CH_LAND_N4:
        case CH_STAR1:
            pos[0] = 700.0f; pos[1] = 520.0f; pos[2] = -2680.0f;
            vec3f_copy(focus, mpos);
            fov = 95.0f; k = 0.12f;
            break;

        case CH_JET_N5:
            pos[0] = 620.0f; pos[1] = 340.0f; pos[2] = -2280.0f;
            vec3f_copy(focus, mpos);
            fov = 165.0f; k = 0.12f;
            break;
        case CH_LAND_N5:
            pos[0] = 1250.0f; pos[1] = 780.0f; pos[2] = -2480.0f;
            vec3f_copy(focus, mpos);
            fov = 140.0f; k = 0.12f;
            break;

        case CH_PORTAL_RUN:
            /* Behind-and-above Mario, the gate portal filling the right side. */
            pos[0] = 1550.0f; pos[1] = 1260.0f; pos[2] = -2600.0f;
            focus[0] = 1880.0f; focus[1] = 1080.0f; focus[2] = -2890.0f;
            fov = 112.0f; k = 0.18f;
            break;
        case CH_PORTAL_FALL:
            /* Level-on with the drop line: ceiling portal at the top of frame,
             * Mario falling through the middle, N6 below.  Fixed aim so the
             * portal cannot slide out of shot while he falls. */
            pos[0] = 430.0f; pos[1] = 1560.0f; pos[2] = -2980.0f;
            focus[0] = -600.0f; focus[1] = 1980.0f; focus[2] = -2980.0f;
            fov = 168.0f; k = 0.32f;
            break;
        case CH_STAR2:
            /* Three-quarter on the N6 star; the star spawns in frame at the
             * left of the drop line, so the pick-up is visible. */
            pos[0] = 250.0f; pos[1] = 1620.0f; pos[2] = -2980.0f;
            focus[0] = -700.0f; focus[1] = 1650.0f; focus[2] = -3080.0f;
            fov = 130.0f; k = 0.20f;
            break;

        case CH_GOOMBA_LOOP: {
            /* One continuous follow shot: the desired camera is derived from
             * the goomba's live position (rising with it when it comes out of
             * the ceiling) and eased slowly, so it glides instead of snapping
             * between the floor and ceiling framings. */
            float gx = s_chainGoombaX[0];
            float gy = s_chainGoombaY[0];
            float gz = s_chainGoombaZ[0];
            float h = gy - 1500.0f;
            if (h < 0.0f) h = 0.0f;
            pos[0] = gx - 780.0f;
            pos[1] = 1750.0f + h * 0.55f;
            pos[2] = gz + 240.0f;
            focus[0] = gx;
            focus[1] = gy + 50.0f;
            focus[2] = gz;
            fov = 150.0f;
            k = 0.07f;
            break;
        }
        case CH_FINALE: {
            /* 360-degree splat showcase around the chamber. */
            const float PI2 = 6.2831853f;
            int ft = s_chainTimer > 240 ? 240 : s_chainTimer;
            float a = (float) ft * (PI2 / 240.0f) + 0.6f;
            pos[0] = 1700.0f * cosf(a);
            pos[1] = 1500.0f;
            pos[2] = -1200.0f + 1700.0f * sinf(a);
            focus[0] = 0.0f;
            focus[1] = 700.0f;
            focus[2] = -1200.0f;
            fov = 160.0f;
            k = 0.06f;
            break;
        }
        case CH_GOOMBA_SETUP:
            /* Side-on stand-off: Mario, the goomba and the floor between. */
            pos[0] = -40.0f; pos[1] = 1750.0f; pos[2] = -2950.0f;
            focus[0] = -600.0f; focus[1] = 1600.0f; focus[2] = -2970.0f;
            fov = 120.0f; k = 0.16f;
            break;

        default:
            break;
    }

    if (!s_cineInit) {
        vec3f_copy(s_cinePos, pos);
        vec3f_copy(s_cineFocus, focus);
        s_cineFov = fov;
        s_cineInit = 1;
    } else {
        chain_cam_ease(s_cinePos, pos, k);
        chain_cam_ease(s_cineFocus, focus, k * 1.15f);
        s_cineFov += (fov - s_cineFov) * 0.09f;
    }
    custom_camera_set_freelook(1, s_cinePos, s_cineFocus);
    gfx_stereo_fov_pct = (int) (s_cineFov + 0.5f);
}

/* Put the course and Mario back to the start without reloading the level, so
 * `ptc chainloop` can demonstrate the whole run continuously. */
static void ptc_chain_loop_restart(void) {
    struct MarioState *m = gMarioState;

    if (m == NULL) {
        return;
    }
    for (int gi = 0; gi < CH_GOOMBA_N; gi++) {
        s_chainGoombaState[gi] = 0;
        s_chainGoombaTimer[gi] = 0;
        if (s_chainGoomba[gi] != NULL) {
            s_chainGoomba[gi]->header.gfx.node.flags |= GRAPH_RENDER_INVISIBLE;
        }
    }
    s_chainGoombaPortalPlaced = 0;
    s_chainGoombaLoops = 0;
    s_chainGoombaShifted = 0;
    s_chainGoombaSquash = 0;

    ptc_star_reset();
    ptc_chain_star_place(0, CH_STAR1_X, CH_STAR1_Y, CH_STAR1_Z);
    ptc_chain_star_place(1, CH_STAR2_X, CH_STAR2_Y, CH_STAR2_Z);
    ptc_chain_spawn_boxes();
    portal_reset();
    portal_give_gun();
    ptc_chain_shoot_player_portals();

    gfx_splat_active = 0;
    gfx_splat_target = 0;
    gfx_splat_shape = 0;

    m->pos[0] = CH_BOX_X;
    m->pos[1] = 0.0f;
    m->pos[2] = -1000.0f;
    m->vel[0] = m->vel[1] = m->vel[2] = 0.0f;
    m->forwardVel = 0.0f;
    m->faceAngle[1] = 0x8000;
    m->intendedMag = 0.0f;
    set_mario_action(m, ACT_IDLE, 0);
    if (m->marioObj != NULL) {
        vec3f_copy(m->marioObj->header.gfx.pos, m->pos);
        m->marioObj->header.gfx.unk38.animFrame = 0;
    }

    s_chainFlags = 0;
    s_chainTimer = 0;
    s_chainJumped = 0;
    s_chainMode = CH_BOX_BALL;
    s_chainTeleBase = portal_teleport_count();
    s_cineInit = 0;            /* deliberate reset to the opening shot */
}

static void ptc_chain_update(void) {
    struct MarioState *m = gMarioState;

    if (s_chainMode == CH_OFF || m == NULL) {
        return;
    }
    ptc_chain_camera(m);
    s_chainTimer++;

    switch (s_chainMode) {
        case CH_BOX_BALL:
            if (s_chainTimer == 1) {
                ptc_chain_hint(1, -560.0f, 170.0f, -1200.0f);
            }
            if (s_chainTimer == 45) {
                if (!boxphys_equipped()) {
                    boxphys_equip_toggle();
                }
                boxphys_test_fire_at(-700.0f, 90.0f, -1150.0f);
            }
            if (s_chainTimer > 72) {
                s_chainFlags |= CH_FLAG_BALL;
                ptc_chain_hint(0, 0.0f, 0.0f, 0.0f);
                ptc_chain_advance(CH_BOX_WALK_SLAM);
            }
            break;

        case CH_BOX_WALK_SLAM:
            /* Walk right up to the pyramid, then leap onto the top cube. */
            if (m->pos[2] < CH_BOX_D_Z + 50.0f || s_chainTimer > 90) {
                ptc_chain_advance(CH_BOX_SLAM_JUMP);
            }
            break;

        case CH_BOX_SLAM_JUMP:
            /* Short jetpack hop onto the pyramid top: the pack is the only
             * mover with a sane vertical speed here (a raw launch overshoots
             * the room).  It also shows the flames before the slam. */
            if (s_chainTimer == 1) {
                movekit_set_flag(2, 1);
                movekit_force_trigger(1);
                m->faceAngle[1] = 0x8000;
                m->vel[0] = m->vel[2] = 0.0f;
            }
            {
                float dx = CH_BOX_X - m->pos[0];
                float dz = CH_BOX_PYRAMID_Z - m->pos[2];
                float d = sqrtf(dx * dx + dz * dz);
                if (m->action != ACT_GROUND_POUND && d > 25.0f) {
                    float sp = d * 0.10f;
                    if (sp > 6.0f) sp = 6.0f;
                    m->faceAngle[1] = atan2s(dz, dx);
                    m->forwardVel = sp;
                }
            }
            if (m->action != ACT_GROUND_POUND
                && (m->pos[1] > 300.0f || s_chainTimer > 90)) {
                movekit_force_trigger(0);
                movekit_set_flag(2, 0);
                m->forwardVel = 0.0f;
                m->vel[0] = m->vel[1] = m->vel[2] = 0.0f;
                set_mario_action(m, ACT_GROUND_POUND, 0);
            }
            if (s_chainTimer > 4 && ptc_chain_grounded(m)) {
                s_chainFlags |= CH_FLAG_SLAM;
                ptc_chain_advance(CH_BOX_WALK_PUNCH);
            } else if (s_chainTimer > 150) {
                s_chainFlags |= CH_FLAG_SLAM;
                ptc_chain_advance(CH_BOX_WALK_PUNCH);
            }
            break;

        case CH_BOX_WALK_PUNCH:
            if (s_chainTimer == 1) {
                /* Step off the debris and punch a nearby ground cube. */
                if (!boxphys_nearest_box(m->pos[0], m->pos[2], 150.0f,
                                         &s_chainTargetX, &s_chainTargetZ)) {
                    s_chainTargetX = CH_BOX_X + 240.0f;
                    s_chainTargetZ = CH_BOX_PYRAMID_Z + 40.0f;
                }
            }
            {
                float dx = s_chainTargetX - m->pos[0];
                float dz = s_chainTargetZ - m->pos[2];
                if (sqrtf(dx * dx + dz * dz) < 110.0f || s_chainTimer > 90) {
                    ptc_chain_advance(CH_BOX_PUNCH);
                }
            }
            break;

        case CH_BOX_PUNCH:
            if (s_chainTimer == 1) {
                float dx = s_chainTargetX - m->pos[0];
                float dz = s_chainTargetZ - m->pos[2];
                m->faceAngle[1] = atan2s(dz, dx);
                set_mario_action(m, ACT_PUNCHING, 0);
            }
            if (s_chainTimer > 30) {
                s_chainFlags |= CH_FLAG_PUNCH;
                ptc_chain_advance(CH_BOX_WALK_KICK);
            }
            break;

        case CH_BOX_WALK_KICK:
            if (s_chainTimer == 1) {
                if (!boxphys_nearest_box(m->pos[0], m->pos[2], 100.0f,
                                         &s_chainTargetX, &s_chainTargetZ)) {
                    s_chainTargetX = CH_BOX_X - 200.0f;
                    s_chainTargetZ = CH_BOX_PYRAMID_Z - 60.0f;
                }
            }
            {
                float dx = s_chainTargetX - m->pos[0];
                float dz = s_chainTargetZ - m->pos[2];
                if (sqrtf(dx * dx + dz * dz) < 140.0f || s_chainTimer > 90) {
                    ptc_chain_advance(CH_BOX_KICK);
                }
            }
            break;

        case CH_BOX_KICK:
            if (s_chainTimer == 1) {
                float dx = s_chainTargetX - m->pos[0];
                float dz = s_chainTargetZ - m->pos[2];
                m->faceAngle[1] = atan2s(dz, dx);
                m->vel[1] = 10.0f;
                set_mario_action(m, ACT_JUMP_KICK, 0);
            }
            if (s_chainTimer > 34) {
                s_chainFlags |= CH_FLAG_KICK;
                ptc_chain_advance(CH_JET_L1);
            }
            break;

        case CH_JET_L1:
            if (s_chainTimer == 1) {
                movekit_set_flag(0, 0);   /* no spurious wall-run during ascent */
                movekit_set_flag(2, 1);
                movekit_force_trigger(1);
            }
            if ((m->pos[1] > 500.0f && m->pos[0] < -800.0f && m->pos[2] < -1400.0f)
                || s_chainTimer > 55) {
                movekit_force_trigger(0);
                movekit_set_flag(2, 0);
                /* Kill the horizontal carry so he drops onto L1 instead of
                 * sliding off the west edge into a knockback. */
                m->forwardVel = 0.0f;
                m->vel[0] = 0.0f;
                m->vel[2] = 0.0f;
                s_chainFlags |= CH_FLAG_JETPACK;
                ptc_chain_advance(CH_RUN_WALL);
            } else if (s_chainTimer > 90) {
                ptc_chain_advance(CH_RUN_WALL);
            }
            break;

        case CH_RUN_WALL:
            /* Jump near the L1 edge.  A short assist once he is beside the
             * wall commits him into it: the wall-run trigger wants speed >= 30
             * plus velocity into the surface, and a scripted run-up alone is
             * marginal at this distance. */
            if (!s_chainJumped && ptc_chain_grounded(m) && m->pos[0] > -720.0f) {
                /* Commit the jump towards the wall.  The air action rebuilds
                 * the horizontal velocity from forwardVel every tick, so that
                 * is what has to carry the speed, not m->vel. */
                m->forwardVel = 34.0f;
                m->faceAngle[1] = 0x4000;   /* east */
                m->vel[0] = 34.0f;
                m->vel[1] = 20.0f;
                m->vel[2] = 0.0f;
                set_mario_action(m, ACT_FREEFALL, 0);
                s_chainJumped = 1;
            }

            if (m->action == ACT_WALLRUN) {
                s_chainFlags |= CH_FLAG_WALLRUN;
                ptc_chain_advance(CH_WALLRUN);
            } else if (s_chainTimer > 70) {
                ptc_chain_advance(CH_WALL_LAND);
            }
            break;

        case CH_WALLRUN:
            if (m->action != ACT_WALLRUN) {
                ptc_chain_advance(CH_WALL_LAND);
            }
            break;

        case CH_WALL_LAND:
            if (ptc_chain_grounded(m) && m->pos[1] < 600.0f) {
                ptc_chain_advance(CH_GRAPPLE_AIM);
            } else if (s_chainTimer > 120) {
                ptc_chain_advance(CH_GRAPPLE_AIM);
            }
            break;

        case CH_GRAPPLE_AIM:
            if (s_chainTimer == 1) {
                extern int gfx_cam_noclip;
                gfx_cam_noclip = 0;
                movekit_force_retract(0);
                /* The anchor is on the room ceiling, above the gap. */
                ptc_chain_hint(1, -150.0f, 2780.0f, -3000.0f);
            }
            if (s_chainTimer > 26) {
                movekit_force_trigger(0);
                ptc_chain_advance(CH_GRAPPLE_FIRE);
            }
            break;

        case CH_GRAPPLE_FIRE:
            if (s_chainTimer == 1) {
                movekit_force_anchor(-150.0f, 2780.0f, -3000.0f);
                movekit_force_trigger(1);   /* keep the rope constraint on */
                m->vel[0] = 4.0f;           /* swing south under the anchor */
                m->vel[2] = -22.0f;
            }
            if (movekit_state() & 8) {
                s_chainFlags |= CH_FLAG_GRAPPLE;
                ptc_chain_advance(CH_GRAPPLE_HOLD);
            } else if (s_chainTimer > 12) {
                ptc_chain_advance(CH_GRAPPLE_HOLD);
            }
            break;

        case CH_GRAPPLE_HOLD:
            s_chainFlags |= CH_FLAG_GRAPPLE;
            if (s_chainTimer <= 105) {
                /* Spider-Man pendulum under the ceiling anchor.  The retract
                 * half is 2x longer than the swing half. */
                float t = (float) s_chainTimer / 105.0f;
                float e = t * t * (3.0f - 2.0f * t);
                float ang = (-6.0f + 22.0f * e) * 3.14159265f / 180.0f;
                float R = 2450.0f - 380.0f * e;
                float sn = sinf(ang), cs = cosf(ang);
                m->pos[0] = -150.0f + R * sn;
                m->pos[1] = 2780.0f - R * cs;
                m->pos[2] = -2430.0f + (-3000.0f - -2430.0f) * e;
                m->vel[0] = m->vel[1] = m->vel[2] = 0.0f;
                m->forwardVel = 0.0f;
            } else {
                ptc_chain_advance(CH_GRAPPLE_RELEASE);
            }
            break;

        case CH_GRAPPLE_RELEASE:
            if (s_chainTimer == 1) {
                extern int gfx_cam_noclip;
                movekit_force_trigger(0);
                movekit_force_retract(0);
                m->vel[1] = 8.0f;
                m->vel[0] = 10.0f;          /* east across N4 */
                m->vel[2] = -6.0f;
                m->forwardVel = 12.0f;
                m->faceAngle[1] = atan2s(-6.0f, 10.0f);
                ptc_chain_hint(0, 0.0f, 0.0f, 0.0f);
                gfx_cam_noclip = 1;   /* back to the noclip chase camera */
            }
            if (s_chainTimer > 8) {
                ptc_chain_advance(CH_GRAPPLE_TRAVEL);
            }
            break;

        case CH_GRAPPLE_TRAVEL:
            /* Ballistic carry after the first release: keep the momentum and
             * show the gap before the second hook, instead of dropping
             * straight onto N4. */
            if (s_chainTimer == 1) {
                s_chainFlags |= CH_FLAG_GRAPPLE;
                m->vel[0] = 12.0f;
                m->vel[1] = 6.0f;
                m->vel[2] = -3.0f;
                m->forwardVel = 12.0f;
                m->faceAngle[1] = atan2s(-3.0f, 12.0f);
            }
            if (s_chainTimer > 10) {
                ptc_chain_advance(CH_GRAPPLE_AIM2);
            }
            break;

        case CH_GRAPPLE_AIM2:
            if (s_chainTimer == 1) {
                /* Anchor B is above and east of the carry.  It is fixed now;
                 * the swing start pose is measured at attach time, so the
                 * second rope begins exactly where Mario is. */
                s_chainAnchorB[0] = m->pos[0] + 300.0f;
                s_chainAnchorB[1] = m->pos[1] + 1200.0f;
                s_chainAnchorB[2] = m->pos[2] - 20.0f;
                ptc_chain_hint(1, s_chainAnchorB[0], s_chainAnchorB[1], s_chainAnchorB[2]);
            }
            if (s_chainTimer > 6) {
                ptc_chain_advance(CH_GRAPPLE_FIRE2);
            }
            break;

        case CH_GRAPPLE_FIRE2:
            if (s_chainTimer == 1) {
                movekit_force_anchor(s_chainAnchorB[0], s_chainAnchorB[1],
                                     s_chainAnchorB[2]);
                movekit_force_trigger(1);
                s_chainSwingAng0 = atan2f(m->pos[0] - s_chainAnchorB[0],
                                          -(m->pos[1] - s_chainAnchorB[1]));
                s_chainSwingR0 = sqrtf(
                    (m->pos[0] - s_chainAnchorB[0]) * (m->pos[0] - s_chainAnchorB[0])
                  + (m->pos[1] - s_chainAnchorB[1]) * (m->pos[1] - s_chainAnchorB[1])
                  + (m->pos[2] - s_chainAnchorB[2]) * (m->pos[2] - s_chainAnchorB[2]));
                vec3f_copy(s_chainSwingPrev, m->pos);
            }
            if ((movekit_state() & 8) || s_chainTimer > 2) {
                s_chainFlags |= CH_FLAG_GRAPPLE;
                ptc_chain_advance(CH_GRAPPLE_HOLD2);
            }
            break;

        case CH_GRAPPLE_HOLD2:
            s_chainFlags |= CH_FLAG_GRAPPLE;
            if (s_chainTimer <= 55) {
                /* Second pendulum, same eased shape as the first: the start
                 * angle/radius come from the attach pose, so there is no
                 * positional pop when the new rope takes over. */
                float t = (float) (s_chainTimer - 1) / 54.0f;
                float e = t * t * (3.0f - 2.0f * t);
                float ang = s_chainSwingAng0 - 0.85f * e;
                float R = s_chainSwingR0 - 180.0f * e;
                float px = m->pos[0], py = m->pos[1], pz = m->pos[2];

                m->pos[0] = s_chainAnchorB[0] + sinf(ang) * R;
                m->pos[1] = s_chainAnchorB[1] - cosf(ang) * R;
                m->pos[2] = s_chainAnchorB[2] + (pz - s_chainAnchorB[2]) * (1.0f - e);
                vec3f_set(s_chainSwingPrev, px, py, pz);
                m->vel[0] = m->vel[1] = m->vel[2] = 0.0f;
                m->forwardVel = 0.0f;
            } else {
                ptc_chain_advance(CH_GRAPPLE_RELEASE2);
            }
            break;

        case CH_GRAPPLE_RELEASE2:
            if (s_chainTimer == 1) {
                float vx = m->pos[0] - s_chainSwingPrev[0];
                float vy = m->pos[1] - s_chainSwingPrev[1];
                float vz = m->pos[2] - s_chainSwingPrev[2];
                movekit_force_trigger(0);
                movekit_force_retract(0);
                m->vel[0] = vx * 1.15f;
                m->vel[1] = vy * 1.15f + 3.0f;
                m->vel[2] = vz * 1.15f;
                m->forwardVel = sqrtf(m->vel[0] * m->vel[0] + m->vel[2] * m->vel[2]);
                if (m->forwardVel > 1.0f) {
                    m->faceAngle[1] = atan2s(m->vel[2], m->vel[0]);
                }
                ptc_chain_hint(0, 0.0f, 0.0f, 0.0f);
            }
            if (s_chainTimer > 8) {
                ptc_chain_advance(CH_LAND_N4);
            }
            break;

        case CH_LAND_N4:
            if (!ptc_chain_grounded(m)) {
                /* Air-steer onto the N4 star while falling from the ceiling. */
                float dx = 280.0f - m->pos[0];
                float dz = -2950.0f - m->pos[2];
                if (fabsf(dx) > 2.0f) m->pos[0] += (dx > 0.0f ? 1.0f : -1.0f) * 6.0f;
                if (fabsf(dz) > 2.0f) m->pos[2] += (dz > 0.0f ? 1.0f : -1.0f) * 6.0f;
            }
            if (ptc_chain_grounded(m) || s_chainTimer > 120) {
                ptc_chain_advance(CH_STAR1);
            }
            break;

        case CH_STAR1:
            if (s_starCollected[0]) {
                s_chainFlags |= CH_FLAG_STAR1;
                ptc_chain_advance(CH_JET_N5);
            } else if (s_chainTimer > 120) {
                ptc_chain_advance(CH_JET_N5);
            }
            break;

        case CH_JET_N5:
            if (s_chainTimer == 1) {
                movekit_set_flag(0, 0);
                movekit_set_flag(2, 1);
                movekit_force_trigger(1);
            }
            if ((m->pos[1] > 980.0f && m->pos[0] > 1150.0f) || m->pos[1] > 1400.0f) {
                movekit_force_trigger(0);
                movekit_set_flag(2, 0);
                s_chainFlags |= CH_FLAG_JETPACK;
                ptc_chain_advance(CH_LAND_N5);
            } else if (s_chainTimer > 120) {
                ptc_chain_advance(CH_LAND_N5);
            }
            break;

        case CH_LAND_N5:
            if (ptc_chain_grounded(m) || s_chainTimer > 90) {
                ptc_chain_advance(CH_PORTAL_RUN);
            }
            break;

        case CH_PORTAL_RUN:
            if (s_chainTimer > 30 && portal_teleport_count() == s_chainTeleBase) {
                float tx, tz;
                if (m->pos[0] > 1820.0f) {        /* west of the N7 tower */
                    tx = 1760.0f; tz = -2700.0f;
                } else if (m->pos[2] > -2860.0f) { /* south alongside the wall */
                    tx = 1770.0f; tz = -2890.0f;
                } else {                            /* east into the portal */
                    tx = 2000.0f; tz = -2890.0f;
                }
                {
                    float dx = tx - m->pos[0];
                    float dz = tz - m->pos[2];
                    float d = sqrtf(dx * dx + dz * dz);
                    if (d > 1.0f) {
                        float step = d < 10.0f ? d : 10.0f;
                        m->pos[0] += dx / d * step;
                        m->pos[2] += dz / d * step;
                    }
                    m->faceAngle[1] = atan2s(dz, dx);
                }
            }
            if (portal_teleport_count() != s_chainTeleBase) {
                s_chainFlags |= CH_FLAG_PORTAL;
                ptc_chain_advance(CH_PORTAL_FALL);
            } else if (s_chainTimer > 140) {
                ptc_chain_advance(CH_PORTAL_FALL);
            }
            break;

        case CH_PORTAL_FALL:
            /* Cinematic float: cap the fall so the ceiling-portal drop is a
             * readable beat instead of a 0.3 s blur. */
            if (m->pos[1] > 1540.0f && m->vel[1] < -13.0f) {
                m->vel[1] = -13.0f;
            }
            if (ptc_chain_grounded(m) && m->pos[1] < 1700.0f) {
                ptc_chain_advance(CH_STAR2);
            } else if (s_chainTimer > 180) {
                ptc_chain_advance(CH_STAR2);
            }
            break;

        case CH_STAR2:
            if (s_starCollected[1]) {
                s_chainFlags |= CH_FLAG_STAR2;
                if (s_chainTimer > 50) {
                    ptc_chain_advance(CH_PORTAL_LOOP);
                }
            } else if (s_chainTimer > 120) {
                ptc_chain_advance(CH_PORTAL_LOOP);
            }
            break;

        case CH_PORTAL_LOOP:
            if (s_chainTimer == 1) {
                /* Reuse the deterministic smoke-test loop on the room's south
                 * wall behind the N6 star: two portals 180 apart, Mario walks
                 * in, turns right and re-enters, for four real crossings. */
                Vec3f aPos = { -1000.0f, 1660.0f, -3196.0f };
                Vec3f bPos = {  -820.0f, 1660.0f, -3196.0f };
                Vec3f aOrig = { -1000.0f, 1660.0f, -2900.0f };
                Vec3f bOrig = {  -820.0f, 1660.0f, -2900.0f };
                Vec3f start = { -1000.0f, 1500.0f, -2950.0f };
                Vec3f camPos, camFocus;
                vec3f_copy(camPos, gLakituState.pos);
                vec3f_copy(camFocus, gLakituState.focus);
                /* Keep the relay off here: the south-wall pair clips black in
                 * the relay pass, so the flat coloured fans read better.  The
                 * recursion overlay is validated on the west-wall pair
                 * (marioportalwalkloop + relay.recursion). */
                {
                    extern int g_ptc_relay_loop;
                    portal_view_set(g_ptc_relay_loop);
                }
                ptc_mpw_begin(aPos, bPos, aOrig, bOrig, start, 0x8000, 0);
                {
                    extern int gfx_stereo_fov_pct;
                    extern void custom_camera_portal_snap(const Vec3f pos,
                                                          const Vec3f focus);
                    gfx_stereo_fov_pct = 150;
                    /* Keep the star2 framing and spring from there instead of
                     * cutting to the chase pose. */
                    custom_camera_portal_snap(camPos, camFocus);
                }
                s_chainTeleBase = portal_teleport_count();
                s_chainFlags |= CH_FLAG_PORTAL;
            }
            if (s_mpwPasses >= 4) {
                ptc_mario_portal_walk_loop_stop();
                portal_view_set(0);
                ptc_chain_advance(CH_GOOMBA_SETUP);
            } else if (!s_mpw && s_chainTimer > 30) {
                ptc_chain_advance(CH_GOOMBA_SETUP);   /* stalled: do not hang */
            } else if (s_chainTimer > 420) {
                ptc_mario_portal_walk_loop_stop();
                portal_view_set(0);
                ptc_chain_advance(CH_GOOMBA_SETUP);
            }
            break;

        case CH_GOOMBA_SETUP:
            if (s_chainTimer == 1) {
                ptc_chain_goomba_start();
            }
            if (s_chainTimer > 34) {
                s_chainFlags |= CH_FLAG_GOOMBA;
                ptc_chain_advance(CH_GOOMBA_LOOP);
            }
            break;

        case CH_GOOMBA_LOOP:
            ptc_chain_goomba_tick();
            if (s_chainGoombaState[0] == 5 || s_chainTimer > 420) {
                ptc_chain_advance(CH_FINALE);
            }
            break;

        case CH_FINALE:
            if (s_chainTimer == 1) {
                gfx_splat_active = 1;
                gfx_splat_target = 3;     /* all objects */
                gfx_splat_shape = 0;      /* ellipse splats */
                gfx_splat_density = 20;   /* min */
            }
            {
                float u = (float) s_chainTimer / 240.0f;
                if (u > 1.0f) u = 1.0f;
                gfx_splat_density = (int) (20.0f + 80.0f * u);
                if (s_chainTimer == 120) {
                    gfx_splat_shape = 1;  /* paint-stroke splats at 180 deg */
                }
                /* Hold the completed 360 orbit (do not cut back to the
                 * default camera): the recording ends on the splat showcase. */
            }
            if (s_chainLoop && s_chainTimer > 300) {
                ptc_chain_loop_restart();
            }
            break;

        default:
            break;
    }
}

/* Input for the chain, called from ptc_mario_input_override(). */
static void ptc_chain_input(struct MarioState *m) {
    switch (s_chainMode) {
        case CH_BOX_BALL:
        case CH_BOX_BALL_WAIT:
        case CH_BOX_PUNCH:
        case CH_BOX_KICK:
        case CH_GRAPPLE_AIM:
        case CH_GRAPPLE_FIRE:
        case CH_GRAPPLE_HOLD:
        case CH_GRAPPLE_RELEASE:
            break;

        case CH_GOOMBA_SETUP:
        case CH_GOOMBA_LOOP:
            if (m != NULL) {
                m->faceAngle[1] = 0xC000;   /* face the goomba (north) */
            }
            break;

        case CH_BOX_WALK_PUNCH:
        case CH_BOX_WALK_KICK:
            ptc_chain_walk_to(m, s_chainTargetX, s_chainTargetZ);
            break;
        case CH_BOX_WALK_SLAM:
            ptc_chain_walk_to(m, CH_BOX_X, CH_BOX_D_Z);
            break;

        case CH_JET_L1:
            ptc_chain_walk_to(m, -950.0f, -1500.0f);  /* steer onto L1 */
            break;

        case CH_RUN_WALL:
            /* Stick must be held while airborne too: the wall-run trigger
             * requires INPUT_NONZERO_ANALOG. */
            ptc_chain_stick(m, 0x4000, 32.0f);       /* east, towards the wall */
            break;

        case CH_WALLRUN:
            ptc_chain_stick(m, 0x8000, 32.0f);   /* south along the wall */
            break;

        case CH_JET_N5:
            ptc_chain_stick(m, 0x4000, 32.0f);   /* east to N5 */
            break;

        case CH_STAR1:
            ptc_chain_walk_to(m, CH_STAR1_X, CH_STAR1_Z);
            break;

        case CH_STAR2:
            ptc_chain_walk_to(m, CH_STAR2_X, CH_STAR2_Z);
            break;

        case CH_PORTAL_RUN:
            break;   /* position is scripted in the update */

        default:
            break;
    }
}

/* ---------------------------------------------------- camera wall test */

/* Camera-noclip demo: Mario near the west wall, camera aimed to sit outside
 * the room behind him.  With WALL NOCLIP on (default) the camera goes through
 * and the west wall disappears; with it off the camera pulls in. */
static void ptc_cam_wall_test(void) {
    extern void custom_camera_apply_mode(int mode);
    extern void custom_camera_set_yaw_hint(int on, s16 yaw);
    extern float gfx_cam_dist, gfx_cam_height;

    gfx_cam_dist = 700.0f;
    gfx_cam_height = 120.0f;
    custom_camera_apply_mode(3);
    custom_camera_set_yaw_hint(1, 0x4000);   /* camera west of Mario */
    ptc_chain_place_ground(-2500.0f, 0.0f, -3000.0f, 0x4000);
    gfx_set_debug_note("CAM NOCLIP: camera through the west wall (wall hidden)");
}

/* ------------------------------------------------- corner portal test */

/* Reproducible corner test: portals 20 units from the SW corner where the blue
 * (west) and green (south) walls meet, Mario standing in the corner facing one
 * at a time.  Used to debug the mirrored Mario seen inside a portal. */
static void ptc_corner_portal(int face_b) {
    /* The ellipse is 135 wide (rx 67.5), so a portal cannot literally sit 20
     * units from the corner - the rim test rejects it.  100 units keeps the
     * whole rim on its wall while still hugging the corner. */
    /* Aim the portals at Mario's mid-body height (the mapping preserves height
     * relative to the portal centre, so this is what centres him in the
     * window when the test camera is level with the floor). */
    Vec3f aOrig = { -2400.0f, 90.0f, -3100.0f };
    Vec3f aTarget = { -2700.0f, 90.0f, -3100.0f };   /* west wall, normal +X */
    Vec3f bOrig = { -2500.0f, 90.0f, -3000.0f };
    Vec3f bTarget = { -2500.0f, 90.0f, -3300.0f };   /* south wall, normal +Z */

    /* The vanilla camera fights corners, so this test uses the custom cam. */
    {
        extern void custom_camera_apply_mode(int mode);
        custom_camera_apply_mode(3);   /* 3 = CUSTOM */
    }

    ptc_goomba_reset_state();
    portal_reset();
    portal_give_gun();
    portal_shoot_at(0, aOrig, aTarget);
    portal_shoot_at(1, bOrig, bTarget);
    portal_view_force(face_b ? 1 : 0);

    if (gMarioState != NULL) {
        gMarioState->pos[0] = -2380.0f;
        gMarioState->pos[1] = 0.0f;
        gMarioState->pos[2] = -2950.0f;
        gMarioState->vel[0] = gMarioState->vel[1] = gMarioState->vel[2] = 0.0f;
        /* Face the west portal (-X) or the south portal (-Z). */
        gMarioState->faceAngle[1] = face_b ? 0x8000 : 0xC000;
        set_mario_action(gMarioState, ACT_IDLE, 0);
        if (gMarioState->marioObj != NULL) {
            vec3f_copy(gMarioState->marioObj->header.gfx.pos, gMarioState->pos);
        }
        gCameraMovementFlags |= CAM_MOVE_INIT_CAMERA;
    }
    g_portal_debug_color = 0;
}

/* ------------------------------------------------------- move annexes */

/* Two purpose-built annexes in the chamber:
 *   wall-run corridor  (-1090..-790, z 600..1200, walls 420 tall)
 *   jetpack shaft      (1700..2100, z -760..-240, 1300 tall, pad at 1200)
 * `ptc wallrun` / `ptc jetpack` warp there, arm the equipment and drive a
 * scripted input for ~4 s; the result lands in `PTC ... annex=...`. */
#define PTC_WR_WALL_X   (-2340.0f)
#define PTC_WR_Z        (600.0f)
#define PTC_JP_X        (2800.0f)
#define PTC_JP_Z        (-200.0f)

static void ptc_annex_reset(void) {
    s_annexMode = 0;
    s_annexTimer = 0;
    s_annexSeen = 0;
    movekit_force_trigger(0);
}

static void ptc_annex_place(float x, float y, float z, s16 yaw) {
    if (gMarioState == NULL) {
        return;
    }
    gMarioState->pos[0] = x;
    gMarioState->pos[1] = y;
    gMarioState->pos[2] = z;
    gMarioState->vel[0] = gMarioState->vel[1] = gMarioState->vel[2] = 0.0f;
    gMarioState->faceAngle[1] = yaw;
    set_mario_action(gMarioState, ACT_FREEFALL, 0);
    if (gMarioState->marioObj != NULL) {
        vec3f_copy(gMarioState->marioObj->header.gfx.pos, gMarioState->pos);
    }
    gCameraMovementFlags |= CAM_MOVE_INIT_CAMERA;
}

static void ptc_annex_wallrun(void) {
    ptc_annex_reset();
    movekit_set_flag(0, 1);              /* wall-run enabled */
    /* Airborne beside the west wall, moving into it and along it. */
    ptc_annex_place(PTC_WR_WALL_X + 26.0f, 260.0f, PTC_WR_Z, 0x0000);
    if (gMarioState != NULL) {
        gMarioState->vel[0] = -14.0f;
        gMarioState->vel[2] = 42.0f;
    }
    s_annexMode = 1;
}

static void ptc_annex_grapple(void) {
    ptc_annex_reset();
    movekit_set_flag(1, 1);              /* grapple enabled */
    /* Inside the wall-run corridor, facing the opposite wall (70 units away). */
    ptc_annex_place(-2250.0f, 260.0f, 300.0f, 0x4000);   /* face +X wall */
    s_annexMode = 3;
}

static void ptc_annex_jetpack(void) {
    ptc_annex_reset();
    movekit_set_flag(2, 1);              /* equip the jetpack (and refill) */
    ptc_annex_place(PTC_JP_X, 0.0f, PTC_JP_Z, 0x0000);   /* on the floor */
    s_annexStartY = 0.0f;
    s_annexMode = 2;
}

static void ptc_annex_update(void) {
    struct MarioState *m = gMarioState;

    if (s_annexMode == 0) {
        return;
    }
    if (s_annexMode == 4) {
        /* jetshot: hold the trigger indefinitely (no pass/fail bookkeeping).
         * Used by scripts/emissive-glow-test.sh to keep the nozzle flame and
         * its registered light on screen for a reproducible capture. */
        movekit_force_trigger(1);
        return;
    }
    s_annexTimer++;
    if (s_annexMode == 2) {
        /* Only hold the trigger: movekit_set_flag(2,1) refills the tank, so
         * asserting it every tick would hide whether fuel is really burning. */
        movekit_force_trigger(1);
    } else if (s_annexMode == 3) {
        movekit_set_flag(1, 1);
        /* Pulse the trigger so the grapple sees a fresh press edge. */
        movekit_force_trigger(((s_annexTimer / 15) & 1) ? 0 : 1);
    }
    if (s_annexMode == 1) {
        if (movekit_state() & 1) {
            s_annexSeen = 1;
        }
    } else if (s_annexMode == 2) {
        if ((movekit_state() & 4) && m != NULL
            && (m->vel[1] > 2.0f || m->pos[1] > s_annexStartY + 100.0f)) {
            s_annexSeen = 1;
        }
    } else if (s_annexMode == 3) {
        if (movekit_state() & 8) {       /* rope attached */
            s_annexSeen = 1;
        }
    }
    if (s_annexTimer > 140) {           /* ~4.6 s at 30 Hz */
        if (!s_annexSeen) {
            snprintf(s_failReason, sizeof(s_failReason),
                     "%s test did not trigger",
                     s_annexMode == 1 ? "wallrun"
                                      : (s_annexMode == 2 ? "jetpack" : "grapple"));
        }
        ptc_annex_reset();
    }
}

/* ------------------------------------------------------------ diagnostics */

static void ptc_apply_command(const char *action) {
    if (action == NULL) {
        return;
    }
    if (strcmp(action, "run") == 0 || strcmp(action, "start") == 0) {
        ptc_start_test();
    } else if (strcmp(action, "portalwalk") == 0) {
        ptc_portal_walk_start(0);
    } else if (strcmp(action, "portalwalkback") == 0) {
        ptc_portal_walk_start(1);
    } else if (strcmp(action, "portalwalkceil") == 0) {
        ptc_portal_walk_start(2);
    } else if (strcmp(action, "marioportalwalkloop") == 0) {
        ptc_mario_portal_walk_loop_start();
    } else if (strcmp(action, "marioportalwalkstop") == 0) {
        ptc_mario_portal_walk_loop_stop();
    } else if (strcmp(action, "reset") == 0) {
        ptc_reset();
    } else if (strcmp(action, "calibrate") == 0) {
        ptc_calibrate_start();
    } else if (strcmp(action, "momentum") == 0 || strcmp(action, "launch") == 0) {
        ptc_momentum_start();
    } else if (strcmp(action, "lightshow") == 0 || strcmp(action, "lensorbit") == 0) {
        extern void gfx_opengl_light_count(int count);
        extern void gfx_opengl_light_shadows(int on);
        extern void gfx_opengl_ssfx_set(int which, int value);
        extern void custom_camera_apply_mode(int mode);
        extern void custom_camera_set_freelook(int on, const Vec3f pos, const Vec3f focus);
        struct MarioState *sm = gMarioState;

        s_chainMode = CH_OFF;
        s_testMode = 0;
        s_showOn = 1;
        s_showTimer = 0;
        s_showStartMs = 0;
        s_showFlipped = 0;
        if (sm != NULL) {
            sm->pos[0] = 0.0f;
            sm->pos[1] = 0.0f;
            sm->pos[2] = -1800.0f;
            sm->vel[0] = sm->vel[1] = sm->vel[2] = 0.0f;
            sm->forwardVel = 0.0f;
            sm->faceAngle[1] = 0x0000;
            set_mario_action(sm, ACT_IDLE, 0);
            if (sm->marioObj != NULL) {
                vec3f_copy(sm->marioObj->header.gfx.pos, sm->pos);
            }
            s_showCenter[0] = sm->pos[0];
            s_showCenter[1] = sm->pos[1] + 80.0f;
            s_showCenter[2] = sm->pos[2];
        }
        gfx_opengl_light_count(3);
        gfx_opengl_light_shadows(1);
        gfx_opengl_ssfx_set(105, 100);   /* intensity */
        gfx_opengl_ssfx_set(104, 1);     /* light shadows */
        gfx_opengl_ssfx_set(106, 3);
        {
            extern int gfx_stereo_in_scale_pct;
            gfx_stereo_in_scale_pct = 100;
        }
        gfx_opengl_ssfx_set(100, 1);     /* motion blur on */
        gfx_opengl_ssfx_set(101, 50);
        gfx_opengl_ssfx_set(102, 6);
        custom_camera_apply_mode(3);
        custom_camera_set_freelook(1, (Vec3f){ 0.0f, 650.0f, -900.0f },
                                   (Vec3f){ 0.0f, 90.0f, -1800.0f });
        gfx_set_debug_note("LIGHT LOOP: 10 s orbit, counter-rotating camera, backflip @5 s");
    } else if (strncmp(action, "note ", 5) == 0) {
        extern void gfx_set_debug_note(const char *text);
        gfx_set_debug_note(action + 5);
    } else if (strcmp(action, "blurshow") == 0) {
        s_chainMode = CH_OFF;
        ptc_blurshow_begin();
    } else if (strcmp(action, "blurshowstop") == 0) {
        ptc_blurshow_stop();
    } else if (strcmp(action, "lightshowstop") == 0) {
        extern void gfx_opengl_light_count(int count);
        s_showOn = 0;
        /* Release the light slots so gameplay lights (jetpack, flames) come
         * back instead of the show's last frame staying frozen on. */
        gfx_opengl_light_count(0);
    } else if (strcmp(action, "lighttest") == 0) {
        /* Smoke test: two coloured lights around Mario, shadows on, motion
         * blur on, and a 3x3x3 probe grid with the reflective-sphere debug. */
        extern void gfx_opengl_light_set(int index, float x, float y, float z,
                                         float r, float g, float b, float radius);
        extern void gfx_opengl_light_count(int count);
        extern void gfx_opengl_light_shadows(int on);
        extern void gfx_opengl_probe_config(float x, float y, float z, int spacing,
                                            int dim, int radius, int resolution,
                                            int vis);
        extern void gfx_opengl_ssfx_set(int which, int value);
        extern int gfx_opengl_light_state(int which);
        extern int gfx_stereo_in_scale_pct;
        struct MarioState *lm = gMarioState;
        float mx = lm ? lm->pos[0] : 0.0f;
        float my = lm ? lm->pos[1] : 0.0f;
        float mz = lm ? lm->pos[2] : 0.0f;

        gfx_opengl_light_set(0, mx + 500.0f, my + 1200.0f, mz - 250.0f,
                             1.0f, 0.55f, 0.25f, 2600.0f);
        gfx_opengl_light_set(1, mx - 600.0f, my + 800.0f, mz + 350.0f,
                             0.25f, 0.55f, 1.0f, 2200.0f);
        gfx_opengl_light_count(2);
        gfx_opengl_light_shadows(1);
        gfx_opengl_ssfx_set(105, 90);    /* intensity */
        gfx_opengl_ssfx_set(100, 1);     /* motion blur on */
        gfx_opengl_ssfx_set(101, 40);
        gfx_opengl_ssfx_set(102, 6);
        gfx_opengl_probe_config(mx, my + 250.0f, mz, 450, 3, 18, 16, 1);
        gfx_set_debug_note("LIGHT TEST: 2 lights + shadows + blur + probe grid");
        printf("PTC lighttest lights=%d shadows=%d probes=%d\n",
               gfx_opengl_light_state(0), gfx_opengl_light_state(1),
               gfx_opengl_light_state(3));
    } else if (strcmp(action, "toolswap") == 0) {
        movekit_tool_toggle();
    } else if (strcmp(action, "toolgun") == 0) {
        movekit_tool_set(0);
    } else if (strcmp(action, "toolgrapple") == 0) {
        movekit_tool_set(1);
    } else if (strcmp(action, "place") == 0) {
        ptc_place_portals();
    } else if (strcmp(action, "portalview") == 0) {
        portal_view_toggle();
    } else if (strcmp(action, "portalforce") == 0) {
        portal_view_force(0);
    } else if (strcmp(action, "portalauto") == 0) {
        portal_view_force(-1);
    } else if (strcmp(action, "portaltest") == 0) {
        extern int gfx_stereo_2d;
        extern int gfx_stereo_same_both;
        ptc_place_portals();
        portal_view_force(0);
        if (gMarioState != NULL) {
            gMarioState->pos[0] = -600.0f;
            gMarioState->pos[1] = 0.0f;
            gMarioState->pos[2] = -900.0f;
            gMarioState->vel[0] = gMarioState->vel[1] = gMarioState->vel[2] = 0.0f;
            gMarioState->faceAngle[1] = 0;   /* face +Z, toward portal A */
            set_mario_action(gMarioState, ACT_IDLE, 0);
            if (gMarioState->marioObj != NULL) {
                vec3f_copy(gMarioState->marioObj->header.gfx.pos, gMarioState->pos);
            }
        }
        g_portal_debug_color = 1;   /* stage 1: solid green relay */
        gfx_stereo_2d = 1;
        gfx_stereo_same_both = 1;
    } else if (strncmp(action, "knob ", 5) == 0) {
        /* `ptc knob <name> <value>` - no rebuild needed. */
        char name[48];
        float value;
        if (sscanf(action + 5, "%47s %f", name, &value) == 2) {
            if (!ptc_knob_set(name, value)) {
                snprintf(s_failReason, sizeof(s_failReason), "unknown knob %s", name);
            }
        }
    } else if (strcmp(action, "reload") == 0) {
        ptc_knob_reload();
    } else if (strcmp(action, "chain") == 0) {
        s_chainLoop = 0;
        ptc_chain_setup();
    } else if (strcmp(action, "chainloop") == 0) {
        s_chainLoop = 1;
        ptc_chain_setup();
    } else if (strcmp(action, "splatfinale") == 0) {
        s_chainMode = CH_FINALE;
        s_chainTimer = 0;
        s_chainFlags = 0;
        s_cineOn = 1;
        s_cineInit = 0;
        gfx_splat_active = 1;
        gfx_splat_target = 3;
        gfx_splat_shape = 0;
        gfx_splat_density = 20;
    } else if (strcmp(action, "camwall") == 0) {
        ptc_cam_wall_test();
    } else if (strcmp(action, "fadeon") == 0) {
        s_fade_enable = 1;
    } else if (strcmp(action, "fadeoff") == 0) {
        s_fade_enable = 0;
    } else if (strcmp(action, "fadetest") == 0) {
        extern void custom_camera_apply_mode(int mode);
        extern void custom_camera_set_freelook(int on, const Vec3f pos, const Vec3f focus);
        Vec3f cp = { -100.0f, 1500.0f, -3000.0f };
        Vec3f cf = { -600.0f, 1600.0f, -3000.0f };
        s_chainMode = 0;
        s_cineOn = 0;
        custom_camera_apply_mode(3);
        custom_camera_set_freelook(1, cp, cf);
        ptc_chain_place_ground(-600.0f, 1500.0f, -3000.0f, 0x4000);
        gfx_set_debug_note("FADE TEST: N8 between camera and Mario");
    } else if (strcmp(action, "chainbox") == 0) {
        ptc_chain_setup();
        ptc_chain_advance(CH_BOX_BALL);
    } else if (strcmp(action, "chainwall") == 0) {
        ptc_chain_setup();
        ptc_chain_place_ground(-950.0f, 450.0f, -1500.0f, 0x4000);
        ptc_chain_advance(CH_RUN_WALL);
    } else if (strcmp(action, "chaingrapple") == 0) {
        ptc_chain_setup();
        ptc_chain_place_ground(-450.0f, 330.0f, -2550.0f, 0x8000);
        ptc_chain_advance(CH_GRAPPLE_AIM);
    } else if (strcmp(action, "chainjet") == 0) {
        ptc_chain_setup();
        ptc_chain_place_ground(850.0f, 300.0f, -2900.0f, 0x4000);
        ptc_chain_advance(CH_JET_N5);
    } else if (strcmp(action, "chainportal") == 0) {
        ptc_chain_setup();
        ptc_chain_place_ground(1450.0f, 900.0f, -2900.0f, 0x4000);
        ptc_chain_advance(CH_PORTAL_RUN);
    } else if (strcmp(action, "chaingoomba") == 0) {
        ptc_chain_setup();
        ptc_chain_place_ground(-800.0f, 1500.0f, -3100.0f, 0x4000);
        ptc_chain_advance(CH_GOOMBA_SETUP);
    } else if (strcmp(action, "chainoff") == 0) {
        extern void custom_camera_set_focus_hint(int on, const Vec3f pos);
        extern void custom_camera_set_yaw_hint(int on, s16 yaw);
        extern void custom_camera_set_freelook(int on, const Vec3f pos, const Vec3f focus);
        extern int gfx_stereo_fov_pct;
        s_chainMode = 0;
        s_cineOn = 0;
        s_cineInit = 0;
        portal_view_set(1);
        movekit_force_retract(0);
        if (s_chainHudSaved) {
            gHudDisplay.flags = s_chainHudFlags;
            s_chainHudSaved = 0;
        }
        if (s_finaleSplatSaved[0] || s_finaleSplatSaved[3]) {
            gfx_splat_active = s_finaleSplatSaved[0];
            gfx_splat_target = s_finaleSplatSaved[1];
            gfx_splat_shape = s_finaleSplatSaved[2];
            gfx_splat_density = s_finaleSplatSaved[3];
        }
        custom_camera_set_freelook(0, NULL, NULL);
        gfx_stereo_fov_pct = 100;
        movekit_force_trigger(0);
        custom_camera_set_focus_hint(0, NULL);
        custom_camera_set_yaw_hint(0, 0);
    } else if (strcmp(action, "gunaim") == 0) {
        extern void gfx_set_debug_note(const char *s);
extern int gfx_cam_mode;
        gfx_set_debug_note("GUN AIM: barrel points at the floating marker");
        ptc_gun_aim_test();
    } else if (strcmp(action, "gunaimoff") == 0) {
        ptc_gun_aim_end();
    } else if (strcmp(action, "goomba") == 0) {
        extern void gfx_set_debug_note(const char *s);
extern int gfx_cam_mode;
        gfx_set_debug_note("GOOMBA: portal lemming loop (crossings)");
        ptc_goomba_start();
    } else if (strcmp(action, "scene") == 0) {
        ptc_scene_toggle();
    } else if (strcmp(action, "emptytest") == 0) {
        extern int gfx_stereo_2d;
        extern int gfx_stereo_same_both;
        /* Side by side on the same wall, ~180 apart: both faces are visible
         * and each portal gets its own relay window. */
        Vec3f aOrig = { -90.0f, 140.0f, 120.0f };
        Vec3f aTarget = { -90.0f, 140.0f, -280.0f };
        Vec3f bOrig = { 90.0f, 140.0f, 120.0f };
        Vec3f bTarget = { 90.0f, 140.0f, -280.0f };
        portal_reset();
        portal_give_gun();
        portal_shoot_at(0, aOrig, aTarget);
        portal_shoot_at(1, bOrig, bTarget);
        portal_view_force(0);
        if (gMarioState != NULL) {
            gMarioState->pos[0] = 0.0f;
            gMarioState->pos[1] = 0.0f;
            gMarioState->pos[2] = 0.0f;
            gMarioState->vel[0] = gMarioState->vel[1] = gMarioState->vel[2] = 0.0f;
            gMarioState->faceAngle[1] = 0x8000;   /* face -Z, toward portal A */
            set_mario_action(gMarioState, ACT_IDLE, 0);
            if (gMarioState->marioObj != NULL) {
                vec3f_copy(gMarioState->marioObj->header.gfx.pos, gMarioState->pos);
            }
        }
        g_portal_debug_color = 0;   /* scene relay; the room has a checker chart */
        gfx_stereo_2d = 0;
        gfx_stereo_same_both = 0;
        gCameraMovementFlags |= CAM_MOVE_INIT_CAMERA;
    } else if (strcmp(action, "portalcolor") == 0) {
        g_portal_debug_color = !g_portal_debug_color;
    } else if (strcmp(action, "portalcam") == 0) {
        if (gMarioState != NULL) {
            gMarioState->pos[0] = -600.0f;
            gMarioState->pos[1] = 0.0f;
            gMarioState->pos[2] = -900.0f;
            gMarioState->vel[0] = gMarioState->vel[1] = gMarioState->vel[2] = 0.0f;
            gMarioState->faceAngle[1] = 0;
            set_mario_action(gMarioState, ACT_IDLE, 0);
            gCameraMovementFlags |= CAM_MOVE_INIT_CAMERA;
        }
    } else if (strcmp(action, "eyeL") == 0) {
        extern int gfx_stereo_2d;
        extern int gfx_stereo_same_both;
        gfx_stereo_2d = 1;
        gfx_stereo_same_both = 1;
    } else if (strcmp(action, "eyeR") == 0) {
        extern int gfx_stereo_2d;
        extern int gfx_stereo_same_both;
        gfx_stereo_2d = 1;
        gfx_stereo_same_both = 2;
    } else if (strcmp(action, "eyeOff") == 0) {
        extern int gfx_stereo_same_both;
        gfx_stereo_same_both = 0;
    } else if (strcmp(action, "portalalign") == 0) {
        g_portal_debug_align = !g_portal_debug_align;
        portal_view_force(0);
    } else if (strcmp(action, "portalblit") == 0) {
        extern int gfx_portal_debug_blit;
        gfx_portal_debug_blit = !gfx_portal_debug_blit;
    } else if (strcmp(action, "camblend") == 0) {
        camera_portal_blend_toggle();
    } else if (strcmp(action, "rumble") == 0) {
        controller_rumble_play(1.0f, 0.8f);
    } else if (strcmp(action, "annex") == 0) {
        if (gMarioState != NULL) {
            struct Camera *cam = gCurrentArea != NULL ? gCurrentArea->camera : NULL;
            gMarioState->pos[0] = 1500.0f;
            gMarioState->pos[1] = 0.0f;
            gMarioState->pos[2] = 400.0f;
            gMarioState->vel[0] = gMarioState->vel[1] = gMarioState->vel[2] = 0.0f;
            gMarioState->faceAngle[1] = 0x4000;
            set_mario_action(gMarioState, ACT_IDLE, 0);
            if (gMarioState->marioObj != NULL) {
                vec3f_copy(gMarioState->marioObj->header.gfx.pos, gMarioState->pos);
            }
            /* Mario stops west of the pyramid, facing it.  The chase camera
             * settles behind him around x=1200, clear of platform P4, so the
             * whole stack is visible instead of the camera clipping a slab. */
            (void) cam;
        }
    } else if (strcmp(action, "balltest") == 0) {
        boxphys_test_fire();
    } else if (strcmp(action, "ballreset") == 0) {
        boxphys_reset();
    } else if (strcmp(action, "ballequip") == 0) {
        boxphys_equip_toggle();
    } else if (strcmp(action, "aspecttest") == 0) {
        ptc_aspect_calib_toggle();
    } else if (strcmp(action, "resetpad") == 0) {
        if (gMarioState != NULL) {
            gMarioState->pos[0] = 1250.0f;
            gMarioState->pos[1] = 60.0f;
            gMarioState->pos[2] = 750.0f;
            gMarioState->vel[0] = gMarioState->vel[1] = gMarioState->vel[2] = 0.0f;
            set_mario_action(gMarioState, ACT_IDLE, 0);
            if (gMarioState->marioObj != NULL) {
                vec3f_copy(gMarioState->marioObj->header.gfx.pos, gMarioState->pos);
            }
            gCameraMovementFlags |= CAM_MOVE_INIT_CAMERA;
        }
    } else if (strcmp(action, "slam") == 0) {
        if (gMarioState != NULL) {
            gMarioState->pos[0] = 1450.0f;
            gMarioState->pos[1] = 0.0f;
            gMarioState->pos[2] = 400.0f;
            gMarioState->vel[0] = gMarioState->vel[1] = gMarioState->vel[2] = 0.0f;
            gMarioState->faceAngle[1] = 0x4000;
            set_mario_action(gMarioState, ACT_IDLE, 0);
            if (gMarioState->marioObj != NULL) {
                vec3f_copy(gMarioState->marioObj->header.gfx.pos, gMarioState->pos);
            }
            boxphys_slam_test();
            gCameraMovementFlags |= CAM_MOVE_INIT_CAMERA;
        }
    } else if (strcmp(action, "groundslam") == 0) {
        if (gMarioState != NULL) {
            gMarioState->pos[0] = 1800.0f;
            gMarioState->pos[1] = 620.0f;
            gMarioState->pos[2] = 400.0f;
            gMarioState->vel[0] = gMarioState->vel[1] = gMarioState->vel[2] = 0.0f;
            set_mario_action(gMarioState, ACT_GROUND_POUND, 0);
            if (gMarioState->marioObj != NULL) {
                vec3f_copy(gMarioState->marioObj->header.gfx.pos, gMarioState->pos);
            }
            gCameraMovementFlags |= CAM_MOVE_INIT_CAMERA;
        }
    } else if (strcmp(action, "cubetop") == 0) {
        if (gMarioState != NULL) {
            gMarioState->pos[0] = 1800.0f;
            gMarioState->pos[1] = 620.0f;
            gMarioState->pos[2] = 400.0f;
            gMarioState->vel[0] = gMarioState->vel[1] = gMarioState->vel[2] = 0.0f;
            set_mario_action(gMarioState, ACT_FREEFALL, 0);
            if (gMarioState->marioObj != NULL) {
                vec3f_copy(gMarioState->marioObj->header.gfx.pos, gMarioState->pos);
            }
            gCameraMovementFlags |= CAM_MOVE_INIT_CAMERA;
        }
    } else if (strcmp(action, "wallrun") == 0) {
        ptc_annex_wallrun();
    } else if (strcmp(action, "jetpack") == 0) {
        ptc_annex_jetpack();
    } else if (strcmp(action, "jetshot") == 0) {
        /* Emissive/bloom capture helper (scripts/emissive-glow-test.sh): stand
         * Mario on the open floor by the pyramid facing the chase camera and
         * hold the trigger, so the nozzle flame and the world-space light it
         * registers are both on screen for a fixed window. */
        extern void gfx_set_debug_note(const char *text);
        ptc_annex_reset();
        movekit_set_flag(2, 1);                     /* equip + refill */
        /* PTC spawn: the chase camera settles behind-left of Mario with the
         * floor and the coloured platforms in frame (no warp into a wall). */
        ptc_annex_place(-600.0f, 0.0f, -950.0f, 0x0000);
        s_annexStartY = 0.0f;
        s_annexMode = 4;
        gfx_set_debug_note("JET SHOT: nozzle flame + light, open floor");
    } else if (strcmp(action, "jetshotstop") == 0) {
        ptc_annex_reset();                          /* release the trigger */
    } else if (strcmp(action, "grapple") == 0) {
        ptc_annex_grapple();
    } else if (strncmp(action, "povcam", 6) == 0) {
        extern void gfx_set_debug_note(const char *s);
extern int gfx_cam_mode;
        gfx_set_debug_note("POV: complete Mario through the portal window");
        s_pov_shot = (action[6] == '2') ? 2 : 1;
        if (!ptc_pov_place()) {
            snprintf(s_failReason, sizeof(s_failReason), "povcam: portal shot failed");
        }
        ptc_pov_apply();
    } else if (strcmp(action, "povoff") == 0) {
        extern void custom_camera_set_freelook(int on, const Vec3f pos, const Vec3f focus);
        extern int gfx_stereo_2d;
        extern int gfx_stereo_same_both;
        s_pov_shot = 0;
        custom_camera_set_freelook(0, NULL, NULL);
        gfx_stereo_2d = 0;
        gfx_stereo_same_both = 0;
    } else if (strncmp(action, "cornerportal", 12) == 0) {
        ptc_corner_portal(action[12] == '2');
    } else if (strcmp(action, "walltest") == 0) {
        s_wallTest = portal_test_walls();
    } else if (strcmp(action, "stop") == 0) {
        ptc_stop();
    }
}

void ptc_debug_command(const char *action) {
    if (action == NULL) {
        return;
    }
    snprintf(s_pendingCommand, sizeof(s_pendingCommand), "%s", action);
    s_pendingCommandReady = 1;
}

void ptc_format_status(char *buf, int len) {
    Vec3f pa = { 0, 0, 0 }, na = { 0, 0, 0 };
    Vec3f pb = { 0, 0, 0 }, nb = { 0, 0, 0 };
    struct MarioState *m = gMarioState;
    float pcamD = 0.0f, pcamA = 0.0f;
    extern void portal_last_camera_jump(float *dist, float *angleDeg);
    extern int gfx_opengl_light_state(int which);
    extern int gfx_opengl_denoise_state(int which);
    extern int gfx_opengl_ssfx_get(int which);
    extern int gfx_rate_state(int which);
    extern float gfx_stereo_fps;
    extern int g_sim_slow_div;
    extern int g_sim_interp;
    extern int g_sim_present_t1;
    extern int gfx_quick_dev_enabled(void);
    extern int gfx_quick_dev_level(void);
    extern int gfx_quick_dev_mute(void);
    extern int g_levelInitCount;

    portal_last_camera_jump(&pcamD, &pcamA);
    extern int camera_portal_remap_count(void);
    extern void custom_camera_debug(int out[8]);
    int cdbg[8] = { 0 };
    int hasA = portal_get(0, pa, na);
    int hasB = portal_get(1, pb, nb);
    custom_camera_debug(cdbg);

    if (buf == NULL || len <= 0) {
        return;
    }

    snprintf(buf, len,
             "PTC level=%d area=%d phase=%d mode=%d active=%d fail=%d reason=\"%s\" "
             "star=%d star2=%d star_visible=%d portals=%d shotA=%d shotB=%d pdbg=%d "
             "view=%d vsrc=%d pven=%d "
             "mom_enter=%.1f mom_exit=%.1f mom_apex=%.1f momy=%.1f momv=%.1f,%.1f,%.1f "
             "mom_hold=%.1f mom_hcos=%.3f mom_ratio=%.3f mom_turn=%.3f walltest=0x%X "
             "p0=%.1f,%.1f,%.1f n0=%.3f,%.3f,%.3f p1=%.1f,%.1f,%.1f n1=%.3f,%.3f,%.3f "
             "tele=%d src=%d dst=%d onplat=%d pcam=%.0f,%.1f mres=%.1f "
             "mario=%.2f,%.2f,%.2f vel=%.2f,%.2f,%.2f fv=%.1f iy=%d in=0x%04X im=%.1f hook=%d action=0x%08X floor=%.2f "
             "cam=%.1f,%.1f,%.1f focus=%.1f,%.1f,%.1f "
             "wall=%d ceil=%d ledge=%d "
             "coins=%d stars=%d save=%d before=%d/%d "
             "cal=%.1f,%.1f,%.1f ledge_reach=%.1f goomba=%d,%.1f,%.1f gmodel=%d "
             "gobj=%d,0x%x,%d,%d annex=%d,%d,%d gun=%d,%d,%d tool=%d lgt=%d,%d,%d,%d,%d,%d,%d,%d show=%d,%d dn=%d,%d,%d,%d,%d,%d,%d rate=%d,%d,%d fps=%.1f "
             "chain=%d,%d,0x%x,%d,%d cmode=%d loop=%d "
             "face=%.1f,%.1f,%.1f anim=%d mh=%.1f remap=%d cdbg=%d,%d,%d,%d,%d walk=%d,%d,%d "
             "mpw=%d,%d,%d,%d,%d,%d,%d fc=%d,%d,%d,%d cg=%d,%.0f,%.0f,%.0f,0x%x "
             "slowmo=%d,%d,%d rays=%d,%d,%d glow=%d,%d,%d,%d,%d qd=%d,%d,%d lvl=%d",
             gCurrLevelNum, gCurrAreaIndex, s_phase, s_testMode, s_testActive, s_failed, s_failReason,
             s_starCollected[0], s_starCollected[1], ptc_star_visible(), portal_count(), s_shotA, s_shotB, g_portal_debug,
             g_portal_view.active, g_portal_view.source, portal_view_enabled(),
             s_momEntrySpeed, s_momExitSpeed, s_momApex,
             s_momExitY, s_momExitVX, s_momExitVY, s_momExitVZ,
             s_momHoldSpeed, s_momHoldCos, s_momRatio, s_momTurnDot, s_wallTest,
             hasA ? pa[0] : 0.0f, hasA ? pa[1] : 0.0f, hasA ? pa[2] : 0.0f,
             hasA ? na[0] : 0.0f, hasA ? na[1] : 0.0f, hasA ? na[2] : 0.0f,
             hasB ? pb[0] : 0.0f, hasB ? pb[1] : 0.0f, hasB ? pb[2] : 0.0f,
             hasB ? nb[0] : 0.0f, hasB ? nb[1] : 0.0f, hasB ? nb[2] : 0.0f,
             portal_teleport_count(), portal_last_src(), portal_last_dst(), s_onPlatform,
             pcamD, pcamA, portal_last_mario_residual(),
             m ? m->pos[0] : 0.0f, m ? m->pos[1] : 0.0f, m ? m->pos[2] : 0.0f,
             m ? m->vel[0] : 0.0f, m ? m->vel[1] : 0.0f, m ? m->vel[2] : 0.0f,
             m ? m->forwardVel : 0.0f, m ? (int) m->intendedYaw : 0,
             m ? m->input : 0, m ? m->intendedMag : 0.0f,
             s_inputHookCalls, m ? m->action : 0, m ? m->floorHeight : 0.0f,
             gLakituState.pos[0], gLakituState.pos[1], gLakituState.pos[2],
             gLakituState.focus[0], gLakituState.focus[1], gLakituState.focus[2],
             m && m->wall != NULL, m && m->ceil != NULL,
             m && (m->action == ACT_LEDGE_GRAB || m->action == ACT_START_HANGING),
             m ? m->numCoins : 0, m ? m->numStars : 0,
             ptc_save_star_count(), s_realStarsBefore, s_saveStarsBefore,
             s_calSingle, s_calDouble, s_calTriple, s_calTriple + 100.0f,
             s_goombaActive, s_goombaZ,
             (float) s_goombaCrossings,
             (gLoadedGraphNodes != NULL && gLoadedGraphNodes[MODEL_GOOMBA] != NULL) ? 1 : 0,
             s_goombaIn != NULL,
             s_goombaIn != NULL ? s_goombaIn->header.gfx.node.flags : 0,
             s_goombaIn != NULL ? (s_goombaIn->header.gfx.sharedChild != NULL) : 0,
             s_goombaIn != NULL ? (int) s_goombaIn->activeFlags : -1,
             s_annexMode, s_annexSeen, s_annexTimer,
             portal_gun_has(), portal_gun_aim_error_deg10(),
             portal_gun_yaw_offset_deg10(), movekit_tool(),
             gfx_opengl_light_state(0), gfx_opengl_light_state(1),
             gfx_opengl_light_state(2), gfx_opengl_light_state(3),
             gfx_opengl_light_state(4), gfx_opengl_light_state(5),
             gfx_opengl_light_state(6), gfx_opengl_light_state(7),
             s_showOn, s_showTimer % 600,
             gfx_opengl_denoise_state(0), gfx_opengl_denoise_state(1),
             gfx_opengl_denoise_state(2), gfx_opengl_denoise_state(5),
             gfx_opengl_denoise_state(6), gfx_opengl_denoise_state(3),
             gfx_opengl_denoise_state(8),
             gfx_rate_state(1), gfx_rate_state(2), gfx_rate_state(3),
             gfx_stereo_fps,
             s_chainMode, s_chainTimer, s_chainFlags,
             s_chainGoombaCross,
             boxphys_test_boxes_near(CH_BOX_X, 120.0f, CH_BOX_B_Z, 300.0f),
             gfx_cam_mode, s_chainLoop,
             m ? m->faceAngle[0] : 0.0f, m ? m->faceAngle[1] : 0.0f,
             m ? m->faceAngle[2] : 0.0f,
             (m != NULL && m->marioObj != NULL)
                 ? (int) m->marioObj->header.gfx.unk38.animFrame : -1,
             (m != NULL && m->marioObj != NULL) ? m->marioObj->hitboxHeight : 0.0f,
             camera_portal_remap_count(),
             cdbg[0], cdbg[1], cdbg[2], cdbg[3], cdbg[4],
             s_portalWalk, s_portalWalkDelay, s_portalWalkTimer,
             s_mpw, s_mpwPasses, s_mpwPhase, s_mpwTarget, s_mpwTimer,
             s_mpwLastSrc, s_mpwLastDst,
             custom_camera_freecam_state(0), custom_camera_freecam_state(1),
             custom_camera_freecam_state(2), custom_camera_freecam_state(3),
             s_chainGoomba[0] != NULL,
             s_chainGoomba[0] ? s_chainGoomba[0]->oPosX : 0.0f,
             s_chainGoomba[0] ? s_chainGoomba[0]->oPosY : 0.0f,
             s_chainGoomba[0] ? s_chainGoomba[0]->oPosZ : 0.0f,
             s_chainGoomba[0] ? (unsigned) s_chainGoomba[0]->header.gfx.node.flags : 0u,
             g_sim_slow_div, g_sim_interp, g_sim_present_t1,
             gfx_opengl_ssfx_get(250), gfx_opengl_ssfx_get(251),
             gfx_opengl_ssfx_get(255),
             gfx_opengl_ssfx_get(261), gfx_opengl_ssfx_get(262),
             gfx_opengl_ssfx_get(263), gfx_opengl_ssfx_get(268),
             gfx_opengl_light_state(0),
             gfx_quick_dev_enabled(), gfx_quick_dev_level(), gfx_quick_dev_mute(),
             g_levelInitCount);

    {
        size_t used = strlen(buf);
        if (used + 2 < (size_t)len) {
            buf[used++] = ' ';
            ptc_knob_dump(buf + used, len - (int)used);
        }
    }
}
