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

/* Per-object occluder fade.  Bit N = fade object N (0..8 platforms, 9 ramp,
 * 10..13 room walls), set by ptc_occlusion_update(). */
u32 g_ptc_fade_mask = 0;

/* Animated 0..1 fade amount per object (0 = opaque, 1 = fully faded). */
static f32 s_ptc_fade[24];

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
static const Gfx *const s_ptc_wall_dl[4] = {
    ptc_dl_wall_0, ptc_dl_wall_1, ptc_dl_wall_2, ptc_dl_wall_3,
};
static const Gfx *const s_ptc_wall_fade[4] = {
    ptc_dl_wall_0_fade, ptc_dl_wall_1_fade,
    ptc_dl_wall_2_fade, ptc_dl_wall_3_fade,
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
    if (id == 21) {
        return (Gfx *) ptc_dl_mirror;
    }
    id -= 17;
    if (id >= 0 && id < 4) {
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
    for (int i = 0; i < 14; i++) {
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
    Vec3f o, d, target;

    g_ptc_fade_mask = 0;
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
    /* Walls: 10 front (z-), 11 back (z+), 12 left (x-), 13 right (x+). */
    if (ptc_ray_plane(o, d, 2, -2600.0f, -1200.0f, 2450.0f, 0.0f, 1400.0f)) {
        g_ptc_fade_mask |= 1u << 10;
    }
    if (ptc_ray_plane(o, d, 2, 1250.0f, -1200.0f, 2450.0f, 0.0f, 1400.0f)) {
        g_ptc_fade_mask |= 1u << 11;
    }
    if (ptc_ray_plane(o, d, 0, -1200.0f, 0.0f, 1400.0f, -2600.0f, 1250.0f)) {
        g_ptc_fade_mask |= 1u << 12;
    }
    if (ptc_ray_plane(o, d, 0, 2450.0f, 0.0f, 1400.0f, -2600.0f, 1250.0f)) {
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
static int s_realStarsBefore;
static int s_realStarsAfter;
static int s_saveStarsBefore;
static int s_saveStarsAfter;
static int s_inputHookCalls;

static char s_pendingCommand[24];
static volatile int s_pendingCommandReady;
static void ptc_apply_command(const char *action);
static void ptc_goomba_update(void);
static void ptc_annex_update(void);
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
                star = spawn_object_abs_with_rot(NULL, 0, MODEL_STAR, bhvPtcDebugStar,
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
    s_testActive = 0;
    s_phase = PTC_PHASE_IDLE;
    s_phaseTimer = 0;
    s_failed = 0;
    s_failReason[0] = '\0';
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
                s_momEntrySpeed = s_momPrevSpeed;
                s_momExitSpeed = sqrtf(m->vel[0] * m->vel[0] + m->vel[1] * m->vel[1]
                                       + m->vel[2] * m->vel[2]);
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
                } else {
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

void ptc_update(void) {
    struct MarioState *m = gMarioState;

    ptc_occlusion_update();

    if (s_pendingCommandReady) {
        s_pendingCommandReady = 0;
        ptc_apply_command(s_pendingCommand);
    }

    ptc_goomba_update();
    ptc_annex_update();

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

void ptc_mario_input_override(struct MarioState *m) {
    if (m == NULL) {
        return;
    }
    s_inputHookCalls++;

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

/* ------------------------------------------------- corner portal test */

/* Reproducible corner test: portals 20 units from the SW corner where the blue
 * (west) and green (south) walls meet, Mario standing in the corner facing one
 * at a time.  Used to debug the mirrored Mario seen inside a portal. */
static void ptc_corner_portal(int face_b) {
    /* The ellipse is 135 wide (rx 67.5), so a portal cannot literally sit 20
     * units from the corner - the rim test rejects it.  100 units keeps the
     * whole rim on its wall while still hugging the corner. */
    Vec3f aOrig = { -2400.0f, 200.0f, -3100.0f };
    Vec3f aTarget = { -2700.0f, 200.0f, -3100.0f };   /* west wall, normal +X */
    Vec3f bOrig = { -2500.0f, 200.0f, -3000.0f };
    Vec3f bTarget = { -2500.0f, 200.0f, -3300.0f };   /* south wall, normal +Z */

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
    } else if (strcmp(action, "reset") == 0) {
        ptc_reset();
    } else if (strcmp(action, "calibrate") == 0) {
        ptc_calibrate_start();
    } else if (strcmp(action, "momentum") == 0) {
        ptc_momentum_start();
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
    } else if (strcmp(action, "goomba") == 0) {
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
    } else if (strcmp(action, "grapple") == 0) {
        ptc_annex_grapple();
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
    int hasA = portal_get(0, pa, na);
    int hasB = portal_get(1, pb, nb);

    if (buf == NULL || len <= 0) {
        return;
    }

    snprintf(buf, len,
             "PTC level=%d area=%d phase=%d mode=%d active=%d fail=%d reason=\"%s\" "
             "star=%d star2=%d star_visible=%d portals=%d shotA=%d shotB=%d pdbg=%d "
             "view=%d vsrc=%d "
             "mom_enter=%.1f mom_exit=%.1f mom_apex=%.1f momy=%.1f momv=%.1f,%.1f,%.1f walltest=0x%X "
             "p0=%.1f,%.1f,%.1f n0=%.3f,%.3f,%.3f p1=%.1f,%.1f,%.1f n1=%.3f,%.3f,%.3f "
             "tele=%d src=%d dst=%d onplat=%d "
             "mario=%.2f,%.2f,%.2f vel=%.2f,%.2f,%.2f fv=%.1f in=0x%04X im=%.1f hook=%d action=0x%08X floor=%.2f "
             "cam=%.1f,%.1f,%.1f focus=%.1f,%.1f,%.1f "
             "wall=%d ceil=%d ledge=%d "
             "coins=%d stars=%d save=%d before=%d/%d "
             "cal=%.1f,%.1f,%.1f ledge_reach=%.1f goomba=%d,%.1f,%.1f gmodel=%d "
             "gobj=%d,0x%x,%d,%d annex=%d,%d,%d",
             gCurrLevelNum, gCurrAreaIndex, s_phase, s_testMode, s_testActive, s_failed, s_failReason,
             s_starCollected[0], s_starCollected[1], ptc_star_visible(), portal_count(), s_shotA, s_shotB, g_portal_debug,
             g_portal_view.active, g_portal_view.source,
             s_momEntrySpeed, s_momExitSpeed, s_momApex,
             s_momExitY, s_momExitVX, s_momExitVY, s_momExitVZ, s_wallTest,
             hasA ? pa[0] : 0.0f, hasA ? pa[1] : 0.0f, hasA ? pa[2] : 0.0f,
             hasA ? na[0] : 0.0f, hasA ? na[1] : 0.0f, hasA ? na[2] : 0.0f,
             hasB ? pb[0] : 0.0f, hasB ? pb[1] : 0.0f, hasB ? pb[2] : 0.0f,
             hasB ? nb[0] : 0.0f, hasB ? nb[1] : 0.0f, hasB ? nb[2] : 0.0f,
             portal_teleport_count(), portal_last_src(), portal_last_dst(), s_onPlatform,
             m ? m->pos[0] : 0.0f, m ? m->pos[1] : 0.0f, m ? m->pos[2] : 0.0f,
             m ? m->vel[0] : 0.0f, m ? m->vel[1] : 0.0f, m ? m->vel[2] : 0.0f,
             m ? m->forwardVel : 0.0f, m ? m->input : 0, m ? m->intendedMag : 0.0f,
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
             s_annexMode, s_annexSeen, s_annexTimer);

    {
        size_t used = strlen(buf);
        if (used + 2 < (size_t)len) {
            buf[used++] = ' ';
            ptc_knob_dump(buf + used, len - (int)used);
        }
    }
}
