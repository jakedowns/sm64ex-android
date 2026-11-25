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
#include "engine/graph_node.h"

int gfx_cam_mode = 0;

/* Freecam (mode 5): a completely manual camera.  It never follows, tracks,
 * avoids obstacles or animates on its own; only the right stick (yaw/pitch),
 * the left stick (when FREE MOVE is on) and the pinch/drag gestures move it. */
int gfx_freecam_move = 0;
int gfx_freecam_lx = 0;
int gfx_freecam_ly = 0;
float gfx_freecam_nudge = 0.0f;    /* pinch delta (distance from the anchor) */
float gfx_freecam_ydelta = 0.0f;   /* drag delta (vertical) */
static Vec3f s_fcPos;
static Vec3f s_fcAnchor;
static s16 s_fcYaw;
static s16 s_fcPitch;
static int s_fcInit;

// Live tunables (CAMERA tab sliders -> nativeSetStereoInt keys 46..49).
float gfx_cam_dist = 700.0f;         // orbit radius, world units
float gfx_cam_height = 110.0f;       // look-at height above Mario's feet
float gfx_cam_recentre_pct = 50.0f;  // recentre speed while running
float gfx_cam_sens_pct = 100.0f;     // right-stick orbit sensitivity
/* Custom camera: do not pull in when a wall is between Mario and the camera.
 * With OCCLUSION off the occluding PTC wall is hidden completely (see ptc.c),
 * so the view stays on Mario; with OCCLUSION on (debug mode 1-3) the wall is
 * kept and the scene shader punches a circle through it instead, which keeps
 * the surroundings readable.  Toggle in the CAMERA tab / adb key 52. */
int gfx_cam_noclip = 1;

static s16 s_yaw;
static s16 s_pitch = 0x1800;
static f32 s_dist() { return gfx_cam_dist; }
/* One-shot orbit distance used by the portal handoff: the mapped camera is
 * exactly where the previous frame's rig put it, which can be a few units off
 * the configured chase distance.  Ease back to gfx_cam_dist instead of
 * snapping, so the framing stays continuous. */
static f32 s_distOverride;
static Vec3f s_pos;
static int s_init = 0;

static s16 s_fpYaw;
static s16 s_fpPitch;

/* R-trigger "spring" recentre: a critically damped chase of the camera yaw
 * towards directly behind Mario, like a racing-game chase cam. */
static int s_spring;
static float s_yawVel;
static u32 s_prevOvlButtons;

/* Scripted yaw override (chain finale): eases the camera to a chosen side of
 * Mario without a cut.  Used so the goomba lemming loop is framed. */
static int s_yawHintOn;
static s16 s_yawHint;

/* Scripted look target (chain demo): the camera position still follows Mario,
 * only the focus pans smoothly towards this point. */
static int s_hintOn;
static Vec3f s_hintPos;
static float s_hintW;

static s16 approach_s16_cam(s16 cur, s16 target, s16 inc);
static f32 clamps(f32 v, f32 lo, f32 hi);

void custom_camera_first_person_reset(void) {
    s_fpYaw = gMarioState != NULL ? gMarioState->faceAngle[1] : 0;
    s_fpPitch = 0;
}

/* Called after update_lakitu() so the first-person eye position wins over the
 * third-person follow.  Right stick looks around; with no input the yaw eases
 * back to Mario's facing. */
void first_person_camera_update(struct Camera *c) {
    Vec3f eye, dir, focus;
    s16 inX = 0, inY = 0;

    if (gMarioState == NULL || gMarioObject == NULL) {
        return;
    }
    if (gfx_ovl_analog && gfx_ovl_right_active) {
        inX = (s16) gfx_ovl_right_x;
        inY = (s16) gfx_ovl_right_y;
    } else if (gPlayer2Controller != NULL) {
        inX = gPlayer2Controller->stickX;
        inY = gPlayer2Controller->stickY;
    }
    {
        f32 sens = gfx_cam_sens_pct / 100.0f;
        if (inX > 12 || inX < -12) {
            // Half-speed FPV look; overlay right-stick X is mirrored relative
            // to the N64 stick.
            s_fpYaw -= (s16)((f32) inX * 13.0f * sens);
        }
        if (inY > 12 || inY < -12) {
            s_fpPitch = (s16) clamps((f32) s_fpPitch + (f32) inY * 5.0f * sens,
                                     -0x3F00, 0x3F00);
        }
    }
    /* No auto-recentre: it felt like the view kept turning back to a default
     * look target after the stick was released.  FPV yaw changes only while
     * the look stick is pushed. */
    (void) 0;

    dir[0] = sins(s_fpYaw) * coss(s_fpPitch);
    dir[1] = sins(s_fpPitch);
    dir[2] = coss(s_fpYaw) * coss(s_fpPitch);
    vec3f_copy(eye, gMarioState->pos);
    {
        float hoff = gfx_cam_height - 110.0f;
        if (hoff < -90.0f) hoff = -90.0f;
        if (hoff > 300.0f) hoff = 300.0f;
        eye[1] += 140.0f + hoff;
    }
    eye[0] += dir[0] * 12.0f;
    eye[2] += dir[2] * 12.0f;
    focus[0] = eye[0] + dir[0] * 400.0f;
    focus[1] = eye[1] + dir[1] * 400.0f;
    focus[2] = eye[2] + dir[2] * 400.0f;

    vec3f_copy(c->pos, eye);
    vec3f_copy(c->focus, focus);
    vec3f_copy(gLakituState.pos, eye);
    vec3f_copy(gLakituState.focus, focus);
    /* The movement code interprets gLakituState.yaw as the direction from
     * the camera towards Mario; s_fpYaw is the look direction, so flip it. */
    gLakituState.yaw = (s16)(s_fpYaw + 0x8000);
    c->yaw = gLakituState.yaw;
    gLakituState.focusDistance = 400.0f;
    gLakituState.roll = 0;
    gMarioObject->header.gfx.node.flags |= GRAPH_RENDER_INVISIBLE;
}

/* Debug freelook: pin the camera to an explicit position/focus so tests can
 * frame a portal exactly and nothing lerps it away. */
static int s_freelook;
/* Frames after a portal crossing: use a softer spring so the chase camera
 * catches up over ~0.3 s instead of taking one large step. */
static int s_crossFrames;

void custom_camera_portal_crossing(void) {
    s_crossFrames = 10;
}
static Vec3f s_flPos;
static Vec3f s_flFocus;

void custom_camera_set_freelook(int on, const Vec3f pos, const Vec3f focus) {
    s_freelook = on ? 1 : 0;
    if (pos != NULL) {
        vec3f_copy(s_flPos, pos);
    }
    if (focus != NULL) {
        vec3f_copy(s_flFocus, focus);
    }
}

int custom_camera_freelook_active(void) {
    return s_freelook;
}

/* Debug: current orbit internals for the PTC status dump. */
void custom_camera_debug(int out[8]) {
    out[0] = s_init;
    out[1] = s_freelook;
    out[2] = s_spring;
    out[3] = s_yawHintOn;
    out[4] = (int) s_yaw;
    out[5] = (int) s_pitch;
    out[6] = (int) s_distOverride;
    out[7] = (int) s_pos[0];
}

void custom_camera_set_yaw_hint(int on, s16 yaw) {
    s_yawHintOn = on ? 1 : 0;
    s_yawHint = yaw;
}

void custom_camera_set_focus_hint(int on, const Vec3f pos) {
    s_hintOn = on ? 1 : 0;
    if (on && pos != NULL) {
        vec3f_copy(s_hintPos, pos);
    }
}

void custom_camera_reset(void) {
    s_init = 0;
    s_distOverride = 0.0f;
}

/* Portal handoff: adopt an exact camera position/focus so the crossing frame
 * keeps the framing Mario had before the teleport.  The orbit yaw/pitch are
 * derived from the offset so the regular chase camera continues from here. */
void custom_camera_portal_snap(const Vec3f pos, const Vec3f focus) {
    f32 dist;
    s16 pitch, offsetYaw;

    if (gMarioState == NULL) {
        return;
    }
    s_freelook = 0;
    s_spring = 0;
    s_yawVel = 0.0f;
    s_init = 1;
    vec3f_copy(s_pos, pos);
    /* Distance/pitch describe the camera offset from the orbit target (focus
     * -> camera), while s_yaw is the camera's look yaw (focus -> camera yaw
     * plus 0x8000), i.e. the direction from the camera towards Mario. */
    vec3f_get_dist_and_angle(focus, pos, &dist, &pitch, &offsetYaw);
    s_pitch = pitch;
    s_yaw = calculate_yaw(pos, focus);
    s_distOverride = dist > 1.0f ? dist : 0.0f;
}


int custom_camera_freecam_state(int which) {
    switch (which) {
        case 0: return gfx_freecam_move;
        case 1: return gfx_freecam_lx;
        case 2: return gfx_freecam_ly;
        case 3: return s_fcYaw;
        case 4: return s_fcPitch;
        default: return s_fcInit;
    }
}

void custom_camera_freecam_update(struct Camera *c) {
    Vec3f dir, focus;
    s16 inX = 0, inY = 0;

    if (gMarioState == NULL || c == NULL) {
        return;
    }
    if (!s_fcInit) {
        Vec3f d;
        vec3f_copy(s_fcPos, gLakituState.pos);
        vec3f_copy(s_fcAnchor, gMarioState->pos);
        d[0] = gLakituState.focus[0] - gLakituState.pos[0];
        d[1] = gLakituState.focus[1] - gLakituState.pos[1];
        d[2] = gLakituState.focus[2] - gLakituState.pos[2];
        s_fcYaw = (s16) atan2s(d[0], d[2]);
        s_fcPitch = (s16) atan2s(d[1], sqrtf(d[0] * d[0] + d[2] * d[2]));
        s_fcInit = 1;
    }

    if (gfx_ovl_analog && gfx_ovl_right_active) {
        inX = (s16) gfx_ovl_right_x;
        inY = (s16) gfx_ovl_right_y;
    }
    {
        f32 sens = gfx_cam_sens_pct / 100.0f;
        if (inX > 12 || inX < -12) {
            s_fcYaw -= (s16) ((f32) inX * 13.0f * sens);
        }
        if (inY > 12 || inY < -12) {
            s_fcPitch = (s16) clamps((f32) s_fcPitch + (f32) inY * 5.0f * sens,
                                     -0x3F00, 0x3F00);
        }
    }

    /* Pinch: move toward/away from Mario's position at enable time. */
    if (fabsf(gfx_freecam_nudge) > 0.01f) {
        f32 dx = s_fcPos[0] - s_fcAnchor[0];
        f32 dz = s_fcPos[2] - s_fcAnchor[2];
        f32 d = sqrtf(dx * dx + dz * dz);
        f32 nd = d + gfx_freecam_nudge;
        if (nd < 120.0f) nd = 120.0f;
        if (nd > 4000.0f) nd = 4000.0f;
        if (d > 1.0f) {
            s_fcPos[0] = s_fcAnchor[0] + dx / d * nd;
            s_fcPos[2] = s_fcAnchor[2] + dz / d * nd;
        }
        gfx_freecam_nudge = 0.0f;
    }
    /* Vertical drag. */
    if (fabsf(gfx_freecam_ydelta) > 0.001f) {
        s_fcPos[1] += gfx_freecam_ydelta;
        if (s_fcPos[1] < 30.0f) s_fcPos[1] = 30.0f;
        gfx_freecam_ydelta = 0.0f;
    }

    /* FREE MOVE: left stick translates the camera along its actual look
     * vector (pitch included), so looking up and pushing up rises instead of
     * moving forward in world space.  Strafe stays horizontal. */
    if (gfx_freecam_move && (gfx_freecam_lx != 0 || gfx_freecam_ly != 0)) {
        f32 cp = coss(s_fcPitch);
        f32 fx = sins(s_fcYaw) * cp;
        f32 fy = sins(s_fcPitch);
        f32 fz = coss(s_fcYaw) * cp;
        /* Screen-right is the user's right: the old right vector pointed the
         * other way, so horizontal stick movement felt inverted. */
        f32 rx = -coss(s_fcYaw), rz = sins(s_fcYaw);
        f32 mx = (f32) gfx_freecam_lx / 127.0f;
        f32 my = (f32) gfx_freecam_ly / 127.0f;
        f32 sp = 14.0f;
        s_fcPos[0] += (fx * my + rx * mx) * sp;
        s_fcPos[1] += fy * my * sp;
        s_fcPos[2] += (fz * my + rz * mx) * sp;
    }

    dir[0] = sins(s_fcYaw) * coss(s_fcPitch);
    dir[1] = sins(s_fcPitch);
    dir[2] = coss(s_fcYaw) * coss(s_fcPitch);
    focus[0] = s_fcPos[0] + dir[0] * 400.0f;
    focus[1] = s_fcPos[1] + dir[1] * 400.0f;
    focus[2] = s_fcPos[2] + dir[2] * 400.0f;

    vec3f_copy(gLakituState.pos, s_fcPos);
    vec3f_copy(gLakituState.focus, focus);
    vec3f_copy(gLakituState.curPos, s_fcPos);
    vec3f_copy(gLakituState.curFocus, focus);
    vec3f_copy(gLakituState.goalPos, s_fcPos);
    vec3f_copy(gLakituState.goalFocus, focus);
    vec3f_copy(c->pos, s_fcPos);
    vec3f_copy(c->focus, focus);
}

void custom_camera_apply_mode(int mode) {
    if (mode < 0 || mode > 5) {
        mode = 0;
    }
    if (mode == 5) {
        s_fcInit = 0;   /* capture the current pose on the next update */
    }
    if (gfx_cam_mode == 4 && mode != 4 && gMarioObject != NULL) {
        gMarioObject->header.gfx.node.flags &= ~GRAPH_RENDER_INVISIBLE;
    }
    if (mode == 4) {
        custom_camera_first_person_reset();
        if (gMarioObject != NULL) {
            gMarioObject->header.gfx.node.flags |= GRAPH_RENDER_INVISIBLE;
        }
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
    float camK = 0.35f;

    if (s_crossFrames > 0) {
        camK = 0.12f;
        s_crossFrames--;
    }

    if (gMarioState == NULL) {
        return;
    }

    if (s_freelook) {
        /* No lerp, no collision: exactly where the test asked for. */
        vec3f_copy(c->pos, s_flPos);
        vec3f_copy(gLakituState.pos, s_flPos);
        vec3f_copy(c->focus, s_flFocus);
        vec3f_copy(gLakituState.focus, s_flFocus);
        c->yaw = calculate_yaw((Vec3f){ c->focus[0], c->focus[1], c->focus[2] }, c->pos);
        gLakituState.yaw = c->yaw;
        gLakituState.roll = 0;
        gLakituState.focusDistance = 400.0f;
        return;
    }

    target[0] = gMarioState->pos[0];
    target[1] = gMarioState->pos[1] + gfx_cam_height;
    target[2] = gMarioState->pos[2];

    if (!s_init) {
        /* s_yaw is the look yaw (see the orbit offset below), so initialise it
         * from the camera towards Mario, not the other way round. */
        s_yaw = calculate_yaw(c->pos, target);
        s_pos[0] = c->pos[0];
        s_pos[1] = c->pos[1];
        s_pos[2] = c->pos[2];
        s_init = 1;
    }

    // Right stick / overlay analog orbits the camera.
    if (gfx_ovl_analog && gfx_ovl_right_active) {
        inX = (s16) gfx_ovl_right_x;
        inY = (s16) gfx_ovl_right_y;
    } else if (gPlayer2Controller != NULL) {
        inX = gPlayer2Controller->stickX;
        inY = gPlayer2Controller->stickY;
    }
    {
        f32 sens = gfx_cam_sens_pct / 100.0f;
        if (inX > 12 || inX < -12) {
            s_yaw -= (s16)((f32) inX * 26.0f * sens);
        }
        if (inY > 12 || inY < -12) {
            // Inverted Y: stick up lowers the orbit (camera looks up).
            /* Allow looking well above the horizon so portals can be placed
             * high on walls (was clamped at 0x0400, i.e. almost level). */
            s_pitch = (s16) clamps((f32) s_pitch - (f32) inY * 10.0f * sens,
                                   -0x1C00, 0x3400);
        }
    }
    /* R shoulder: spring the camera behind Mario.  A strong stiffness with
     * damping gives the lag/overshoot of a racing chase camera.  R is also the
     * grapple retract, so leave it alone while the rope is attached. */
    {
        extern int movekit_state(void);
        extern u32 gfx_ovl_buttons;
        u32 now = gfx_ovl_buttons;
        int pressed = (gPlayer1Controller != NULL
                       && (gPlayer1Controller->buttonPressed & R_TRIG))
                   || ((now & R_TRIG) && !(s_prevOvlButtons & R_TRIG));
        s_prevOvlButtons = now;
        if (pressed && !(movekit_state() & 8)) {
            s_spring = 1;
        }
    }

    if (s_yawHintOn) {
        s_yaw = approach_s16_cam(s_yaw, s_yawHint, 0x600);
    } else if (s_spring) {
        /* s_yaw is the look yaw; a camera behind Mario looks along his facing. */
        s16 behind = gMarioState->faceAngle[1];
        float diff = (float)(s16)(behind - s_yaw);
        s_yawVel += diff * 0.20f;
        s_yawVel *= 0.82f;
        s_yaw = (s16)(s_yaw + (s16)s_yawVel);
        if (fabsf(diff) < 80.0f && fabsf(s_yawVel) < 40.0f) {
            s_spring = 0;
            s_yawVel = 0.0f;
            s_yaw = behind;
        }
    } else if (inY > 12 || inY < -12) {
        // stick is being used: no auto recentre
    } else {
        // Gently recentre behind Mario while he is running.
        if (gMarioState->forwardVel > 8.0f) {
            s16 behind = gMarioState->faceAngle[1];
            s16 inc = (s16)(512.0f * (gfx_cam_recentre_pct / 100.0f));
            s_yaw = approach_s16_cam(s_yaw, behind, inc);
        }
    }

    // Desired position on the orbit sphere.
    {
        f32 dist = s_dist();
        if (s_distOverride > 1.0f) {
            s_distOverride += (dist - s_distOverride) * 0.15f;
            if (fabsf(s_distOverride - dist) < 0.5f) {
                s_distOverride = 0.0f;
            } else {
                dist = s_distOverride;
            }
        }
        dir[0] = -sins(s_yaw) * coss(s_pitch);
        dir[2] = -coss(s_yaw) * coss(s_pitch);
        dir[1] = sins(s_pitch);
        desired[0] = target[0] + dir[0] * dist;
        desired[1] = target[1] + dir[1] * dist;
        desired[2] = target[2] + dir[2] * dist;
    }

    // Pull in when a wall/floor is between Mario and the camera.
    rayHit[0] = desired[0] - target[0];
    rayHit[1] = desired[1] - target[1];
    rayHit[2] = desired[2] - target[2];
    if (gfx_cam_noclip) {
        /* No pull-in: ease straight to the orbit position and let ptc.c hide
         * the wall we pass through. */
        s_pos[0] += (desired[0] - s_pos[0]) * camK;
        s_pos[1] += (desired[1] - s_pos[1]) * camK;
        s_pos[2] += (desired[2] - s_pos[2]) * camK;
        surf = NULL;
    } else {
        find_surface_on_ray(target, rayHit, &surf, s_pos);
    }
    if (surf != NULL) {
        f32 dx = target[0] - s_pos[0];
        f32 dy = target[1] - s_pos[1];
        f32 dz = target[2] - s_pos[2];
        f32 d = sqrtf(dx * dx + dy * dy + dz * dz) - 30.0f;
        f32 dist = sqrtf(rayHit[0] * rayHit[0] + rayHit[1] * rayHit[1]
                       + rayHit[2] * rayHit[2]);
        if (d < 90.0f) {
            d = 90.0f;
        }
        if (dist < 1.0f) {
            dist = s_dist();
        }
        s_pos[0] = target[0] + (desired[0] - target[0]) * (d / dist);
        s_pos[1] = target[1] + (desired[1] - target[1]) * (d / dist);
        s_pos[2] = target[2] + (desired[2] - target[2]) * (d / dist);
    } else {
        s_pos[0] += (desired[0] - s_pos[0]) * camK;
        s_pos[1] += (desired[1] - s_pos[1]) * camK;
        s_pos[2] += (desired[2] - s_pos[2]) * camK;
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
    gLakituState.pos[0] = s_pos[0];
    gLakituState.pos[1] = s_pos[1];
    gLakituState.pos[2] = s_pos[2];

    /* Optional scripted pan: ease the look-at point towards a hint (grapple
     * anchor, ball target, goomba) without moving the camera itself. */
    s_hintW += ((s_hintOn ? 1.0f : 0.0f) - s_hintW) * 0.12f;
    if (s_hintW > 0.01f) {
        float w = s_hintW * 0.85f;
        target[0] += (s_hintPos[0] - target[0]) * w;
        target[1] += (s_hintPos[1] - target[1]) * w;
        target[2] += (s_hintPos[2] - target[2]) * w;
    }

    vec3f_copy(c->focus, target);
    gLakituState.focus[0] = target[0];
    gLakituState.focus[1] = target[1];
    gLakituState.focus[2] = target[2];
    gLakituState.yaw = calculate_yaw(target, s_pos);
    c->yaw = gLakituState.yaw;
    c->nextYaw = c->yaw;
}
