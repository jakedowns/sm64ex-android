#ifndef MOVEKIT_H
#define MOVEKIT_H

#include "types.h"

struct MarioState;

/** Per-tick update for the grapple + jetpack (called after the physics tick). */
void movekit_update(struct MarioState *m);
/** Draws the grapple rope / anchor marker (called after the scene). */
void movekit_render(void);
/** Called from the airborne action dispatch before the switch. */
void movekit_try_wallrun(struct MarioState *m);
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

#endif
