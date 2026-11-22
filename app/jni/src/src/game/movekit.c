/**
 * Movement kit: Titanfall-style wall-run, a grapple gun and a fuel jetpack.
 *
 * All three are additive and behind flags (ENH tab). L_TRIG is the generic
 * "use" button: it thrusts when the jetpack is equipped, otherwise it fires /
 * holds the grapple. Wall-running is automatic while airborne.
 */

#include <ultra64.h>
#include <math.h>

#include "sm64.h"
#include "movekit.h"
#include "engine/math_util.h"
#include "engine/surface_collision.h"
#include "game/camera.h"
#include "game/level_update.h"
#include "game/mario.h"
#include "game/mario_step.h"
#include "game/memory.h"
#include "game/game_init.h"
#include "game/rendering_graph_node.h"

#define WALLRUN_TICKS       52      // ~1.75 s at 30 Hz
#define WALLRUN_MIN_SPEED   30.0f
#define WALLRUN_CAP         55.0f
#define WALLRUN_ACCEL       0.8f
#define WALLRUN_JUMP_UP     10.0f
#define WALLRUN_JUMP_NORMAL 12.0f
#define WALLRUN_ROLL        0x0230  // ~4.9 degrees

#define GRAPPLE_RANGE       3400.0f
#define GRAPPLE_PULL        2.6f
#define GRAPPLE_MIN_DIST    70.0f
#define GRAPPLE_MAX_SPEED   60.0f

#define JET_THRUST          6.5f   /* must beat gravity (~4/tick) */
#define JET_DRAIN           0.9f
#define JET_REFILL          0.35f
#define JET_MAX_UP          30.0f
#define JET_AIR_ACCEL       1.7f
#define JET_MAX_HORIZ       34.0f

static int s_wallrun_on = 1;
static int s_grapple_on = 1;
static int s_jetpack_on = 0;

/* Equipped hand tool.  One-press toggle (controller Y / overlay TOOL / debug
 * channel) so the portal gun and the grapple can be used interchangeably
 * without opening a menu.  Tests keep working because the forced trigger path
 * bypasses this gate. */
#define TOOL_PORTAL_GUN 0
#define TOOL_GRAPPLE    1
static int s_tool = TOOL_PORTAL_GUN;
static float s_fuel = 100.0f;
static float s_fuel_max = 100.0f;
static s16 s_roll = 0;
static int s_cooldown = 0;
static Vec3f s_lastWallNormal;
static f32 s_lastWallY = 0.0f;
static int s_haveLastWall = 0;
static f32 s_rope_len = 0.0f;
static u32 s_force_retract_down;
/* Rope presentation: the hook flies from the hand to the anchor on fire and
 * whips back into the hand on release instead of vanishing. */
static int s_ropeShoot;
static int s_ropeRelease;
/* Jetpack exhaust trail: puffs left behind while the pack burns. */
#define JET_PUFF_MAX 26
#define JET_PUFF_LIFE 20
static Vec3f s_puffPos[JET_PUFF_MAX];
static int s_puffAge[JET_PUFF_MAX];
static int s_puffHead;
static int s_puffInit;
static int s_fuel_tier = 0;
static int s_refill_tier = 0;

static int s_attached = 0;
static int s_thrusting;      /* jetpack fired this tick */
static int s_fx_tick;
static Vec3f s_anchor;
static Vec3f s_from;

/* ------------------------------------------------------------------ maths */

static f32 vdot(const Vec3f a, const Vec3f b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void vcross(Vec3f out, const Vec3f a, const Vec3f b) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static void vnorm(Vec3f v) {
    f32 l = sqrtf(vdot(v, v));
    if (l > 0.0001f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

/* --------------------------------------------------------------- wall-run */

void movekit_try_wallrun(struct MarioState *m) {
    struct WallCollisionData wc;
    struct Surface *wall;
    f32 speed, into;

    if (m == NULL || !s_wallrun_on || s_cooldown > 0) {
        return;
    }
    if (!(m->input & INPUT_NONZERO_ANALOG)) {
        return;
    }
    speed = sqrtf(m->vel[0] * m->vel[0] + m->vel[2] * m->vel[2]);
    if (speed < WALLRUN_MIN_SPEED) {
        return;
    }

    wc.x = m->pos[0];
    wc.y = m->pos[1];
    wc.z = m->pos[2];
    wc.radius = 64.0f;   /* must exceed Mario's collision radius (~50 here) */
    wc.offsetY = 40.0f;
    if (!find_wall_collisions(&wc)) {
        return;
    }
    /* Pick the wall Mario is most moving into.  Taking the last of up to
     * four overlapping surfaces can pick a perpendicular face at a corner. */
    wall = NULL;
    into = 0.0f;
    for (int wi = 0; wi < wc.numWalls; wi++) {
        struct Surface *w = wc.walls[wi];
        f32 i2;
        if (w->normal.y > 0.6f || w->normal.y < -0.6f) {
            continue;   // only near-vertical walls
        }
        i2 = m->vel[0] * w->normal.x + m->vel[2] * w->normal.z;
        if (i2 < into) {
            into = i2;
            wall = w;
        }
    }
    if (wall == NULL || into > -1.0f) {
        return;   // not moving into any wall
    }

    // Same-wall regrab rule: only if we dropped at least 20 units or the wall
    // turned by more than ~25 degrees.
    if (s_haveLastWall) {
        f32 d = wall->normal.x * s_lastWallNormal[0]
              + wall->normal.y * s_lastWallNormal[1]
              + wall->normal.z * s_lastWallNormal[2];
        if (d > 0.906f && m->pos[1] > s_lastWallY - 20.0f) {
            return;
        }
    }

    // Side chosen so the tangent follows the current velocity.
    {
        f32 tx = -wall->normal.z;
        f32 tz = wall->normal.x;
        int side = (tx * m->vel[0] + tz * m->vel[2] >= 0.0f) ? 1 : 0;
        set_mario_action(m, ACT_WALLRUN, side);
        m->actionTimer = WALLRUN_TICKS;
        set_mario_animation(m, MARIO_ANIM_RUNNING);
        s_lastWallNormal[0] = wall->normal.x;
        s_lastWallNormal[1] = wall->normal.y;
        s_lastWallNormal[2] = wall->normal.z;
        s_lastWallY = m->pos[1];
        s_haveLastWall = 1;
    }
}

u32 movekit_act_wallrun(struct MarioState *m) {
    struct WallCollisionData wc;
    struct Surface *wall;
    f32 speed, tx, tz;
    int side = (m->actionArg & 1) ? 1 : -1;

    if (m->actionTimer == 0 || !s_wallrun_on) {
        s_roll = 0;
        if (m->marioObj != NULL) {
            m->marioObj->header.gfx.angle[2] = 0;
            m->marioObj->oMoveAngleRoll = 0;
        }
        m->faceAngle[2] = 0;
        s_cooldown = 20;
        return set_mario_action(m, ACT_FREEFALL, 0);
    }

    wc.x = m->pos[0];
    wc.y = m->pos[1];
    wc.z = m->pos[2];
    wc.radius = 64.0f;   /* must exceed Mario's collision radius (~50 here) */
    wc.offsetY = 40.0f;
    if (!find_wall_collisions(&wc)) {
        s_roll = 0;
        s_cooldown = 20;
        return set_mario_action(m, ACT_FREEFALL, 0);
    }
    /* Sustain on the same wall as the trigger, not on whatever face happens to
     * be last in the overlap list at a corner. */
    wall = NULL;
    {
        f32 best = -2.0f;
        for (int wi = 0; wi < wc.numWalls; wi++) {
            struct Surface *w = wc.walls[wi];
            f32 d;
            if (w->normal.y > 0.6f || w->normal.y < -0.6f) {
                continue;
            }
            d = w->normal.x * s_lastWallNormal[0]
              + w->normal.y * s_lastWallNormal[1]
              + w->normal.z * s_lastWallNormal[2];
            if (d > best) {
                best = d;
                wall = w;
            }
        }
        if (wall == NULL) {
            s_roll = 0;
            s_cooldown = 20;
            return set_mario_action(m, ACT_FREEFALL, 0);
        }
    }

    tx = -wall->normal.z * (f32) side;
    tz = wall->normal.x * (f32) side;
    if (tx * m->vel[0] + tz * m->vel[2] < 0.0f) {
        tx = -tx;
        tz = -tz;
        side = -side;
    }
    speed = sqrtf(m->vel[0] * m->vel[0] + m->vel[2] * m->vel[2]) + WALLRUN_ACCEL;
    if (speed > WALLRUN_CAP) {
        speed = WALLRUN_CAP;
    }
    if (speed < WALLRUN_MIN_SPEED) {
        speed = WALLRUN_MIN_SPEED;
    }

    m->vel[0] = tx * speed;
    m->vel[2] = tz * speed;
    m->vel[1] = 3.0f;          // perform_air_step applies gravity on top
    m->forwardVel = speed;
    m->faceAngle[1] = atan2s(tz, tx);
    m->actionTimer--;

    // Camera cant, mirrored per wall side.
    s_roll = (s16)(side * WALLRUN_ROLL);
    /* Roll the body onto the wall plane: the run animation then reads as feet
     * planted on the surface instead of running in mid-air beside it. */
    if (m->marioObj != NULL) {
        m->marioObj->header.gfx.angle[2] = (s16)(side * 0x4000);
        m->marioObj->oMoveAngleRoll = (s16)(side * 0x4000);
    }
    m->faceAngle[2] = (s16)(side * 0x4000);

    perform_air_step(m, 0);

    // Keep contact with the wall instead of drifting off it.
    m->pos[0] -= wall->normal.x * 2.0f;
    m->pos[2] -= wall->normal.z * 2.0f;

    if (m->input & INPUT_A_PRESSED) {
        m->vel[0] = tx * speed * 1.12f - wall->normal.x * WALLRUN_JUMP_NORMAL;
        m->vel[2] = tz * speed * 1.12f - wall->normal.z * WALLRUN_JUMP_NORMAL;
        m->vel[1] = WALLRUN_JUMP_UP;
        s_roll = 0;
        if (m->marioObj != NULL) {
            m->marioObj->header.gfx.angle[2] = 0;
            m->marioObj->oMoveAngleRoll = 0;
        }
        m->faceAngle[2] = 0;
        s_cooldown = 18;
        return set_mario_action(m, ACT_WALL_KICK_AIR, 0);
    }
    return FALSE;
}

s16 movekit_camera_roll(void) {
    return s_roll;
}

/* --------------------------------------------------------- grapple/jetpack */

static void grapple_update(struct MarioState *m, u32 pressed, u32 down, u32 retract) {
    if (!s_grapple_on) {
        s_attached = 0;
        return;
    }

    if (pressed) {
        Vec3f dir, hit;
        struct Surface *surf = NULL;

        dir[0] = gLakituState.focus[0] - gLakituState.pos[0];
        dir[1] = gLakituState.focus[1] - gLakituState.pos[1];
        dir[2] = gLakituState.focus[2] - gLakituState.pos[2];
        vnorm(dir);
        dir[0] *= GRAPPLE_RANGE;
        dir[1] *= GRAPPLE_RANGE;
        dir[2] *= GRAPPLE_RANGE;

        find_surface_on_ray(gLakituState.pos, dir, &surf, hit);
        if (surf != NULL && surf->object == NULL) {
            vec3f_copy(s_anchor, hit);
            s_from[0] = m->pos[0];
            s_from[1] = m->pos[1] + 60.0f;
            s_from[2] = m->pos[2];
            s_rope_len = sqrtf((hit[0] - m->pos[0]) * (hit[0] - m->pos[0])
                             + (hit[1] - (m->pos[1] + 60.0f)) * (hit[1] - (m->pos[1] + 60.0f))
                             + (hit[2] - m->pos[2]) * (hit[2] - m->pos[2]));
            s_attached = 1;
        }
    }

    if (!down) {
        s_attached = 0;
        return;
    }
    if (!s_attached) {
        return;
    }

    {
        Vec3f d;
        f32 dist;

        d[0] = s_anchor[0] - m->pos[0];
        d[1] = s_anchor[1] - (m->pos[1] + 60.0f);
        d[2] = s_anchor[2] - m->pos[2];
        dist = sqrtf(vdot(d, d));
        if (dist < 1.0f) {
            s_attached = 0;
            return;
        }
        d[0] /= dist;
        d[1] /= dist;
        d[2] /= dist;

        // Rope constraint: kill outward radial velocity, keep the tangential
        // component so Mario swings under gravity, and reel in slowly.
        {
            f32 along = vdot(m->vel, d);
            if (along < 0.0f) {
                m->vel[0] -= d[0] * along;
                m->vel[1] -= d[1] * along;
                m->vel[2] -= d[2] * along;
            }
        }
        if (dist > s_rope_len) {
            f32 k = dist - s_rope_len;
            m->pos[0] += d[0] * k;
            m->pos[1] += d[1] * k;
            m->pos[2] += d[2] * k;
        }
        /* R reels the rope in; otherwise the length is fixed so Mario can
         * actually swing instead of being dragged straight to the anchor. */
        if (retract && s_rope_len > GRAPPLE_MIN_DIST) {
            s_rope_len -= 10.0f;
            if (s_rope_len < GRAPPLE_MIN_DIST) {
                s_rope_len = GRAPPLE_MIN_DIST;
            }
        }
        m->vel[0] += d[0] * GRAPPLE_PULL * 0.35f;
        m->vel[1] += d[1] * GRAPPLE_PULL * 0.35f;
        m->vel[2] += d[2] * GRAPPLE_PULL * 0.35f;

        {
            f32 sp = sqrtf(vdot(m->vel, m->vel));
            if (sp > GRAPPLE_MAX_SPEED) {
                f32 k = GRAPPLE_MAX_SPEED / sp;
                m->vel[0] *= k;
                m->vel[1] *= k;
                m->vel[2] *= k;
            }
        }
        if (dist < GRAPPLE_MIN_DIST) {
            s_attached = 0;
            return;
        }
        s_from[0] = m->pos[0];
        s_from[1] = m->pos[1] + 60.0f;
        s_from[2] = m->pos[2];
    }
}

/* Smoke-test override: the PTC cannot synthesise the physical L trigger. */
static u32 s_force_down;
static u32 s_force_prev;

void movekit_force_trigger(int on) {
    if (!on && s_attached) {
        s_ropeRelease = 12;   /* 2x the old fade, back into the hand */
    }
    s_force_down = on ? L_TRIG : 0;
}

void movekit_force_retract(int on) {
    s_force_retract_down = on ? R_TRIG : 0;
}

void movekit_force_anchor(float x, float y, float z) {
    struct MarioState *m = gMarioState;
    if (m == NULL) {
        return;
    }
    s_anchor[0] = x;
    s_anchor[1] = y;
    s_anchor[2] = z;
    s_from[0] = m->pos[0];
    s_from[1] = m->pos[1] + 60.0f;
    s_from[2] = m->pos[2];
    {
        float dx = x - m->pos[0];
        float dy = y - (m->pos[1] + 60.0f);
        float dz = z - m->pos[2];
        s_rope_len = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    s_attached = 1;
    s_ropeShoot = 8;
    s_ropeRelease = 0;
}

#ifdef __ANDROID__
#include <android/log.h>
#endif

static void jetpack_update(struct MarioState *m, u32 down) {
    s_thrusting = 0;
#ifdef __ANDROID__
    {
        static int dbg;
        if ((dbg++ % 30) == 0) {
            __android_log_print(ANDROID_LOG_INFO, "SM64Leia",
                                "jet down=%u fuel=%.1f vy=%.1f", down, s_fuel, m->vel[1]);
        }
    }
#endif
    if (!s_jetpack_on) {
        return;
    }
    s_attached = 0;   // L is the jetpack when it is equipped

    s_fx_tick++;
    if (!s_puffInit) {
        for (int i = 0; i < JET_PUFF_MAX; i++) s_puffAge[i] = 999;
        s_puffInit = 1;
    }
    for (int i = 0; i < JET_PUFF_MAX; i++) {
        if (s_puffAge[i] < 999) s_puffAge[i]++;
    }
    if (down && s_fuel > 0.0f) {
        s_thrusting = 1;
        {
            /* Nozzle position: behind the pack, level with Mario's hips. */
            float fx = sins(m->faceAngle[1]);
            float fz = coss(m->faceAngle[1]);
            int slot = s_puffHead;
            s_puffPos[slot][0] = m->pos[0] - fx * 40.0f;
            s_puffPos[slot][1] = m->pos[1] + 30.0f;
            s_puffPos[slot][2] = m->pos[2] - fz * 40.0f;
            s_puffAge[slot] = 0;
            s_puffHead = (s_puffHead + 1) % JET_PUFF_MAX;
        }
        /* Grounded actions zero vel[1] every tick, so a grounded Mario would
         * burn fuel without ever leaving the floor.  Kick him into the air
         * first, then let the thrust sustain the climb. */
        if (!(m->action & ACT_FLAG_AIR)) {
            set_mario_action(m, ACT_FREEFALL, 0);
            m->vel[1] = 12.0f;
        }
        m->vel[1] += JET_THRUST;
        if (m->vel[1] > JET_MAX_UP) {
            m->vel[1] = JET_MAX_UP;
        }
        /* The left stick steers while the pack is firing. */
        if (m->intendedMag > 4.0f) {
            f32 mag = m->intendedMag / 32.0f;
            f32 hx, hz, h;
            if (mag > 1.0f) {
                mag = 1.0f;
            }
            m->vel[0] += sins(m->intendedYaw) * JET_AIR_ACCEL * mag;
            m->vel[2] += coss(m->intendedYaw) * JET_AIR_ACCEL * mag;
            hx = m->vel[0];
            hz = m->vel[2];
            h = sqrtf(hx * hx + hz * hz);
            if (h > JET_MAX_HORIZ) {
                m->vel[0] = hx * (JET_MAX_HORIZ / h);
                m->vel[2] = hz * (JET_MAX_HORIZ / h);
            }
        }
        /* The freefall action rebuilds m->vel from forwardVel each tick, so
         * carry the jetpack's horizontal velocity there or it is discarded. */
        {
            f32 hs = sqrtf(m->vel[0] * m->vel[0] + m->vel[2] * m->vel[2]);
            if (hs > 0.5f) {
                m->forwardVel = hs;
                m->faceAngle[1] = (s16) atan2s(m->vel[2], m->vel[0]);
            }
        }
        s_fuel -= JET_DRAIN;
        if (s_fuel < 0.0f) {
            s_fuel = 0.0f;
        }
    } else {
        f32 mult = 1.0f + 0.5f * (f32) s_refill_tier;
        f32 rate = ((m->action & ACT_FLAG_AIR) ? JET_REFILL * 0.5f : JET_REFILL) * mult;
        s_fuel += rate;
        if (s_fuel > s_fuel_max) {
            s_fuel = s_fuel_max;
        }
    }
}

void movekit_update(struct MarioState *m) {
    u32 pressed, down;
    u32 playerPressed, playerDown;

    if (m == NULL) {
        return;
    }
    if (s_cooldown > 0) {
        s_cooldown--;
    }
    if (s_ropeShoot > 0) s_ropeShoot--;
    if (s_ropeRelease > 0) s_ropeRelease--;
    if (m->action != ACT_WALLRUN && s_roll != 0) {
        s_roll = (s16)(s_roll * 0.7f);
        if (s_roll > -32 && s_roll < 32) {
            s_roll = 0;
        }
    }

    /* The L trigger belongs to the equipped tool.  Forced test triggers are
     * merged after this gate, so the PTC/chain keep driving the grapple. */
    playerPressed = gPlayer1Controller->buttonPressed;
    playerDown = gPlayer1Controller->buttonDown;
    if (s_tool != TOOL_GRAPPLE) {
        playerPressed &= ~L_TRIG;
        playerDown &= ~L_TRIG;
    }

    /* A forced trigger also produces a one-tick "press" edge, so the grapple
     * (which fires on press) can be exercised by the smoke test. */
    pressed = (playerPressed | (s_force_down & ~s_force_prev)) & L_TRIG;
    down = (playerDown | s_force_down) & L_TRIG;
    s_force_prev = s_force_down;

    if (s_jetpack_on) {
        jetpack_update(m, down);
    } else {
        grapple_update(m, pressed, down,
                       (gPlayer1Controller->buttonDown | s_force_retract_down) & R_TRIG);
    }
}

/* ------------------------------------------------------------- rendering */

static void fill_vtx(Vtx *v, f32 x, f32 y, f32 z, u8 r, u8 g, u8 b, u8 a) {
    v->v.ob[0] = (s16)x;
    v->v.ob[1] = (s16)y;
    v->v.ob[2] = (s16)z;
    v->v.flag = 0;
    v->v.tc[0] = 0;
    v->v.tc[1] = 0;
    v->v.cn[0] = r;
    v->v.cn[1] = g;
    v->v.cn[2] = b;
    v->v.cn[3] = a;
}

/* ------------------------------------------------------------- rendering */

#define MK_MAX_QUADS 64

static Vtx *s_mk_vtx;
static int s_mk_quads;

static void mk_quad(const Vec3f a, const Vec3f b, const Vec3f c, const Vec3f d,
                    u8 r, u8 g, u8 bl, u8 al) {
    Vtx *v;
    if (s_mk_vtx == NULL || s_mk_quads >= MK_MAX_QUADS) {
        return;
    }
    v = &s_mk_vtx[s_mk_quads * 4];
    fill_vtx(&v[0], a[0], a[1], a[2], r, g, bl, al);
    fill_vtx(&v[1], b[0], b[1], b[2], r, g, bl, al);
    fill_vtx(&v[2], c[0], c[1], c[2], r, g, bl, al);
    fill_vtx(&v[3], d[0], d[1], d[2], r, g, bl, al);
    s_mk_quads++;
}

static void mk_flush(void) {
    int i = 0;
    while (i < s_mk_quads) {
        int q = s_mk_quads - i;
        if (q > 16) q = 16;
        gSPVertex(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(&s_mk_vtx[i * 4]), q * 4, 0);
        for (int k = 0; k < q; k++) {
            int b = k * 4;
            gSP2Triangles(gDisplayListHead++, b, b + 1, b + 2, 0, b, b + 2, b + 3, 0);
        }
        i += q;
    }
    s_mk_quads = 0;
    s_mk_vtx = NULL;
}

void movekit_render(void) {
    Mat4 ident;
    Mtx *mtx;
    Vec3f camR, camU;
    struct MarioState *m = gMarioState;

    if (gCurrentArea == NULL) {
        return;
    }

    mtx = alloc_display_list(sizeof(*mtx));
    s_mk_vtx = alloc_display_list(sizeof(Vtx) * MK_MAX_QUADS * 4);
    if (mtx == NULL || s_mk_vtx == NULL) {
        return;
    }
    /* Same as the test scene: the object graph leaves an object matrix on the
     * stack, so load the camera view explicitly. */
    mtxf_lookat(ident, gLakituState.pos, gLakituState.focus, 0);
    mtxf_to_mtx(mtx, ident);
    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(mtx),
              G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gDPSetCombineMode(gDisplayListHead++, G_CC_SHADE, G_CC_SHADE);
    gDPSetRenderMode(gDisplayListHead++, G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2);
    gSPClearGeometryMode(gDisplayListHead++, G_LIGHTING);
    gSPSetGeometryMode(gDisplayListHead++, G_ZBUFFER);

    camR[0] = gLakituState.pos[0] - gLakituState.focus[0];
    camR[1] = 0.0f;
    camR[2] = gLakituState.pos[2] - gLakituState.focus[2];
    vnorm(camR);
    camU[0] = 0.0f;
    camU[1] = 1.0f;
    camU[2] = 0.0f;

    /* --- grapple rope: low-subdivision catenary ribbon ------------------ */
    if (s_attached || s_ropeShoot > 0 || s_ropeRelease > 0) {
        Vec3f from, anchor, dir, perp, prev, p, a, b;
        f32 len, sag;
        int segs = 8;

        from[0] = s_from[0];
        from[1] = s_from[1];
        from[2] = s_from[2];
        anchor[0] = s_anchor[0];
        anchor[1] = s_anchor[1];
        anchor[2] = s_anchor[2];
        if (s_ropeShoot > 0) {
            f32 u = 1.0f - (f32) s_ropeShoot / 8.0f;
            anchor[0] = from[0] + (s_anchor[0] - from[0]) * u;
            anchor[1] = from[1] + (s_anchor[1] - from[1]) * u;
            anchor[2] = from[2] + (s_anchor[2] - from[2]) * u;
        } else if (s_ropeRelease > 0) {
            f32 u = (f32) s_ropeRelease / 12.0f;
            anchor[0] = from[0] + (s_anchor[0] - from[0]) * u;
            anchor[1] = from[1] + (s_anchor[1] - from[1]) * u;
            anchor[2] = from[2] + (s_anchor[2] - from[2]) * u;
        }
        dir[0] = anchor[0] - from[0];
        dir[1] = anchor[1] - from[1];
        dir[2] = anchor[2] - from[2];
        len = sqrtf(vdot(dir, dir));
        vnorm(dir);
        vcross(perp, dir, camR);
        vnorm(perp);
        sag = len * 0.09f;
        if (sag > 70.0f) sag = 70.0f;

        for (int i = 0; i < segs; i++) {
            f32 t0 = (f32) i / (f32) segs;
            f32 t1 = (f32) (i + 1) / (f32) segs;
            f32 s0 = sag * 4.0f * t0 * (1.0f - t0);
            f32 s1 = sag * 4.0f * t1 * (1.0f - t1);
            f32 w0 = 2.4f + 1.2f * (1.0f - t0);   /* thicker at the hand */
            f32 w1 = 2.4f + 1.2f * (1.0f - t1);

            p[0] = from[0] + dir[0] * len * t0;
            p[1] = from[1] + dir[1] * len * t0 - s0;
            p[2] = from[2] + dir[2] * len * t0;
            a[0] = p[0] + perp[0] * w0; a[1] = p[1] + perp[1] * w0; a[2] = p[2] + perp[2] * w0;
            b[0] = p[0] - perp[0] * w0; b[1] = p[1] - perp[1] * w0; b[2] = p[2] - perp[2] * w0;
            prev[0] = a[0]; prev[1] = a[1]; prev[2] = a[2];

            p[0] = from[0] + dir[0] * len * t1;
            p[1] = from[1] + dir[1] * len * t1 - s1;
            p[2] = from[2] + dir[2] * len * t1;
            {
                Vec3f a1, b1;
                a1[0] = p[0] + perp[0] * w1; a1[1] = p[1] + perp[1] * w1; a1[2] = p[2] + perp[2] * w1;
                b1[0] = p[0] - perp[0] * w1; b1[1] = p[1] - perp[1] * w1; b1[2] = p[2] - perp[2] * w1;
                mk_quad(prev, a1, b1, b, 0xD8, 0xDC, 0xE8, 0xF0);
            }
        }
    }

    /* --- jetpack prop, exhaust and diagetic fuel gauge ------------------ */
    if (s_jetpack_on && m != NULL) {
        Vec3f fwd, right, up, c, p0, p1, p2, p3;
        f32 yaw = (f32) m->faceAngle[1] / 65536.0f * 6.2831853f;
        f32 cx = m->pos[0] + sins(m->faceAngle[1]) * -22.0f;
        f32 cy = m->pos[1] + 96.0f;
        f32 cz = m->pos[2] + coss(m->faceAngle[1]) * -22.0f;
        (void) yaw;

        fwd[0] = sins(m->faceAngle[1]);
        fwd[1] = 0.0f;
        fwd[2] = coss(m->faceAngle[1]);
        right[0] = coss(m->faceAngle[1]);
        right[1] = 0.0f;
        right[2] = -sins(m->faceAngle[1]);
        up[0] = 0.0f; up[1] = 1.0f; up[2] = 0.0f;
        c[0] = cx; c[1] = cy; c[2] = cz;

        /* body: a 36x52x24 steel box */
        {
            f32 hx = 18.0f, hy = 26.0f, hz = 12.0f;
            Vec3f o;
            /* six faces; corners built from the basis vectors */
            for (int f = 0; f < 6; f++) {
                Vec3f A, B, C, D;
                f32 s1 = (f & 1) ? -1.0f : 1.0f;
                if (f < 2) {          /* +/- right */
                    o[0] = right[0] * hx * s1; o[1] = 0; o[2] = right[2] * hx * s1;
                    A[0] = c[0] + o[0] - up[0] * hy - fwd[0] * hz;
                    A[1] = c[1] - hy; A[2] = c[2] + o[2] - fwd[2] * hz;
                    B[0] = A[0] + fwd[0] * 2 * hz; B[1] = A[1]; B[2] = A[2] + fwd[2] * 2 * hz;
                    C[0] = B[0]; C[1] = B[1] + 2 * hy; C[2] = B[2];
                    D[0] = A[0]; D[1] = A[1] + 2 * hy; D[2] = A[2];
                } else if (f < 4) {   /* +/- up */
                    o[0] = up[0] * hy * s1; o[1] = hy * s1; o[2] = up[2] * hy * s1;
                    A[0] = c[0] - right[0] * hx + (f == 2 ? -hz * fwd[0] : hz * fwd[0]);
                    A[1] = c[1] + o[1]; A[2] = c[2] - right[2] * hx + (f == 2 ? -hz * fwd[2] : hz * fwd[2]);
                    B[0] = A[0] + right[0] * 2 * hx; B[1] = A[1]; B[2] = A[2] + right[2] * 2 * hx;
                    C[0] = B[0]; C[1] = B[1]; C[2] = B[2] + (f == 2 ? -2 * hz * fwd[2] : 2 * hz * fwd[2]);
                    D[0] = A[0]; D[1] = A[1]; D[2] = A[2] + (f == 2 ? -2 * hz * fwd[2] : 2 * hz * fwd[2]);
                    C[1] = B[1]; D[1] = A[1];
                } else {              /* +/- forward */
                    f32 s2 = (f == 4) ? 1.0f : -1.0f;
                    A[0] = c[0] + right[0] * -hx + fwd[0] * hz * s2;
                    A[1] = c[1] - hy;
                    A[2] = c[2] + right[2] * -hx + fwd[2] * hz * s2;
                    B[0] = A[0] + right[0] * 2 * hx; B[1] = A[1]; B[2] = A[2] + right[2] * 2 * hx;
                    C[0] = B[0]; C[1] = B[1] + 2 * hy; C[2] = B[2];
                    D[0] = A[0]; D[1] = A[1] + 2 * hy; D[2] = A[2];
                }
                mk_quad(A, B, C, D, 0x8A, 0x92, 0xA0, 0xFF);
            }
        }

        /* Nozzle flame: a bright, anchored cone below the pack, drawn long
         * enough to clear Mario's legs from any camera angle. */
        if (s_thrusting) {
            for (int i = 0; i < 5; i++) {
                f32 jitter = (f32) ((s_fx_tick * (i + 2)) % 9) - 4.0f;
                f32 w = 13.0f + (f32) i * 4.0f;
                f32 taper = 1.0f - (f32) i * 0.12f;
                Vec3f A, B, C, D, o;
                u8 r = 0xFF;
                u8 g = (u8) (245 - i * 25);
                u8 bl = (u8) (150 - i * 28);
                u8 al = (u8) (255 - i * 18);
                o[0] = c[0] + fwd[0] * (14.0f + (f32) i * 8.0f) + camR[0] * jitter;
                o[1] = c[1] - 38.0f - (f32) i * 24.0f;
                o[2] = c[2] + fwd[2] * (14.0f + (f32) i * 8.0f) + camR[2] * jitter;
                A[0] = o[0] - camR[0] * w - camU[0] * w * taper;
                A[1] = o[1] - w * taper;
                A[2] = o[2] - camR[2] * w;
                B[0] = o[0] + camR[0] * w - camU[0] * w * taper;
                B[1] = o[1] - w * taper;
                B[2] = o[2] + camR[2] * w;
                C[0] = o[0] + camR[0] * w + camU[0] * w * taper;
                C[1] = o[1] + w * taper;
                C[2] = o[2] + camR[2] * w;
                D[0] = o[0] - camR[0] * w + camU[0] * w * taper;
                D[1] = o[1] + w * taper;
                D[2] = o[2] - camR[2] * w;
                mk_quad(A, B, C, D, r, g, bl, al);
            }
        }

        /* Exhaust trail: puffs left in the air, growing and fading with age. */
        for (int i = 0; i < JET_PUFF_MAX; i++) {
            f32 t, w, al;
            Vec3f A, B, C, D, o;
            if (s_puffAge[i] >= JET_PUFF_LIFE) continue;
            t = (f32) s_puffAge[i] / (f32) JET_PUFF_LIFE;
            w = 9.0f + t * 30.0f;
            al = (u8) (170.0f * (1.0f - t));
            o[0] = s_puffPos[i][0];
            o[1] = s_puffPos[i][1];
            o[2] = s_puffPos[i][2];
            A[0] = o[0] - camR[0] * w - camU[0] * w;
            A[1] = o[1] - w;
            A[2] = o[2] - camR[2] * w;
            B[0] = o[0] + camR[0] * w - camU[0] * w;
            B[1] = o[1] - w;
            B[2] = o[2] + camR[2] * w;
            C[0] = o[0] + camR[0] * w + camU[0] * w;
            C[1] = o[1] + w;
            C[2] = o[2] + camR[2] * w;
            D[0] = o[0] - camR[0] * w + camU[0] * w;
            D[1] = o[1] + w;
            D[2] = o[2] - camR[2] * w;
            mk_quad(A, B, C, D, 0xFF, (u8) (210.0f - 150.0f * t),
                    (u8) (70.0f * (1.0f - t)), al);
        }

        /* diagetic fuel bar on the back face */
        {
            f32 frac = s_fuel / s_fuel_max;
            f32 w = 30.0f * (frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac));
            Vec3f A, B, C, D;
            f32 bx = c[0] - fwd[0] * 13.0f;
            f32 by = c[1] + 8.0f;
            f32 bz = c[2] - fwd[2] * 13.0f;
            A[0] = bx - right[0] * 15.0f - up[0] * 4.0f; A[1] = by - 4.0f; A[2] = bz - right[2] * 15.0f;
            B[0] = bx + right[0] * (w - 15.0f); B[1] = A[1]; B[2] = bz + right[2] * (w - 15.0f);
            C[0] = B[0]; C[1] = by + 4.0f; C[2] = B[2];
            D[0] = A[0]; D[1] = C[1]; D[2] = A[2];
            mk_quad(A, B, C, D, (u8) (255 - (int) (w * 5)), (u8) (80 + (int) (w * 5)), 0x40, 0xFF);
        }
    }

    mk_flush();

    gSPClearGeometryMode(gDisplayListHead++, G_ZBUFFER);
}

/* --------------------------------------------------------------- getters */

int movekit_fuel_pct(void) {
    return (int)((s_fuel / s_fuel_max) * 100.0f + 0.5f);
}

int movekit_state(void) {
    return (s_wallrun_on ? 1 : 0) | (s_grapple_on ? 2 : 0) | (s_jetpack_on ? 4 : 0)
         | (s_attached ? 8 : 0);
}

void movekit_upgrade(int which) {
    if (which == 0) {
        s_fuel_tier = (s_fuel_tier + 1) % 3;
        s_fuel_max = 100.0f + 50.0f * (f32) s_fuel_tier;
        if (s_fuel > s_fuel_max) {
            s_fuel = s_fuel_max;
        }
    } else {
        s_refill_tier = (s_refill_tier + 1) % 3;
    }
}

int movekit_fuel_tier(void) {
    return s_fuel_tier;
}

int movekit_refill_tier(void) {
    return s_refill_tier;
}

void movekit_set_flag(int which, int on) {
    switch (which) {
        case 0: s_wallrun_on = on ? 1 : 0; break;
        case 1: s_grapple_on = on ? 1 : 0; break;
        case 2:
            s_jetpack_on = on ? 1 : 0;
            if (s_jetpack_on) {
                s_fuel = s_fuel_max;
            }
            break;
    }
}

/* --------------------------------------------------------- tool switching */

static void movekit_tool_note(void) {
    extern void gfx_set_debug_note(const char *s);
    gfx_set_debug_note(s_tool == TOOL_GRAPPLE
        ? "TOOL: GRAPPLE  (L TRIGGER fires / reels)"
        : "TOOL: PORTAL GUN  (L1 / R1)");
}

int movekit_tool(void) {
    return s_tool;
}

int movekit_tool_is_grapple(void) {
    return s_tool == TOOL_GRAPPLE;
}

int movekit_portal_equipped(void) {
    return s_tool == TOOL_PORTAL_GUN;
}

void movekit_tool_set(int tool) {
    int next = tool ? TOOL_GRAPPLE : TOOL_PORTAL_GUN;

    if (next == s_tool) {
        return;
    }
    if (s_attached) {
        /* Holstering the grapple lets the rope snap back to the hand. */
        s_attached = 0;
        s_ropeRelease = 12;
    }
    s_tool = next;
    movekit_tool_note();
}

void movekit_tool_toggle(void) {
    movekit_tool_set(s_tool == TOOL_GRAPPLE ? TOOL_PORTAL_GUN : TOOL_GRAPPLE);
}
