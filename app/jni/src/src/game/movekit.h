#ifndef MOVEKIT_H
#define MOVEKIT_H

#include "types.h"

struct MarioState;

/** Per-tick update for the grapple + jetpack (called after the physics tick). */
void movekit_update(struct MarioState *m);
/** Draws the grapple rope / anchor marker (called after the scene). */
void movekit_render(void);
/** Called from the airborne action dispatch before the switch.  Returns 1
 *  when a release-window jump-off consumed the tick, so the caller must not
 *  run the old action; starting a wall run still returns 0 and dispatches. */
int movekit_try_wallrun(struct MarioState *m);
/** The ACT_WALLRUN action body. */
u32 movekit_act_wallrun(struct MarioState *m);
/** Camera roll contributed by wall-running, in angle units. */
s16 movekit_camera_roll(void);

int movekit_fuel_pct(void);
void movekit_upgrade(int which);   // 0 = fuel capacity, 1 = refill rate
int movekit_fuel_tier(void);
int movekit_refill_tier(void);
int movekit_state(void);   // bit0 wallrun, bit1 grapple, bit2 jetpack, bit3 attached
void movekit_set_flag(int which, int on);

/** Wall-run jump-off telemetry (see movekit.c): 0 release ticks left,
 *  1 release window length, 2 last jump source, 3 stick blend %, 4 release
 *  speed x10, 5 wall runs started, 6 release windows armed.  Last exit vector
 *  components for 0 = x, 1 = z. */
int movekit_wr_state(int which);
f32 movekit_wr_last_vel(int axis);
/** Normal of the current/last wall-run surface (0 = x, 1 = y, 2 = z).  The
 *  corner rig uses axis 0: +1 wall A (room west wall), ~0 wall B. */
f32 movekit_wr_wall_normal(int axis);
void movekit_wr_clear(void);
/** Wall-run runtime dials (fx 320-329): 320 blend %, 321 stick gain %,
 *  322 min outward x0.1, 323 release ticks, 324 release on, 325 jump-up x0.1,
 *  326 outward push x0.1, 327 tangent gain %, 328 stick dead-zone,
 *  329 along-run tangential bonus %. */
void movekit_dial_set(int which, int value);
int movekit_dial_get(int which);
/** Grapple targeting debug view dials (fx 310-315).
 *  310 view on/off (default off), 311 fan half-angle in 0.1 deg (default 80),
 *  312 fan ring samples (default 8), 313 HUD line (default off),
 *  314 range in world units (default 3400), 315 out-of-range probe x0.1. */
void movekit_grapple_debug_set(int which, int value);
int  movekit_grapple_debug_get(int which);
/** Resolve the current aim with the same query the attach uses.
 *  Returns 0 no surface, 1 would attach, 2 surface but out of range.
 *  `dist` is the surface distance, `ray_deg` the winning fan sample's angle
 *  off the aim axis (0 = centre ray), `hit` the resolved world point. */
int movekit_grapple_aim_state(float *dist, float *ray_deg, float hit[3]);
/** Force the "use" trigger on for automated tests (0 = release). */
void movekit_force_trigger(int on);
void movekit_force_retract(int on);
void movekit_force_anchor(float x, float y, float z);

/** Equipped hand tool: 0 = portal gun, 1 = grapple.  L_TRIG follows it. */
int  movekit_tool(void);
int  movekit_tool_is_grapple(void);
int  movekit_portal_equipped(void);
void movekit_tool_set(int tool);
void movekit_tool_toggle(void);

#endif
