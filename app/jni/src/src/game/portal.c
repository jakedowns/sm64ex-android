/**
 * Portal gun: pickup prop, two placed portals, hitscan placement and teleport.
 *
 * Portals are stored as an oriented ellipse on a collision surface (centre,
 * surface normal pointing out of the wall, tangent/bitangent). The renderer
 * draws them as a coloured fan after the scene, and the simulation tick checks
 * whether Mario's centre of mass crossed a portal plane inside the ellipse.
 *
 * Clone / through-portal camera relay is deliberately not implemented here yet.
 */

#include <ultra64.h>
#include <math.h>

#include <string.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "sm64.h"
#include "portal.h"
#include "engine/math_util.h"
#include "engine/surface_collision.h"
#include "game/camera.h"
#include "game/level_update.h"
#include "game/mario.h"
#include "game/memory.h"
#include "game/object_helpers.h"
#include "game/rendering_graph_node.h"
#include "game/game_init.h"
#include "game/print.h"
#include "audio/external.h"
#include "game/area.h"
#include "gfx_dimensions.h"
#include "ptc_scene.h"

/* Generated low-poly portal-gun mesh + display list (CC0 model). */
#include "ptc_gun_model.inc"

#define PORTAL_RX        67.5f     // ellipse half width (world units, 1.5x)
#define PORTAL_RY        120.0f    // ellipse half height (1.5x)
#define PORTAL_SEGMENTS  24
#define PORTAL_RAY_DIST  4000.0f   // hitscan length
#define PORTAL_EXIT_PUSH 130.0f    // clear the entry trigger after exiting
#define PORTAL_ENTER_DIST 110.0f    // proximity needed to use a floor/ramp portal
#define PORTAL_COOLDOWN  20        // ticks before the next teleport
#define PORTAL_PICKUP_R  200.0f

#define GUN_HALF_X 18.0f
#define GUN_HALF_Y 10.0f
#define GUN_HALF_Z 55.0f

struct Portal {
    int active;
    Vec3f pos;
    Vec3f normal;   // out of the surface
    Vec3f tan;      // in-plane, roughly horizontal
    Vec3f bitan;    // in-plane, roughly vertical
    float rx, ry;
};

static struct Portal s_portal[2];
static int s_lastPlaced = 1;
// Last placement result, for headless debugging: 0 none, 1 placed, 2 bad
// surface, 3 could not fit the ellipse, 4 no gun.
int g_portal_debug = 0;
int g_portal_debug_align = 0;   /* magenta source fan behind the relay decal */
int g_portal_debug_color = 0;   /* solid relay colour: isolate geometry/UVs */
PortalViewState g_portal_view;
static int s_portal_view_enabled = 1;
static int s_forceView = -1;   /* debug: -1 auto, 0/1 force that source */
static int s_hasGun = 0;
static int s_gunSpawned = 0;
static Vec3f s_gunPos = {0.0f, 0.0f, 0.0f};
static float s_gunSpin = 0.0f;
static int s_cooldown = 0;
static Vec3f s_prevCom;
static int s_havePrevCom = 0;
static int s_lastLevelArea = -1;
static int s_ticksInArea = 0;
static int s_lastTeleportSrc = -1;
static int s_lastTeleportDst = -1;
static int s_teleportCount = 0;

/* ------------------------------------------------------------------ math */

static float vdot3(const Vec3f a, const Vec3f b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void vcross3(Vec3f out, const Vec3f a, const Vec3f b) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static void vnorm3(Vec3f v) {
    float len = sqrtf(vdot3(v, v));
    if (len > 0.0001f) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

// Row-vector 4x4 multiply: res = a * b (this engine stores matrices transposed
// relative to GL, with translation in row 3).
static void mat4_mul_rm(float res[4][4], const float a[4][4], const float b[4][4]) {
    float tmp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j]
                      + a[i][3] * b[3][j];
        }
    }
    memcpy(res, tmp, sizeof(tmp));
}

/* ------------------------------------------------------- portal placement */

static int portal_surface_ok(struct Surface *surf) {
    if (surf == NULL) return 0;
    // Object-owned surfaces: NPCs, coins, question blocks, breakable bricks,
    // cork boxes, signs, doors... all excluded.
    if (surf->object != NULL) return 0;
    switch (surf->type) {
        case SURFACE_WATER:
        case SURFACE_BURNING:
        case SURFACE_DEATH_PLANE:
        case SURFACE_VANISH_CAP_WALLS:
        case SURFACE_INSTANT_QUICKSAND:
        case SURFACE_INSTANT_MOVING_QUICKSAND:
        case SURFACE_FLOWING_WATER:
        case SURFACE_DEEP_MOVING_QUICKSAND:
            return 0;
        default:
            return 1;
    }
}

// Builds the ellipse basis from a surface normal.
static void portal_basis_from_normal(const Vec3f n, Vec3f tan, Vec3f bitan) {
    Vec3f up = {0.0f, 1.0f, 0.0f};
    Vec3f worldX = {1.0f, 0.0f, 0.0f};

    vcross3(tan, up, n);
    if (vdot3(tan, tan) < 0.01f) {
        // Floor / ceiling: use world X as the reference.
        vcross3(tan, worldX, n);
    }
    vnorm3(tan);
    vcross3(bitan, n, tan);
    vnorm3(bitan);
}

// Terrain collision is tessellated per triangle, so "same surface" cannot mean
// the same Surface pointer. Compare the plane instead: same normal and the hit
// point lies on the plane of the original hit.
static int portal_rim_ok(const Vec3f centre, const Vec3f n, const Vec3f tan, const Vec3f bitan,
                         float rx, float ry, const Vec3f planePos) {
    static const float ang[8] = { 0.0f, 0.7854f, 1.5708f, 2.3562f, 3.1416f, 3.9270f, 4.7124f, 5.4978f };

    for (int i = 0; i < 8; i++) {
        Vec3f p;
        Vec3f dir;
        struct Surface *hit = NULL;
        Vec3f hitPos;

        p[0] = centre[0] + (tan[0] * cosf(ang[i]) * rx) + (bitan[0] * sinf(ang[i]) * ry);
        p[1] = centre[1] + (tan[1] * cosf(ang[i]) * rx) + (bitan[1] * sinf(ang[i]) * ry);
        p[2] = centre[2] + (tan[2] * cosf(ang[i]) * rx) + (bitan[2] * sinf(ang[i]) * ry);
        // Lift the probe off the surface, then look back at it.
        p[0] += n[0] * 8.0f;
        p[1] += n[1] * 8.0f;
        p[2] += n[2] * 8.0f;
        dir[0] = -n[0] * 32.0f;
        dir[1] = -n[1] * 32.0f;
        dir[2] = -n[2] * 32.0f;
        find_surface_on_ray(p, dir, &hit, hitPos);
        if (hit == NULL) {
            return 0;
        }
        {
            Vec3f hn;
            float dn, dp;
            hn[0] = hit->normal.x;
            hn[1] = hit->normal.y;
            hn[2] = hit->normal.z;
            vnorm3(hn);
            dn = vdot3(hn, n);
            dp = (hitPos[0] - planePos[0]) * n[0]
               + (hitPos[1] - planePos[1]) * n[1]
               + (hitPos[2] - planePos[2]) * n[2];
            if (dn < 0.99f || fabsf(dp) > 6.0f) {
                return 0;
            }
        }
    }
    return 1;
}

static void portal_shoot_ray(int which, Vec3f orig, Vec3f rayDir) {
    struct Surface *surf = NULL;
    Vec3f hit;
    Vec3f n, tan, bitan;
    Vec3f centre;
    Vec3f dir;

    vec3f_copy(dir, rayDir);
    vnorm3(dir);
    dir[0] *= PORTAL_RAY_DIST;
    dir[1] *= PORTAL_RAY_DIST;
    dir[2] *= PORTAL_RAY_DIST;

    find_surface_on_ray(orig, dir, &surf, hit);
    if (!portal_surface_ok(surf)) {
        g_portal_debug = 2;
        return;
    }

    vec3f_set(n, surf->normal.x, surf->normal.y, surf->normal.z);
    vnorm3(n);
    portal_basis_from_normal(n, tan, bitan);

    /* Floor/ceiling portals: rotate the tall axis of the ellipse towards the
     * camera so it reads as a doorway in front of the player instead of a
     * fixed east-west oval. */
    if (fabsf(n[1]) > 0.7f) {
        Vec3f look, flat;
        float d;
        vec3f_copy(look, gLakituState.focus);
        vec3f_sub(look, gLakituState.pos);
        d = vdot3(look, n);
        flat[0] = look[0] - n[0] * d;
        flat[1] = look[1] - n[1] * d;
        flat[2] = look[2] - n[2] * d;
        if (vdot3(flat, flat) > 4.0f) {
            vnorm3(flat);
            vec3f_copy(bitan, flat);
            vcross3(tan, bitan, n);
            vnorm3(tan);
        }
    }

    vec3f_copy(centre, hit);
    // Nudge the portal off the surface a little so it does not z-fight.
    centre[0] += n[0] * 4.0f;
    centre[1] += n[1] * 4.0f;
    centre[2] += n[2] * 4.0f;

    // Edge fitting: push the ellipse away from edges until every rim probe
    // still lands on the same surface; shrink it if that is not enough.
    float rx = PORTAL_RX;
    float ry = PORTAL_RY;
    int fitted = 0;

    for (int shrink = 0; shrink < 3 && !fitted; shrink++) {
        for (int iter = 0; iter < 8 && !fitted; iter++) {
            if (portal_rim_ok(centre, n, tan, bitan, rx, ry, hit)) {
                fitted = 1;
                break;
            }
            // Move the centre away from the failing side: try the four cardinal
            // directions and keep the first that improves things.
            Vec3f cand;
            int improved = 0;
            static const float dirs[4][2] = { {1, 0}, {-1, 0}, {0, 1}, {0, -1} };
            for (int d = 0; d < 4 && !improved; d++) {
                vec3f_copy(cand, centre);
                cand[0] += (tan[0] * dirs[d][0] + bitan[0] * dirs[d][1]) * rx * 0.35f;
                cand[1] += (tan[1] * dirs[d][0] + bitan[1] * dirs[d][1]) * ry * 0.35f;
                cand[2] += (tan[2] * dirs[d][0] + bitan[2] * dirs[d][1]) * rx * 0.35f;
                if (portal_rim_ok(cand, n, tan, bitan, rx, ry, hit)) {
                    vec3f_copy(centre, cand);
                    fitted = 1;
                    improved = 1;
                }
            }
            if (!improved) {
                vec3f_copy(centre, hit);
                centre[0] += n[0] * 4.0f;
                centre[1] += n[1] * 4.0f;
                centre[2] += n[2] * 4.0f;
            }
        }
        if (!fitted) {
            rx *= 0.75f;
            ry *= 0.75f;
        }
    }
    if (!fitted) {
        g_portal_debug = 3;
        return;   // too small / too close to an edge: leave the old portal
    }

    s_portal[which].active = 1;
    vec3f_copy(s_portal[which].pos, centre);
    vec3f_copy(s_portal[which].normal, n);
    vec3f_copy(s_portal[which].tan, tan);
    vec3f_copy(s_portal[which].bitan, bitan);
    s_portal[which].rx = rx;
    s_portal[which].ry = ry;
    s_havePrevCom = 0;
    s_lastPlaced = which;
    g_portal_debug = 1;
}

void portal_shoot(int which) {
    Vec3f orig, dir;

    if (which < 0 || which > 1 || gMarioState == NULL) {
        return;
    }
    if (!s_hasGun) {
        g_portal_debug = 4;
        return;
    }

    vec3f_copy(orig, gLakituState.pos);
    vec3f_copy(dir, gLakituState.focus);
    vec3f_sub(dir, orig);

    /* Fire, then report whether the surface accepted the portal. */
    play_sound(SOUND_ACTION_THROW, gDefaultSoundArgs);
    portal_shoot_ray(which, orig, dir);
    printf("portal: shot %d debug=%d active=%d pos=%.1f,%.1f,%.1f\n",
           which, g_portal_debug, s_portal[which].active,
           s_portal[which].pos[0], s_portal[which].pos[1], s_portal[which].pos[2]);
    if (g_portal_debug == 1) {
        play_sound(SOUND_MENU_CHANGE_SELECT, gDefaultSoundArgs);
    } else {
        play_sound(SOUND_MENU_CAMERA_BUZZ, gDefaultSoundArgs);
    }
}

int portal_shoot_at(int which, const Vec3f origin, const Vec3f target) {
    Vec3f orig, dir;

    if (which < 0 || which > 1 || gMarioState == NULL) {
        return 0;
    }
    if (!s_hasGun) {
        g_portal_debug = 4;
        return 0;
    }

    orig[0] = origin[0];
    orig[1] = origin[1];
    orig[2] = origin[2];
    dir[0] = target[0] - origin[0];
    dir[1] = target[1] - origin[1];
    dir[2] = target[2] - origin[2];
    if (vdot3(dir, dir) < 0.001f) {
        g_portal_debug = 2;
        return 0;
    }
    portal_shoot_ray(which, orig, dir);
    return s_portal[which].active;
}

/* ------------------------------------------------------------- simulation */

static void portal_clear(void) {
    s_portal[0].active = 0;
    s_portal[1].active = 0;
    s_havePrevCom = 0;
    s_cooldown = 0;
    s_lastTeleportSrc = -1;
    s_lastTeleportDst = -1;
}

void portal_reset(void) {
    portal_clear();
    g_portal_debug = 0;
    s_teleportCount = 0;
}

static void portal_rotate_vec(const struct Portal *from, const struct Portal *to,
                              const Vec3f in, Vec3f out) {
    float x = vdot3(in, from->tan);
    float y = vdot3(in, from->bitan);
    float z = vdot3(in, from->normal);
    // Mirror through the portal plane (180 degrees about the local up axis).
    x = -x;
    z = -z;
    out[0] = to->tan[0] * x + to->bitan[0] * y + to->normal[0] * z;
    out[1] = to->tan[1] * x + to->bitan[1] * y + to->normal[1] * z;
    out[2] = to->tan[2] * x + to->bitan[2] * y + to->normal[2] * z;
}

static void portal_teleport(int srcIdx, const Vec3f at) {
    struct Portal *src = &s_portal[srcIdx];
    struct Portal *dst = &s_portal[1 - srcIdx];
    Vec3f rel, newCom, vel, newVel;
    float height = gMarioState->marioObj != NULL ? gMarioState->marioObj->hitboxHeight : 160.0f;

    rel[0] = at[0] - src->pos[0];
    rel[1] = at[1] - src->pos[1];
    rel[2] = at[2] - src->pos[2];

    float x = vdot3(rel, src->tan);
    float y = vdot3(rel, src->bitan);
    float z = vdot3(rel, src->normal);
    // Tangential offset is rotated 180 degrees (left/right swap).  The
    // normal offset stays positive: entering in front of A must emerge in
    // front of B, otherwise a trigger at 40-70 units would place Mario
    // behind the destination wall.
    x = -x;

    // The normal offset is the entry trigger depth (up to PORTAL_ENTER_DIST).
    // Carrying it into the exit position makes the emerge point depend on
    // which tick the trigger fired, which is very noisy for fast falls.  Keep
    // the tangential offset but place Mario right at the exit surface.
    (void)z;

    // Push out of the exit portal so we do not immediately re-enter it.
    newCom[0] = dst->pos[0] + dst->tan[0] * x + dst->bitan[0] * y + dst->normal[0] * PORTAL_EXIT_PUSH;
    newCom[1] = dst->pos[1] + dst->tan[1] * x + dst->bitan[1] * y + dst->normal[1] * PORTAL_EXIT_PUSH;
    newCom[2] = dst->pos[2] + dst->tan[2] * x + dst->bitan[2] * y + dst->normal[2] * PORTAL_EXIT_PUSH;

    gMarioState->pos[0] = newCom[0];
    gMarioState->pos[1] = newCom[1] - height * 0.5f;
    gMarioState->pos[2] = newCom[2];
    if (gMarioState->marioObj != NULL) {
        vec3f_copy(gMarioState->marioObj->header.gfx.pos, gMarioState->pos);
        gMarioState->marioObj->oPosX = gMarioState->pos[0];
        gMarioState->marioObj->oPosY = gMarioState->pos[1];
        gMarioState->marioObj->oPosZ = gMarioState->pos[2];
    }

    // Momentum: rotate the velocity through the same portal transform.
    vel[0] = gMarioState->vel[0];
    vel[1] = gMarioState->vel[1];
    vel[2] = gMarioState->vel[2];
    portal_rotate_vec(src, dst, vel, newVel);
    vec3f_copy(gMarioState->vel, newVel);

    // This fork rebuilds the horizontal air velocity from forwardVel every
    // tick (update_air_without_turn), which would erase a portal launch.
    // Store the rotated momentum in forwardVel, then face out of the exit
    // surface: the normal points away from the wall/floor the portal sits on.
    {
        f32 hsp = sqrtf(newVel[0] * newVel[0] + newVel[2] * newVel[2]);
        f32 nh = sqrtf(dst->normal[0] * dst->normal[0]
                     + dst->normal[2] * dst->normal[2]);
        s16 exitYaw;

        gMarioState->forwardVel = hsp;
        if (nh > 0.1f) {
            /* Wall / ramp: face the way the destination portal points. */
            exitYaw = atan2s(dst->normal[2], dst->normal[0]);
        } else if (hsp > 1.0f) {
            /* Floor/ceiling: the normal is vertical; use the exit velocity. */
            exitYaw = atan2s(newVel[2], newVel[0]);
        } else {
            exitYaw = gMarioState->faceAngle[1];
        }
        gMarioState->faceAngle[1] = exitYaw;
        gMarioState->marioObj->oMoveAngleYaw = exitYaw;
    }

    // Re-aim the camera at the new position, but ease the rendered handoff so
    // the view does not cut abruptly (see camera_portal_blend_step()).
    camera_portal_blend_begin();
    gCameraMovementFlags |= CAM_MOVE_INIT_CAMERA;
    s_cooldown = PORTAL_COOLDOWN;
    s_havePrevCom = 0;
    s_lastTeleportSrc = srcIdx;
    s_lastTeleportDst = 1 - srcIdx;
    s_teleportCount++;
    play_sound(SOUND_MENU_CAMERA_ZOOM_IN, gDefaultSoundArgs);
}

static int portal_point_inside(const struct Portal *p, const Vec3f point) {
    Vec3f rel;
    rel[0] = point[0] - p->pos[0];
    rel[1] = point[1] - p->pos[1];
    rel[2] = point[2] - p->pos[2];
    float x = vdot3(rel, p->tan) / p->rx;
    float y = vdot3(rel, p->bitan) / p->ry;
    return (x * x + y * y) <= 1.0f;
}

void portal_update(void) {
    static int lastArea = -1;
    int area = gCurrLevelNum * 100 + gCurrAreaIndex;

    if (area != lastArea) {
        lastArea = area;
        portal_clear();
        s_gunSpawned = 0;
        s_ticksInArea = 0;
    }
    if (s_ticksInArea < 1000) {
        s_ticksInArea++;
    }
    if (s_cooldown > 0) {
        s_cooldown--;
    }

    s_gunSpin += 0x0300;

    // Gun pickup: spawn the prop next to Mario in the castle grounds.
    // Wait for the warp into the area to finish (Mario gets a valid object and
    // position a few ticks after the level loads), otherwise the gun is placed
    // at the old origin and never seen.
    if (!s_hasGun && !s_gunSpawned && gCurrLevelNum == 16 && gCurrAreaIndex == 1
        && gMarioState != NULL && gMarioState->marioObj != NULL
        && s_ticksInArea > 8
        && (gMarioState->pos[0] != 0.0f || gMarioState->pos[2] != 0.0f)) {
        vec3f_copy(s_gunPos, gMarioState->pos);
        s_gunPos[0] += 160.0f;
        s_gunPos[2] -= 160.0f;
        s_gunPos[1] += 30.0f;
        s_gunSpawned = 1;
        printf("portal: gun spawned at %.0f %.0f %.0f (mario %.0f %.0f %.0f)\n",
               s_gunPos[0], s_gunPos[1], s_gunPos[2],
               gMarioState->pos[0], gMarioState->pos[1], gMarioState->pos[2]);
    }
    if (!s_hasGun && s_gunSpawned && gMarioState != NULL) {
        Vec3f rel;
        rel[0] = gMarioState->pos[0] - s_gunPos[0];
        rel[1] = (gMarioState->pos[1] + 60.0f) - s_gunPos[1];
        rel[2] = gMarioState->pos[2] - s_gunPos[2];
        if (vdot3(rel, rel) < PORTAL_PICKUP_R * PORTAL_PICKUP_R) {
            s_hasGun = 1;
            s_gunSpawned = 0;
            printf("portal: gun picked up (mario %.0f %.0f %.0f)\n",
                   gMarioState->pos[0], gMarioState->pos[1], gMarioState->pos[2]);
            play_sound(SOUND_MENU_CHANGE_SELECT, gDefaultSoundArgs);
        }
    }

    if (gMarioState == NULL || gMarioState->marioObj == NULL) {
        return;
    }

    {
        Vec3f com;
        float height = gMarioState->marioObj->hitboxHeight;
        com[0] = gMarioState->pos[0];
        com[1] = gMarioState->pos[1] + height * 0.5f;
        com[2] = gMarioState->pos[2];

        // Proximity trigger, not a plane crossing: solid walls stop Mario a
        // body-radius short of the portal, so his centre never crosses the
        // plane. Entering means "inside the ellipse and close to the plane".
        if (s_portal[0].active && s_portal[1].active && s_cooldown == 0) {
            for (int i = 0; i < 2; i++) {
                struct Portal *p = &s_portal[i];
                float d = vdot3(com, p->normal) - vdot3(p->pos, p->normal);
                if (d > 0.0f && d < PORTAL_ENTER_DIST && portal_point_inside(p, com)) {
                    portal_teleport(i, com);
                    break;
                }
            }
        }
        vec3f_copy(s_prevCom, com);
        s_havePrevCom = 1;
    }
}

/* -------------------------------------------------------------- rendering */

static void emit_model_matrix(const Vec3f pos, const Vec3f tan, const Vec3f bitan,
                              const Vec3f n, const Mat4 view) {
    float model[4][4];
    float mv[4][4];
    Mtx *mtx = alloc_display_list(sizeof(*mtx));

    model[0][0] = tan[0];  model[0][1] = tan[1];  model[0][2] = tan[2];  model[0][3] = 0.0f;
    model[1][0] = bitan[0];model[1][1] = bitan[1];model[1][2] = bitan[2];model[1][3] = 0.0f;
    model[2][0] = n[0];    model[2][1] = n[1];    model[2][2] = n[2];    model[2][3] = 0.0f;
    model[3][0] = pos[0];  model[3][1] = pos[1];  model[3][2] = pos[2];  model[3][3] = 1.0f;

    mat4_mul_rm(mv, model, view);
    mtxf_to_mtx(mtx, mv);
    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(mtx),
              G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
}

static void fill_vtx(Vtx *v, float x, float y, float z, u8 r, u8 g, u8 b, u8 a) {
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

static void draw_portal(const struct Portal *p, u8 r, u8 g, u8 b, const Mat4 view) {
    Vtx *v = alloc_display_list(sizeof(Vtx) * (PORTAL_SEGMENTS + 2));

    if (v == NULL) {
        return;
    }

    emit_model_matrix(p->pos, p->tan, p->bitan, p->normal, view);

    fill_vtx(&v[0], 0.0f, 0.0f, 0.0f, r, g, b, 0xB8);
    for (int i = 0; i <= PORTAL_SEGMENTS; i++) {
        float a = (float)i * (2.0f * 3.14159265f) / (float)PORTAL_SEGMENTS;
        fill_vtx(&v[i + 1], cosf(a) * p->rx, sinf(a) * p->ry, 0.0f, r, g, b, 0xC8);
    }

    gSPVertex(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(v), PORTAL_SEGMENTS + 2, 0);
    for (int i = 0; i < PORTAL_SEGMENTS; i++) {
        gSP1Triangle(gDisplayListHead++, 0, i + 1, i + 2, 0);
    }
}

static void draw_gun(const Mat4 view) {
    Vec3f tan, bitan, up;
    s16 yaw = (s16) s_gunSpin;

    // Spin around world Y; the barrel (local +Z) points along bitan.
    tan[0] = coss(yaw);
    tan[1] = 0.0f;
    tan[2] = -sins(yaw);
    up[0] = 0.0f;
    up[1] = 1.0f;
    up[2] = 0.0f;
    vcross3(bitan, up, tan);

    emit_model_matrix(s_gunPos, tan, up, bitan, view);
    gSPDisplayList(gDisplayListHead++, (Gfx *) ptc_gun_dl);
}

static void draw_held_gun(const Mat4 view) {
    struct MarioState *m = gMarioState;
    Vec3f right, fwd, up, pos;
    s16 yaw;

    if (!s_hasGun || m == NULL || m->marioObj == NULL) {
        return;
    }

    /* Keep the prop in Mario's right hand: follow the interpolated render
     * position/angle so it does not jitter against the 60 Hz model. */
    pos[0] = m->pos[0];
    pos[1] = m->pos[1];
    pos[2] = m->pos[2];
    yaw = m->faceAngle[1];

    right[0] = coss(yaw);
    right[1] = 0.0f;
    right[2] = -sins(yaw);
    fwd[0] = sins(yaw);
    fwd[1] = 0.0f;
    fwd[2] = coss(yaw);
    up[0] = 0.0f;
    up[1] = 1.0f;
    up[2] = 0.0f;

    pos[0] += right[0] * 26.0f + fwd[0] * 8.0f;
    pos[1] += 92.0f;
    pos[2] += right[2] * 26.0f + fwd[2] * 8.0f;

    emit_model_matrix(pos, right, up, fwd, view);
    gSPDisplayList(gDisplayListHead++, ptc_gun_dl);
}

void portal_view_toggle(void) {
    s_portal_view_enabled = !s_portal_view_enabled;
    if (!s_portal_view_enabled) {
        g_portal_view.active = 0;
    }
}

void portal_view_force(int src) {
    s_forceView = (src < 0) ? -1 : (src & 1);
}

/* Column-vector local -> world: columns are the portal axes, column 3 is the
 * origin, matching mtxf_lookat()'s convention. */
static void mat4_local_to_world(float out[4][4], const Vec3f pos, const Vec3f tan,
                                const Vec3f bitan, const Vec3f n) {
    out[0][0] = tan[0];   out[0][1] = bitan[0]; out[0][2] = n[0];     out[0][3] = pos[0];
    out[1][0] = tan[1];   out[1][1] = bitan[1]; out[1][2] = n[1];     out[1][3] = pos[1];
    out[2][0] = tan[2];   out[2][1] = bitan[2]; out[2][2] = n[2];     out[2][3] = pos[2];
    out[3][0] = 0.0f;     out[3][1] = 0.0f;     out[3][2] = 0.0f;     out[3][3] = 1.0f;
}

static void mat4_world_to_local(float out[4][4], const Vec3f pos, const Vec3f tan,
                                const Vec3f bitan, const Vec3f n) {
    out[0][0] = tan[0];   out[0][1] = tan[1];   out[0][2] = tan[2];   out[0][3] = -vdot3(pos, tan);
    out[1][0] = bitan[0]; out[1][1] = bitan[1]; out[1][2] = bitan[2]; out[1][3] = -vdot3(pos, bitan);
    out[2][0] = n[0];     out[2][1] = n[1];     out[2][2] = n[2];     out[2][3] = -vdot3(pos, n);
    out[3][0] = 0.0f;     out[3][1] = 0.0f;     out[3][2] = 0.0f;     out[3][3] = 1.0f;
}

/* Build the camera-space portal transform and the source fan geometry. */
static void portal_view_update(const Mat4 view) {
    struct Portal *a, *b, *src, *dst;
    int i, best = -1;

    g_portal_view.active = 0;
    if (!s_portal_view_enabled || !s_portal[0].active || !s_portal[1].active) {
        return;
    }

    /* Pick the portal the camera is actually looking at: it must be in front
     * of the camera, its front side must face the camera, and its centre must
     * be closer to the view axis than the other portal's.  Requiring the
     * camera-to-focus ray to pass through the ellipse (the previous test)
     * dropped the through-view whenever the portal sat slightly off-centre,
     * which is most of the time. */
    if (s_forceView >= 0) {
        best = s_forceView;
        if (!s_portal[best].active) best = -1;
    } else {
        Vec3f look, rel;
        float lookLen, bestScore = 1e30f;
        float maxdeg = g_ptc_select_maxdeg;
        float min_cos = cosf(maxdeg * 3.14159265f / 180.0f);
        if (min_cos < 0.0f) min_cos = 0.0f;
        vec3f_copy(look, gLakituState.focus);
        vec3f_sub(look, gLakituState.pos);
        lookLen = sqrtf(vdot3(look, look));
        if (lookLen > 1e-4f) {
            for (i = 0; i < 2; i++) {
                float along, dist, cosang, score;
                if (!s_portal[i].active) continue;
                rel[0] = s_portal[i].pos[0] - gLakituState.pos[0];
                rel[1] = s_portal[i].pos[1] - gLakituState.pos[1];
                rel[2] = s_portal[i].pos[2] - gLakituState.pos[2];
                along = vdot3(rel, look);
                if (along <= 0.0f) continue;                       /* behind */
                if (g_ptc_select_facing
                    && vdot3(rel, s_portal[i].normal) >= 0.0f) continue; /* back side */
                dist = sqrtf(vdot3(rel, rel));
                if (dist < 1e-3f) continue;
                cosang = along / (dist * lookLen);
                if (cosang < min_cos) continue;                    /* too far off-axis */
                score = 1.0f - cosang;
                if (score < bestScore) {
                    bestScore = score;
                    best = i;
                }
            }
        }
    }
    if (best < 0) {
        return;
    }

    src = &s_portal[best];
    dst = &s_portal[1 - best];
    a = src;
    b = dst;

    {
        float tsrc[4][4], tsrc_inv[4][4], tdst[4][4], tdst_inv[4][4], r[4][4];
        float m1[4][4], m[4][4], c[4][4];
        int i2, j2;

        mat4_local_to_world(tsrc, a->pos, a->tan, a->bitan, a->normal);
        mat4_world_to_local(tsrc_inv, a->pos, a->tan, a->bitan, a->normal);
        mat4_local_to_world(tdst, b->pos, b->tan, b->bitan, b->normal);
        mat4_world_to_local(tdst_inv, b->pos, b->tan, b->bitan, b->normal);

        memset(r, 0, sizeof(r));
        r[0][0] = -1.0f;
        r[1][1] = 1.0f;
        r[2][2] = -1.0f;
        r[3][3] = 1.0f;

        /* Relay transform, column-vector convention.  The virtual camera is
         * the real camera mapped through the portal pair, so the *scene* is
         * rendered with the inverse: M^-1 = localToWorld(src) * R *
         * worldToLocal(dst).  The renderer stores matrices transposed (row
         * vectors: v' = v * M), so the camera-space product is the transpose
         * of M^-1.  Mixing the two conventions (the old view * M * view^-1)
         * put the relay camera at a garbage position. */
        mat4_mul_rm(m, tsrc, r);
        mat4_mul_rm(m1, m, tdst_inv);
        for (i2 = 0; i2 < 4; i2++) {
            for (j2 = 0; j2 < 4; j2++) {
                c[i2][j2] = m1[j2][i2];
            }
        }

        memcpy(g_portal_view.c_matrix, c, sizeof(c));

        /* Debug: where the mirror actually puts the camera.  `want` is
         * M * camPos (column-vector maths); `got` is the eye implied by the
         * C * modelview product the renderer actually uses. */
        {
            static int s_dbg;
            if ((s_dbg++ % 60) == 0) {
                float cp[4] = { gLakituState.pos[0], gLakituState.pos[1],
                                gLakituState.pos[2], 1.0f };
                float mf[4][4], mi[4][4], want[4];
                mat4_mul_rm(mf, tdst, r);
                mat4_mul_rm(mi, mf, tsrc_inv);
                for (int i = 0; i < 4; i++) {
                    want[i] = mi[i][0] * cp[0] + mi[i][1] * cp[1]
                            + mi[i][2] * cp[2] + mi[i][3] * cp[3];
                }
                /* effective view = C * V (row-vector multiply) */
                float ev[4][4];
                for (int i = 0; i < 4; i++) {
                    for (int j = 0; j < 4; j++) {
                        ev[i][j] = c[i][0] * view[0][j] + c[i][1] * view[1][j]
                                 + c[i][2] * view[2][j] + c[i][3] * view[3][j];
                    }
                }
                float got[3] = { 0.0f, 0.0f, 0.0f };
                for (int j = 0; j < 3; j++) {
                    got[j] = -(ev[3][0] * ev[j][0] + ev[3][1] * ev[j][1] + ev[3][2] * ev[j][2]);
                }
                __android_log_print(ANDROID_LOG_INFO, "SM64PORTAL",
                    "mirror cam=(%.1f,%.1f,%.1f) want=(%.1f,%.1f,%.1f) got=(%.1f,%.1f,%.1f) src=%d",
                    gLakituState.pos[0], gLakituState.pos[1], gLakituState.pos[2],
                    want[0], want[1], want[2], got[0], got[1], got[2], best);
            }
        }
    }

    memcpy(g_portal_view.view, view, sizeof(Mat4));
    g_portal_view.source = best;
    vec3f_copy(g_portal_view.normal, a->normal);
    g_portal_view.decal_offset = g_ptc_decal_offset;
    g_portal_view.count = PORTAL_VIEW_SEGS;
    vec3f_copy(g_portal_view.center, a->pos);
    for (i = 0; i < PORTAL_VIEW_SEGS; i++) {
        float ang = (float)i * (2.0f * 3.14159265f) / (float)PORTAL_VIEW_SEGS;
        g_portal_view.rim[i][0] = a->pos[0] + a->tan[0] * cosf(ang) * a->rx
                                           + a->bitan[0] * sinf(ang) * a->ry;
        g_portal_view.rim[i][1] = a->pos[1] + a->tan[1] * cosf(ang) * a->rx
                                           + a->bitan[1] * sinf(ang) * a->ry;
        g_portal_view.rim[i][2] = a->pos[2] + a->tan[2] * cosf(ang) * a->rx
                                           + a->bitan[2] * sinf(ang) * a->ry;
    }
    g_portal_view.active = 1;
}

static void portal_draw_reticle(const Mat4 view);

void portal_render(void) {
    Mat4 view;

    if (gCurrentArea == NULL) {
        return;
    }
    if (!s_portal[0].active && !s_portal[1].active && !s_gunSpawned && !s_hasGun) {
        g_portal_view.active = 0;
        return;
    }

    mtxf_lookat(view, gLakituState.pos, gLakituState.focus, 0);
    portal_view_update(view);

    gSPSetGeometryMode(gDisplayListHead++, G_ZBUFFER);
    gSPClearGeometryMode(gDisplayListHead++, G_LIGHTING);
    /* Portals are visible from both sides (e.g. the back of a floor portal). */
    gSPClearGeometryMode(gDisplayListHead++, G_CULL_FRONT | G_CULL_BACK);
    gDPSetCombineMode(gDisplayListHead++, G_CC_SHADE, G_CC_SHADE);
    gDPSetRenderMode(gDisplayListHead++, G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2);

    /* The source portal is drawn by the renderer's textured overlay when the
     * through-view is active; keep the flat fan as a fallback otherwise.
     * Draw the most recently placed portal last: two shots at the same aim
     * overlap exactly, and the newest one must not be hidden underneath. */
    for (int k = 0; k < 2; k++) {
        int i = (k == 0) ? (1 - s_lastPlaced) : s_lastPlaced;
        if (!s_portal[i].active) continue;
        if (g_portal_view.active && g_portal_view.source == i && !g_portal_debug_align) continue;
        if (i == 0) {
            if (g_portal_view.active && g_portal_view.source == i)
                draw_portal(&s_portal[0], 0xFF, 0x00, 0xFF, view);  // magenta alignment
            else
                draw_portal(&s_portal[0], 0x30, 0x60, 0xFF, view);  // blue
        } else {
            if (g_portal_view.active && g_portal_view.source == i)
                draw_portal(&s_portal[1], 0xFF, 0x00, 0xFF, view);
            else
                draw_portal(&s_portal[1], 0xFF, 0x80, 0x20, view);  // orange
        }
    }
    if (s_gunSpawned) {
        draw_gun(view);
    }
    draw_held_gun(view);
    portal_draw_reticle(view);

    gSPSetGeometryMode(gDisplayListHead++, G_CULL_BACK);
    gSPClearGeometryMode(gDisplayListHead++, G_ZBUFFER);
}

/* ------------------------------------------------------- aiming reticle */

/* Raycast for the reticle.  The ray is nudged up by the same amount the
 * crosshair sits above screen centre.  Fills `out` with the hit point. */
static int portal_reticle_hit(Vec3f out) {
    Vec3f orig, fwd, right, up, dir, hit;
    struct Surface *surf = NULL;
    float len;

    if (gMarioState == NULL) {
        return 0;
    }
    vec3f_copy(orig, gLakituState.pos);
    vec3f_copy(fwd, gLakituState.focus);
    vec3f_sub(fwd, orig);
    len = sqrtf(vdot3(fwd, fwd));
    if (len < 0.001f) {
        return 0;
    }
    fwd[0] /= len;
    fwd[1] /= len;
    fwd[2] /= len;

    up[0] = 0.0f; up[1] = 1.0f; up[2] = 0.0f;
    vcross3(right, fwd, up);
    if (vdot3(right, right) < 0.001f) {
        right[0] = 1.0f; right[1] = 0.0f; right[2] = 0.0f;
    }
    vnorm3(right);
    vcross3(up, right, fwd);
    vnorm3(up);

    dir[0] = fwd[0] + up[0] * 0.055f;
    dir[1] = fwd[1] + up[1] * 0.055f;
    dir[2] = fwd[2] + up[2] * 0.055f;
    vnorm3(dir);
    dir[0] *= PORTAL_RAY_DIST;
    dir[1] *= PORTAL_RAY_DIST;
    dir[2] *= PORTAL_RAY_DIST;

    find_surface_on_ray(orig, dir, &surf, hit);
    if (surf == NULL) {
        return 0;
    }
    vec3f_copy(out, hit);
    return 1;
}

/* World-space crosshair at the actual aim-ray hit point: true per-eye depth
 * (no UI-depth slider), small and thin.  Sized in world units proportional to
 * distance so it stays roughly constant on screen. */
static void portal_draw_reticle(const Mat4 view) {
    Vec3f hit, fwd, right, up, pos;
    float d, arm, thick, inner;
    Vtx *v;
    int b;

    if (!s_hasGun || gMarioState == NULL || !portal_reticle_hit(hit)) {
        return;
    }

    vec3f_copy(fwd, gLakituState.focus);
    vec3f_sub(fwd, gLakituState.pos);
    if (vdot3(fwd, fwd) < 0.001f) {
        return;
    }
    vnorm3(fwd);
    up[0] = 0.0f; up[1] = 1.0f; up[2] = 0.0f;
    vcross3(right, fwd, up);
    if (vdot3(right, right) < 0.001f) {
        right[0] = 1.0f; right[1] = 0.0f; right[2] = 0.0f;
    }
    vnorm3(right);
    vcross3(up, right, fwd);
    vnorm3(up);

    d = sqrtf((hit[0] - gLakituState.pos[0]) * (hit[0] - gLakituState.pos[0])
            + (hit[1] - gLakituState.pos[1]) * (hit[1] - gLakituState.pos[1])
            + (hit[2] - gLakituState.pos[2]) * (hit[2] - gLakituState.pos[2]));
    if (d < 80.0f) d = 80.0f;
    arm = d * 0.0040f;
    thick = d * 0.0012f;
    inner = arm * 0.35f;

    /* Pull a couple of units towards the camera so it does not z-fight. */
    pos[0] = hit[0] - fwd[0] * 2.5f;
    pos[1] = hit[1] - fwd[1] * 2.5f;
    pos[2] = hit[2] - fwd[2] * 2.5f;

    v = alloc_display_list(sizeof(Vtx) * 16);
    if (v == NULL) {
        return;
    }
    b = 0;
    /* left */
    fill_vtx(&v[b++], -arm, -thick, 0, 255, 255, 255, 210);
    fill_vtx(&v[b++], -inner, -thick, 0, 255, 255, 255, 210);
    fill_vtx(&v[b++], -inner, thick, 0, 255, 255, 255, 210);
    fill_vtx(&v[b++], -arm, thick, 0, 255, 255, 255, 210);
    /* right */
    fill_vtx(&v[b++], inner, -thick, 0, 255, 255, 255, 210);
    fill_vtx(&v[b++], arm, -thick, 0, 255, 255, 255, 210);
    fill_vtx(&v[b++], arm, thick, 0, 255, 255, 255, 210);
    fill_vtx(&v[b++], inner, thick, 0, 255, 255, 255, 210);
    /* top */
    fill_vtx(&v[b++], -thick, inner, 0, 255, 255, 255, 210);
    fill_vtx(&v[b++], thick, inner, 0, 255, 255, 255, 210);
    fill_vtx(&v[b++], thick, arm, 0, 255, 255, 255, 210);
    fill_vtx(&v[b++], -thick, arm, 0, 255, 255, 255, 210);
    /* bottom */
    fill_vtx(&v[b++], -thick, -arm, 0, 255, 255, 255, 210);
    fill_vtx(&v[b++], thick, -arm, 0, 255, 255, 255, 210);
    fill_vtx(&v[b++], thick, -inner, 0, 255, 255, 255, 210);
    fill_vtx(&v[b++], -thick, -inner, 0, 255, 255, 255, 210);

    emit_model_matrix(pos, right, up, fwd, view);

    gSPSetGeometryMode(gDisplayListHead++, G_ZBUFFER | G_SHADE | G_CULL_BACK);
    gSPClearGeometryMode(gDisplayListHead++, G_LIGHTING);
    gDPSetCombineMode(gDisplayListHead++, G_CC_SHADE, G_CC_SHADE);
    gDPSetRenderMode(gDisplayListHead++, G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2);
    gSPVertex(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(v), 16, 0);
    for (int q = 0; q < 4; q++) {
        int vb = q * 4;
        gSP1Triangle(gDisplayListHead++, vb, vb + 1, vb + 2, 0);
        gSP1Triangle(gDisplayListHead++, vb, vb + 2, vb + 3, 0);
    }
    gDPPipeSync(gDisplayListHead++);
    gDPSetRenderMode(gDisplayListHead++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
    gDPSetCombineMode(gDisplayListHead++, G_CC_MODULATERGB, G_CC_MODULATERGB);
    gSPSetGeometryMode(gDisplayListHead++, G_LIGHTING);
}

/* UI reticle retired: it could only fake depth with a parallax shift. */
float g_portal_reticle_depth_px = 0.0f;
int portal_render_reticle(void) {
    g_portal_reticle_depth_px = 0.0f;
    return 0;
}

extern int g_ptc_aspect_calib;
extern float gfx_cam_tan_half_fov_y;
extern float gfx_cam_near, gfx_cam_far;

/* Camera-facing magenta square (160x160 world units at 400u) used to verify
 * that the 3D projection keeps square pixels.  Toggled by `ptc aspecttest`. */
void portal_render_aspect_calib(void) {
    Vec3f fwd, right, up, pos;
    Mat4 view;
    Vtx *v;

    if (!g_ptc_aspect_calib || gMarioState == NULL) {
        return;
    }
    vec3f_copy(fwd, gLakituState.focus);
    vec3f_sub(fwd, gLakituState.pos);
    if (vdot3(fwd, fwd) < 0.001f) {
        return;
    }
    vnorm3(fwd);
    up[0] = 0.0f; up[1] = 1.0f; up[2] = 0.0f;
    vcross3(right, fwd, up);
    if (vdot3(right, right) < 0.001f) {
        right[0] = 1.0f; right[1] = 0.0f; right[2] = 0.0f;
    }
    vnorm3(right);
    vcross3(up, right, fwd);
    vnorm3(up);

    pos[0] = gLakituState.pos[0] + fwd[0] * 400.0f;
    pos[1] = gLakituState.pos[1] + fwd[1] * 400.0f;
    pos[2] = gLakituState.pos[2] + fwd[2] * 400.0f;

    v = alloc_display_list(sizeof(Vtx) * 4);
    if (v == NULL) {
        return;
    }
    fill_vtx(&v[0], -80.0f, -80.0f, 0.0f, 255, 0, 255, 255);
    fill_vtx(&v[1],  80.0f, -80.0f, 0.0f, 255, 0, 255, 255);
    fill_vtx(&v[2],  80.0f,  80.0f, 0.0f, 255, 0, 255, 255);
    fill_vtx(&v[3], -80.0f,  80.0f, 0.0f, 255, 0, 255, 255);

    mtxf_lookat(view, gLakituState.pos, gLakituState.focus, 0);
    emit_model_matrix(pos, right, up, fwd, view);

    /* The scene may have left an ortho (skybox/HUD) projection active.
     * Load the real camera perspective explicitly so the square is measured
     * through the same projection as the world geometry. */
    {
        Mtx *proj = alloc_display_list(sizeof(Mtx));
        u16 norm = 0;
        f32 fov = 45.0f;
        if (proj != NULL) {
            if (gfx_cam_tan_half_fov_y > 0.01f) {
                fov = 2.0f * atanf(gfx_cam_tan_half_fov_y) * 180.0f / 3.14159265f;
            }
            guPerspective(proj, &norm, fov, gfx_current_dimensions.aspect_ratio,
                          gfx_cam_near > 0.0f ? gfx_cam_near : 10.0f,
                          gfx_cam_far > 0.0f ? gfx_cam_far : 20000.0f, 1.0f);
            gSPPerspNormalize(gDisplayListHead++, norm);
            gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(proj),
                      G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
        }
    }

    gSPSetGeometryMode(gDisplayListHead++, G_ZBUFFER | G_SHADE | G_CULL_BACK);
    gSPClearGeometryMode(gDisplayListHead++, G_LIGHTING);
    gDPSetCombineMode(gDisplayListHead++, G_CC_SHADE, G_CC_SHADE);
    gDPSetRenderMode(gDisplayListHead++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
    gSPVertex(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(v), 4, 0);
    gSP1Triangle(gDisplayListHead++, 0, 1, 2, 0);
    gSP1Triangle(gDisplayListHead++, 0, 2, 3, 0);
    gSPClearGeometryMode(gDisplayListHead++, G_ZBUFFER);
}

int portal_test_walls(void) {
    static const Vec3f wallOrig[4] = {
        { 0.0f, 400.0f, -1500.0f },
        { 0.0f, 400.0f,   600.0f },
        {-600.0f, 400.0f,  -700.0f },
        { 600.0f, 400.0f,  -700.0f },
    };
    static const Vec3f wallTarget[4] = {
        { 0.0f, 400.0f, -2599.0f },
        { 0.0f, 400.0f,  1249.0f },
        {-1199.0f, 400.0f, -700.0f },
        { 2449.0f, 400.0f, -700.0f },
    };
    int mask = 0;
    int hadGun = s_hasGun;

    s_hasGun = 1;
    for (int i = 0; i < 4; i++) {
        portal_reset();
        s_hasGun = 1;
        if (portal_shoot_at(0, wallOrig[i], wallTarget[i]) && s_portal[0].active) {
            mask |= 1 << i;
        }
    }
    portal_reset();
    s_hasGun = hadGun;
    return mask;
}

void portal_give_gun(void) {
    s_hasGun = 1;
    s_gunSpawned = 0;
}

void portal_drop_gun(void) {
    s_hasGun = 0;
    s_gunSpawned = 0;
    portal_clear();
    play_sound(SOUND_MENU_CHANGE_SELECT, gDefaultSoundArgs);
}

int gfx_portal_field(int which, int field) {
    if (which < 0 || which > 1 || field < 0 || field > 5) {
        return 0;
    }
    if (field < 3) {
        return (int) s_portal[which].pos[field];
    }
    return (int) (s_portal[which].normal[field - 3] * 1000.0f);
}

int gfx_mario_pos(int axis) {
    if (gMarioState == NULL || axis < 0 || axis > 2) {
        return 0;
    }
    return (int) gMarioState->pos[axis];
}

int portal_gun_has(void) {
    return s_hasGun;
}

int portal_count(void) {
    return (s_portal[0].active ? 1 : 0) + (s_portal[1].active ? 1 : 0);
}

int portal_get(int which, Vec3f pos, Vec3f normal) {
    if (which < 0 || which > 1 || !s_portal[which].active) {
        return 0;
    }
    vec3f_copy(pos, s_portal[which].pos);
    vec3f_copy(normal, s_portal[which].normal);
    return 1;
}

int portal_last_src(void) {
    return s_lastTeleportSrc;
}

int portal_last_dst(void) {
    return s_lastTeleportDst;
}

int portal_teleport_count(void) {
    return s_teleportCount;
}
