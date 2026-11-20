#ifndef BOXPHYS_H
#define BOXPHYS_H

#include "types.h"

/* Box3D physics prototype for the Portal Test Chamber annex:
 * a stack of cubes and a ball blaster. */
void boxphys_init(void);
void boxphys_reset(void);
void boxphys_update(void);
void boxphys_render(void);
void boxphys_equip_toggle(void);
int  boxphys_equipped(void);
void boxphys_fire(void);
int  boxphys_test_fire(void);

#endif
