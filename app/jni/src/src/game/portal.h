#ifndef PORTAL_H
#define PORTAL_H

#include "types.h"

/** Portal gun state: pickup prop, two portals, hitscan placement, teleport. */
void portal_update(void);        // simulation tick: pickup + teleport checks
void portal_render(void);        // append the portal / gun geometry to the dlist
void portal_shoot(int which);    // 0 = blue (P1), 1 = orange (P2)
int  portal_gun_has(void);       // 1 once Mario picked the gun up
int  portal_count(void);         // number of placed portals (0..2)

#endif
