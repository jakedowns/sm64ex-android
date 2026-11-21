#ifndef PORTAL_H
#define PORTAL_H

#include "types.h"

#define PORTAL_VIEW_SEGS 32

/* Through-portal camera view (behind the debug toggle for now).
 * `active` means the renderer should do the extra scene pass and draw the
 * source portal as a textured ellipse.  `c_matrix` is the camera-space
 * transform V*M*V^-1; `view` is the real camera view matrix.  `center`/`rim`
 * are world-space fan vertices for the source portal. */
typedef struct {
    int active;
    int source;
    float c_matrix[4][4];
    float view[4][4];
    int count;
    float center[3];
    float rim[PORTAL_VIEW_SEGS][3];
    /* Surface normal of the source portal and the world-space nudge applied
     * to the decal so it wins the depth test against the wall it is drawn on
     * (4 units of separation is far below 16-bit depth precision at range). */
    float normal[3];
    float decal_offset;
} PortalViewState;

extern PortalViewState g_portal_view;    /* primary (the portal looked at) */
extern PortalViewState g_portal_view2;   /* the other portal */
void portal_view_toggle(void);
void portal_view_force(int src);

/** Portal gun state: pickup prop, two portals, hitscan placement, teleport. */
extern int g_portal_debug;
extern int g_portal_debug_align;
extern int g_portal_debug_color;       // last placement result (1 = ok, 2..4 = failure)

void portal_update(void);        // simulation tick: pickup + teleport checks
void portal_render(void);        // append the portal / gun geometry to the dlist
void portal_shoot(int which);    // 0 = blue (P1), 1 = orange (P2)
int  portal_gun_has(void);       // 1 once Mario picked the gun up
int  portal_gun_aim_error_deg10(void);   // barrel vs reticle hit, tenths of a degree
int  portal_gun_yaw_offset_deg10(void);  // barrel vs Mario's facing, tenths
void portal_aim_override(int on, const Vec3f point);  // test hook: fixed reticle target
int  portal_count(void);         // number of placed portals (0..2)

/* Debug / regression-test helpers.  These do not change normal gameplay. */
void portal_give_gun(void);
void portal_drop_gun(void);
void portal_reset(void);
/* Casts the same hitscan as portal_shoot(), but from an explicit origin
 * towards a target point.  Used by the deterministic Portal Test Chamber. */
int  portal_shoot_at(int which, const Vec3f origin, const Vec3f target);
int  portal_get(int which, Vec3f pos, Vec3f normal);
int  portal_last_src(void);
int  portal_last_dst(void);
int  portal_teleport_count(void);
int  portal_test_walls(void);   // bitmask: front/back/left/right wall accepted

/* Aiming reticle drawn in the N64 UI pass with its own stereo depth. */
extern float g_portal_reticle_depth_px;
int  portal_render_reticle(void);
void portal_render_aspect_calib(void);

#endif
