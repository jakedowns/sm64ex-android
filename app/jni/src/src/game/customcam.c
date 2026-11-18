/**
 * Camera mode selection + a compact "custom" third-person orbit camera.
 *
 * Modes: 0 = vanilla (Lakitu), 1 = bettercam (vanilla + analog camera stick),
 *        2 = puppycam/free camera, 3 = custom orbit camera.
 */

#include <ultra64.h>

#include "sm64.h"
#include "engine/math_util.h"
#include "engine/surface_collision.h"
#include "game/camera.h"
#include "game/level_update.h"
#include "game/mario.h"
#include "game/bettercamera.h"
#include "game/game_init.h"   // gPlayer2Controller

int gfx_cam_mode = 0;

static s16 s_yaw;
static s16 s_pitch = 0x1800;
static f32 s_dist = 700.0f;
static Vec3f s_pos;
static int s_init = 0;

void custom_camera_reset(void) {
    s_init = 0;
}

void custom_camera_apply_mode(int mode) {
    if (mode < 0 || mode > 3) {
        mode = 0;
    }
#ifdef BETTERCAMERA
    if (mode == 2) {
        newcam_active = 1;
    } else {
        if (newcam_active) {
            // Leaving the free camera: hand control back to a normal mode.
            gLakituState.mode = CAMERA_MODE_CLOSE;
            gLakituState.defMode = CAMERA_MODE_CLOSE;
            gCameraMovementFlags |= CAM_MOVE_INIT_CAMERA;
        }
        newcam_active = 0;
        newcam_analogue = (mode == 1) ? 1 : 0;
    }
#endif
    gfx_cam_mode = mode;
    s_init = 0;
}

static s16 approach_s16_cam(s16 cur, s16 target, s16 inc) {
    s16 diff = (s16)(target - cur);
    if (diff > inc) return (s16)(cur + inc);
    if (diff < -inc) return (s16)(cur - inc);
    return target;
}

static f32 clamps(f32 v, f32 lo, f32 hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/**
 * Traditional third-person orbit: the camera sits behind/above Mario, the right
 * stick (or the overlay analog stick) orbits it, it slowly drifts back behind
 * him while he runs, and a raycast pulls it in when a wall is in the way.
 */
void custom_camera_update(struct Camera *c) {
    Vec3f target, desired, rayHit, dir;
    struct Surface *surf = NULL;
    s16 inX = 0, inY = 0;

    if (gMarioState == NULL) {
        return;
    }

    target[0] = gMarioState->pos[0];
    target[1] = gMarioState->pos[1] + 110.0f;
    target[2] = gMarioState->pos[2];

    if (!s_init) {
        s_yaw = calculate_yaw(target, c->pos);
        s_pos[0] = c->pos[0];
        s_pos[1] = c->pos[1];
        s_pos[2] = c->pos[2];
        s_init = 1;
    }

    // Right stick / overlay analog orbits the camera.
    if (gfx_ovl_analog && gfx_ovl_active) {
        inX = (s16) gfx_ovl_right_x;
        inY = (s16) gfx_ovl_right_y;
    } else if (gPlayer2Controller != NULL) {
        inX = gPlayer2Controller->stickX;
        inY = gPlayer2Controller->stickY;
    }
    if (inX > 12 || inX < -12) {
        s_yaw -= inX * 26;
    }
    if (inY > 12 || inY < -12) {
        s_pitch = (s16) clamps((f32) s_pitch + (f32) inY * 10.0f, 0x0400, 0x3400);
    } else {
        // Gently recentre behind Mario while he is running.
        if (gMarioState->forwardVel > 8.0f) {
            s16 behind = (s16)(gMarioState->faceAngle[1] + 0x8000);
            s_yaw = approach_s16_cam(s_yaw, behind, 0x0100);
        }
    }

    // Desired position on the orbit sphere.
    dir[0] = -sins(s_yaw) * coss(s_pitch);
    dir[2] = -coss(s_yaw) * coss(s_pitch);
    dir[1] = sins(s_pitch);
    desired[0] = target[0] + dir[0] * s_dist;
    desired[1] = target[1] + dir[1] * s_dist;
    desired[2] = target[2] + dir[2] * s_dist;

    // Pull in when a wall/floor is between Mario and the camera.
    rayHit[0] = desired[0] - target[0];
    rayHit[1] = desired[1] - target[1];
    rayHit[2] = desired[2] - target[2];
    find_surface_on_ray(target, rayHit, &surf, s_pos);
    if (surf != NULL) {
        f32 dx = target[0] - s_pos[0];
        f32 dy = target[1] - s_pos[1];
        f32 dz = target[2] - s_pos[2];
        f32 d = sqrtf(dx * dx + dy * dy + dz * dz) - 30.0f;
        if (d < 90.0f) {
            d = 90.0f;
        }
        s_pos[0] = target[0] + (desired[0] - target[0]) * (d / s_dist);
        s_pos[1] = target[1] + (desired[1] - target[1]) * (d / s_dist);
        s_pos[2] = target[2] + (desired[2] - target[2]) * (d / s_dist);
    } else {
        s_pos[0] += (desired[0] - s_pos[0]) * 0.35f;
        s_pos[1] += (desired[1] - s_pos[1]) * 0.35f;
        s_pos[2] += (desired[2] - s_pos[2]) * 0.35f;
    }

    // Never let the camera go under the floor.
    {
        struct Surface *floor = NULL;
        f32 floorY = find_floor(s_pos[0], s_pos[1] + 200.0f, s_pos[2], &floor);
        if (floor != NULL && s_pos[1] < floorY + 80.0f) {
            s_pos[1] = floorY + 80.0f;
        }
    }

    vec3f_copy(c->pos, s_pos);
    vec3f_copy(c->focus, target);
    gLakituState.pos[0] = s_pos[0];
    gLakituState.pos[1] = s_pos[1];
    gLakituState.pos[2] = s_pos[2];
    gLakituState.focus[0] = target[0];
    gLakituState.focus[1] = target[1];
    gLakituState.focus[2] = target[2];
    gLakituState.yaw = calculate_yaw(target, s_pos);
    c->yaw = gLakituState.yaw;
    c->nextYaw = c->yaw;
}
