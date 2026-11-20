#ifndef GFX_PC_H
#define GFX_PC_H

#include <stdint.h>

struct GfxRenderingAPI;
struct GfxWindowManagerAPI;

struct GfxDimensions {
    uint32_t width, height;
    float aspect_ratio;
};

extern struct GfxDimensions gfx_current_dimensions;

// Camera projection state captured from the display list, used by the
// Android screen-space post effects to rebuild view-space positions.
extern float gfx_cam_near;
extern float gfx_cam_far;
extern float gfx_cam_tan_half_fov_y;

// Splat rendering, first slice (see plan-cool-additions.md §5.6).
// Tagged object geometry is drawn as camera-facing point sprites instead of
// triangles. The tag travels through the display list as a renderer-local
// marker command, so it survives the deferred/multi-pass (per-eye) execution.
#define GFX_OP_SPLAT_MARKER 0x3f  // unused F3DEX opcode; only this renderer sees it

extern int gfx_splat_target;    // 0 off, 1 Mario, 2 trees, 3 every object
extern int gfx_splat_size_px;   // splat footprint (eye-buffer pixels)
extern int gfx_splat_opacity;   // 0..100
extern int gfx_splat_softness;  // 0..100, 0 = crisp disc, 100 = soft Gaussian
extern int gfx_splat_density;   // 0..100, interpolated splats on large triangles
extern int gfx_splat_shape;     // 0 radial gradient, 1 ragged paint strokes
extern int gfx_splat_active;    // set while executing a marked batch

void gfx_sp_splat_marker(int on);
extern int gfx_portal_debug_blit;   /* debug: full-screen portal FBO */

void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi, const char *window_title);
void gfx_start_frame(void);
void gfx_run(Gfx *commands);
void gfx_end_frame(void);
void gfx_precache_textures(void);
void gfx_shutdown(void);

#endif
