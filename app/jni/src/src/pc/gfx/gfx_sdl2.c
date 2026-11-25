#ifdef WAPI_SDL2

#ifdef __MINGW32__
#define FOR_WINDOWS 1
#else
#define FOR_WINDOWS 0
#endif

#if FOR_WINDOWS
#define GLEW_STATIC
#include <GL/glew.h>
#include <SDL.h>
#define GL_GLEXT_PROTOTYPES 1
#include <SDL_opengl.h>
#else
#include <SDL.h>
#define GL_GLEXT_PROTOTYPES 1

#ifdef OSX_BUILD
#include <SDL_opengl.h>
#else
#include <SDL_opengles2.h>
#endif

#endif // End of OS-Specific GL defines

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx_window_manager_api.h"
#include "gfx_screen_config.h"
#include "../pc_main.h"
#include "../configfile.h"
#include "../cliopts.h"

#include "src/pc/controller/controller_keyboard.h"
#include "src/pc/controller/controller_touchscreen.h"

#include "game/ptc.h"
#include "game/ptc_trace.h"
#include "level_table.h"

// TODO: figure out if this shit even works
#ifdef VERSION_EU
# define FRAMERATE 25
#else
# define FRAMERATE 30
#endif

#ifdef __ANDROID__
extern int render_multiplier;
// Defined in gfx_pc.c; enabled here because Android renders into Leia's SBS surface.
extern int gfx_stereo_enabled;
extern int gfx_stereo_eye;
extern int gfx_stereo_eye_width;
extern int gfx_stereo_eye_width_override;
extern int gfx_stereo_swap_eyes;
extern int gfx_stereo_flip_y;
extern int gfx_stereo_same_both;
extern int gfx_stereo_vp[2][2];
extern int gfx_stereo_debug_tint;
extern int gfx_stereo_in_scale_pct;
extern int gfx_stereo_toe_pct;
extern int gfx_stereo_aspect_pct;
extern int gfx_stereo_aspect_tenths;
extern float gfx_geo_y_scale;
extern float gfx_geo_x_scale;
extern float gfx_ui_fit_scale;
extern float gfx_ui_box_pct;
extern int gfx_stereo_nocull;
extern int gfx_stereo_fov_pct;
extern int gfx_stereo_2d;
extern int gfx_stereo_zbuf;
extern int gfx_stereo_wire;
// Splat mode config (gfx_pc.c)
extern int gfx_splat_target;
extern int gfx_splat_size_px;
extern int gfx_splat_opacity;
extern int gfx_splat_softness;
extern int gfx_splat_density;
extern int gfx_splat_shape;
extern float gfx_stereo_fps;
extern int gfx_stereo_perf;
extern int gfx_stereo_out_w;
extern int gfx_stereo_out_h;
extern int gfx_stereo_fit;
extern float gfx_touch_scale;
extern float gfx_touch_dx;
extern float gfx_touch_dy;
extern int gfx_touch_float;
extern int gfx_ovl_active;
extern int gfx_ovl_stick_x;
extern int gfx_ovl_stick_y;
extern int gfx_ovl_right_x;
extern int gfx_ovl_right_active;
extern int gfx_ovl_right_y;
extern int gfx_ovl_analog;
extern unsigned int gfx_ovl_buttons;
extern int gfx_hide_touch_controls;
extern int gfx_fps_mode;
extern int gfx_hfr_interp;
// UI-after-post knobs (defined in gfx_pc.c).
extern int gfx_ui_post;
extern int gfx_ui_parallax;
extern int gfx_ui_buf;
extern float gfx_ui_depth_px;
extern int gfx_toe_auto;
// Camera tracking (defined in gfx_pc.c).
extern int gfx_hdt_enabled;
extern int gfx_gyro_enabled;
extern float gfx_hdt_strength;
extern float gfx_gyro_strength;
extern float gfx_head_pose[3];
extern int gfx_head_valid;
extern float gfx_gyro_ypr[3];
extern float gfx_gyro_pivot;
extern float gfx_cam_dist, gfx_cam_height, gfx_cam_recentre_pct, gfx_cam_sens_pct;
extern int gfx_cam_noclip;
extern float gfx_render_delta;
extern float gfx_stereo_ipd_px;
extern float gfx_stereo_convergence_px;

// Screen-space FX knobs live in gfx_opengl.c (the pass runs there).
extern void gfx_opengl_ssfx_set(int which, int value);
extern int gfx_opengl_ssfx_get(int which);

#include <jni.h>
#include <time.h>

/* Defined below; the debug-key switch appears earlier in this file. */
extern int gfx_rate_enabled;
int gfx_rate_state(int which);
extern int gfx_rate_target_fps;
extern int gfx_rate_adaptive;
extern int gfx_rate_effective_fps;
void gfx_rate_reset(void);

static float s_base_ipd = 300.0f;
static float s_base_convergence = 10.0f;   // matches the slider default
static float s_depth_scale = 0.5f;

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeSetStereo(JNIEnv *env, jobject thiz,
                                                            jint which, jint value) {
    (void) env;
    (void) thiz;
    switch (which) {
        case 0: s_base_ipd = (float) value; break;
        case 1: s_base_convergence = (float) value; break;
        case 2: s_depth_scale = (float) value / 100.0f; break;
    }
    gfx_stereo_ipd_px = s_base_ipd * s_depth_scale;
    gfx_stereo_convergence_px = s_base_convergence * s_depth_scale;
}

static int s_draw_w = 0;
static int s_draw_h = 0;

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeSetStereoMode(JNIEnv *env, jobject thiz,
                                                                 jint enabled) {
    (void) env;
    (void) thiz;
    gfx_stereo_enabled = enabled ? 1 : 0;
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetDrawableWidth(JNIEnv *env, jobject thiz) {
    (void) env; (void) thiz;
    return s_draw_w;
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetDrawableHeight(JNIEnv *env, jobject thiz) {
    (void) env; (void) thiz;
    return s_draw_h;
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeSetStereoInt(JNIEnv *env, jobject thiz,
                                                                jint which, jint value) {
    (void) env; (void) thiz;
    switch (which) {
        case 3: gfx_stereo_eye_width_override = value; break;
        case 4: gfx_stereo_swap_eyes = value; break;
        case 5: gfx_stereo_flip_y = value; break;
        case 6: gfx_stereo_same_both = value; break;
        case 7: gfx_stereo_debug_tint = value; break;
        case 12: gfx_stereo_perf = value; break;
        case 14: gfx_stereo_in_scale_pct = value; break;
        case 15: gfx_stereo_zbuf = value; break;
        case 16: gfx_stereo_wire = value; break;
        case 17: gfx_stereo_toe_pct = value; break;
        case 18: gfx_stereo_aspect_pct = value; break;
        case 19: gfx_stereo_nocull = value; break;
        case 20: gfx_stereo_fov_pct = value; break;
        case 21: gfx_stereo_2d = value; break;
        case 13: gfx_stereo_fit = value; break;
        case 30: configStayInLevel = value != 0; break;
        case 35: gfx_ui_post = value; break;
        case 36: gfx_ui_parallax = value; break;
        case 37: gfx_ui_buf = value; break;
        case 38: gfx_ui_depth_px = (float) value; break;
        case 50: gfx_geo_y_scale = (float) value / 100.0f; break;
        case 57: gfx_stereo_aspect_tenths = value; break;
        case 58: gfx_ui_fit_scale = (float) value / 100.0f; break;
        case 59: gfx_ui_box_pct = (float) value; break;
        case 51: gfx_geo_x_scale = (float) value / 100.0f; break;
        case 40: gfx_toe_auto = value != 0; break;
        case 41: gfx_hdt_enabled = value != 0; break;
        case 42: gfx_gyro_enabled = value != 0; break;
        case 43: gfx_hdt_strength = (float) value; break;
        case 44: gfx_gyro_strength = (float) value; break;
        case 45: gfx_gyro_pivot = (float) value; break;
        case 46: {
            extern f32 gCameraZoomDist;
            extern u16 newcam_distance_target;
            float d = (float) value;
            gfx_cam_dist = d;
            gCameraZoomDist = d;
            newcam_distance_target = (u16) value;
            break;
        }
        case 47: gfx_cam_height = (float) value; break;
        case 48: gfx_cam_recentre_pct = (float) value; break;
        case 49: gfx_cam_sens_pct = (float) value; break;
        case 52: gfx_cam_noclip = value != 0; break;
        case 8: gfx_touch_dx = (float) value * 4.0f; break;    // percent -> px (approx)
        case 9: gfx_touch_dy = (float) value * 4.0f; break;
        case 10: gfx_touch_scale = (float) value / 100.0f; break;
        // Splat mode (first slice): target, size px, opacity %, softness %.
        // Keys 60-63 so we do not collide with the geo-calibration keys 50/51.
        case 60: gfx_splat_target = value; break;
        case 61: gfx_splat_size_px = value; break;
        case 62: gfx_splat_opacity = value; break;
        case 63: gfx_splat_softness = value; break;
        case 64: gfx_splat_density = value; break;
        case 65: gfx_splat_shape = value ? 1 : 0; break;
        /* Portal camera handoff: 0 = legacy 24-frame swoop, 1 = seamless. */
        case 66: {
            extern int g_camera_portal_mode;
            g_camera_portal_mode = value != 0 ? 1 : 0;
            break;
        }
        /* Rate limiting. */
        case 200: gfx_rate_enabled = value != 0; break;
        case 201: gfx_rate_target_fps = value < 0 ? 0 : (value > 240 ? 240 : value); break;
        case 202: gfx_rate_adaptive = value != 0; break;
        case 203: gfx_rate_reset(); break;
    }
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetVpX(JNIEnv *env, jobject thiz, jint eye) {
    (void) env; (void) thiz;
    return (eye >= 0 && eye < 2) ? gfx_stereo_vp[eye][0] : -1;
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetVpW(JNIEnv *env, jobject thiz, jint eye) {
    (void) env; (void) thiz;
    return (eye >= 0 && eye < 2) ? gfx_stereo_vp[eye][1] : -1;
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeOverlayInput(JNIEnv *env, jclass clazz,
                                                                jint active, jint sx,
                                                                jint sy, jint buttons) {
    (void) env; (void) clazz;
    {
        extern int gfx_cam_mode;
        extern int gfx_freecam_move;
        extern int gfx_freecam_lx, gfx_freecam_ly;
        if (gfx_cam_mode == 5 && gfx_freecam_move) {
            /* Freecam movement swallows the left stick so Mario stays put. */
            gfx_freecam_lx = sx;
            gfx_freecam_ly = sy;
            gfx_ovl_stick_x = 0;
            gfx_ovl_stick_y = 0;
        } else {
            gfx_freecam_lx = 0;
            gfx_freecam_ly = 0;
            gfx_ovl_stick_x = sx;
            gfx_ovl_stick_y = sy;
        }
    }
    gfx_ovl_active = active;
    gfx_ovl_buttons = (unsigned int) buttons;
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeOverlayStick2(JNIEnv *env, jclass clazz,
                                                                jint rx, jint ry, jint active) {
    (void) env; (void) clazz;
    gfx_ovl_right_x = rx;
    gfx_ovl_right_y = ry;
    gfx_ovl_right_active = active;
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeOverlayAnalog(JNIEnv *env, jclass clazz,
                                                                jint on) {
    (void) env; (void) clazz;
    gfx_ovl_analog = on;
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeHideInGameControls(JNIEnv *env, jclass clazz,
                                                                      jint hide) {
    (void) env; (void) clazz;
    gfx_hide_touch_controls = hide;
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeSetFpsMode(JNIEnv *env, jclass clazz, jint mode) {
    (void) env; (void) clazz;
    extern void gfx_set_fps_mode(int);
    gfx_set_fps_mode(mode);
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetFpsMode(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    return gfx_fps_mode;
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeSetUncappedTest(JNIEnv *env, jclass clazz,
                                                                   jint on) {
    (void) env; (void) clazz;
    extern void gfx_set_test_uncapped(int);
    gfx_set_test_uncapped(on);
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetUncappedTest(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    extern int gfx_get_test_uncapped(void);
    return gfx_get_test_uncapped();
}

/* QUICK DEV: debug boot shortcut. The Java activity persists the toggle in
 * SharedPreferences and pushes it here from onCreate, which runs before the SDL
 * thread starts native main(), so thread5_game_loop sees the final values. */
static int s_quickDevOn;
static int s_quickDevLevel = LEVEL_PTC; /* Portal Test Chamber */
static int s_quickDevMute;

void gfx_set_quick_dev(int on, int level) {
    s_quickDevOn = on ? 1 : 0;
    if (level >= LEVEL_MIN && level <= LEVEL_MAX) {
        s_quickDevLevel = level;
    }
}

void gfx_set_quick_dev_mute(int on) {
    extern void set_sound_disabled(u8 disabled);
    s_quickDevMute = on ? 1 : 0;
    set_sound_disabled(s_quickDevMute);
}

int gfx_quick_dev_enabled(void) {
    return s_quickDevOn;
}

int gfx_quick_dev_level(void) {
    return s_quickDevLevel;
}

int gfx_quick_dev_mute(void) {
    return s_quickDevMute;
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeSetQuickDev(JNIEnv *env, jclass clazz,
                                                               jint on, jint level) {
    (void) env; (void) clazz;
    gfx_set_quick_dev(on, level);
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeSetQuickDevMute(JNIEnv *env, jclass clazz,
                                                                   jint on) {
    (void) env; (void) clazz;
    gfx_set_quick_dev_mute(on);
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeSetHfrInterp(JNIEnv *env, jclass clazz, jint on) {
    (void) env; (void) clazz;
    gfx_hfr_interp = on ? 1 : 0;
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetHfrInterp(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    return gfx_hfr_interp;
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeSetHeadPose(JNIEnv *env, jclass clazz,
                                                               jfloat x, jfloat y, jfloat z,
                                                               jint valid) {
    (void) env; (void) clazz;
    gfx_head_pose[0] = x;
    gfx_head_pose[1] = y;
    gfx_head_pose[2] = z;
    gfx_head_valid = valid ? 1 : 0;
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeSetGyro(JNIEnv *env, jclass clazz,
                                                           jfloat pitch, jfloat yaw, jfloat roll) {
    (void) env; (void) clazz;
    gfx_gyro_ypr[0] = pitch;
    gfx_gyro_ypr[1] = yaw;
    gfx_gyro_ypr[2] = roll;
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetHeadPose(JNIEnv *env, jclass clazz, jint axis) {
    (void) env; (void) clazz;
    if (axis < 0 || axis > 2) return 0;
    return (jint) gfx_head_pose[axis];
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeShootPortal(JNIEnv *env, jclass clazz, jint which) {
    (void) env; (void) clazz;
    extern void portal_shoot(int which);
    portal_shoot((int) which);
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGivePortalGun(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    extern void portal_give_gun(void);
    portal_give_gun();
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeDropPortalGun(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    extern void portal_drop_gun(void);
    portal_drop_gun();
}

extern volatile u32 g_hw_input_ms;
extern int g_hard_paused;
extern void game_reset_to_intro(void);
JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeStepTicks(JNIEnv *env, jclass clazz, jint n) {
    (void) env; (void) clazz;
    extern void game_step_ticks(int n);
    game_step_ticks((int) n);
}

/* Trailer audio tap (see pc/audio/audio_sdl.c). */
extern void audio_sdl_record(int on);
JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeSetAudioRec(JNIEnv *env, jclass clazz, jint on) {
    (void) env; (void) clazz;
    audio_sdl_record(on ? 1 : 0);
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeSetHardPause(JNIEnv *env, jclass clazz, jint on) {
    (void) env; (void) clazz;
    g_hard_paused = on ? 1 : 0;
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeResetToIntro(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    game_reset_to_intro();
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeHwInputAgeMs(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    if (g_hw_input_ms == 0) return 1000000;
    return (jint) (SDL_GetTicks() - g_hw_input_ms);
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetPortalGun(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    extern int portal_gun_has(void);
    return portal_gun_has();
}

/* Wall clock for scripted demos: osGetTime() is stubbed to 0 in this port,
 * so anything that needs real seconds must use this. */
double gfx_wall_time_sec(void) {
    return (double) SDL_GetTicks() / 1000.0;
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetRateState(JNIEnv *env, jclass clazz,
                                                                jint which) {
    (void) env; (void) clazz;
    return (jint) gfx_rate_state(which);
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeFrameTimeUs(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    return (jint) gfx_rate_state(6);
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeSetGameHudHidden(JNIEnv *env, jclass clazz,
                                                                   jint on) {
    (void) env; (void) clazz;
    extern int gHudForceHidden;
    gHudForceHidden = on ? 1 : 0;
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeJetpack(JNIEnv *env, jclass clazz, jint on) {
    (void) env; (void) clazz;
    extern void movekit_set_flag(int which, int on);
    extern void movekit_force_trigger(int on);
    static int equipped;
    if (on) {
        if (!equipped) {
            movekit_set_flag(2, 1);
            equipped = 1;
        }
        movekit_force_trigger(1);
    } else {
        movekit_force_trigger(0);
    }
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeToolToggle(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    extern void movekit_tool_toggle(void);
    movekit_tool_toggle();
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetTool(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    extern int movekit_tool(void);
    return movekit_tool();
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetMarioPos(JNIEnv *env, jclass clazz, jint axis) {
    (void) env; (void) clazz;
    extern int gfx_mario_pos(int axis);
    return gfx_mario_pos((int) axis);
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetPortalField(JNIEnv *env, jclass clazz,
                                                                  jint which, jint field) {
    (void) env; (void) clazz;
    extern int gfx_portal_field(int which, int field);
    return gfx_portal_field((int) which, (int) field);
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetPortalDebug(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    extern int g_portal_debug;
    return g_portal_debug;
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetPortalCount(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    extern int portal_count(void);
    return portal_count();
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativePtcCommand(JNIEnv *env, jclass clazz,
                                                              jstring action) {
    (void) clazz;
    const char *s = action != NULL ? (*env)->GetStringUTFChars(env, action, NULL) : NULL;
    ptc_debug_command(s != NULL ? s : "");
    if (s != NULL) {
        (*env)->ReleaseStringUTFChars(env, action, s);
    }
}

JNIEXPORT jstring JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativePtcStatus(JNIEnv *env, jclass clazz) {
    char buf[2048];
    (void) clazz;
    ptc_format_status(buf, (int) sizeof(buf));
    return (*env)->NewStringUTF(env, buf);
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativePtcPhase(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    char buf[2048];
    char *p;
    ptc_format_status(buf, (int) sizeof(buf));
    p = strstr(buf, "phase=");
    return p != NULL ? (jint) atoi(p + 6) : 0;
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativePtcStar(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    return ptc_star_collected();
}

/* Queue N simulation ticks for the game loop (see game/ptc_trace.c). */
JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeRunTicks(JNIEnv *env, jclass clazz, jint n) {
    (void) env; (void) clazz;
    ptc_trace_request_ticks((int) n);
}

/* Trace text accumulated by the last runticks batch. */
JNIEXPORT jstring JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativePtcTrace(JNIEnv *env, jclass clazz) {
    (void) clazz;
    const char *trace = ptc_trace_get();
    return (*env)->NewStringUTF(env, trace != NULL ? trace : "");
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeSetFreecamMove(JNIEnv *env, jclass clazz, jint on) {
    (void) env; (void) clazz;
    extern int gfx_freecam_move;
    gfx_freecam_move = on ? 1 : 0;
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeFreecamNudge(JNIEnv *env, jclass clazz, jfloat d) {
    (void) env; (void) clazz;
    extern float gfx_freecam_nudge;
    gfx_freecam_nudge += (float) d;
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeFreecamY(JNIEnv *env, jclass clazz, jfloat d) {
    (void) env; (void) clazz;
    extern float gfx_freecam_ydelta;
    gfx_freecam_ydelta += (float) d;
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeSetCamMode2(JNIEnv *env, jclass clazz, jint mode) {
    (void) env; (void) clazz;
    extern void custom_camera_apply_mode(int mode);
    custom_camera_apply_mode((int) mode);
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetCamMode2(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    extern int gfx_cam_mode;
    return gfx_cam_mode;
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeMovekitUpgrade(JNIEnv *env, jclass clazz, jint which) {
    (void) env; (void) clazz;
    extern void movekit_upgrade(int which);
    movekit_upgrade((int) which);
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetFuelTier(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    extern int movekit_fuel_tier(void);
    return movekit_fuel_tier();
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetRefillTier(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    extern int movekit_refill_tier(void);
    return movekit_refill_tier();
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetMovekitState(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    extern int movekit_state(void);
    return movekit_state();
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetJetpackFuel(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    extern int movekit_fuel_pct(void);
    return movekit_fuel_pct();
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetCamMode(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
#ifdef BETTERCAMERA
    extern u8 newcam_active;
    return newcam_active ? 1 : 0;
#else
    return 0;
#endif
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeToggleCamMode(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    extern u8 camera_mode_button_pressed;
    camera_mode_button_pressed = 1;
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetHfrMtxCount(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    extern int gfx_hfr_mtx_count(void);
    return gfx_hfr_mtx_count();
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetFps(JNIEnv *env, jobject thiz) {
    (void) env; (void) thiz;
    return (jint) (gfx_stereo_fps + 0.5f);
}

/* Test note shown on the green line in the Android debug panel.  adb can set
 * it, and scripted tests update it per step ("what is being tested / what
 * should happen"). */
static char s_debug_note[160];

void gfx_set_debug_note(const char *s) {
    if (s == NULL) {
        s = "";
    }
    strncpy(s_debug_note, s, sizeof(s_debug_note) - 1);
    s_debug_note[sizeof(s_debug_note) - 1] = '\0';
}

const char *gfx_get_debug_note(void) {
    return s_debug_note;
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeSetDebugNote(JNIEnv *env, jclass clazz,
                                                                jstring text) {
    const char *c = text ? (*env)->GetStringUTFChars(env, text, NULL) : NULL;
    (void) clazz;
    gfx_set_debug_note(c);
    if (c != NULL) {
        (*env)->ReleaseStringUTFChars(env, text, c);
    }
}

JNIEXPORT jstring JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetDebugNote(JNIEnv *env, jclass clazz) {
    (void) clazz;
    return (*env)->NewStringUTF(env, s_debug_note);
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetEyeWidth(JNIEnv *env, jobject thiz) {
    (void) env; (void) thiz;
    return gfx_stereo_eye_width;
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetStereoEnabled(JNIEnv *env, jobject thiz) {
    (void) env; (void) thiz;
    return gfx_stereo_enabled;
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeSetSSFX(JNIEnv *env, jobject thiz,
                                                           jint which, jint value) {
    (void) env; (void) thiz;
    gfx_opengl_ssfx_set(which, value);
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetSSFX(JNIEnv *env, jobject thiz,
                                                           jint which) {
    (void) env; (void) thiz;
    return gfx_opengl_ssfx_get(which);
}

/* Gameplay enhancements (RAGDOLL / HITRAG / PPCAM) and shadow knobs. */
JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeSetEnh(JNIEnv *env, jobject thiz,
                                                         jint which, jint value) {
    (void) env; (void) thiz;
    switch (which) {
        case 0: death_ragdoll_enabled = value ? 1 : 0; break;
        case 1: hit_ragdoll_enabled = value ? 1 : 0; break;
        case 2: configPuppycamEnhanced = value ? 1 : 0; break;
        case 3: { extern void movekit_set_flag(int, int); movekit_set_flag(0, value); break; }
        case 4: { extern void movekit_set_flag(int, int); movekit_set_flag(1, value); break; }
        case 5: { extern void movekit_set_flag(int, int); movekit_set_flag(2, value); break; }
    }
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetEnh(JNIEnv *env, jobject thiz, jint which) {
    (void) env; (void) thiz;
    switch (which) {
        case 0: return death_ragdoll_enabled;
        case 1: return hit_ragdoll_enabled;
        case 2: return configPuppycamEnhanced;
    }
    return 0;
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeDebugKill(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    extern void mario_debug_kill(void);
    mario_debug_kill();
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeLevelSelect(JNIEnv *env, jclass clazz) {
    (void) env; (void) clazz;
    extern void level_select_request_open(s16 autoLevel);
    level_select_request_open(-1);
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeDebugLevel(JNIEnv *env, jclass clazz,
                                                              jint level) {
    (void) env; (void) clazz;
    extern void level_select_debug_warp(s16 level);
    level_select_debug_warp((s16) level);
}

JNIEXPORT void JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeSetShadow(JNIEnv *env, jobject thiz,
                                                            jint which, jint value) {
    (void) env; (void) thiz;
    gfx_opengl_ssfx_set(50 + which, value);
}

JNIEXPORT jint JNICALL
Java_com_vdavid003_sm64port_sm64portActivity_nativeGetShadow(JNIEnv *env, jobject thiz,
                                                             jint which) {
    (void) env; (void) thiz;
    return gfx_opengl_ssfx_get(50 + which);
}
#endif

static const Uint32 FRAME_TIME = 1000 / FRAMERATE;

// HFR: render-rate mode (simulation always stays at 30 Hz).
// 0 = 30, 1 = 60, 2 = 120, 3 = uncapped. 120 Hz by default.
int gfx_fps_mode = 2;
// HFR: 0 disables frame interpolation (useful for A/B comparison).
int gfx_hfr_interp = 1;

// Test-only execution mode: run one fixed 30 Hz simulation tick per loop
// iteration, with no display pacing and no rendering.  Physics, collision and
// movement rules are untouched; only the wall-clock wait is removed.
int gfx_test_uncapped = 0;

void gfx_set_test_uncapped(int on) {
    gfx_test_uncapped = on ? 1 : 0;
}

int gfx_get_test_uncapped(void) {
    return gfx_test_uncapped;
}

void gfx_set_fps_mode(int mode) {
    if (mode < 0) mode = 0;
    if (mode > 3) mode = 3;
    gfx_fps_mode = mode;
}

static double gfx_now_secs(void) {
    return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
}

static double gfx_target_frame_secs(void) {
    switch (gfx_fps_mode) {
        case 1: return 1.0 / 60.0;
        case 2: return 1.0 / 120.0;
        case 3: return 0.0;          // uncapped
        default: return 1.0 / 30.0;
    }
}

/* ------------------------------------------------------------ rate limit */
int gfx_rate_enabled = 1;
int gfx_rate_target_fps = 0;      /* 0 = follow gfx_fps_mode */
int gfx_rate_adaptive = 0;
int gfx_rate_effective_fps = 0;
static double s_rate_next = 0.0;
static double s_rate_last = 0.0;
static double s_rate_ema_dt = 0.0;
static double s_rate_ema_jitter = 0.0;
static int s_rate_last_frame_us = 0;
static int s_rate_slow_frames = 0;
static int s_rate_fast_frames = 0;

static double gfx_rate_target_secs(void) {
    int fps;
    if (!gfx_rate_enabled) return 0.0;
    fps = gfx_rate_target_fps > 0 ? gfx_rate_target_fps : gfx_rate_effective_fps;
    if (fps <= 0) {
        double t = gfx_target_frame_secs();
        return t;
    }
    return 1.0 / (double) fps;
}

static void gfx_rate_note_frame(double now) {
    double target = gfx_rate_target_secs();
    if (s_rate_last > 0.0) {
        double dt = now - s_rate_last;
        if (dt > 0.0 && dt < 0.5) {
            s_rate_last_frame_us = (int) (dt * 1000000.0);
            s_rate_ema_dt = s_rate_ema_dt > 0.0 ? s_rate_ema_dt * 0.92 + dt * 0.08 : dt;
            s_rate_ema_jitter = s_rate_ema_jitter * 0.92 + fabs(dt - s_rate_ema_dt) * 0.08;
            if (target > 0.0) {
                if (dt > target * 1.4) {
                    s_rate_slow_frames++;
                    s_rate_fast_frames = 0;
                    if (gfx_rate_adaptive && s_rate_slow_frames > 45) {
                        int base = gfx_rate_target_fps > 0 ? gfx_rate_target_fps : 60;
                        if (gfx_rate_effective_fps == 0) gfx_rate_effective_fps = base;
                        if (gfx_rate_effective_fps > 24) gfx_rate_effective_fps -= 5;
                        s_rate_slow_frames = 0;
                    }
                } else if (dt < target * 0.9) {
                    s_rate_fast_frames++;
                    s_rate_slow_frames = 0;
                    if (gfx_rate_adaptive && gfx_rate_effective_fps > 0
                        && s_rate_fast_frames > 180) {
                        int base = gfx_rate_target_fps > 0 ? gfx_rate_target_fps : 60;
                        if (gfx_rate_effective_fps < base) gfx_rate_effective_fps += 5;
                        s_rate_fast_frames = 0;
                    }
                }
            }
        }
    }
    s_rate_last = now;
}

/* Deadline pacer: sleep to an absolute next-frame deadline.  Overruns reset
 * the deadline instead of bursting to catch up. */
static void gfx_rate_pace(double now) {
    double target = gfx_rate_target_secs();
    double wait;
    if (target <= 0.0) {
        s_rate_next = 0.0;
        return;
    }
    if (s_rate_next <= 0.0 || now - s_rate_next > target * 4.0) {
        s_rate_next = now;
    }
    s_rate_next += target;
    wait = s_rate_next - gfx_now_secs();
    if (wait > 0.0003) {
        struct timespec ts;
        ts.tv_sec = (time_t) wait;
        ts.tv_nsec = (long) ((wait - (double) ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
    } else if (wait < -target * 2.0) {
        s_rate_next = gfx_now_secs();
    }
}

void gfx_rate_configure(int enabled, int fps, int adaptive) {
    gfx_rate_enabled = enabled ? 1 : 0;
    gfx_rate_target_fps = fps < 0 ? 0 : (fps > 240 ? 240 : fps);
    gfx_rate_adaptive = adaptive ? 1 : 0;
    gfx_rate_effective_fps = 0;
    s_rate_next = 0.0;
}

void gfx_rate_reset(void) {
    gfx_rate_effective_fps = 0;
    s_rate_next = 0.0;
    s_rate_slow_frames = 0;
    s_rate_fast_frames = 0;
}

int gfx_rate_state(int which) {
    switch (which) {
        case 0: return gfx_rate_enabled;
        case 1: return gfx_rate_target_fps;
        case 2: return gfx_rate_adaptive;
        case 3: return (int) (s_rate_ema_dt * 1000000.0);
        case 4: return (int) (s_rate_ema_jitter * 1000000.0);
        case 5: return gfx_rate_effective_fps;
        case 6: return s_rate_last_frame_us;   /* raw last frame, microseconds */
    }
    return -1;
}

static SDL_Window *wnd;
static SDL_GLContext ctx = NULL;
static int inverted_scancode_table[512];

static kb_callback_t kb_key_down = NULL;
static kb_callback_t kb_key_up = NULL;
static void (*kb_all_keys_up)(void) = NULL;
static void (*touch_down_callback)(void* event);
static void (*touch_motion_callback)(void* event);
static void (*touch_up_callback)(void* event);

const SDL_Scancode windows_scancode_table[] = {
  /*  0                        1                            2                         3                            4                     5                            6                            7  */
  /*  8                        9                            A                         B                            C                     D                            E                            F  */
  SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_ESCAPE,         SDL_SCANCODE_1,           SDL_SCANCODE_2,              SDL_SCANCODE_3,       SDL_SCANCODE_4,              SDL_SCANCODE_5,              SDL_SCANCODE_6,          /* 0 */
  SDL_SCANCODE_7,              SDL_SCANCODE_8,              SDL_SCANCODE_9,           SDL_SCANCODE_0,              SDL_SCANCODE_MINUS,   SDL_SCANCODE_EQUALS,         SDL_SCANCODE_BACKSPACE,      SDL_SCANCODE_TAB,        /* 0 */

  SDL_SCANCODE_Q,              SDL_SCANCODE_W,              SDL_SCANCODE_E,           SDL_SCANCODE_R,              SDL_SCANCODE_T,       SDL_SCANCODE_Y,              SDL_SCANCODE_U,              SDL_SCANCODE_I,          /* 1 */
  SDL_SCANCODE_O,              SDL_SCANCODE_P,              SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_RIGHTBRACKET,   SDL_SCANCODE_RETURN,  SDL_SCANCODE_LCTRL,          SDL_SCANCODE_A,              SDL_SCANCODE_S,          /* 1 */

  SDL_SCANCODE_D,              SDL_SCANCODE_F,              SDL_SCANCODE_G,           SDL_SCANCODE_H,              SDL_SCANCODE_J,       SDL_SCANCODE_K,              SDL_SCANCODE_L,              SDL_SCANCODE_SEMICOLON,  /* 2 */
  SDL_SCANCODE_APOSTROPHE,     SDL_SCANCODE_GRAVE,          SDL_SCANCODE_LSHIFT,      SDL_SCANCODE_BACKSLASH,      SDL_SCANCODE_Z,       SDL_SCANCODE_X,              SDL_SCANCODE_C,              SDL_SCANCODE_V,          /* 2 */

  SDL_SCANCODE_B,              SDL_SCANCODE_N,              SDL_SCANCODE_M,           SDL_SCANCODE_COMMA,          SDL_SCANCODE_PERIOD,  SDL_SCANCODE_SLASH,          SDL_SCANCODE_RSHIFT,         SDL_SCANCODE_PRINTSCREEN,/* 3 */
  SDL_SCANCODE_LALT,           SDL_SCANCODE_SPACE,          SDL_SCANCODE_CAPSLOCK,    SDL_SCANCODE_F1,             SDL_SCANCODE_F2,      SDL_SCANCODE_F3,             SDL_SCANCODE_F4,             SDL_SCANCODE_F5,         /* 3 */

  SDL_SCANCODE_F6,             SDL_SCANCODE_F7,             SDL_SCANCODE_F8,          SDL_SCANCODE_F9,             SDL_SCANCODE_F10,     SDL_SCANCODE_NUMLOCKCLEAR,   SDL_SCANCODE_SCROLLLOCK,     SDL_SCANCODE_HOME,       /* 4 */
  SDL_SCANCODE_UP,             SDL_SCANCODE_PAGEUP,         SDL_SCANCODE_KP_MINUS,    SDL_SCANCODE_LEFT,           SDL_SCANCODE_KP_5,    SDL_SCANCODE_RIGHT,          SDL_SCANCODE_KP_PLUS,        SDL_SCANCODE_END,        /* 4 */

  SDL_SCANCODE_DOWN,           SDL_SCANCODE_PAGEDOWN,       SDL_SCANCODE_INSERT,      SDL_SCANCODE_DELETE,         SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_NONUSBACKSLASH, SDL_SCANCODE_F11,        /* 5 */
  SDL_SCANCODE_F12,            SDL_SCANCODE_PAUSE,          SDL_SCANCODE_UNKNOWN,     SDL_SCANCODE_LGUI,           SDL_SCANCODE_RGUI,    SDL_SCANCODE_APPLICATION,    SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_UNKNOWN,    /* 5 */

  SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_UNKNOWN,     SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_F13,     SDL_SCANCODE_F14,            SDL_SCANCODE_F15,            SDL_SCANCODE_F16,        /* 6 */
  SDL_SCANCODE_F17,            SDL_SCANCODE_F18,            SDL_SCANCODE_F19,         SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_UNKNOWN,    /* 6 */

  SDL_SCANCODE_INTERNATIONAL2, SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_UNKNOWN,     SDL_SCANCODE_INTERNATIONAL1, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_UNKNOWN,    /* 7 */
  SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_INTERNATIONAL4, SDL_SCANCODE_UNKNOWN,     SDL_SCANCODE_INTERNATIONAL5, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_INTERNATIONAL3, SDL_SCANCODE_UNKNOWN,        SDL_SCANCODE_UNKNOWN     /* 7 */
};

const SDL_Scancode scancode_rmapping_extended[][2] = {
    {SDL_SCANCODE_KP_ENTER, SDL_SCANCODE_RETURN},
    {SDL_SCANCODE_RALT, SDL_SCANCODE_LALT},
    {SDL_SCANCODE_RCTRL, SDL_SCANCODE_LCTRL},
    {SDL_SCANCODE_KP_DIVIDE, SDL_SCANCODE_SLASH},
    //{SDL_SCANCODE_KP_PLUS, SDL_SCANCODE_CAPSLOCK}
};

const SDL_Scancode scancode_rmapping_nonextended[][2] = {
    {SDL_SCANCODE_KP_7, SDL_SCANCODE_HOME},
    {SDL_SCANCODE_KP_8, SDL_SCANCODE_UP},
    {SDL_SCANCODE_KP_9, SDL_SCANCODE_PAGEUP},
    {SDL_SCANCODE_KP_4, SDL_SCANCODE_LEFT},
    {SDL_SCANCODE_KP_6, SDL_SCANCODE_RIGHT},
    {SDL_SCANCODE_KP_1, SDL_SCANCODE_END},
    {SDL_SCANCODE_KP_2, SDL_SCANCODE_DOWN},
    {SDL_SCANCODE_KP_3, SDL_SCANCODE_PAGEDOWN},
    {SDL_SCANCODE_KP_0, SDL_SCANCODE_INSERT},
    {SDL_SCANCODE_KP_PERIOD, SDL_SCANCODE_DELETE},
    {SDL_SCANCODE_KP_MULTIPLY, SDL_SCANCODE_PRINTSCREEN}
};

#define IS_FULLSCREEN() ((SDL_GetWindowFlags(wnd) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)

static void gfx_sdl_set_fullscreen() {
    if (configWindow.reset)
        configWindow.fullscreen = false;
    if (configWindow.fullscreen == IS_FULLSCREEN())
        return;
    if (configWindow.fullscreen) {
        SDL_SetWindowFullscreen(wnd, SDL_WINDOW_FULLSCREEN_DESKTOP);
        SDL_ShowCursor(SDL_DISABLE);
    } else {
        SDL_SetWindowFullscreen(wnd, 0);
        SDL_ShowCursor(SDL_ENABLE);
        configWindow.exiting_fullscreen = true;
    }
}

static void gfx_sdl_reset_dimension_and_pos(void) {
    if (configWindow.exiting_fullscreen)
        configWindow.exiting_fullscreen = false;

    if (configWindow.reset) {
        configWindow.x = WAPI_WIN_CENTERPOS;
        configWindow.y = WAPI_WIN_CENTERPOS;
        configWindow.w = DESIRED_SCREEN_WIDTH;
        configWindow.h = DESIRED_SCREEN_HEIGHT;
        configWindow.reset = false;
    } else if (!configWindow.settings_changed) {
        return;
    }

    int xpos = (configWindow.x == WAPI_WIN_CENTERPOS) ? SDL_WINDOWPOS_CENTERED : configWindow.x;
    int ypos = (configWindow.y == WAPI_WIN_CENTERPOS) ? SDL_WINDOWPOS_CENTERED : configWindow.y;

    SDL_SetWindowSize(wnd, configWindow.w, configWindow.h);
    SDL_SetWindowPosition(wnd, xpos, ypos);
    SDL_GL_SetSwapInterval(configWindow.vsync); // in case vsync changed
}

void test_vsync(void) {
#ifdef __ANDROID__
    for (int i = 0; i < 8; ++i)
        SDL_GL_SwapWindow(wnd);

    Uint32 start = SDL_GetTicks();
    SDL_GL_SwapWindow(wnd);
    SDL_GL_SwapWindow(wnd);
    SDL_GL_SwapWindow(wnd);
    SDL_GL_SwapWindow(wnd);
    Uint32 end = SDL_GetTicks();

    const float average = 4.0 * 1000.0 / (end - start);

    /*Android's vsync seems finicky but timer based sync seems unusable too.
     * I think vsync does kind of work but not half-vsync and stuff like that.
     * Let's try to render multiple times if neccessary to lower the framerate.
     * I don't think this is a great solution but it works.
     * On SGI models, turning vsync off will help with framerate, but the best is 60fps patch.
     * The actual solution would be to render or copy the buffer to a texture
     * and then render that to the screen.*/
    render_multiplier = (average + 15) / 30;
    if (render_multiplier == 0)
        render_multiplier = 1;
#endif
}

static void gfx_sdl_init(const char *window_title) {
    SDL_Init(SDL_INIT_VIDEO);

    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");

    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    #ifdef USE_GLES
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);  // These attributes allow for hardware acceleration on RPis.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    #endif

    //SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    //SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

    int xpos = (configWindow.x == WAPI_WIN_CENTERPOS) ? SDL_WINDOWPOS_CENTERED : configWindow.x;
    int ypos = (configWindow.y == WAPI_WIN_CENTERPOS) ? SDL_WINDOWPOS_CENTERED : configWindow.y;

    wnd = SDL_CreateWindow(
        window_title,
        xpos, ypos, configWindow.w, configWindow.h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    ctx = SDL_GL_CreateContext(wnd);

#ifdef __ANDROID__
    // Android target feeds the Leia interlacer with a 2x1 SBS frame.
    // With the mpv-style picture asset, the producer buffer IS the interlaced
    // view size; do NOT resize it here (that corrupted the EGL context).
    gfx_stereo_enabled = 1;
    {
        int ww = 0, wh = 0, dw = 0, dh = 0;
        SDL_GetWindowSize(wnd, &ww, &wh);
        SDL_GL_GetDrawableSize(wnd, &dw, &dh);
        s_draw_w = dw;
        s_draw_h = dh;
        SDL_Log("SM64 stereo: window=%dx%d drawable=%dx%d", ww, wh, dw, dh);
    }
#endif

    test_vsync();
    SDL_GL_SetSwapInterval(configWindow.vsync);

    gfx_sdl_set_fullscreen();

    for (size_t i = 0; i < sizeof(windows_scancode_table) / sizeof(SDL_Scancode); i++) {
        inverted_scancode_table[windows_scancode_table[i]] = i;
    }

    for (size_t i = 0; i < sizeof(scancode_rmapping_extended) / sizeof(scancode_rmapping_extended[0]); i++) {
        inverted_scancode_table[scancode_rmapping_extended[i][0]] = inverted_scancode_table[scancode_rmapping_extended[i][1]] + 0x100;
    }

    for (size_t i = 0; i < sizeof(scancode_rmapping_nonextended) / sizeof(scancode_rmapping_nonextended[0]); i++) {
        inverted_scancode_table[scancode_rmapping_nonextended[i][0]] = inverted_scancode_table[scancode_rmapping_nonextended[i][1]];
        inverted_scancode_table[scancode_rmapping_nonextended[i][1]] += 0x100;
    }
}

static void gfx_sdl_main_loop(void (*run_one_game_iter)(void)) {
    extern void gfx_render_last(void);
    extern void gfx_start_frame(void);
    extern float gfx_render_delta;
    extern float gfx_render_alpha;

    if (gfx_test_uncapped) {
        // One full 1/30 s simulation tick per iteration.  send_display_list()
        // skips the GL pass in this mode, so this loop is limited only by CPU.
        // The screen intentionally keeps the last rendered frame.
        run_one_game_iter();
        return;
    }

    static double last = 0.0;
    static double acc = 0.0;
    const double TICK = 1.0 / 30.0;      // fixed simulation step

    double now = gfx_now_secs();
    if (last == 0.0) last = now;
    double dt = now - last;
    last = now;
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.1) dt = 0.1;              // clamp after resume/pause

    acc += dt;

    // In HFR modes the picture runs one simulation tick behind, so that every
    // frame can be blended between two known states. Alpha therefore starts at
    // 0 right after a tick and grows to 1 just before the next one.
    gfx_render_alpha = (gfx_fps_mode > 0 && gfx_hfr_interp) ? 0.0f : 1.0f;

    int sims = 0;
    while (acc >= TICK) {
        run_one_game_iter();             // sim + one render (gfx_run inside)
        acc -= TICK;
        sims++;
        if (sims >= 3) {                 // never spiral
            acc = 0.0;
            break;
        }
    }
    gfx_render_delta = (float)(acc / TICK);

    // Render-rate above 30 Hz: present extra frames from the last display list.
    if (gfx_fps_mode > 0 && sims == 0) {
        extern int gfx_slowmo_div;
        extern float gfx_slowmo_alpha_between(float delta);
        if (gfx_slowmo_div > 1) {
            // Slow motion spreads one sim step over gfx_slowmo_div ticks;
            // sub-tick frames advance by a fraction of one of those ticks, so
            // the motion stays even instead of snapping once per tick.
            gfx_render_alpha = gfx_slowmo_alpha_between(gfx_render_delta);
        } else {
            gfx_render_alpha = gfx_hfr_interp ? gfx_render_delta : 1.0f;
        }
        gfx_start_frame();
        gfx_render_last();
    }
    gfx_rate_note_frame(gfx_now_secs());
    gfx_rate_pace(gfx_now_secs());
}

static void gfx_sdl_get_dimensions(uint32_t *width, uint32_t *height) {
    SDL_GetWindowSize(wnd, width, height);
}

static int translate_scancode(int scancode) {
    if (scancode < 512) {
        return inverted_scancode_table[scancode];
    } else {
        return 0;
    }
}

static void gfx_sdl_onkeydown(int scancode) {
    if (kb_key_down)
        kb_key_down(translate_scancode(scancode));

    const Uint8 *state = SDL_GetKeyboardState(NULL);

    if (state[SDL_SCANCODE_LALT] && state[SDL_SCANCODE_RETURN]) {
        configWindow.fullscreen = !configWindow.fullscreen;
        configWindow.settings_changed = true;
    }
}

static void gfx_sdl_onkeyup(int scancode) {
    if (kb_key_up)
        kb_key_up(translate_scancode(scancode));
}

#ifdef TOUCH_CONTROLS
static void gfx_sdl_fingerdown(SDL_TouchFingerEvent sdl_event) {
    struct TouchEvent event;
    event.x = sdl_event.x;
    event.y = sdl_event.y;
    event.touchID = sdl_event.fingerId + 1;
    if (touch_down_callback != NULL) {
        touch_down_callback((void*)&event);
    }
}

static void gfx_sdl_fingermotion(SDL_TouchFingerEvent sdl_event) {
    struct TouchEvent event;
    event.x = sdl_event.x;
    event.y = sdl_event.y;
    event.touchID = sdl_event.fingerId + 1;
    if (touch_motion_callback != NULL) {
        touch_motion_callback((void*)&event);
    }
}

static void gfx_sdl_fingerup(SDL_TouchFingerEvent sdl_event) {
    struct TouchEvent event;
    event.x = sdl_event.x;
    event.y = sdl_event.y;
    event.touchID = sdl_event.fingerId + 1;
    if (touch_up_callback != NULL) {
        touch_up_callback((void*)&event);
    }
}
#endif

static void gfx_sdl_handle_events(void) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
#ifndef TARGET_WEB
            // Scancodes are broken in Emscripten SDL2: https://bugzilla.libsdl.org/show_bug.cgi?id=3259
            case SDL_KEYDOWN:
                gfx_sdl_onkeydown(event.key.keysym.scancode);
                break;
            case SDL_KEYUP:
                gfx_sdl_onkeyup(event.key.keysym.scancode);
                break;
#endif
#ifdef TOUCH_CONTROLS
	    case SDL_FINGERDOWN:
                gfx_sdl_fingerdown(event.tfinger);
                break;
	    case SDL_FINGERMOTION:
                gfx_sdl_fingermotion(event.tfinger);
                break;
	    case SDL_FINGERUP:
                gfx_sdl_fingerup(event.tfinger);
                break;
#endif
            case SDL_WINDOWEVENT: // TODO: Check if this makes sense to be included in the Web build
                if (!IS_FULLSCREEN()) {
                    switch (event.window.event) {
                        case SDL_WINDOWEVENT_MOVED:
                            if (!configWindow.exiting_fullscreen) {
                                if (event.window.data1 >= 0) configWindow.x = event.window.data1;
                                if (event.window.data2 >= 0) configWindow.y = event.window.data2;
                            }
                            break;
                        case SDL_WINDOWEVENT_SIZE_CHANGED:
                            configWindow.w = event.window.data1;
                            configWindow.h = event.window.data2;
                            break;
                    }
                }
                break;
            case SDL_QUIT:
                game_exit();
                break;
        }
    }

    if (configWindow.settings_changed) {
        gfx_sdl_set_fullscreen();
        gfx_sdl_reset_dimension_and_pos();
        configWindow.settings_changed = false;
    }
}

static void gfx_sdl_set_keyboard_callbacks(kb_callback_t on_key_down, kb_callback_t on_key_up, void (*on_all_keys_up)(void)) {
    kb_key_down = on_key_down;
    kb_key_up = on_key_up;
    kb_all_keys_up = on_all_keys_up;
}

static void gfx_sdl_set_touchscreen_callbacks(void (*down)(void* event), void (*motion)(void* event), void (*up)(void* event)) {
    touch_down_callback = down;
    touch_motion_callback = motion;
    touch_up_callback = up;
}

static bool gfx_sdl_start_frame(void) {
    return true;
}

static void gfx_sdl_swap_buffers_begin(void) {
    SDL_GL_SwapWindow(wnd);
}

static void gfx_sdl_swap_buffers_end(void) {
}

static double gfx_sdl_get_time(void) {
    return 0.0;
}


static void gfx_sdl_shutdown(void) {
    if (SDL_WasInit(0)) {
        if (ctx) { SDL_GL_DeleteContext(ctx); ctx = NULL; }
        if (wnd) { SDL_DestroyWindow(wnd); wnd = NULL; }
        SDL_Quit();
    }
}

struct GfxWindowManagerAPI gfx_sdl = {
    gfx_sdl_init,
    gfx_sdl_set_keyboard_callbacks,
    gfx_sdl_set_touchscreen_callbacks,
    gfx_sdl_main_loop,
    gfx_sdl_get_dimensions,
    gfx_sdl_handle_events,
    gfx_sdl_start_frame,
    gfx_sdl_swap_buffers_begin,
    gfx_sdl_swap_buffers_end,
    gfx_sdl_get_time,
    gfx_sdl_shutdown
};

#endif // BACKEND_WM
