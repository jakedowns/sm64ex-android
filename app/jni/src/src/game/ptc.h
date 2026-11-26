#ifndef PTC_H
#define PTC_H

#include "types.h"

struct MarioState;
struct Object;

/* Portal Test Chamber phases (also exported for the headless test runner). */
enum PtcPhase {
    PTC_PHASE_IDLE = 0,
    PTC_PHASE_PLACE = 1,
    PTC_PHASE_WALK = 2,
    PTC_PHASE_TELEPORT = 3,
    PTC_PHASE_STAR = 4,
    PTC_PHASE_DONE = 5,
    PTC_PHASE_FAIL = 6,
    PTC_PHASE_CALIBRATE = 7,
    PTC_PHASE_MOM_PLACE = 8,
    PTC_PHASE_MOM_DROP = 9,
    PTC_PHASE_MOM_FLY = 10,
    PTC_PHASE_MOM_STAR = 11,
};

void ptc_reset(void);
void ptc_start_test(void);
void ptc_calibrate_start(void);
void ptc_place_portals(void);
void ptc_stop(void);

/* Per-simulation-tick update (called from play_mode_normal after portal_update). */
void ptc_update(void);
/* Per-frame 2D labels / live coordinate readout. */
void ptc_hud_update(void);
/* Scripted input is applied after update_mario_inputs(), before the action. */
void ptc_mario_input_override(struct MarioState *m);

/* Debug-star plumbing (behaviour code lives in ptc_star.inc.c). */
int  ptc_star_collected(void);          // star 0 (primary traversal)
int  ptc_star2_collected(void);         // star 1 (momentum test)
int  ptc_star_collected_index(int index);
void ptc_chain_state(int *phase, int *flags, int *crossings, int *star0, int *star1);
void ptc_star_mark_collected(void);
void ptc_star_mark_collected_index(int index);
void ptc_star_reset(void);
int  ptc_star_visible(void);
int  ptc_star_visible_index(int index);
void ptc_momentum_start(void);

/* Endless A<->B walk loop through two adjacent portals (smoke test). */
void ptc_mario_portal_walk_loop_start(void);
void ptc_mario_portal_walk_loop_stop(void);

/* Headless diagnostics.  ptc_format_status() returns one parseable line. */
void ptc_debug_command(const char *action);
void ptc_format_status(char *buf, int len);

/* Wall-run 90-degree corner rig (ptc corner0..7 / cornerliveN / corneroff).
 * Dials are fx 410-415; ptc_wrc_format() returns the compact `wrc=` summary. */
void ptc_wrc_dial_set(int which, int value);
int  ptc_wrc_dial_get(int which);
void ptc_wrc_format(char *buf, int len);

#endif
