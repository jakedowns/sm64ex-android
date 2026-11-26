/**
 * Movement kit: Titanfall-style wall-run, a grapple gun and a fuel jetpack.
 *
 * All three are additive and behind flags (ENH tab). L_TRIG is the generic
 * "use" button: it thrusts when the jetpack is equipped, otherwise it fires /
 * holds the grapple. Wall-running is automatic while airborne.
 */

#include <ultra64.h>
#include <math.h>
#include <stdio.h>

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
#include "../pc/gfx/gfx_pc.h"   /* emissive tag + per-frame light registry */

#define WALLRUN_TICKS       52      // ~1.75 s at 30 Hz
#define WALLRUN_MIN_SPEED   30.0f
#define WALLRUN_CAP         55.0f
#define WALLRUN_ACCEL       0.8f
#define WALLRUN_ROLL        0x0230  // ~4.9 degrees

/* Wall-run jump-off + release window (runtime dials, `cmd fx 320`-`329`).
 * The jump-off used to be a fixed tangent + normal vector that ignored the
 * stick.  It now decomposes the stick (m->intendedYaw/mag, which the game has
 * already mapped through the camera) into the wall basis and blends it with
 * the old vector; the result is renormalised to the old total speed (plus the
 * optional along-run bonus), so level geometry still clears.  Fingers are 0.1
 * units for 322/325/326 and percent for 320/321/327/329. */
#define WALLRUN_BLEND_DEF      65   /* 320: stick share of the exit direction  */
#define WALLRUN_STICK_GAIN_DEF 120  /* 321: stick speed, % of the run speed    */
#define WALLRUN_INTO_MIN_DEF   40   /* 322: min outward direction x0.1 (clamp) */
#define WALLRUN_REL_TICKS_DEF  5    /* 323: release window, ticks (~167 ms)    */
#define WALLRUN_REL_ON_DEF     1    /* 324: release window enabled             */
#define WALLRUN_JUMP_UP_DEF    100  /* 325: exit vy x0.1                       */
#define WALLRUN_JUMP_NORM_DEF  120  /* 326: outward push x0.1                  */
#define WALLRUN_JUMP_TAN_DEF   112  /* 327: tangent speed, % of run speed      */
#define WALLRUN_STICK_MIN_DEF  8    /* 328: stick dead-zone, 0..32             */
#define WALLRUN_ALONG_BONUS_DEF 15  /* 329: extra tangential speed, % of run   */

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

/* Wall-run jump-off dials (fx 320-329). */
static int s_wr_blend        = WALLRUN_BLEND_DEF;
static int s_wr_stick_gain   = WALLRUN_STICK_GAIN_DEF;
static int s_wr_into_min     = WALLRUN_INTO_MIN_DEF;
static int s_wr_rel_ticks    = WALLRUN_REL_TICKS_DEF;
static int s_wr_rel_on       = WALLRUN_REL_ON_DEF;
static int s_wr_jump_up      = WALLRUN_JUMP_UP_DEF;
static int s_wr_jump_normal  = WALLRUN_JUMP_NORM_DEF;
static int s_wr_jump_tangent = WALLRUN_JUMP_TAN_DEF;
static int s_wr_stick_min    = WALLRUN_STICK_MIN_DEF;
static int s_wr_along_bonus  = WALLRUN_ALONG_BONUS_DEF;

/* Release window: the wall run just ended without a jump, so a jump press in
 * the next few ticks is still a wall jump-off.  The wall basis and run speed
 * are snapshotted at release because the wall surface is gone by then. */
static int s_wr_rel_left   = 0;
static int s_wr_last_jump  = 0;   /* 0 none, 1 from ACT_WALLRUN, 2 release window */
static int s_wr_init_count = 0;   /* wall runs started (telemetry) */
static int s_wr_arm_count  = 0;   /* release windows armed (telemetry) */
static f32 s_wr_last_vx    = 0.0f;
static f32 s_wr_last_vz    = 0.0f;
static Vec3f s_wr_rel_t    = { 0.0f, 0.0f, 0.0f };
static Vec3f s_wr_rel_n    = { 0.0f, 0.0f, 0.0f };
static f32 s_wr_rel_speed  = 0.0f;
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

/* ------------------------------------------------- grapple aiming / debug */
/*
 * The grapple resolves its anchor through one function (grapple_aim_resolve)
 * that both the fire path and the targeting debug view call.  The debug view
 * therefore draws the exact query a press would run: same origin, same range,
 * same fan, same surface filter.  Nothing here is a second implementation.
 *
 * Aim axis: camera position -> camera focus, as it has always been.  The ray
 * starts at the CAMERA, not at the gun; the debug view draws the hand -> point
 * leg too, because that is the rope the attach will actually draw.
 *
 * Fan: the axis ray is cast first and wins unconditionally, so every shot that
 * attached before this change still attaches at the same point.  Only when the
 * axis misses are rings of samples inside the half-angle cast (inner ring
 * first, nearest hit wins).  `fx 311 0` restores the pure single-ray behaviour.
 */
#define GRP_NONE    0   /* nothing within the probe distance */
#define GRP_ATTACH  1   /* would attach */
#define GRP_RANGE   2   /* surface exists but is past the range */

static int   s_grp_dbg;                       /* fx 310: view on/off (off) */
static float s_grp_half_deg = 8.0f;           /* fx 311: half-angle, 0.1 deg */
static int   s_grp_rays = 8;                  /* fx 312: ring samples */
static int   s_grp_hud;                       /* fx 313: HUD line (off) */
static float s_grp_range = GRAPPLE_RANGE;     /* fx 314: range in units */
static float s_grp_probe = 4.0f;              /* fx 315: probe x0.1, 4.0x */

typedef struct {
    Vec3f origin;    /* ray origin the attach uses (camera) */
    Vec3f axis;      /* normalised centre direction */
    Vec3f hit;       /* endpoint: attach point, far point, or range end */
    Vec3f normal;    /* surface normal at `hit` (0 when none) */
    f32   dist;      /* origin -> hit */
    f32   beyond;    /* distance of the far surface when status == RANGE */
    f32   rayDeg;    /* winning sample's angle off the axis, degrees */
    int   sample;    /* 0 axis, >0 fan ring sample, -1 none */
    int   status;
} GrappleAim;

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

/* Tangent (run direction) and outward normal from the last wall contact.  The
 * actual horizontal velocity is the best tangent at release (the action may
 * have flipped `side` at a corner); `side` is only the fallback. */
static void wr_last_basis(struct MarioState *m, int side, Vec3f t, Vec3f n) {
    f32 sp;
    n[0] = s_lastWallNormal[0];
    n[1] = 0.0f;
    n[2] = s_lastWallNormal[2];
    vnorm(n);
    sp = sqrtf(m->vel[0] * m->vel[0] + m->vel[2] * m->vel[2]);
    if (sp > 1.0f) {
        t[0] = m->vel[0] / sp;
        t[1] = 0.0f;
        t[2] = m->vel[2] / sp;
    } else {
        t[0] = -n[2] * (f32) side;
        t[1] = 0.0f;
        t[2] = n[0] * (f32) side;
    }
}

/* Snapshot the wall basis when the wall run ends without a jump, so a jump
 * press inside the next few ticks still produces a wall jump-off. */
static void wr_arm_release(Vec3f t, Vec3f n, f32 speed) {
    if (!s_wr_rel_on || s_wr_rel_ticks <= 0) {
        s_wr_rel_left = 0;
        return;
    }
    vec3f_copy(s_wr_rel_t, t);
    vec3f_copy(s_wr_rel_n, n);
    s_wr_rel_speed = speed;
    s_wr_arm_count++;
    /* movekit_update() runs later in this same tick and decrements it, so add
     * one to make the window exactly s_wr_rel_ticks usable ticks. */
    s_wr_rel_left = s_wr_rel_ticks + 1;
}

/* Exit velocity for a wall-run jump-off.  t = tangent (unit, run direction),
 * n = wall normal (unit, pointing away from the wall), runSpeed = the speed
 * the wall run had this tick.  The stick's world direction (sins/coss of
 * intendedYaw, already camera-mapped by the game) is blended in, the outward
 * component is clamped so a stick held into the wall cannot throw Mario back
 * into it, and the result is rescaled to the old fixed-vector magnitude so
 * level geometry still clears. */
static void wr_jump(struct MarioState *m, Vec3f t, Vec3f n,
                    f32 runSpeed, int source) {
    f32 baseT = runSpeed * (f32) s_wr_jump_tangent * 0.01f;
    f32 baseN = (f32) s_wr_jump_normal * 0.1f;
    f32 alongBonus = 0.0f;
    f32 dx = t[0] * baseT + n[0] * baseN;
    f32 dz = t[2] * baseT + n[2] * baseN;
    f32 mag = m->intendedMag / 32.0f;
    f32 blend = 0.0f;

    if (mag >= (f32) s_wr_stick_min / 32.0f) {
        f32 sx, sz, sg, sAlong;
        blend = (f32) s_wr_blend * 0.01f;
        if (mag > 1.0f) {
            mag = 1.0f;
        }
        sx = sins(m->intendedYaw);
        sz = coss(m->intendedYaw);
        sg = runSpeed * (f32) s_wr_stick_gain * 0.01f * mag;
        /* Holding along the run direction keeps (and gains) tangential speed;
         * holding across it only rotates the exit, it does not speed it up. */
        sAlong = sx * t[0] + sz * t[2];
        if (sAlong > 0.0f) {
            alongBonus = sAlong * mag * runSpeed
                       * (f32) s_wr_along_bonus * 0.01f * blend;
        }
        /* Keep the baseline outward push under the stick vector too, so a
         * held stick never has less wall clearance than the neutral vector. */
        dx = dx * (1.0f - blend) + (sx * sg + n[0] * baseN) * blend;
        dz = dz * (1.0f - blend) + (sz * sg + n[2] * baseN) * blend;
    }

    /* "Into the wall" (negative along n) becomes a small outward push. */
    {
        f32 vn = dx * n[0] + dz * n[2];
        f32 minAway = (f32) s_wr_into_min * 0.1f;
        if (vn < minAway) {
            dx += n[0] * (minAway - vn);
            dz += n[2] * (minAway - vn);
        }
    }

    {
        f32 exT = baseT + alongBonus;
        f32 exitSpeed = sqrtf(exT * exT + baseN * baseN);
        f32 len = sqrtf(dx * dx + dz * dz);
        f32 vx, vz;

        if (len < 0.001f) {
            dx = t[0];
            dz = t[2];
            len = 1.0f;
        }
        vx = dx / len * exitSpeed;
        vz = dz / len * exitSpeed;

        m->vel[0] = vx;
        m->vel[1] = (f32) s_wr_jump_up * 0.1f;
        m->vel[2] = vz;
        /* The airborne actions rebuild horizontal velocity from forwardVel and
         * faceAngle every tick (update_air_without_turn), so the launch has to
         * live there too or it is erased on the next tick. */
        m->forwardVel = exitSpeed;
        m->faceAngle[1] = atan2s(vz, vx);
        s_wr_last_vx = vx;
        s_wr_last_vz = vz;
        s_wr_last_jump = source;
    }

    s_wr_rel_left = 0;
    s_roll = 0;
    if (m->marioObj != NULL) {
        m->marioObj->header.gfx.angle[2] = 0;
        m->marioObj->oMoveAngleRoll = 0;
    }
    m->faceAngle[2] = 0;
    s_cooldown = 18;
    set_mario_action(m, ACT_WALL_KICK_AIR, 0);
}

int movekit_try_wallrun(struct MarioState *m) {
    struct WallCollisionData wc;
    struct Surface *wall;
    f32 speed, into;

    if (m == NULL) {
        return 0;
    }
    /* Release cooldown: the wall run ended without a jump a few ticks ago, so
     * this press is still a jump-off from that wall. */
    if (s_wr_rel_left > 0 && (m->input & INPUT_A_PRESSED)
        && (m->action & ACT_FLAG_AIR) && m->action != ACT_WALLRUN) {
        wr_jump(m, s_wr_rel_t, s_wr_rel_n, s_wr_rel_speed, 2);
        return 1;
    }
    if (!s_wallrun_on || s_cooldown > 0) {
        return 0;
    }
    if (!(m->input & INPUT_NONZERO_ANALOG)) {
        return 0;
    }
    speed = sqrtf(m->vel[0] * m->vel[0] + m->vel[2] * m->vel[2]);
    if (speed < WALLRUN_MIN_SPEED) {
        return 0;
    }

    wc.x = m->pos[0];
    wc.y = m->pos[1];
    wc.z = m->pos[2];
    wc.radius = 64.0f;   /* must exceed Mario's collision radius (~50 here) */
    wc.offsetY = 40.0f;
    if (!find_wall_collisions(&wc)) {
        return 0;
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
        return 0;   // not moving into any wall
    }

    // Same-wall regrab rule: only if we dropped at least 20 units or the wall
    // turned by more than ~25 degrees.
    if (s_haveLastWall) {
        f32 d = wall->normal.x * s_lastWallNormal[0]
              + wall->normal.y * s_lastWallNormal[1]
              + wall->normal.z * s_lastWallNormal[2];
        if (d > 0.906f && m->pos[1] > s_lastWallY - 20.0f) {
            return 0;
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
        s_wr_init_count++;
    }
    /* Return 0 so the caller still dispatches ACT_WALLRUN this tick, exactly
     * like the old void function did. */
    return 0;
}

u32 movekit_act_wallrun(struct MarioState *m) {
    struct WallCollisionData wc;
    struct Surface *wall;
    f32 speed, tx, tz;
    int side = (m->actionArg & 1) ? 1 : -1;

    if (m->actionTimer == 0 || !s_wallrun_on) {
        Vec3f t, n;
        f32 sp = sqrtf(m->vel[0] * m->vel[0] + m->vel[2] * m->vel[2]);
        if (sp < WALLRUN_MIN_SPEED) {
            sp = WALLRUN_MIN_SPEED;
        }
        wr_last_basis(m, side, t, n);
        wr_arm_release(t, n, sp);
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
        Vec3f t, n;
        f32 sp = sqrtf(m->vel[0] * m->vel[0] + m->vel[2] * m->vel[2]);
        if (sp < WALLRUN_MIN_SPEED) {
            sp = WALLRUN_MIN_SPEED;
        }
        wr_last_basis(m, side, t, n);
        wr_arm_release(t, n, sp);
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
            Vec3f t, n;
            f32 sp = sqrtf(m->vel[0] * m->vel[0] + m->vel[2] * m->vel[2]);
            if (sp < WALLRUN_MIN_SPEED) {
                sp = WALLRUN_MIN_SPEED;
            }
            wr_last_basis(m, side, t, n);
            wr_arm_release(t, n, sp);
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
        Vec3f t = { tx, 0.0f, tz };
        Vec3f n = { wall->normal.x, 0.0f, wall->normal.z };
        /* The wall normal points away from the surface, so "outward" is +n.
         * (The old fixed vector subtracted it, i.e. pushed into the wall; the
         * next airborne tick then rebuilt the velocity from forwardVel and
         * discarded it entirely.) */
        vnorm(n);
        wr_jump(m, t, n, speed, 1);
        return TRUE;
    }
    return FALSE;
}

s16 movekit_camera_roll(void) {
    return s_roll;
}

/* --------------------------------------------------------- grapple/jetpack */

/* One collision query: exactly what the attach has always done (ray length =
 * range, object-backed surfaces rejected).  Returns 1 on a usable surface. */
static int grp_cast(Vec3f origin, Vec3f dir_unit, f32 length,
                    struct Surface **surf_out, Vec3f hit_out) {
    struct Surface *surf = NULL;
    Vec3f ray, from;

    from[0] = origin[0];
    from[1] = origin[1];
    from[2] = origin[2];
    ray[0] = dir_unit[0] * length;
    ray[1] = dir_unit[1] * length;
    ray[2] = dir_unit[2] * length;
    find_surface_on_ray(from, ray, &surf, hit_out);
    if (surf == NULL || surf->object != NULL) {
        return 0;
    }
    *surf_out = surf;
    return 1;
}

/* Orthonormal basis around the aim axis (right, up). */
static void grp_basis(const Vec3f axis, Vec3f right, Vec3f up) {
    Vec3f worldUp;

    worldUp[0] = 0.0f;
    worldUp[1] = 1.0f;
    worldUp[2] = 0.0f;
    vcross(right, axis, worldUp);
    if (vdot(right, right) < 0.0001f) {
        right[0] = 1.0f;
        right[1] = 0.0f;
        right[2] = 0.0f;
    }
    vnorm(right);
    vcross(up, right, axis);
    vnorm(up);
}

static f32 grp_dist(const Vec3f a, const Vec3f b) {
    Vec3f d;
    d[0] = b[0] - a[0];
    d[1] = b[1] - a[1];
    d[2] = b[2] - a[2];
    return sqrtf(vdot(d, d));
}

/* Resolve the aim the way the attach does.  Callers: grapple_update (fire) and
 * the debug draw / status / HUD.  `s_grp_*` are the live tuning values. */
static void grapple_aim_resolve(GrappleAim *a) {
    Vec3f right, up, sample, hit;
    struct Surface *surf = NULL;
    f32 half;

    a->sample = -1;
    a->dist = 0.0f;
    a->beyond = 0.0f;
    a->rayDeg = 0.0f;
    a->status = GRP_NONE;
    vec3f_copy(a->origin, gLakituState.pos);
    a->axis[0] = gLakituState.focus[0] - gLakituState.pos[0];
    a->axis[1] = gLakituState.focus[1] - gLakituState.pos[1];
    a->axis[2] = gLakituState.focus[2] - gLakituState.pos[2];
    if (vdot(a->axis, a->axis) < 0.0001f) {
        vec3f_copy(a->hit, a->origin);
        a->normal[0] = a->normal[1] = a->normal[2] = 0.0f;
        return;
    }
    vnorm(a->axis);
    a->normal[0] = a->normal[1] = a->normal[2] = 0.0f;

    /* 1. Centre ray.  This is the historical attach path, untouched. */
    if (grp_cast(a->origin, a->axis, s_grp_range, &surf, hit)) {
        vec3f_copy(a->hit, hit);
        a->normal[0] = surf->normal.x;
        a->normal[1] = surf->normal.y;
        a->normal[2] = surf->normal.z;
        vnorm(a->normal);
        a->dist = grp_dist(a->origin, hit);
        a->sample = 0;
        a->status = GRP_ATTACH;
        return;
    }

    /* 2. Fan fallback: two rings inside the half-angle, inner ring first, so
     *    the winner is always the sample closest to the axis. */
    half = s_grp_half_deg * 0.017453292f;
    if (half > 1.5533f) half = 1.5533f;   /* 89 degrees */
    if (half > 0.0001f && s_grp_rays >= 2) {
        int rays = s_grp_rays > 16 ? 16 : s_grp_rays;
        static const f32 ringScale[2] = { 0.5f, 1.0f };

        grp_basis(a->axis, right, up);
        for (int r = 0; r < 2; r++) {
            f32 ang = half * ringScale[r];
            f32 ca = cosf(ang);
            f32 sa = sinf(ang);
            int best = -1;
            f32 bestDist = 1.0e30f;
            Vec3f bestHit = { 0.0f, 0.0f, 0.0f };

            for (int i = 0; i < rays; i++) {
                f32 th = 6.2831853f * (f32) i / (f32) rays;
                f32 ct = cosf(th);
                f32 st = sinf(th);

                sample[0] = a->axis[0] * ca + (right[0] * ct + up[0] * st) * sa;
                sample[1] = a->axis[1] * ca + (right[1] * ct + up[1] * st) * sa;
                sample[2] = a->axis[2] * ca + (right[2] * ct + up[2] * st) * sa;
                vnorm(sample);
                if (!grp_cast(a->origin, sample, s_grp_range, &surf, hit)) {
                    continue;
                }
                {
                    f32 d = grp_dist(a->origin, hit);
                    if (d < bestDist) {
                        bestDist = d;
                        best = i;
                        vec3f_copy(bestHit, hit);
                        a->normal[0] = surf->normal.x;
                        a->normal[1] = surf->normal.y;
                        a->normal[2] = surf->normal.z;
                        vnorm(a->normal);
                    }
                }
            }
            if (best >= 0) {
                vec3f_copy(a->hit, bestHit);
                a->dist = bestDist;
                a->rayDeg = ang / 0.017453292f;
                a->sample = 1 + r * rays + best;
                a->status = GRP_ATTACH;
                return;
            }
        }
    }

    /* 3. Nothing inside range: probe further along the axis so the view can
     *    tell "out of range" from "no surface at all". */
    {
        f32 probe = s_grp_range * s_grp_probe;
        if (probe > s_grp_range + 1.0f
            && grp_cast(a->origin, a->axis, probe, &surf, hit)) {
            vec3f_copy(a->hit, hit);
            a->normal[0] = surf->normal.x;
            a->normal[1] = surf->normal.y;
            a->normal[2] = surf->normal.z;
            vnorm(a->normal);
            a->beyond = grp_dist(a->origin, hit);
            a->dist = s_grp_range;
            a->status = GRP_RANGE;
            return;
        }
    }

    /* 4. Open sky.  Endpoint is the range limit so the view has something to
     *    draw; there is no surface, so no marker and no normal. */
    a->hit[0] = a->origin[0] + a->axis[0] * s_grp_range;
    a->hit[1] = a->origin[1] + a->axis[1] * s_grp_range;
    a->hit[2] = a->origin[2] + a->axis[2] * s_grp_range;
    a->dist = s_grp_range;
}

static void grapple_update(struct MarioState *m, u32 pressed, u32 down, u32 retract) {
    if (!s_grapple_on) {
        s_attached = 0;
        return;
    }

    if (pressed) {
        GrappleAim aim;

        grapple_aim_resolve(&aim);
        if (aim.status == GRP_ATTACH) {
            vec3f_copy(s_anchor, aim.hit);
            s_from[0] = m->pos[0];
            s_from[1] = m->pos[1] + 60.0f;
            s_from[2] = m->pos[2];
            s_rope_len = sqrtf((aim.hit[0] - m->pos[0]) * (aim.hit[0] - m->pos[0])
                             + (aim.hit[1] - (m->pos[1] + 60.0f)) * (aim.hit[1] - (m->pos[1] + 60.0f))
                             + (aim.hit[2] - m->pos[2]) * (aim.hit[2] - m->pos[2]));
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
    if (s_wr_rel_left > 0) {
        s_wr_rel_left--;
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
    /* Keep s_mk_vtx: callers flush mid-object (e.g. around the emissive flame
     * batch) and keep adding quads afterwards. */
}

/* ------------------------------------------- grapple targeting debug view */
/*
 * World-space debug geometry (fx 310).  Drawn inside movekit_render(), i.e. in
 * the 3D scene part of the master display list, so it is replayed per eye and
 * lands at the same world position in 2D and in stereo.  Static vertex storage
 * (like ptc_scene.c); the renderer copies nothing, so it must be rebuilt every
 * frame, which it is.
 */
#define GRP_DRAW_MAX_QUADS 96
static Vtx s_grp_vtx[GRP_DRAW_MAX_QUADS * 4];
static int s_grp_quads;

static void grp_emit(const Vec3f a, const Vec3f b, const Vec3f c, const Vec3f d,
                     u8 r, u8 g, u8 bl, u8 al) {
    Vtx *v;

    if (s_grp_quads >= GRP_DRAW_MAX_QUADS) {
        return;
    }
    v = &s_grp_vtx[s_grp_quads * 4];
    fill_vtx(&v[0], a[0], a[1], a[2], r, g, bl, al);
    fill_vtx(&v[1], b[0], b[1], b[2], r, g, bl, al);
    fill_vtx(&v[2], c[0], c[1], c[2], r, g, bl, al);
    fill_vtx(&v[3], d[0], d[1], d[2], r, g, bl, al);
    s_grp_quads++;
}

static void grp_flush(void) {
    Gfx *h = gDisplayListHead;
    int i = 0;

    while (i < s_grp_quads) {
        int q = s_grp_quads - i;
        if (q > 16) q = 16;
        gSPVertex(h++, s_grp_vtx + i * 4, q * 4, 0);
        for (int k = 0; k < q; k++) {
            int b = k * 4;
            gSP2Triangles(h++, b, b + 1, b + 2, 0, b, b + 2, b + 3, 0);
        }
        i += q;
    }
    gDisplayListHead = h;
    s_grp_quads = 0;
}

/* Thin camera-facing ribbon from a to b. */
static void grp_line(const Vec3f a, const Vec3f b, f32 halfw, const Vec3f eye,
                     u8 r, u8 g, u8 bl, u8 al) {
    Vec3f d, mid, view, perp, A, B, C, D;
    f32 len;

    d[0] = b[0] - a[0];
    d[1] = b[1] - a[1];
    d[2] = b[2] - a[2];
    len = sqrtf(vdot(d, d));
    if (len < 0.5f || halfw <= 0.0f) {
        return;
    }
    mid[0] = (a[0] + b[0]) * 0.5f;
    mid[1] = (a[1] + b[1]) * 0.5f;
    mid[2] = (a[2] + b[2]) * 0.5f;
    view[0] = eye[0] - mid[0];
    view[1] = eye[1] - mid[1];
    view[2] = eye[2] - mid[2];
    vcross(perp, d, view);
    if (vdot(perp, perp) < 0.0001f) {
        return;
    }
    vnorm(perp);
    perp[0] *= halfw;
    perp[1] *= halfw;
    perp[2] *= halfw;

    A[0] = a[0] + perp[0]; A[1] = a[1] + perp[1]; A[2] = a[2] + perp[2];
    B[0] = b[0] + perp[0]; B[1] = b[1] + perp[1]; B[2] = b[2] + perp[2];
    C[0] = b[0] - perp[0]; C[1] = b[1] - perp[1]; C[2] = b[2] - perp[2];
    D[0] = a[0] - perp[0]; D[1] = a[1] - perp[1]; D[2] = a[2] - perp[2];
    /* Counter-clockwise seen from the camera (A, D, C, B), so the ribbon also
     * survives a pass that leaves G_CULL_BACK on. */
    grp_emit(A, D, C, B, r, g, bl, al);
}

/* Cross in the surface plane + a spike along the normal. */
static void grp_marker(const Vec3f p, const Vec3f n, const Vec3f eye, f32 scale,
                       u8 r, u8 g, u8 bl) {
    Vec3f t1, t2, up, base, a, b;
    f32 w = scale * 0.10f;
    if (w < 0.6f) w = 0.6f;

    up[0] = 0.0f; up[1] = 1.0f; up[2] = 0.0f;
    vcross(t1, n, up);
    if (vdot(t1, t1) < 0.0001f) {
        t1[0] = 1.0f; t1[1] = 0.0f; t1[2] = 0.0f;
    }
    vnorm(t1);
    vcross(t2, t1, n);
    vnorm(t2);

    /* Lift off the surface so the cross does not z-fight with it. */
    base[0] = p[0] + n[0] * 2.0f;
    base[1] = p[1] + n[1] * 2.0f;
    base[2] = p[2] + n[2] * 2.0f;

    a[0] = base[0] - t1[0] * scale; a[1] = base[1] - t1[1] * scale; a[2] = base[2] - t1[2] * scale;
    b[0] = base[0] + t1[0] * scale; b[1] = base[1] + t1[1] * scale; b[2] = base[2] + t1[2] * scale;
    grp_line(a, b, w, eye, r, g, bl, 0xF0);
    a[0] = base[0] - t2[0] * scale; a[1] = base[1] - t2[1] * scale; a[2] = base[2] - t2[2] * scale;
    b[0] = base[0] + t2[0] * scale; b[1] = base[1] + t2[1] * scale; b[2] = base[2] + t2[2] * scale;
    grp_line(a, b, w, eye, r, g, bl, 0xF0);

    b[0] = base[0] + n[0] * scale * 1.8f;
    b[1] = base[1] + n[1] * scale * 1.8f;
    b[2] = base[2] + n[2] * scale * 1.8f;
    grp_line(base, b, w * 0.8f, eye, r, g, bl, 0xB0);
}

static void grapple_draw_debug(struct MarioState *m) {
    GrappleAim aim;
    Vec3f eye, right, up, pts[17], from;
    u8 cr, cg, cb;
    f32 fanLen, wFan, wAxis, ang, ca, sa, markerScale;
    int rays;

    grapple_aim_resolve(&aim);
    if (aim.status == GRP_ATTACH) {
        cr = 0x3C; cg = 0xFF; cb = 0x6E;      /* green: would attach */
    } else if (aim.status == GRP_RANGE) {
        cr = 0xFF; cg = 0xB4; cb = 0x28;      /* amber: surface, out of range */
    } else {
        cr = 0xFF; cg = 0x4C; cb = 0x4C;      /* red: no surface */
    }

    /* Own state: vertex colours (G_SHADE) and both faces visible, so the
     * ribbons do not vanish when a previous pass left culling on.  The XLU
     * Z-buffered mode from movekit_render's prologue still applies, so the
     * fan is occluded by real geometry - which is the point of the view. */
    gSPClearGeometryMode(gDisplayListHead++, G_CULL_FRONT | G_CULL_BACK);
    gSPSetGeometryMode(gDisplayListHead++, G_ZBUFFER | G_SHADE);

    vec3f_copy(eye, gLakituState.pos);
    rays = s_grp_rays < 3 ? 3 : (s_grp_rays > 16 ? 16 : s_grp_rays);

    /* Fan length: stop at the resolved point rather than always at max range,
     * so the cone reads as "this is the volume that resolves here". */
    fanLen = (aim.status == GRP_ATTACH) ? aim.dist * 1.05f : s_grp_range;
    if (fanLen > s_grp_range) fanLen = s_grp_range;
    if (fanLen < 60.0f) fanLen = 60.0f;
    wFan = fanLen * 0.0012f;
    if (wFan < 0.8f) wFan = 0.8f;
    if (wFan > 4.0f) wFan = 4.0f;
    wAxis = wFan * 1.6f;

    /* Cone boundary: spokes from the (camera) apex plus the rim ring. */
    ang = s_grp_half_deg * 0.017453292f;
    if (ang > 1.5533f) ang = 1.5533f;
    if (ang > 0.004f) {
        grp_basis(aim.axis, right, up);
        ca = cosf(ang);
        sa = sinf(ang);
        for (int i = 0; i < rays; i++) {
            f32 th = 6.2831853f * (f32) i / (f32) rays;
            f32 ct = cosf(th);
            f32 st = sinf(th);
            Vec3f d;

            d[0] = aim.axis[0] * ca + (right[0] * ct + up[0] * st) * sa;
            d[1] = aim.axis[1] * ca + (right[1] * ct + up[1] * st) * sa;
            d[2] = aim.axis[2] * ca + (right[2] * ct + up[2] * st) * sa;
            vnorm(d);
            pts[i][0] = aim.origin[0] + d[0] * fanLen;
            pts[i][1] = aim.origin[1] + d[1] * fanLen;
            pts[i][2] = aim.origin[2] + d[2] * fanLen;
            grp_line(aim.origin, pts[i], wFan, eye, cr, cg, cb, 0x50);
        }
        for (int i = 0; i < rays; i++) {
            grp_line(pts[i], pts[(i + 1) % rays], wFan * 0.85f, eye, cr, cg, cb, 0x44);
        }
    }

    /* Axis ray, up to the attach point / range limit. */
    {
        Vec3f end;
        f32 len = (aim.status == GRP_ATTACH) ? aim.dist : s_grp_range;

        end[0] = aim.origin[0] + aim.axis[0] * len;
        end[1] = aim.origin[1] + aim.axis[1] * len;
        end[2] = aim.origin[2] + aim.axis[2] * len;
        grp_line(aim.origin, end, wAxis, eye, cr, cg, cb, 0x70);
    }

    /* Off-axis winner: the sample the fan picked instead of the axis. */
    if (aim.status == GRP_ATTACH && aim.sample > 0) {
        grp_line(aim.origin, aim.hit, wAxis, eye, cr, cg, cb, 0xE0);
    }

    /* Surface point + normal. */
    if (aim.status != GRP_NONE) {
        markerScale = aim.status == GRP_RANGE ? aim.beyond * 0.018f : aim.dist * 0.018f;
        if (markerScale < 14.0f) markerScale = 14.0f;
        if (markerScale > 90.0f) markerScale = 90.0f;
        grp_marker(aim.hit, aim.normal, eye, markerScale, cr, cg, cb);
    }

    /* The leg the rope will actually draw: hand -> anchor.  The attach ray
     * starts at the camera, so this is where the hook visibly leaves Mario. */
    if (aim.status == GRP_ATTACH && m != NULL) {
        from[0] = m->pos[0];
        from[1] = m->pos[1] + 60.0f;
        from[2] = m->pos[2];
        grp_line(from, aim.hit, wFan * 0.7f, eye, 0x9A, 0xB0, 0xD0, 0x70);
        grp_line(from, aim.origin, wFan * 0.5f, eye, 0x60, 0x68, 0x78, 0x40);
    }

    /* Optional HUD line (fx 313).  The green debug line is ours while on. */
    if (s_grp_hud) {
        extern void gfx_set_debug_note(const char *s);
        char buf[160];
        f32 yaw = atan2f(aim.axis[2], aim.axis[0]) / 0.017453292f;
        f32 pitch = atan2f(aim.axis[1],
                           sqrtf(aim.axis[0] * aim.axis[0] + aim.axis[2] * aim.axis[2]))
                  / 0.017453292f;
        const char *state = aim.status == GRP_ATTACH ? "ATTACH"
                          : (aim.status == GRP_RANGE ? "RANGE" : "NO SURFACE");

        snprintf(buf, sizeof(buf),
                 "GRP %s yaw=%.1f pitch=%.1f fan=%.1f ray=%.1f d=%.0f p=%.0f,%.0f,%.0f",
                 state, yaw, pitch, s_grp_half_deg, aim.rayDeg,
                 aim.status == GRP_RANGE ? aim.beyond : aim.dist,
                 aim.hit[0], aim.hit[1], aim.hit[2]);
        gfx_set_debug_note(buf);
    }

    grp_flush();
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
            /* Emissive tag (GLOW): flush either side so only the cone lands in
             * the emissive buffer, and tag it warm white-orange at full
             * strength. Short and cyclic because this batch is rebuilt per tick
             * and replayed once per eye. */
            mk_flush();
            gfx_sp_emissive_marker(GFX_EMISSIVE_PACK(255, 168, 72, 235));
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
            mk_flush();
            gfx_sp_emissive_marker(0);

            /* World-space light at the nozzle: same point as the first flame
             * quad, a touch below the pack, so the floor and walls around Mario
             * pick up warm light. Rebuilt every frame by the registry, so it
             * disappears the moment the thrust stops. */
            if (gfx_jet_light_on) {
                float pos[3];
                float rgb[3];
                float k = (float)gfx_jet_light_intensity / 100.0f;
                /* Small deterministic flicker, ~+/-15%. */
                float flick = 0.85f + 0.15f * (float)((s_fx_tick * 37) % 7) / 6.0f;
                pos[0] = c[0] + fwd[0] * 14.0f;
                pos[1] = c[1] - 38.0f;
                pos[2] = c[2] + fwd[2] * 14.0f;
                rgb[0] = 1.00f * k * flick;
                rgb[1] = 0.62f * k * flick;
                rgb[2] = 0.26f * k * flick;
                gfx_light_request(0, pos, rgb, (float)gfx_jet_light_radius);
            } else {
                gfx_light_clear(0);
            }
        } else {
            gfx_light_clear(0);
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

    /* Grapple targeting debug view (fx 310).  Same Z-buffer/render mode as the
     * rope above; drawn after it so the marker is on top of the rope. */
    if (s_grp_dbg) {
        grapple_draw_debug(m);
    }

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

/* ------------------------------------------------ wall-run dials/telemetry */

void movekit_wr_clear(void) {
    s_wr_rel_left = 0;
    s_wr_last_jump = 0;
    s_wr_init_count = 0;
    s_wr_arm_count = 0;
    s_wr_last_vx = 0.0f;
    s_wr_last_vz = 0.0f;
    /* The PTC rig runs several cases back to back on the same wall; without
     * this the same-wall regrab rule and the 20-tick cooldown would block the
     * next case's trigger. */
    s_haveLastWall = 0;
    s_cooldown = 0;
}

/* which: 0 ticks left in the release window, 1 window length in ticks,
 * 2 last jump source (0 none, 1 ACT_WALLRUN, 2 release window), 3 blend %,
 * 4 release speed x10, 5 wall runs started, 6 release windows armed. */
int movekit_wr_state(int which) {
    switch (which) {
        case 0: return s_wr_rel_left > 0 ? s_wr_rel_left : 0;
        case 1: return s_wr_rel_ticks;
        case 2: return s_wr_last_jump;
        case 3: return s_wr_blend;
        case 4: return (int) (s_wr_rel_speed * 10.0f + 0.5f);
        case 5: return s_wr_init_count;
        case 6: return s_wr_arm_count;
    }
    return 0;
}

f32 movekit_wr_last_vel(int axis) {
    return axis ? s_wr_last_vz : s_wr_last_vx;
}

/* fx 320-329.  Values are clamped to sane ranges; 0/1 switches are bools. */
void movekit_dial_set(int which, int value) {
    switch (which) {
        case 320: s_wr_blend = value < 0 ? 0 : (value > 100 ? 100 : value); break;
        case 321: s_wr_stick_gain = value < 0 ? 0 : (value > 300 ? 300 : value); break;
        case 322: s_wr_into_min = value < 0 ? 0 : (value > 100 ? 100 : value); break;
        case 323: s_wr_rel_ticks = value < 0 ? 0 : (value > 60 ? 60 : value); break;
        case 324: s_wr_rel_on = value ? 1 : 0; break;
        case 325: s_wr_jump_up = value < 0 ? 0 : (value > 300 ? 300 : value); break;
        case 326: s_wr_jump_normal = value < 0 ? 0 : (value > 300 ? 300 : value); break;
        case 327: s_wr_jump_tangent = value < 0 ? 0 : (value > 300 ? 300 : value); break;
        case 328: s_wr_stick_min = value < 0 ? 0 : (value > 32 ? 32 : value); break;
        case 329: s_wr_along_bonus = value < 0 ? 0 : (value > 100 ? 100 : value); break;
    }
}

int movekit_dial_get(int which) {
    switch (which) {
        case 320: return s_wr_blend;
        case 321: return s_wr_stick_gain;
        case 322: return s_wr_into_min;
        case 323: return s_wr_rel_ticks;
        case 324: return s_wr_rel_on;
        case 325: return s_wr_jump_up;
        case 326: return s_wr_jump_normal;
        case 327: return s_wr_jump_tangent;
        case 328: return s_wr_stick_min;
        case 329: return s_wr_along_bonus;
    }
    return -1;
}

/* ------------------------------------------- grapple debug dial accessors */
/* fx 310-315 land here from gfx_opengl_ssfx_set/get.  Kept in movekit.c so the
 * debug view and the attach read the same variables. */
void movekit_grapple_debug_set(int which, int value) {
    switch (which) {
        case 310: s_grp_dbg = value ? 1 : 0; break;
        case 311:
            s_grp_half_deg = value < 0 ? 0.0f : (value > 890 ? 89.0f : (f32) value * 0.1f);
            break;
        case 312: s_grp_rays = value < 2 ? 2 : (value > 16 ? 16 : value); break;
        case 313: s_grp_hud = value ? 1 : 0; break;
        case 314:
            s_grp_range = value < 100 ? 100.0f : (value > 6000 ? 6000.0f : (f32) value);
            break;
        case 315:
            s_grp_probe = value < 10 ? 1.0f : (value > 100 ? 10.0f : (f32) value * 0.1f);
            break;
        default: break;
    }
}

int movekit_grapple_debug_get(int which) {
    switch (which) {
        case 310: return s_grp_dbg;
        case 311: return (int) (s_grp_half_deg * 10.0f + 0.5f);
        case 312: return s_grp_rays;
        case 313: return s_grp_hud;
        case 314: return (int) (s_grp_range + 0.5f);
        case 315: return (int) (s_grp_probe * 10.0f + 0.5f);
    }
    return 0;
}

/* Live aim for the status line / HUD: 0 none, 1 would attach, 2 out of range.
 * Fills the distance and the resolved point when the caller asks for them. */
int movekit_grapple_aim_state(float *dist, float *ray_deg, float hit[3]) {
    GrappleAim aim;

    grapple_aim_resolve(&aim);
    if (dist != NULL) *dist = aim.status == GRP_RANGE ? aim.beyond : aim.dist;
    if (ray_deg != NULL) *ray_deg = aim.rayDeg;
    if (hit != NULL) {
        hit[0] = aim.hit[0];
        hit[1] = aim.hit[1];
        hit[2] = aim.hit[2];
    }
    return aim.status;
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
