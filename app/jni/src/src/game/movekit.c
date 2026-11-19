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

#define GRAPPLE_RANGE       1400.0f
#define GRAPPLE_PULL        2.6f
#define GRAPPLE_MIN_DIST    70.0f
#define GRAPPLE_MAX_SPEED   60.0f

#define JET_THRUST          2.2f
#define JET_DRAIN           0.9f
#define JET_REFILL          0.35f
#define JET_MAX_UP          30.0f

static int s_wallrun_on = 1;
static int s_grapple_on = 1;
static int s_jetpack_on = 0;
static float s_fuel = 100.0f;
static float s_fuel_max = 100.0f;
static s16 s_roll = 0;
static int s_cooldown = 0;
static Vec3f s_lastWallNormal;
static f32 s_lastWallY = 0.0f;
static int s_haveLastWall = 0;
static f32 s_rope_len = 0.0f;
static int s_fuel_tier = 0;
static int s_refill_tier = 0;

static int s_attached = 0;
static Vec3f s_anchor;
static Vec3f s_from;

/* ------------------------------------------------------------------ maths */

static f32 vdot(const Vec3f a, const Vec3f b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
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
    wc.radius = 32.0f;
    wc.offsetY = 40.0f;
    if (!find_wall_collisions(&wc)) {
        return;
    }
    wall = wc.walls[wc.numWalls - 1];
    if (wall->normal.y > 0.6f || wall->normal.y < -0.6f) {
        return;   // only near-vertical walls
    }
    into = m->vel[0] * wall->normal.x + m->vel[2] * wall->normal.z;
    if (into > -1.0f) {
        return;   // not moving into the wall
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
        s_cooldown = 20;
        return set_mario_action(m, ACT_FREEFALL, 0);
    }

    wc.x = m->pos[0];
    wc.y = m->pos[1];
    wc.z = m->pos[2];
    wc.radius = 34.0f;
    wc.offsetY = 40.0f;
    if (!find_wall_collisions(&wc)) {
        s_roll = 0;
        s_cooldown = 20;
        return set_mario_action(m, ACT_FREEFALL, 0);
    }
    wall = wc.walls[wc.numWalls - 1];

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

    perform_air_step(m, 0);

    // Keep contact with the wall instead of drifting off it.
    m->pos[0] -= wall->normal.x * 2.0f;
    m->pos[2] -= wall->normal.z * 2.0f;

    if (m->input & INPUT_A_PRESSED) {
        m->vel[0] = tx * speed * 1.12f - wall->normal.x * WALLRUN_JUMP_NORMAL;
        m->vel[2] = tz * speed * 1.12f - wall->normal.z * WALLRUN_JUMP_NORMAL;
        m->vel[1] = WALLRUN_JUMP_UP;
        s_roll = 0;
        s_cooldown = 18;
        return set_mario_action(m, ACT_WALL_KICK_AIR, 0);
    }
    return FALSE;
}

s16 movekit_camera_roll(void) {
    return s_roll;
}

/* --------------------------------------------------------- grapple/jetpack */

static void grapple_update(struct MarioState *m, u32 pressed, u32 down) {
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
        if (s_rope_len > GRAPPLE_MIN_DIST) {
            s_rope_len -= 0.8f;
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

static void jetpack_update(struct MarioState *m, u32 down) {
    if (!s_jetpack_on) {
        return;
    }
    s_attached = 0;   // L is the jetpack when it is equipped

    if (down && s_fuel > 0.0f) {
        m->vel[1] += JET_THRUST;
        if (m->vel[1] > JET_MAX_UP) {
            m->vel[1] = JET_MAX_UP;
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

    if (m == NULL) {
        return;
    }
    if (s_cooldown > 0) {
        s_cooldown--;
    }
    if (m->action != ACT_WALLRUN && s_roll != 0) {
        s_roll = (s16)(s_roll * 0.7f);
        if (s_roll > -32 && s_roll < 32) {
            s_roll = 0;
        }
    }

    pressed = gPlayer1Controller->buttonPressed & L_TRIG;
    down = gPlayer1Controller->buttonDown & L_TRIG;

    if (s_jetpack_on) {
        jetpack_update(m, down);
    } else {
        grapple_update(m, pressed, down);
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

void movekit_render(void) {
    Mat4 view;
    Vtx *v;
    Vec3f dir, right, a, b;

    if (!s_attached || gCurrentArea == NULL) {
        return;
    }
    v = alloc_display_list(sizeof(Vtx) * 4);
    if (v == NULL) {
        return;
    }
    mtxf_lookat(view, gLakituState.pos, gLakituState.focus, 0);

    dir[0] = s_anchor[0] - s_from[0];
    dir[1] = s_anchor[1] - s_from[1];
    dir[2] = s_anchor[2] - s_from[2];
    vnorm(dir);
    // camera right vector (row 0 of the look-at matrix in this convention)
    right[0] = view[0][0];
    right[1] = view[0][1];
    right[2] = view[0][2];
    // perpendicular to the rope in screen space
    a[0] = dir[1] * right[2] - dir[2] * right[1];
    a[1] = dir[2] * right[0] - dir[0] * right[2];
    a[2] = dir[0] * right[1] - dir[1] * right[0];
    vnorm(a);
    b[0] = a[1] * dir[2] - a[2] * dir[1];
    b[1] = a[2] * dir[0] - a[0] * dir[2];
    b[2] = a[0] * dir[1] - a[1] * dir[0];
    vnorm(b);

    fill_vtx(&v[0], s_from[0] + b[0] * 2.0f, s_from[1] + b[1] * 2.0f, s_from[2] + b[2] * 2.0f,
             0xE0, 0xE0, 0xF0, 0xFF);
    fill_vtx(&v[1], s_from[0] - b[0] * 2.0f, s_from[1] - b[1] * 2.0f, s_from[2] - b[2] * 2.0f,
             0xE0, 0xE0, 0xF0, 0xFF);
    fill_vtx(&v[2], s_anchor[0] - b[0] * 2.0f, s_anchor[1] - b[1] * 2.0f, s_anchor[2] - b[2] * 2.0f,
             0xE0, 0xE0, 0xF0, 0xFF);
    fill_vtx(&v[3], s_anchor[0] + b[0] * 2.0f, s_anchor[1] + b[1] * 2.0f, s_anchor[2] + b[2] * 2.0f,
             0xE0, 0xE0, 0xF0, 0xFF);

    {
        Mtx *mtx = alloc_display_list(sizeof(*mtx));
        if (mtx == NULL) {
            return;
        }
        mtxf_to_mtx(mtx, view);
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(mtx),
                  G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    }

    gDPSetCombineMode(gDisplayListHead++, G_CC_SHADE, G_CC_SHADE);
    gDPSetRenderMode(gDisplayListHead++, G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2);
    gSPClearGeometryMode(gDisplayListHead++, G_LIGHTING);
    gSPSetGeometryMode(gDisplayListHead++, G_ZBUFFER);
    gSPVertex(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(v), 4, 0);
    gSP1Triangle(gDisplayListHead++, 0, 1, 2, 0);
    gSP1Triangle(gDisplayListHead++, 0, 2, 3, 0);
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
