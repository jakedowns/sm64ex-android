#ifndef GFX_PC_H
#define GFX_PC_H

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

void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi, const char *window_title);
void gfx_start_frame(void);
void gfx_run(Gfx *commands);
void gfx_end_frame(void);
void gfx_precache_textures(void);
void gfx_shutdown(void);

#endif
