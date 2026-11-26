/**
 * Camera mode selection + a compact "custom" third-person orbit camera.
 *
 * Modes: 0 = vanilla (Lakitu), 1 = bettercam (vanilla + analog camera stick),
 *        2 = puppycam/free camera, 3 = custom orbit camera.
 */

#include <ultra64.h>
#include <android/log.h>

#define CUSTOMCAM_LOG(...) __android_log_print(ANDROID_LOG_INFO, "SM64Leia", __VA_ARGS__)

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

/* ------------------------------------------------------------------------- *
 * Cam-stick response curve + R-shoulder follow (fx keys 290-299).
 *
 * The on-screen right stick is shaped here, once, and every consumer reads the
 * shaped value: the custom orbit camera, the free camera, first person and the
 * vanilla analog-camera path in camera.c.  The same points are drawn by the
 * curve editor (StickCurveView.java) and by the radial heatmap inside the stick
 * circle (TouchOverlay.java), so the picture and the feel are the same data.
 *
 * Curve representation: 5 points (px[i], py[i]) with px strictly increasing
 * from 0 to 1 and py non-decreasing from 0 to 1.  Only the three interior
 * points are editable; the RC keys pack each as (x << 8) | y with both bytes
 * 0..255.  Interpolation is Fritsch-Carlson monotone cubic Hermite, so the
 * response can never invert:
 *   https://en.wikipedia.org/wiki/Monotone_cubic_interpolation
 *   (the same tangent limiting as Heckbert's variant described in
 *    https://signalsmith-audio.co.uk/writing/2021/monotonic-smooth-interpolation/)
 * ------------------------------------------------------------------------- */
#define CAM_STICK_POINTS 5
#define CAM_STICK_KEYS   290

static int   s_stick_curve_on = 1;
static float s_stick_deadzone = 0.10f;      /* fraction of the stick radius */
static int   s_stick_heatmap_on = 1;
static float s_stick_px[CAM_STICK_POINTS] = { 0.00f, 0.25f, 0.50f, 0.75f, 1.00f };
static float s_stick_py[CAM_STICK_POINTS] = { 0.00f, 0.25f, 0.50f, 0.75f, 1.00f };
static float s_stick_tan[CAM_STICK_POINTS];
static int   s_stick_curve_valid;

/* R-shoulder behaviours (fx 298). */
enum {
    CAM_FOLLOW_TOGGLE = 0,   /* tap R: follow until tapped again, then hold */
    CAM_FOLLOW_HOLD   = 1,   /* follow only while R is held */
    CAM_FOLLOW_SNAP   = 2,   /* tap R: snap behind, any cam-stick input releases */
};
static int   s_follow_mode = CAM_FOLLOW_TOGGLE;
static float s_spring_stiff_pct = 100.0f;
static float s_spring_damp_pct = 100.0f;
static int   s_follow_on;        /* toggle state (mode 0) */
static int   s_snap_armed;       /* mode 2: springing until stick input */
static int   s_cstick_digital_on = 1;

static void stick_curve_rebuild(void);
static float stick_curve_eval(float u);

/* Monotone cubic Hermite tangents (Fritsch-Carlson). */
static void stick_curve_rebuild(void) {
    float d[CAM_STICK_POINTS - 1];
    int i;

    for (i = 0; i < CAM_STICK_POINTS - 1; i++) {
        float h = s_stick_px[i + 1] - s_stick_px[i];
        d[i] = h > 1e-6f ? (s_stick_py[i + 1] - s_stick_py[i]) / h : 0.0f;
    }
    s_stick_tan[0] = d[0];
    s_stick_tan[CAM_STICK_POINTS - 1] = d[CAM_STICK_POINTS - 2];
    for (i = 1; i < CAM_STICK_POINTS - 1; i++) {
        s_stick_tan[i] = (d[i - 1] + d[i]) * 0.5f;
    }
    /* Limit the tangents so each segment stays monotone (no overshoot). */
    for (i = 0; i < CAM_STICK_POINTS - 1; i++) {
        if (d[i] == 0.0f) {
            s_stick_tan[i] = 0.0f;
            s_stick_tan[i + 1] = 0.0f;
            continue;
        }
        {
            float a = s_stick_tan[i] / d[i];
            float b = s_stick_tan[i + 1] / d[i];
            float s = a * a + b * b;
            if (s > 9.0f) {
                float t = 3.0f / sqrtf(s);
                s_stick_tan[i] = t * a * d[i];
                s_stick_tan[i + 1] = t * b * d[i];
            }
        }
    }
    s_stick_curve_valid = 1;
}

static float stick_curve_eval(float u) {
    int i = 0;
    float h, t, t2, t3, v;

    if (!s_stick_curve_valid) {
        stick_curve_rebuild();
    }
    if (u <= s_stick_px[0]) return s_stick_py[0];
    if (u >= s_stick_px[CAM_STICK_POINTS - 1]) return s_stick_py[CAM_STICK_POINTS - 1];
    while (i < CAM_STICK_POINTS - 2 && u > s_stick_px[i + 1]) {
        i++;
    }
    h = s_stick_px[i + 1] - s_stick_px[i];
    if (h < 1e-6f) return s_stick_py[i + 1];
    t = (u - s_stick_px[i]) / h;
    t2 = t * t;
    t3 = t2 * t;
    v = (2.0f * t3 - 3.0f * t2 + 1.0f) * s_stick_py[i]
      + (t3 - 2.0f * t2 + t) * h * s_stick_tan[i]
      + (-2.0f * t3 + 3.0f * t2) * s_stick_py[i + 1]
      + (t3 - t2) * h * s_stick_tan[i + 1];
    /* Stay inside the segment (guards float error and keeps it monotone). */
    if (v < s_stick_py[i]) v = s_stick_py[i];
    if (v > s_stick_py[i + 1]) v = s_stick_py[i + 1];
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

/* Force the interior points to be strictly ordered in x and non-decreasing in
 * y, so a dragged point can never fold the curve back on itself. */
static void stick_curve_sanitise(void) {
    int i;
    s_stick_px[0] = 0.0f;
    s_stick_py[0] = 0.0f;
    s_stick_px[CAM_STICK_POINTS - 1] = 1.0f;
    s_stick_py[CAM_STICK_POINTS - 1] = 1.0f;
    for (i = 1; i < CAM_STICK_POINTS - 1; i++) {
        if (s_stick_px[i] < s_stick_px[i - 1] + 0.02f) {
            s_stick_px[i] = s_stick_px[i - 1] + 0.02f;
        }
        if (s_stick_py[i] < s_stick_py[i - 1]) {
            s_stick_py[i] = s_stick_py[i - 1];
        }
        if (s_stick_px[i] > 0.98f) s_stick_px[i] = 0.98f;
        if (s_stick_py[i] > 1.0f) s_stick_py[i] = 1.0f;
    }
    s_stick_curve_valid = 0;
}

/**
 * Shape an overlay cam-stick vector through the deadzone + response curve.
 * Direction is preserved; only the magnitude is remapped.  A linear curve with
 * a zero deadzone is the identity, so this is safe to call unconditionally.
 */
void custom_camera_stick_shape2(int *x, int *y) {
    float vx, vy, mag, u, t, out, k;

    if (!s_stick_curve_on || x == NULL || y == NULL) {
        return;
    }
    vx = (float) *x;
    vy = (float) *y;
    mag = sqrtf(vx * vx + vy * vy);
    if (mag < 0.5f) {
        *x = 0;
        *y = 0;
        return;
    }
    u = mag / 127.0f;
    if (u > 1.0f) u = 1.0f;
    if (u <= s_stick_deadzone) {
        *x = 0;
        *y = 0;
        return;
    }
    t = (u - s_stick_deadzone) / (1.0f - s_stick_deadzone);
    out = stick_curve_eval(t) * 127.0f;
    k = out / mag;
    *x = (int) (vx * k);
    *y = (int) (vy * k);
}

void custom_camera_stick_fx_set(int which, int value) {
    switch (which) {
        case CAM_STICK_KEYS + 0: s_stick_curve_on = value ? 1 : 0; break;
        case CAM_STICK_KEYS + 1:
            s_stick_deadzone = value < 0 ? 0.0f : (value > 90 ? 0.90f : (float) value / 100.0f);
            break;
        case CAM_STICK_KEYS + 2: s_stick_heatmap_on = value ? 1 : 0; break;
        case CAM_STICK_KEYS + 3:
        case CAM_STICK_KEYS + 4:
        case CAM_STICK_KEYS + 5: {
            int i = which - (CAM_STICK_KEYS + 3) + 1;
            if (value < 0) value = 0;
            s_stick_px[i] = (float) ((value >> 8) & 0xFF) / 255.0f;
            s_stick_py[i] = (float) (value & 0xFF) / 255.0f;
            stick_curve_sanitise();
            break;
        }
        case CAM_STICK_KEYS + 6:
            s_spring_stiff_pct = value < 10 ? 10.0f : (value > 300 ? 300.0f : (float) value);
            break;
        case CAM_STICK_KEYS + 7:
            s_spring_damp_pct = value < 40 ? 40.0f : (value > 200 ? 200.0f : (float) value);
            break;
        case CAM_STICK_KEYS + 8:
            s_follow_mode = value < 0 ? 0 : (value > 2 ? 2 : value);
            s_follow_on = 0;
            s_snap_armed = 0;
            break;
        case CAM_STICK_KEYS + 9: s_cstick_digital_on = value ? 1 : 0; break;
        default: return;
    }
    __android_log_print(ANDROID_LOG_INFO, "SM64CamStick",
                        "fx %d=%d curve=%d dz=%.2f hm=%d p1=%.2f/%.2f p2=%.2f/%.2f "
                        "p3=%.2f/%.2f spring=%.0f/%.0f rmode=%d cdig=%d",
                        which, value, s_stick_curve_on, s_stick_deadzone, s_stick_heatmap_on,
                        s_stick_px[1], s_stick_py[1], s_stick_px[2], s_stick_py[2],
                        s_stick_px[3], s_stick_py[3], s_spring_stiff_pct, s_spring_damp_pct,
                        s_follow_mode, s_cstick_digital_on);
}

int custom_camera_stick_fx_get(int which) {
    switch (which) {
        case CAM_STICK_KEYS + 0: return s_stick_curve_on;
        case CAM_STICK_KEYS + 1: return (int) (s_stick_deadzone * 100.0f + 0.5f);
        case CAM_STICK_KEYS + 2: return s_stick_heatmap_on;
        case CAM_STICK_KEYS + 3:
        case CAM_STICK_KEYS + 4:
        case CAM_STICK_KEYS + 5: {
            int i = which - (CAM_STICK_KEYS + 3) + 1;
            int px = (int) (s_stick_px[i] * 255.0f + 0.5f);
            int py = (int) (s_stick_py[i] * 255.0f + 0.5f);
            return (px << 8) | py;
        }
        case CAM_STICK_KEYS + 6: return (int) (s_spring_stiff_pct + 0.5f);
        case CAM_STICK_KEYS + 7: return (int) (s_spring_damp_pct + 0.5f);
        case CAM_STICK_KEYS + 8: return s_follow_mode;
        case CAM_STICK_KEYS + 9: return s_cstick_digital_on;
        default: return 0;
    }
}

int custom_camera_stick_deadzone_pct(void) {
    return (int) (s_stick_deadzone * 100.0f + 0.5f);
}

int custom_camera_stick_heatmap_on(void) {
    return s_stick_heatmap_on;
}

int custom_camera_cstick_digital_on(void) {
    return s_cstick_digital_on;
}

int custom_camera_follow_mode(void) {
    return s_follow_mode;
}

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
float gfx_cam_sens_pct = 100.0f;     // right-stick orbit sensitivity (effective)

/* Sensitivity picker (CAMERA > SETTINGS, fx 330/331).
 *
 * The panel shows LOW / MED / HIGH, which pick a multiplier band, and the
 * "Cam sensitivity" slider trims the value inside that band:
 *
 *     effective % = trim % * band multiplier
 *
 * so the slider never has to be re-ranged when the user wants a much slower or
 * faster camera.  gfx_cam_sens_pct stays the single value the three camera
 * implementations multiply their stick deltas by; the band and the trim are
 * both persisted by the Android panel. */
int gfx_cam_sens_band = 1;           // 0 LOW, 1 MED, 2 HIGH
float gfx_cam_sens_trim = 100.0f;    // slider percent (stereo-int key 49)
static const float s_sensBandMult[3] = { 0.6f, 1.0f, 1.6f };

void gfx_cam_sens_apply(void) {
    int band = gfx_cam_sens_band;
    if (band < 0) band = 0;
    if (band > 2) band = 2;
    gfx_cam_sens_band = band;
    if (gfx_cam_sens_trim < 10.0f) gfx_cam_sens_trim = 10.0f;
    if (gfx_cam_sens_trim > 300.0f) gfx_cam_sens_trim = 300.0f;
    gfx_cam_sens_pct = gfx_cam_sens_trim * s_sensBandMult[band];
}

/* band, trim %, effective % - for cmd dump / the panel readout. */
int gfx_cam_sens_state(int which) {
    switch (which) {
        case 0: return gfx_cam_sens_band;
        case 1: return (int) (gfx_cam_sens_trim + 0.5f);
        case 2: return (int) (gfx_cam_sens_pct + 0.5f);
        default: return 0;
    }
}
/* Custom camera: do not pull in when a wall is between Mario and the camera.
 * With OCCLUSION off the occluding PTC wall is hidden completely (see ptc.c),
 * so the view stays on Mario; with OCCLUSION on (debug mode 1-3) the wall is
 * kept and the scene shader punches a circle through it instead, which keeps
 * the surroundings readable.  Toggle in the CAMERA tab / adb key 52. */
int gfx_cam_noclip = 1;

/* Camera vs ground (CAMERA > SETTINGS, fx 360).
 *
 * 0 COLLIDE - the pre-existing behaviour: floors and ceilings are ordinary
 *              collision surfaces, so they push the camera up and in.
 * 1 CLIP    - floor and ceiling surfaces (|normal.y| > 0.7) are ignored by the
 *              camera collision and by the floor clamp, so the camera can sit
 *              under the floor or above the ceiling.  Walls (and steep slopes)
 *              still collide.  Independent of gfx_cam_noclip, which hides a
 *              wall in front of Mario rather than letting the camera pass. */
int gfx_cam_ground_clip = 0;

int custom_camera_ground_clip(void) {
    return gfx_cam_ground_clip ? 1 : 0;
}

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

/* R-trigger "spring" recentre: a spring/damped chase of the camera yaw
 * towards directly behind Mario, like a racing-game chase cam.  The three
 * behaviours (fx 298) are implemented in custom_camera_update(). */
static int s_spring;
static float s_yawVel;
static u32 s_prevOvlButtons;
static int s_prevRDown;
static int s_logThrottle;
static int s_logPrevYaw;

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
        int sx = (int) gfx_ovl_right_x, sy = (int) gfx_ovl_right_y;
        custom_camera_stick_shape2(&sx, &sy);
        inX = (s16) sx;
        inY = (s16) sy;
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
        /* Start from the pose the previous mode left behind, but only when it
         * is a usable third-person view.  Entering FREE from FIRST PERSON used
         * to inherit the eye pose, and the inverse-angle conversion below used
         * to be passed the arguments in the wrong order: atan2s() here is the
         * (horizontal, vertical) form (see vec3f_get_dist_and_angle), so every
         * inherited level view came back as pitch 0x4000, i.e. straight up.
         * The freecam then opened on the void, which read as "wrapping back to
         * vanilla went black" (the mode label had no FREE case either). */
        Vec3f d;
        f32 horiz, dist2d;
        int useInherited = 1;

        d[0] = gLakituState.focus[0] - gLakituState.pos[0];
        d[1] = gLakituState.focus[1] - gLakituState.pos[1];
        d[2] = gLakituState.focus[2] - gLakituState.pos[2];
        horiz = sqrtf(d[0] * d[0] + d[2] * d[2]);
        if (horiz < 40.0f) {
            useInherited = 0;               /* degenerate / inside-head pose */
        } else {
            /* Same convention as vec3f_get_dist_and_angle: yaw from (z, x),
             * pitch from (horizontal, vertical). */
            s_fcYaw = (s16) atan2s(d[2], d[0]);
            s_fcPitch = (s16) atan2s(horiz, d[1]);
            if (s_fcPitch > 0x3000 || s_fcPitch < -0x3000) {
                useInherited = 0;           /* staring at the sky or the floor */
            }
        }

        vec3f_copy(s_fcAnchor, gMarioState->pos);
        if (useInherited) {
            vec3f_copy(s_fcPos, gLakituState.pos);
            dist2d = sqrtf((s_fcPos[0] - s_fcAnchor[0]) * (s_fcPos[0] - s_fcAnchor[0])
                         + (s_fcPos[2] - s_fcAnchor[2]) * (s_fcPos[2] - s_fcAnchor[2]));
            if (dist2d < 60.0f) {
                useInherited = 0;
            }
        }
        if (!useInherited) {
            f32 back = gfx_cam_dist > 250.0f ? gfx_cam_dist : 250.0f;
            s16 face = gMarioState->faceAngle[1];
            /* Camera behind Mario, looking at his chest height. */
            s_fcPos[0] = s_fcAnchor[0] - sins(face) * back;
            s_fcPos[1] = s_fcAnchor[1] + gfx_cam_height + 120.0f;
            s_fcPos[2] = s_fcAnchor[2] - coss(face) * back;
            if (s_fcPos[1] < 30.0f) s_fcPos[1] = 30.0f;
            d[0] = s_fcAnchor[0] - s_fcPos[0];
            d[1] = (s_fcAnchor[1] + gfx_cam_height) - s_fcPos[1];
            d[2] = s_fcAnchor[2] - s_fcPos[2];
            horiz = sqrtf(d[0] * d[0] + d[2] * d[2]);
            s_fcYaw = (s16) atan2s(d[2], d[0]);
            s_fcPitch = (s16) atan2s(horiz, d[1]);
        }
        CUSTOMCAM_LOG("FREECAM enter mode=%d inherited=%d pos=%.0f,%.0f,%.0f "
                      "yaw=0x%04x pitch=0x%04x",
                      gfx_cam_mode, useInherited,
                      s_fcPos[0], s_fcPos[1], s_fcPos[2],
                      (u16) s_fcYaw, (u16) s_fcPitch);
        s_fcInit = 1;
    }

    if (gfx_ovl_analog && gfx_ovl_right_active) {
        int sx = (int) gfx_ovl_right_x, sy = (int) gfx_ovl_right_y;
        custom_camera_stick_shape2(&sx, &sy);
        inX = (s16) sx;
        inY = (s16) sy;
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
    CUSTOMCAM_LOG("CAM %d -> %d", gfx_cam_mode, mode);
    if (mode == 5) {
        s_fcInit = 0;   /* capture the current pose on the next update */
    } else {
        /* Do not carry a pending pinch/drag delta into the next FREE entry. */
        gfx_freecam_nudge = 0.0f;
        gfx_freecam_ydelta = 0.0f;
        gfx_freecam_move = 0;
        gfx_freecam_lx = 0;
        gfx_freecam_ly = 0;
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

/* A floor or ceiling, by the usual |normal.y| threshold.  Anything shallower
 * is a wall or a steep slope, and keeps colliding in CLIP mode.  This uses the
 * surface normal rather than the surface type/partition, so it works for level
 * geometry, dynamic platforms and the PTC meshes alike. */
static int cam_surface_is_flat(const struct Surface *s) {
    return s != NULL && fabsf(s->normal.y) > 0.7f;
}

/**
 * Camera pull-in raycast that lets floors/ceilings through.
 *
 * find_surface_on_ray() returns the nearest hit of any kind, so a horizontal
 * hit is skipped by re-casting from just past it with the remaining length,
 * until a wall is found or the orbit ray runs out.  On return *hit_surf is the
 * first wall hit (or NULL) and hit_pos its position, exactly like
 * find_surface_on_ray().
 */
static void cam_cast_ray_through_ground(Vec3f orig, Vec3f dir,
                                        struct Surface **hit_surf, Vec3f hit_pos) {
    Vec3f from, seg, nd, hit;
    f32 total, travelled = 0.0f;
    int guard = 0;

    *hit_surf = NULL;
    total = vec3f_length(dir);
    if (total < 1.0f) {
        return;
    }
    vec3f_copy(nd, dir);
    vec3f_normalize(nd);

    while (travelled < total && guard++ < 8) {
        f32 segLen = total - travelled;
        f32 dx, dy, dz, step;
        struct Surface *s = NULL;

        from[0] = orig[0] + nd[0] * travelled;
        from[1] = orig[1] + nd[1] * travelled;
        from[2] = orig[2] + nd[2] * travelled;
        seg[0] = nd[0] * segLen;
        seg[1] = nd[1] * segLen;
        seg[2] = nd[2] * segLen;

        find_surface_on_ray(from, seg, &s, hit);
        if (s == NULL) {
            return;
        }
        if (!cam_surface_is_flat(s)) {
            *hit_surf = s;
            vec3f_copy(hit_pos, hit);
            return;
        }
        /* Skip this floor/ceiling: continue one unit past the hit. */
        dx = hit[0] - from[0];
        dy = hit[1] - from[1];
        dz = hit[2] - from[2];
        step = sqrtf(dx * dx + dy * dy + dz * dz) + 1.0f;
        if (step < 1.0f) {
            step = 1.0f;
        }
        travelled += step;
    }
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
        {
            int sx = inX, sy = inY;
            custom_camera_stick_shape2(&sx, &sy);
            inX = (s16) sx;
            inY = (s16) sy;
        }
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
    /* R shoulder: spring the camera behind Mario.
     *
     * Three selectable behaviours (fx 298):
     *   0 TOGGLE - tap R: follow Mario's forward vector (spring/damping) until
     *              R is tapped again; the offset from that moment is held.
     *   1 HOLD   - follow only while R is down.
     *   2 SNAP   - tap R: snap behind him, hold the offset; any cam-stick input
     *              releases it.
     * R is also the grapple retract, so leave it alone while the rope is
     * attached.  The follow only steers the yaw: distance, the noclip pass and
     * the occlusion punch-through are untouched. */
    {
        extern int movekit_state(void);
        extern u32 gfx_ovl_buttons;
        u32 now = gfx_ovl_buttons | (gPlayer1Controller != NULL
                                     ? gPlayer1Controller->buttonDown : 0);
        int rDown = (now & R_TRIG) != 0;
        int pressed = rDown && !s_prevRDown;
        int manual = (inX > 12 || inX < -12 || inY > 12 || inY < -12);
        int grapple = (movekit_state() & 8) != 0;

        s_prevRDown = rDown;
        s_prevOvlButtons = gfx_ovl_buttons;

        if (grapple) {
            s_follow_on = 0;
            s_snap_armed = 0;
        } else {
            switch (s_follow_mode) {
                case CAM_FOLLOW_HOLD:
                    s_follow_on = rDown;
                    s_snap_armed = 0;
                    break;
                case CAM_FOLLOW_SNAP:
                    if (pressed) {
                        s_snap_armed = 1;
                        s_yawVel = 0.0f;
                    }
                    if (manual) {
                        s_snap_armed = 0;
                    }
                    s_follow_on = 0;
                    break;
                case CAM_FOLLOW_TOGGLE:
                default:
                    if (pressed) {
                        s_follow_on = !s_follow_on;
                        s_yawVel = 0.0f;
                    }
                    s_snap_armed = 0;
                    break;
            }
        }
        s_spring = (s_follow_on || s_snap_armed) ? 1 : 0;
    }

    if (s_yawHintOn) {
        s_yaw = approach_s16_cam(s_yaw, s_yawHint, 0x600);
    } else if (s_spring) {
        /* s_yaw is the look yaw; a camera behind Mario looks along his facing. */
        s16 behind = gMarioState->faceAngle[1];
        float diff = (float)(s16)(behind - s_yaw);
        float k = 0.20f * (s_spring_stiff_pct / 100.0f);
        float d = 1.0f - 0.18f * (s_spring_damp_pct / 100.0f);
        if (d < 0.30f) d = 0.30f;
        if (d > 0.995f) d = 0.995f;
        s_yawVel += diff * k;
        s_yawVel *= d;
        s_yaw = (s16)(s_yaw + (s16)s_yawVel);
        /* TOGGLE/HOLD keep springing so he can turn; SNAP lets go once behind. */
        if (s_follow_mode == CAM_FOLLOW_SNAP
            && !s_follow_on
            && fabsf(diff) < 80.0f && fabsf(s_yawVel) < 40.0f) {
            s_snap_armed = 0;
            s_spring = 0;
            s_yawVel = 0.0f;
            s_yaw = behind;
        }
        if (s_follow_mode == CAM_FOLLOW_HOLD && !s_follow_on) {
            s_yawVel = 0.0f;
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
    } else if (gfx_cam_ground_clip) {
        /* CLIP: floors and ceilings are transparent to the camera, so the
         * camera can go under the floor or above the ceiling; walls still
         * pull it in.  gfx_cam_noclip above still wins outright. */
        cam_cast_ray_through_ground(target, rayHit, &surf, s_pos);
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

    // Never let the camera go under the floor (COLLIDE only: CLIP exists to
    // let it do exactly that).
    if (!gfx_cam_ground_clip) {
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

    /* Cam-stick telemetry (adb: logcat -s SM64CamStick).  Only while there is
     * stick or follow activity, and only every 4th frame, so a quiet session
     * costs nothing and a held stick gives a clean per-frame yaw rate. */
    {
        int active = (gfx_ovl_analog && gfx_ovl_right_active) || s_spring;
        int dyaw = (int)(s16)(s_yaw - s_logPrevYaw);
        s_logPrevYaw = s_yaw;
        if (active) {
            s_logThrottle = 3;
        }
        if (active || s_logThrottle > 0) {
            if (s_logThrottle == 0) {
                __android_log_print(ANDROID_LOG_INFO, "SM64CamStick",
                        "CamStick in=%d,%d yaw=%d dyaw=%d pitch=%d spring=%d rmode=%d "
                        "hold=%d snap=%d stiff=%.0f damp=%.0f",
                        inX, inY, (int) s_yaw, dyaw, (int) s_pitch, s_spring,
                        s_follow_mode, s_follow_on, s_snap_armed,
                        s_spring_stiff_pct, s_spring_damp_pct);
                s_logThrottle = 3;
            } else {
                s_logThrottle--;
            }
        }
    }
}
