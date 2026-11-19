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
PortalViewState g_portal_view;
static int s_portal_view_enabled = 1;
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

    vec3f_copy(centre, hit);
    // Nudge the portal off the surface a little so it does not z-fight.
    centre[0] += n[0] * 1.5f;
    centre[1] += n[1] * 1.5f;
    centre[2] += n[2] * 1.5f;

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
                centre[0] += n[0] * 1.5f;
                centre[1] += n[1] * 1.5f;
                centre[2] += n[2] * 1.5f;
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

    // Facing: turn by the difference of the two portals' tangents.
    {
        s16 fromYaw = atan2s(src->tan[2], src->tan[0]);
        s16 toYaw = atan2s(dst->tan[2], dst->tan[0]);
        s16 delta = (s16)(toYaw - fromYaw) + (s16)0x8000;
        gMarioState->faceAngle[1] = (s16)(gMarioState->faceAngle[1] + delta);
        gMarioState->marioObj->oMoveAngleYaw = gMarioState->faceAngle[1];
    }

    // This fork rebuilds the horizontal air velocity from forwardVel every
    // tick (update_air_without_turn), which would erase a portal launch.
    // Store the rotated momentum in forwardVel and face the exit direction so
    // the airborne step reproduces the same trajectory.
    {
        f32 hsp = sqrtf(newVel[0] * newVel[0] + newVel[2] * newVel[2]);
        gMarioState->forwardVel = hsp;
        if (hsp > 1.0f) {
            // SM64 yaw is measured from +Z: direction (sins, coss), so the
            // yaw for a world velocity is atan2s(vz, vx).
            gMarioState->faceAngle[1] = atan2s(newVel[2], newVel[0]);
            gMarioState->marioObj->oMoveAngleYaw = gMarioState->faceAngle[1];
        }
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

static void mat4_local_to_world(float out[4][4], const Vec3f pos, const Vec3f tan,
                                const Vec3f bitan, const Vec3f n) {
    out[0][0] = tan[0];   out[0][1] = tan[1];   out[0][2] = tan[2];   out[0][3] = 0.0f;
    out[1][0] = bitan[0]; out[1][1] = bitan[1]; out[1][2] = bitan[2]; out[1][3] = 0.0f;
    out[2][0] = n[0];     out[2][1] = n[1];     out[2][2] = n[2];     out[2][3] = 0.0f;
    out[3][0] = pos[0];   out[3][1] = pos[1];   out[3][2] = pos[2];   out[3][3] = 1.0f;
}

static void mat4_world_to_local(float out[4][4], const Vec3f pos, const Vec3f tan,
                                const Vec3f bitan, const Vec3f n) {
    out[0][0] = tan[0];   out[0][1] = bitan[0]; out[0][2] = n[0];     out[0][3] = 0.0f;
    out[1][0] = tan[1];   out[1][1] = bitan[1]; out[1][2] = n[1];     out[1][3] = 0.0f;
    out[2][0] = tan[2];   out[2][1] = bitan[2]; out[2][2] = n[2];     out[2][3] = 0.0f;
    out[3][0] = -vdot3(pos, tan);
    out[3][1] = -vdot3(pos, bitan);
    out[3][2] = -vdot3(pos, n);
    out[3][3] = 1.0f;
}

/* Build the camera-space portal transform and the source fan geometry. */
static void portal_view_update(const Mat4 view) {
    struct Portal *a, *b, *src, *dst;
    Vec3f toCam;
    float d[2];
    int i, best = -1;

    g_portal_view.active = 0;
    if (!s_portal_view_enabled || !s_portal[0].active || !s_portal[1].active) {
        return;
    }

    /* Show the portal the camera is actually looking into. */
    d[0] = d[1] = -1e9f;
    for (i = 0; i < 2; i++) {
        if (!s_portal[i].active) continue;
        toCam[0] = gLakituState.pos[0] - s_portal[i].pos[0];
        toCam[1] = gLakituState.pos[1] - s_portal[i].pos[1];
        toCam[2] = gLakituState.pos[2] - s_portal[i].pos[2];
        d[i] = vdot3(toCam, s_portal[i].normal);
        if (d[i] > 0.0f && (best < 0 || d[i] > d[best])) {
            best = i;
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
        float tsrc[4][4], tsrc_inv[4][4], tdst[4][4], r[4][4];
        float vinv[4][4], m1[4][4], m[4][4], c[4][4];
        int x, y;

        mat4_local_to_world(tsrc, a->pos, a->tan, a->bitan, a->normal);
        mat4_world_to_local(tsrc_inv, a->pos, a->tan, a->bitan, a->normal);
        mat4_local_to_world(tdst, b->pos, b->tan, b->bitan, b->normal);
        (void)tsrc;

        memset(r, 0, sizeof(r));
        r[0][0] = -1.0f;
        r[1][1] = 1.0f;
        r[2][2] = -1.0f;
        r[3][3] = 1.0f;

        /* view^-1 (rigid): R^T and -t*R^T. */
        for (y = 0; y < 3; y++) {
            for (x = 0; x < 3; x++) {
                vinv[y][x] = view[x][y];
            }
            vinv[y][3] = 0.0f;
        }
        for (x = 0; x < 3; x++) {
            vinv[3][x] = -(view[3][0] * view[x][0] + view[3][1] * view[x][1]
                         + view[3][2] * view[x][2]);
        }
        vinv[3][3] = 1.0f;

        /* p_dst = p_src * worldToLocal(src) * R * localToWorld(dst) */
        mat4_mul_rm(m1, tsrc_inv, r);
        mat4_mul_rm(m, m1, tdst);
        /* C = view * M * view^-1 */
        mat4_mul_rm(m1, view, m);
        mat4_mul_rm(c, m1, vinv);

        memcpy(g_portal_view.c_matrix, c, sizeof(c));
    }

    memcpy(g_portal_view.view, view, sizeof(Mat4));
    g_portal_view.source = best;
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
    gDPSetCombineMode(gDisplayListHead++, G_CC_SHADE, G_CC_SHADE);
    gDPSetRenderMode(gDisplayListHead++, G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2);

    /* The source portal is drawn by the renderer's textured overlay when the
     * through-view is active; keep the flat fan as a fallback otherwise.
     * Draw the most recently placed portal last: two shots at the same aim
     * overlap exactly, and the newest one must not be hidden underneath. */
    for (int k = 0; k < 2; k++) {
        int i = (k == 0) ? (1 - s_lastPlaced) : s_lastPlaced;
        if (!s_portal[i].active) continue;
        if (g_portal_view.active && g_portal_view.source == i) continue;
        if (i == 0) {
            draw_portal(&s_portal[0], 0x30, 0x60, 0xFF, view);   // blue
        } else {
            draw_portal(&s_portal[1], 0xFF, 0x80, 0x20, view);   // orange
        }
    }
    if (s_gunSpawned) {
        draw_gun(view);
    }
    draw_held_gun(view);

    gSPClearGeometryMode(gDisplayListHead++, G_ZBUFFER);
}

/* ------------------------------------------------------- aiming reticle */

/* The reticle is part of the game UI pass, not the Java overlay: that gives it
 * per-eye stereo (and keeps it crisp after the FX pass).  Its own depth shift
 * comes from the aim-surface distance, so it does not inherit the UI-depth
 * slider. */
float g_portal_reticle_depth_px = 0.0f;

extern float gfx_stereo_ipd_px;
extern float gfx_dof_focus_auto;

ALIGNED8 static const u16 s_reticle_white[16] = {
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
};

/* Distance to the surface behind the reticle.  The ray is nudged up by the
 * same amount the crosshair sits above screen centre. */
static float portal_reticle_distance(void) {
    Vec3f orig, fwd, right, up, dir, hit;
    struct Surface *surf = NULL;
    float len;

    if (gMarioState == NULL) {
        return 700.0f;
    }
    vec3f_copy(orig, gLakituState.pos);
    vec3f_copy(fwd, gLakituState.focus);
    vec3f_sub(fwd, orig);
    len = sqrtf(vdot3(fwd, fwd));
    if (len < 0.001f) {
        return 700.0f;
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

    /* 45%-of-screen-height reticle => ~5% above the centre ray. */
    dir[0] = fwd[0] + up[0] * 0.055f;
    dir[1] = fwd[1] + up[1] * 0.055f;
    dir[2] = fwd[2] + up[2] * 0.055f;
    vnorm3(dir);
    dir[0] *= PORTAL_RAY_DIST;
    dir[1] *= PORTAL_RAY_DIST;
    dir[2] *= PORTAL_RAY_DIST;

    find_surface_on_ray(orig, dir, &surf, hit);
    if (surf == NULL) {
        return 700.0f;
    }
    len = sqrtf((hit[0] - orig[0]) * (hit[0] - orig[0])
              + (hit[1] - orig[1]) * (hit[1] - orig[1])
              + (hit[2] - orig[2]) * (hit[2] - orig[2]));
    return len < 50.0f ? 50.0f : len;
}

static void portal_reticle_rect(s16 x0, s16 y0, s16 x1, s16 y1) {
    gSPTextureRectangle(gDisplayListHead++, x0 << 2, y0 << 2, x1 << 2, y1 << 2,
                        G_TX_RENDERTILE, 0, 0, 4 << 10, 1 << 10);
}

/* Appends the crosshair to the UI display list.  Returns 1 when it drew. */
int portal_render_reticle(void) {
    static const s16 bars[5][4] = {
        { 118, 106, 138, 109 },   /* left  */
        { 182, 106, 202, 109 },   /* right */
        { 159,  86, 161, 106 },   /* up    */
        { 159, 110, 161, 130 },   /* down  */
        { 157, 105, 163, 111 },   /* dot   */
    };
    float dist, half, z;

    if (!s_hasGun || gMarioState == NULL) {
        g_portal_reticle_depth_px = 0.0f;
        return 0;
    }

    dist = portal_reticle_distance();
    half = 0.5f * gfx_stereo_ipd_px;
    /* Positive shift = in front of the display plane.  Relative to the
     * autofocus plane (Mario), a target behind him needs a negative shift. */
    z = half * (gfx_dof_focus_auto / dist - 1.0f);
    if (z > half) z = half;
    if (z < -half) z = -half;
    g_portal_reticle_depth_px = z;

    gDPPipeSync(gDisplayListHead++);
    gDPSetCycleType(gDisplayListHead++, G_CYC_1CYCLE);
    gDPSetAlphaCompare(gDisplayListHead++, G_AC_NONE);
    gDPSetCombineMode(gDisplayListHead++, G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM);
    gDPSetTextureFilter(gDisplayListHead++, G_TF_POINT);
    gDPSetTexturePersp(gDisplayListHead++, G_TP_NONE);
    gDPSetTextureLUT(gDisplayListHead++, G_TT_NONE);
    gDPSetTextureImage(gDisplayListHead++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, s_reticle_white);
    gDPLoadSync(gDisplayListHead++);
    gDPLoadBlock(gDisplayListHead++, G_TX_LOADTILE, 0, 0, 16 - 1,
                 CALC_DXT(4, G_IM_SIZ_16b_BYTES));
    gDPPipeSync(gDisplayListHead++);
    gDPSetTile(gDisplayListHead++, G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, 0,
               G_TX_RENDERTILE, 0, G_TX_CLAMP, 2, G_TX_NOLOD,
               G_TX_CLAMP, 2, G_TX_NOLOD);
    gDPSetTileSize(gDisplayListHead++, G_TX_RENDERTILE, 0, 0,
                   3 << G_TEXTURE_IMAGE_FRAC, 3 << G_TEXTURE_IMAGE_FRAC);
    gDPSetPrimColor(gDisplayListHead++, 0, 0, 255, 255, 255, 220);
    gDPSetRenderMode(gDisplayListHead++, G_RM_AA_XLU_SURF, G_RM_AA_XLU_SURF2);

    for (int i = 0; i < 5; i++) {
        portal_reticle_rect(bars[i][0], bars[i][1], bars[i][2], bars[i][3]);
    }

    gDPPipeSync(gDisplayListHead++);
    gDPSetRenderMode(gDisplayListHead++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);
    gDPSetCombineMode(gDisplayListHead++, G_CC_MODULATERGB, G_CC_MODULATERGB);
    gDPSetTexturePersp(gDisplayListHead++, G_TP_PERSP);
    gDPSetPrimColor(gDisplayListHead++, 0, 0, 255, 255, 255, 255);
    return 1;
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
        { 0.0f, 400.0f,  1199.0f },
        {-1199.0f, 400.0f, -700.0f },
        { 1199.0f, 400.0f, -700.0f },
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
