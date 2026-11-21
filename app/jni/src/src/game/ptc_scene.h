#ifndef PTC_SCENE_H
#define PTC_SCENE_H

/* Runtime-built Portal Test Chamber scene and the debug knob table.
 *
 * The empty test level only bakes a collision floor into the ROM; everything
 * the camera sees is generated here so room size, chart placement and the
 * checker density can be changed over adb (`ptc knob ...`) without a rebuild.
 */

/* Knobs shared with the portal renderer. */
extern float g_ptc_decal_offset;   /* world-space nudge off the wall (units) */
extern int   g_ptc_decal_depth;    /* 1 = depth-test the relay decal */
extern float g_ptc_select_maxdeg;  /* max angle from the view axis for a portal */
extern int   g_ptc_select_facing;  /* 1 = ignore portals seen from behind */

void ptc_scene_render(void);       /* append the test scene to the display list */
void ptc_scene_toggle(void);
int  ptc_scene_enabled(void);

int  ptc_knob_set(const char *name, float value);
int  ptc_knob_get(const char *name, float *value);
void ptc_knob_reload(void);        /* re-read the config file */
void ptc_knob_dump(char *buf, int len);

#endif
