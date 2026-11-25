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

// --- Emissive materials ----------------------------------------------------
// A batch of geometry tagged as "emitting light". The tag travels through the
// display list as a renderer-local marker command (same idea as the splat
// marker) because the master list is rebuilt per frame and replayed once per
// eye: a plain "current object" global would be stale by the time the triangles
// are flushed. The renderer keeps the tagged triangles and re-draws them into a
// half-resolution emissive buffer, which is what the bloom pass reads.
//
// w1 layout: r<<24 | g<<16 | b<<8 | strength, strength 0 = end of tagged batch.
#define GFX_OP_EMISSIVE_MARKER 0x3e   // unused F3DEX opcode; only this renderer sees it
#define GFX_EMISSIVE_PACK(r, g, b, s) \
    ((((uint32_t)(r) & 0xFF) << 24) | (((uint32_t)(g) & 0xFF) << 16) \
     | (((uint32_t)(b) & 0xFF) << 8) | ((uint32_t)(s) & 0xFF))

extern int gfx_emissive_active;      // strength 1..255 while a tagged batch draws
extern int gfx_emissive_rgb[3];      // 0..255 emission colour of that batch
extern int gfx_emissive_strength;    // == gfx_emissive_active (0 outside a batch)

void gfx_sp_emissive_marker(uint32_t packed);

// --- Per-frame world-space light registry ---------------------------------
// Gameplay registers lights while it builds the display list (before gfx_run);
// gfx_run applies them to the FX shader's four dynamic light slots and then
// clears the registry, so a light can never outlive the emitter. While the PTC
// lightshow owns the slots (it still calls gfx_opengl_light_count() directly)
// the registry is ignored, so the demo behaves exactly as before.
#define GFX_LIGHT_SLOTS 4
#define GFX_LIGHT_AUTO_MAX 8      // nearest-first candidates (flames) per frame

void gfx_light_request(int slot, const float pos[3], const float rgb[3], float radius);
void gfx_light_clear(int slot);
void gfx_light_request_auto(const float pos[3], const float rgb[3], float radius);

extern int gfx_emissive_on;    // EMISSIVE tags dial (default on)
extern int gfx_jet_light_on;       // jetpack nozzle light (default on)
extern int gfx_jet_light_intensity; // percent, default 150
extern int gfx_jet_light_radius;   // world units at full thrust, default 220
extern int gfx_portal_debug_blit;   /* debug: full-screen portal FBO */

void gfx_init(struct GfxWindowManagerAPI *wapi, struct GfxRenderingAPI *rapi, const char *window_title);
void gfx_start_frame(void);
void gfx_run(Gfx *commands);
void gfx_end_frame(void);
void gfx_precache_textures(void);
void gfx_shutdown(void);

#endif
