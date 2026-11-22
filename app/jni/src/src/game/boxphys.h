#ifndef BOXPHYS_H
#define BOXPHYS_H

#include "types.h"

/* Box3D physics prototype for the Portal Test Chamber annex:
 * a stack of cubes and a ball blaster. */
void boxphys_init(void);
void boxphys_reset(void);
void boxphys_slam_test(void);
void boxphys_update(void);
void boxphys_render(void);
void boxphys_equip_toggle(void);
int  boxphys_equipped(void);
void boxphys_fire(void);
int  boxphys_test_fire(void);
/* Chain-demo hooks (N64 units). */
void boxphys_test_spawn(float x, float y, float z);
void boxphys_test_tower(float x, float y, float z, int n);
void boxphys_test_pyramid(float x, float y, float z);
int  boxphys_test_fire_at(float x, float y, float z);
int  boxphys_test_boxes_near(float x, float y, float z, float r);
/* Nearest cube to (x,z) in N64 units, excluding cubes closer than minDist. */
int  boxphys_nearest_box(float x, float z, float minDist, float *outX, float *outZ);

#endif
