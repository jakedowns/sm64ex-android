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
#include "movekit.h"

/* Generated low-poly portal-gun mesh + display list (CC0 model). */
#include "ptc_gun_model.inc"

#define PORTAL_RX        67.5f     // ellipse half width (world units, 1.5x)
#define PORTAL_RY        120.0f    // ellipse half height (1.5x)
#define PORTAL_SEGMENTS  24
#define PORTAL_RAY_DIST  4000.0f   // hitscan length
#define PORTAL_EXIT_PUSH 40.0f    // clear the surface without a visible pop
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
    float reveal;   /* 0..1 scale-up animation on placement */
};

static struct Portal s_portal[2];
static int s_lastPlaced = 1;
// Last placement result, for headless debugging: 0 none, 1 placed, 2 bad
// surface, 3 could not fit the ellipse, 4 no gun.
int g_portal_debug = 0;
/* Depth limit for the screen-space relay recursion (0 = off). */
int g_ptc_portal_recursion = 2;
int g_portal_debug_align = 0;   /* magenta source fan behind the relay decal */
int g_portal_debug_color = 0;   /* solid relay colour: isolate geometry/UVs */
PortalViewState g_portal_view;
PortalViewState g_portal_view2;
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
/* The hand-held gun's muzzle; the authoritative placement ray starts here so
 * an invisible occluder between the camera and Mario is never selected. */
static void portal_gun_muzzle(Vec3f out);
static int portal_reticle_hit(Vec3f out);
/* Momentum telemetry: the velocity Mario carried into the portal and the
 * velocity the portal mapping produced on the other side.  The ratio/dot are
 * exposed so the deterministic tests can prove that a portal preserves both
 * speed and direction instead of just repositioning Mario. */
static Vec3f s_lastEntryVel;
static Vec3f s_lastExitVel;
static float s_lastSpeedRatio = 1.0f;
static float s_lastTurnDot = 1.0f;
/* Transition telemetry: camera pose before the crossing and one camera update
 * after it, so a snapping camera shows up as a numeric jump. */
static Vec3f s_telCamIn, s_telCamOut, s_telFocusIn, s_telFocusOut;
static float s_telCamJump, s_telCamAngle;
static float s_telMarioResidual;   /* |actual exit - ideal mapped exit| */
static int s_telPending;

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
    s_portal[which].reveal = 0.0f;
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
    if (!movekit_portal_equipped()) {
        return;   /* grapple is in hand */
    }
    if (!s_hasGun) {
        g_portal_debug = 4;
        return;
    }

    vec3f_copy(orig, gLakituState.pos);
    vec3f_copy(dir, gLakituState.focus);
    vec3f_sub(dir, orig);

    /* Aim through the reticle, then fire from Mario's hand.  The reticle hit
     * is computed with the hand as the authoritative origin, so a wall or
     * platform between the camera and Mario (e.g. a faded occluder) cannot
     * steal the shot. */
    {
        Vec3f muzzle, target;
        portal_gun_muzzle(muzzle);
        if (portal_reticle_hit(target)) {
            dir[0] = target[0] - muzzle[0];
            dir[1] = target[1] - muzzle[1];
            dir[2] = target[2] - muzzle[2];
        } else {
            /* No surface: keep the camera's aim direction. */
        }
        vec3f_copy(orig, muzzle);
    }

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
    s_lastSpeedRatio = 1.0f;
    s_lastTurnDot = 1.0f;
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

    vec3f_copy(s_telCamIn, gLakituState.pos);
    vec3f_copy(s_telFocusIn, gLakituState.focus);

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
    {
        /* Ideal mapped COM (no surface push) for the transition telemetry. */
        Vec3f ideal;
        ideal[0] = dst->pos[0] + dst->tan[0] * x + dst->bitan[0] * y;
        ideal[1] = dst->pos[1] + dst->tan[1] * x + dst->bitan[1] * y;
        ideal[2] = dst->pos[2] + dst->tan[2] * x + dst->bitan[2] * y;
        s_telMarioResidual = sqrtf(
            (newCom[0] - ideal[0]) * (newCom[0] - ideal[0])
          + (newCom[1] - ideal[1]) * (newCom[1] - ideal[1])
          + (newCom[2] - ideal[2]) * (newCom[2] - ideal[2]));
    }

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

    /* Momentum telemetry: record what went in and what came out.  The portal
     * mapping is orthonormal, so speed must survive exactly and the direction
     * must turn by the pair's fixed angle; the deterministic tests assert
     * these numbers and then sample Mario again one physics tick later to make
     * sure the air state did not throw the launch away. */
    vec3f_copy(s_lastEntryVel, vel);
    vec3f_copy(s_lastExitVel, newVel);
    {
        float inLen = sqrtf(vdot3(vel, vel));
        float outLen = sqrtf(vdot3(newVel, newVel));
        s_lastSpeedRatio = (inLen > 1e-4f) ? (outLen / inLen) : 1.0f;
        if (inLen > 1e-4f && outLen > 1e-4f) {
            s_lastTurnDot = vdot3(vel, newVel) / (inLen * outLen);
        } else {
            s_lastTurnDot = 1.0f;
        }
    }

    // This fork rebuilds the horizontal air velocity from forwardVel every
    // tick (update_air_without_turn), which would erase a portal launch.
    // Store the rotated momentum in forwardVel, then face out of the exit
    // along the rotated motion: the tangential part of the entry direction is
    // preserved (an angled entry stays angled instead of snapping to the
    // destination normal).  For a purely normal entry this is the exit normal.
    {
        f32 hsp = sqrtf(newVel[0] * newVel[0] + newVel[2] * newVel[2]);
        f32 nh = sqrtf(dst->normal[0] * dst->normal[0]
                     + dst->normal[2] * dst->normal[2]);
        s16 exitYaw;
        s16 oldFaceYaw = gMarioState->faceAngle[1];

        gMarioState->forwardVel = hsp;
        if (hsp > 1.0f) {
            /* Wall / ramp / angled entry: keep the mapped heading. */
            exitYaw = atan2s(newVel[2], newVel[0]);
        } else if (nh > 0.1f) {
            /* Floor/ceiling exit with no horizontal launch: face out of the
             * destination surface. */
            exitYaw = atan2s(dst->normal[2], dst->normal[0]);
        } else {
            exitYaw = gMarioState->faceAngle[1];
        }
        gMarioState->faceAngle[1] = exitYaw;
        gMarioState->marioObj->oMoveAngleYaw = exitYaw;

        /* Keep the camera framing continuous: rotate the pre-teleport camera
         * rig around Mario by the same yaw change his facing took.  The view
         * keeps its distance, height, orbit offset and behind-the-player
         * relation, so his screen position and size do not change.  See
         * camera.c. */
        camera_portal_teleport(srcIdx, 1 - srcIdx, newCom, oldFaceYaw);
    }
    gCameraMovementFlags |= CAM_MOVE_INIT_CAMERA;
    s_cooldown = PORTAL_COOLDOWN;
    s_havePrevCom = 0;
    s_lastTeleportSrc = srcIdx;
    s_lastTeleportDst = 1 - srcIdx;
    s_teleportCount++;
    s_telPending = 1;
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
        // Also accept a swept crossing, because a fast launch (portal
        // momentum, grapple, jetpack) can jump the whole proximity band in a
        // single 30 Hz tick and would otherwise skip the portal entirely.
        if (s_portal[0].active && s_portal[1].active && s_cooldown == 0) {
            for (int i = 0; i < 2; i++) {
                struct Portal *p = &s_portal[i];
                float d = vdot3(com, p->normal) - vdot3(p->pos, p->normal);
                float dPrev = s_havePrevCom
                    ? vdot3(s_prevCom, p->normal) - vdot3(p->pos, p->normal)
                    : d;

                if (s_havePrevCom && dPrev > 0.0f && d <= 0.0f) {
                    float t = dPrev / (dPrev - d);
                    Vec3f cross;
                    cross[0] = s_prevCom[0] + (com[0] - s_prevCom[0]) * t;
                    cross[1] = s_prevCom[1] + (com[1] - s_prevCom[1]) * t;
                    cross[2] = s_prevCom[2] + (com[2] - s_prevCom[2]) * t;
                    if (portal_point_inside(p, cross)) {
                        portal_teleport(i, cross);
                        break;
                    }
                }
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
    float sc = p->reveal;
    if (sc < 0.02f) sc = 0.02f;
    if (sc > 1.0f) sc = 1.0f;

    if (v == NULL) {
        return;
    }

    emit_model_matrix(p->pos, p->tan, p->bitan, p->normal, view);

    fill_vtx(&v[0], 0.0f, 0.0f, 0.0f, r, g, b, 0xB8);
    for (int i = 0; i <= PORTAL_SEGMENTS; i++) {
        float a = (float)i * (2.0f * 3.14159265f) / (float)PORTAL_SEGMENTS;
        fill_vtx(&v[i + 1], cosf(a) * p->rx * sc, sinf(a) * p->ry * sc, 0.0f, r, g, b, 0xC8);
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

static int portal_reticle_hit(Vec3f out);

/* Diagnostics for the test dump: how far the barrel is from the reticle aim
 * (should be ~0) and how far the aim is from Mario's facing (shows the gun no
 * longer just follows his yaw). */
static float s_gunAimErrDeg = -1.0f;
static float s_gunYawOffDeg = -1.0f;

static void draw_held_gun(const Mat4 view) {
    struct MarioState *m = gMarioState;
    Vec3f right, fwd, up, pos, hit, aim;
    float len = 0.0f;
    int haveHit;
    s16 yaw;

    if (!s_hasGun || !movekit_portal_equipped() || m == NULL
        || m->marioObj == NULL) {
        return;
    }

    /* Keep the prop in Mario's right hand: follow the interpolated render
     * position/angle so it does not jitter against the 60 Hz model. */
    yaw = m->faceAngle[1];
    right[0] = coss(yaw);
    right[1] = 0.0f;
    right[2] = -sins(yaw);
    fwd[0] = sins(yaw);
    fwd[1] = 0.0f;
    fwd[2] = coss(yaw);

    portal_gun_muzzle(pos);

    /* Barrel (the prop's local +Z) points at the reticle's world-space hit,
     * i.e. the same point the world-space crosshair is drawn on.  Falls back to
     * Mario's facing when the ray hits nothing. */
    haveHit = portal_reticle_hit(hit);
    if (haveHit) {
        aim[0] = hit[0] - pos[0];
        aim[1] = hit[1] - pos[1];
        aim[2] = hit[2] - pos[2];
        len = sqrtf(vdot3(aim, aim));
        if (len > 8.0f) {
            fwd[0] = aim[0] / len;
            fwd[1] = aim[1] / len;
            fwd[2] = aim[2] / len;
        }
    }

    /* Right-handed basis X=right, Y=up, Z=barrel, with Y as close to world up
     * as the aim allows (so the gun rolls naturally when aiming up/down). */
    up[0] = 0.0f;
    up[1] = 1.0f;
    up[2] = 0.0f;
    vcross3(right, up, fwd);
    if (vdot3(right, right) < 0.0001f) {
        /* Aiming straight up or down: keep Mario's horizontal right. */
        yaw = m->faceAngle[1];
        right[0] = coss(yaw);
        right[1] = 0.0f;
        right[2] = -sins(yaw);
    }
    vnorm3(right);
    vcross3(up, fwd, right);
    vnorm3(up);

    /* Diagnostics. */
    {
        Vec3f face;
        float dot;
        face[0] = sins(m->faceAngle[1]);
        face[1] = 0.0f;
        face[2] = coss(m->faceAngle[1]);
        dot = vdot3(fwd, face);
        if (dot > 1.0f) dot = 1.0f;
        if (dot < -1.0f) dot = -1.0f;
        s_gunYawOffDeg = acosf(dot) * 180.0f / 3.14159265f;
        if (haveHit && len > 8.0f) {
            aim[0] = hit[0] - pos[0];
            aim[1] = hit[1] - pos[1];
            aim[2] = hit[2] - pos[2];
            vnorm3(aim);
            dot = vdot3(fwd, aim);
            if (dot > 1.0f) dot = 1.0f;
            if (dot < -1.0f) dot = -1.0f;
            s_gunAimErrDeg = acosf(dot) * 180.0f / 3.14159265f;
        } else {
            s_gunAimErrDeg = -1.0f;
        }
    }

    emit_model_matrix(pos, right, up, fwd, view);
    gSPDisplayList(gDisplayListHead++, ptc_gun_dl);
}

int portal_gun_aim_error_deg10(void) {
    return s_gunAimErrDeg < 0.0f ? -1 : (int)(s_gunAimErrDeg * 10.0f + 0.5f);
}

int portal_gun_yaw_offset_deg10(void) {
    return s_gunYawOffDeg < 0.0f ? -1 : (int)(s_gunYawOffDeg * 10.0f + 0.5f);
}

void portal_view_toggle(void) {
    portal_view_set(!s_portal_view_enabled);
}

void portal_view_set(int on) {
    s_portal_view_enabled = on ? 1 : 0;
    if (!s_portal_view_enabled) {
        g_portal_view.active = 0;
        g_portal_view2.active = 0;
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

/* Build the camera-space portal transform and fan geometry for one portal. */
static void portal_fill_view(int srcIdx, PortalViewState *out, const Mat4 view) {
    struct Portal *a = &s_portal[srcIdx];
    struct Portal *b = &s_portal[1 - srcIdx];
    float tsrc[4][4], tsrc_inv[4][4], tdst[4][4], tdst_inv[4][4], r[4][4];
    float m1[4][4], m[4][4], c[4][4];
    int i, i2, j2;

    out->active = 0;
    if (!a->active || !b->active) {
        return;
    }

    mat4_local_to_world(tsrc, a->pos, a->tan, a->bitan, a->normal);
    mat4_world_to_local(tsrc_inv, a->pos, a->tan, a->bitan, a->normal);
    mat4_local_to_world(tdst, b->pos, b->tan, b->bitan, b->normal);
    mat4_world_to_local(tdst_inv, b->pos, b->tan, b->bitan, b->normal);

    memset(r, 0, sizeof(r));
    r[0][0] = -1.0f;
    r[1][1] = 1.0f;
    r[2][2] = -1.0f;
    r[3][3] = 1.0f;

    /* Relay transform, column-vector convention.  The virtual camera is the
     * real camera mapped through the portal pair, so the *scene* is rendered
     * with the inverse: M^-1 = localToWorld(src) * R * worldToLocal(dst).  The
     * renderer stores matrices transposed (row vectors: v' = v * M), so the
     * camera-space product is the transpose of M^-1. */
    mat4_mul_rm(m, tsrc, r);
    mat4_mul_rm(m1, m, tdst_inv);
    for (i2 = 0; i2 < 4; i2++) {
        for (j2 = 0; j2 < 4; j2++) {
            c[i2][j2] = m1[j2][i2];
        }
    }

    memcpy(out->c_matrix, c, sizeof(c));
    memcpy(out->view, view, sizeof(Mat4));
    out->source = srcIdx;
    vec3f_copy(out->normal, a->normal);
    out->decal_offset = g_ptc_decal_offset;
    out->count = PORTAL_VIEW_SEGS;
    vec3f_copy(out->center, a->pos);
    for (i = 0; i < PORTAL_VIEW_SEGS; i++) {
        float ang = (float)i * (2.0f * 3.14159265f) / (float)PORTAL_VIEW_SEGS;
        out->rim[i][0] = a->pos[0] + a->tan[0] * cosf(ang) * a->rx
                                    + a->bitan[0] * sinf(ang) * a->ry;
        out->rim[i][1] = a->pos[1] + a->tan[1] * cosf(ang) * a->rx
                                    + a->bitan[1] * sinf(ang) * a->ry;
        out->rim[i][2] = a->pos[2] + a->tan[2] * cosf(ang) * a->rx
                                    + a->bitan[2] * sinf(ang) * a->ry;
    }
    out->active = 1;
}

/* Build the camera-space portal transform and the source fan geometry. */
static void portal_view_update(const Mat4 view) {
    int best = -1;

    g_portal_view.active = 0;
    g_portal_view2.active = 0;
    if (!s_portal_view_enabled || !s_portal[0].active || !s_portal[1].active) {
        return;
    }

    if (s_forceView >= 0) {
        best = s_forceView;
        if (!s_portal[best].active) best = -1;
    } else {
        /* Pick the portal the camera is actually looking at: in front of the
         * camera, front side towards it, and closest to the view axis. */
        Vec3f look, rel;
        float lookLen, bestScore = 1e30f;
        float maxdeg = g_ptc_select_maxdeg;
        float min_cos = cosf(maxdeg * 3.14159265f / 180.0f);
        if (min_cos < 0.0f) min_cos = 0.0f;
        vec3f_copy(look, gLakituState.focus);
        vec3f_sub(look, gLakituState.pos);
        lookLen = sqrtf(vdot3(look, look));
        if (lookLen > 1e-4f) {
            for (int i = 0; i < 2; i++) {
                float along, dist, cosang, score;
                if (!s_portal[i].active) continue;
                rel[0] = s_portal[i].pos[0] - gLakituState.pos[0];
                rel[1] = s_portal[i].pos[1] - gLakituState.pos[1];
                rel[2] = s_portal[i].pos[2] - gLakituState.pos[2];
                along = vdot3(rel, look);
                if (along <= 0.0f) continue;
                if (g_ptc_select_facing
                    && vdot3(rel, s_portal[i].normal) >= 0.0f) continue;
                dist = sqrtf(vdot3(rel, rel));
                if (dist < 1e-3f) continue;
                cosang = along / (dist * lookLen);
                if (cosang < min_cos) continue;
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

    /* Both portals are windows: the one being looked at first, then the
     * other, so a side-by-side pair shows a tunnel in each.  The second one
     * must still be in front of the camera: during a portal handoff the camera
     * can legitimately sit behind the destination wall, and a relay decal
     * painted there would cover Mario. */
    portal_fill_view(best, &g_portal_view, view);
    {
        struct Portal *o = &s_portal[1 - best];
        if (vdot3(gLakituState.pos, o->normal) - vdot3(o->pos, o->normal) > 0.0f) {
            portal_fill_view(1 - best, &g_portal_view2, view);
        }
    }
}

static void portal_draw_reticle(const Mat4 view);

void portal_render(void) {
    Mat4 view;

    if (gCurrentArea == NULL) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        if (s_portal[i].active && s_portal[i].reveal < 1.0f) {
            s_portal[i].reveal += 0.10f;
            if (s_portal[i].reveal > 1.0f) s_portal[i].reveal = 1.0f;
        }
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
        PortalViewState *vs = (i == 0) ? &g_portal_view : &g_portal_view2;
        PortalViewState *alt = (i == 0) ? &g_portal_view2 : &g_portal_view;
        if (!s_portal[i].active) continue;
        /* Do not paint the flat fan when the camera is behind the portal: it
         * is on the far side of the wall, and during a portal handoff it would
         * sit between the camera and Mario and hide him (the PTC wall is
         * noclipped away there).  The relay decal already requires the camera
         * to be in front (portal_view_update's facing test). */
        if (!g_portal_debug_align
            && vdot3(gLakituState.pos, s_portal[i].normal)
               - vdot3(s_portal[i].pos, s_portal[i].normal) <= 0.0f) {
            continue;
        }
        /* A portal with a live relay view is a window, not a flat fan. */
        if ((vs->active || alt->active) && !g_portal_debug_align) {
            continue;
        }
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

/* Test hook: when set, the reticle (and therefore the held gun) aims at a
 * fixed world point instead of the camera ray.  Lets a side camera show the
 * barrel pointing at the marker. */
static int s_aimOverride = 0;
static Vec3f s_aimPoint;

void portal_aim_override(int on, const Vec3f point) {
    s_aimOverride = on ? 1 : 0;
    if (on && point != NULL) {
        vec3f_copy(s_aimPoint, point);
    }
}

/* Raycast for the reticle.  The ray is nudged up by the same amount the
 * crosshair sits above screen centre.  Fills `out` with the hit point. */
/* World position of the held gun's muzzle: Mario's right hand, raised.  Used by
 * the reticle/placement ray and by the held-gun model, so the shot and the
 * barrel always agree. */
static void portal_gun_muzzle(Vec3f out) {
    struct MarioState *m = gMarioState;
    s16 yaw;
    float right[3], fwd[3];

    if (m == NULL) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    yaw = m->faceAngle[1];
    right[0] = coss(yaw);
    right[1] = 0.0f;
    right[2] = -sins(yaw);
    fwd[0] = sins(yaw);
    fwd[1] = 0.0f;
    fwd[2] = coss(yaw);

    out[0] = m->pos[0] + right[0] * 26.0f + fwd[0] * 8.0f;
    out[1] = m->pos[1] + 92.0f;
    out[2] = m->pos[2] + right[2] * 26.0f + fwd[2] * 8.0f;
}

static int portal_reticle_hit(Vec3f out) {
    Vec3f camPos, fwd, right, up, dir, camHit, muzzle, toHit, hit;
    struct Surface *surf = NULL;
    float len;

    if (gMarioState == NULL) {
        return 0;
    }
    if (s_aimOverride) {
        vec3f_copy(out, s_aimPoint);
        return 1;
    }
    vec3f_copy(camPos, gLakituState.pos);
    vec3f_copy(fwd, gLakituState.focus);
    vec3f_sub(fwd, camPos);
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

    /* 1. Where does the reticle's camera ray land? */
    {
        Vec3f ray = { dir[0] * PORTAL_RAY_DIST, dir[1] * PORTAL_RAY_DIST,
                      dir[2] * PORTAL_RAY_DIST };
        find_surface_on_ray(camPos, ray, &surf, camHit);
    }

    portal_gun_muzzle(muzzle);

    if (surf != NULL) {
        /* 2. Ignore a surface that sits between the camera and Mario.  That is
         * the invisible-occluder case: the camera (or an occluder fade) is
         * behind a wall, so the reticle would otherwise stick to the wall in
         * front of Mario instead of the surface he is aiming at. */
        Vec3f marioMid;
        float depthHit = vdot3((Vec3f){ camHit[0] - camPos[0], camHit[1] - camPos[1],
                                        camHit[2] - camPos[2] }, dir);
        float depthMario;
        marioMid[0] = gMarioState->pos[0];
        marioMid[1] = gMarioState->pos[1] + 70.0f;
        marioMid[2] = gMarioState->pos[2];
        depthMario = vdot3((Vec3f){ marioMid[0] - camPos[0], marioMid[1] - camPos[1],
                                     marioMid[2] - camPos[2] }, dir);
        if (depthHit >= depthMario - 60.0f) {
            /* 3. Authoritative ray: from the hand to the reticle surface. */
            toHit[0] = camHit[0] - muzzle[0];
            toHit[1] = camHit[1] - muzzle[1];
            toHit[2] = camHit[2] - muzzle[2];
            len = sqrtf(vdot3(toHit, toHit));
            if (len > 1.0f) {
                struct Surface *surf2 = NULL;
                find_surface_on_ray(muzzle, toHit, &surf2, hit);
                if (surf2 != NULL) {
                    vec3f_copy(out, hit);
                    return 1;
                }
            }
            vec3f_copy(out, camHit);
            return 1;
        }
    }

    /* 4. No usable camera hit (sky, or an occluder in front of Mario): shoot
     * from the hand along the camera's view direction. */
    {
        Vec3f ray = { dir[0] * PORTAL_RAY_DIST, dir[1] * PORTAL_RAY_DIST,
                      dir[2] * PORTAL_RAY_DIST };
        surf = NULL;
        find_surface_on_ray(muzzle, ray, &surf, hit);
    }
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

    if (!s_hasGun || !movekit_portal_equipped() || gMarioState == NULL
        || !portal_reticle_hit(hit)) {
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
    arm = d * 0.0080f;      /* 2x reticle */
    thick = d * 0.0024f;
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

void portal_last_velocities(Vec3f entry, Vec3f exitVel) {
    vec3f_copy(entry, s_lastEntryVel);
    vec3f_copy(exitVel, s_lastExitVel);
}

float portal_last_speed_ratio(void) {
    return s_lastSpeedRatio;
}

float portal_last_turn_dot(void) {
    return s_lastTurnDot;
}

/* Called once per simulation tick after the camera has updated, so the
 * post-crossing camera pose includes the handoff/remap for that frame. */
void portal_telemetry_tick(void) {
    Vec3f a, b;
    float la, lb, dot;

    if (!s_telPending) {
        return;
    }
    /* ptc_update runs before the camera update, so defer one tick: the sample
     * must include the camera update that followed the crossing. */
    if (s_telPending == 1) {
        s_telPending = 2;
        return;
    }
    s_telPending = 0;
    vec3f_copy(s_telCamOut, gLakituState.pos);
    vec3f_copy(s_telFocusOut, gLakituState.focus);

    s_telCamJump = sqrtf((s_telCamOut[0] - s_telCamIn[0]) * (s_telCamOut[0] - s_telCamIn[0])
                       + (s_telCamOut[1] - s_telCamIn[1]) * (s_telCamOut[1] - s_telCamIn[1])
                       + (s_telCamOut[2] - s_telCamIn[2]) * (s_telCamOut[2] - s_telCamIn[2]));

    a[0] = s_telCamIn[0] - s_telFocusIn[0];
    a[1] = s_telCamIn[1] - s_telFocusIn[1];
    a[2] = s_telCamIn[2] - s_telFocusIn[2];
    b[0] = s_telCamOut[0] - s_telFocusOut[0];
    b[1] = s_telCamOut[1] - s_telFocusOut[1];
    b[2] = s_telCamOut[2] - s_telFocusOut[2];
    la = sqrtf(vdot3(a, a));
    lb = sqrtf(vdot3(b, b));
    if (la > 1e-3f && lb > 1e-3f) {
        dot = vdot3(a, b) / (la * lb);
        if (dot > 1.0f) dot = 1.0f;
        if (dot < -1.0f) dot = -1.0f;
        s_telCamAngle = acosf(dot) * 180.0f / 3.14159265f;
    } else {
        s_telCamAngle = 0.0f;
    }
}

void portal_last_camera_jump(float *dist, float *angleDeg) {
    if (dist != NULL) *dist = s_telCamJump;
    if (angleDeg != NULL) *angleDeg = s_telCamAngle;
}

float portal_last_mario_residual(void) {
    return s_telMarioResidual;
}
