#include <ultra64.h>
#include <string.h>
#include <math.h>

#include "sm64.h"
#include "gfx_dimensions.h"
#include "main.h"
#include "print.h"
#include "engine/math_util.h"
#include "engine/surface_collision.h"
#include "area.h"
#include "shadow.h"
#include "memory.h"
#include "game_init.h"
#include "rendering_graph_node.h"
#include "behavior_data.h"
#include "mario.h"
#include "memory.h"
#include "../pc/gfx/gfx_pc.h"

/**
 * This file contains the code that processes the scene graph for rendering.
 * The scene graph is responsible for drawing everything except the HUD / text boxes.
 * First the root of the scene graph is processed when geo_process_root
 * is called from level_script.c. The rest of the tree is traversed recursively
 * using the function geo_process_node_and_siblings, which switches over all
 * geo node types and calls a specialized function accordingly.
 * The types are defined in engine/graph_node.h
 *
 * The scene graph typically looks like:
 * - Root (viewport)
 *  - Master list
 *   - Ortho projection
 *    - Background (skybox)
 *  - Master list
 *   - Perspective
 *    - Camera
 *     - <area-specific display lists>
 *     - Object parent
 *      - <group with 240 object nodes>
 *  - Master list
 *   - Script node (Cannon overlay)
 *
 */

s16 gMatStackIndex;
Mat4 gMatStack[32];
Mtx *gMatStackFixed[32];

// First command of the perspective 3D scene (set here, read by gfx_pc.c).
extern Gfx *gGfxShadowStart;

/**
 * Animation nodes have state in global variables, so this struct captures
 * the animation state so a 'context switch' can be made when rendering the
 * held object.
 */
struct GeoAnimState {
    /*0x00*/ u8 type;
    /*0x01*/ u8 enabled;
    /*0x02*/ s16 frame;
    /*0x04*/ f32 translationMultiplier;
    /*0x08*/ u16 *attribute;
    /*0x0C*/ s16 *data;
    s16 prevFrame; // HFR
};

// For some reason, this is a GeoAnimState struct, but the current state consists
// of separate global variables. It won't match EU otherwise.
struct GeoAnimState gGeoTempState;

u8 gCurAnimType;
u8 gCurAnimEnabled;
s16 gCurrAnimFrame;
s16 gPrevAnimFrame; // HFR: previous tick's animation frame
f32 gCurAnimTranslationMultiplier;
u16 *gCurrAnimAttribute;
s16 *gCurAnimData;

struct AllocOnlyPool *gDisplayListHeap;

struct RenderModeContainer {
    u32 modes[8];
};

/* Rendermode settings for cycle 1 for all 8 layers. */
struct RenderModeContainer renderModeTable_1Cycle[2] = { { {
    G_RM_OPA_SURF,
    G_RM_AA_OPA_SURF,
    G_RM_AA_OPA_SURF,
    G_RM_AA_OPA_SURF,
    G_RM_AA_TEX_EDGE,
    G_RM_AA_XLU_SURF,
    G_RM_AA_XLU_SURF,
    G_RM_AA_XLU_SURF,
    } },
    { {
    /* z-buffered */
    G_RM_ZB_OPA_SURF,
    G_RM_AA_ZB_OPA_SURF,
    G_RM_AA_ZB_OPA_DECAL,
    G_RM_AA_ZB_OPA_INTER,
    G_RM_AA_ZB_TEX_EDGE,
    G_RM_AA_ZB_XLU_SURF,
    G_RM_AA_ZB_XLU_DECAL,
    G_RM_AA_ZB_XLU_INTER,
    } } };

/* Rendermode settings for cycle 2 for all 8 layers. */
struct RenderModeContainer renderModeTable_2Cycle[2] = { { {
    G_RM_OPA_SURF2,
    G_RM_AA_OPA_SURF2,
    G_RM_AA_OPA_SURF2,
    G_RM_AA_OPA_SURF2,
    G_RM_AA_TEX_EDGE2,
    G_RM_AA_XLU_SURF2,
    G_RM_AA_XLU_SURF2,
    G_RM_AA_XLU_SURF2,
    } },
    { {
    /* z-buffered */
    G_RM_ZB_OPA_SURF2,
    G_RM_AA_ZB_OPA_SURF2,
    G_RM_AA_ZB_OPA_DECAL2,
    G_RM_AA_ZB_OPA_INTER2,
    G_RM_AA_ZB_TEX_EDGE2,
    G_RM_AA_ZB_XLU_SURF2,
    G_RM_AA_ZB_XLU_DECAL2,
    G_RM_AA_ZB_XLU_INTER2,
    } } };

struct GraphNodeRoot *gCurGraphNodeRoot = NULL;
struct GraphNodeMasterList *gCurGraphNodeMasterList = NULL;
struct GraphNodePerspective *gCurGraphNodeCamFrustum = NULL;
struct GraphNodeCamera *gCurGraphNodeCamera = NULL;

// Cinematic DOF auto-focus target, consumed by the screen-space FX pass.
extern float gfx_dof_focus_auto;
extern int gfx_cam_mode;
extern struct Object *gMarioObject;

// ---- HFR frame interpolation ---------------------------------------------
// The simulation runs at a fixed 30 Hz and builds one display list per tick.
// Extra frames replay that list, with every transform matrix in it lerped
// between the previous and the current tick. Two float matrix stacks are
// built in lockstep (this tick and the previous tick); each gSPMatrix that
// ends up in the list is recorded together with the previous tick's matrix
// and pointed at a scratch matrix which gfx_patch_frame() fills in.
//
// Note: on this build Mtx is float[4][4] (guMtxF2L is a memcpy), so a Mtx and
// a Mat4 have the same layout and can be blended directly.

Mat4 gMatStackPrev[32];

typedef struct {
    Mtx *dst;      // matrix the display list command points at
    Mtx cur;       // this tick's matrix
    Mtx prev;      // previous tick's matrix
} HfrMtxEntry;

// Fixed capacity: overflowing entries simply keep their current matrix.
#define HFR_MAX_MTX 4096
static HfrMtxEntry sMtxTbl[HFR_MAX_MTX];
static s32 sMtxTblSize;

// True when the stored previous tick really is the tick before this one.
// A gap means the node was skipped, teleported or reloaded: don't lerp.
// Slow motion rebuilds the display list only every gfx_slowmo_div ticks while
// gGlobalTimer still advances once per presented tick, so a node's previous
// appearance is up to div ticks old and is still valid motion.
static int hfr_prev_valid(u32 prevTimestamp) {
    extern int gfx_slowmo_div;
    u32 maxGap = (gfx_slowmo_div > 1) ? (u32) gfx_slowmo_div : 1u;
    u32 gap = gGlobalTimer - prevTimestamp;

    return gap >= 1 && gap <= maxGap;
}

// Registers a matrix that a display list command points at. The display list
// keeps pointing at `dst`; gfx_patch_frame() overwrites it in place, so the
// current tick stays available in this table for the next blend.
static void hfr_record(Mtx *dst, Mtx *prev) {
    HfrMtxEntry *e;

    if (sMtxTblSize >= HFR_MAX_MTX) {
        return;
    }
    e = &sMtxTbl[sMtxTblSize++];
    e->dst = dst;
    memcpy(&e->cur, dst, sizeof(Mtx));
    memcpy(&e->prev, prev, sizeof(Mtx));
}

// Blends every recorded matrix between the previous and the current tick.
// alpha = 0 shows the previous tick, alpha = 1 the current one.
void gfx_patch_frame(float alpha) {
    s32 i, j;

    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    for (i = 0; i < sMtxTblSize; i++) {
        HfrMtxEntry *e = &sMtxTbl[i];
        f32 *out = e->dst->m[0];
        const f32 *cur = e->cur.m[0];
        const f32 *prev = e->prev.m[0];

        for (j = 0; j < 16; j++) {
            out[j] = prev[j] + (cur[j] - prev[j]) * alpha;
        }
    }
}

static void hfr_reset(void) {
    sMtxTblSize = 0;
}

// Number of matrices interpolated in the last built display list.
int gfx_hfr_mtx_count(void) {
    return sMtxTblSize;
}

// Distance (squared) above which a jump is a teleport rather than motion.
#define HFR_SNAP_DIST_SQ 4000000.0f

static f32 hfr_dist_sq(Vec3f a, Vec3f b) {
    f32 dx = a[0] - b[0];
    f32 dy = a[1] - b[1];
    f32 dz = a[2] - b[2];

    return dx * dx + dy * dy + dz * dz;
}

struct GraphNodeObject *gCurGraphNodeObject = NULL;
struct GraphNodeHeldObject *gCurGraphNodeHeldObject = NULL;
u16 gAreaUpdateCounter = 0;

// Splat slice: true while traversing an object selected for splatting. The
// object's display list is deferred into the master list, so this is copied
// onto each DisplayListNode and emitted as a marker when the list is drawn.
static u8 gSplatCurrentObject = 0;

#ifdef F3DEX_GBI_2
LookAt lookAt;
#endif

/**
 * Process a master list node.
 */
static void geo_process_master_list_sub(struct GraphNodeMasterList *node) {
    struct DisplayListNode *currList;
    s32 i;
    s32 enableZBuffer = (node->node.flags & GRAPH_RENDER_Z_BUFFER) != 0;
    struct RenderModeContainer *modeList = &renderModeTable_1Cycle[enableZBuffer];
    struct RenderModeContainer *mode2List = &renderModeTable_2Cycle[enableZBuffer];

    // @bug This is where the LookAt values should be calculated but aren't.
    // As a result, environment mapping is broken on Fast3DEX2 without the
    // changes below.
#ifdef F3DEX_GBI_2
    Mtx lMtx;
    guLookAtReflect(&lMtx, &lookAt, 0, 0, 0, /* eye */ 0, 0, 1, /* at */ 1, 0, 0 /* up */);
#endif

    if (enableZBuffer != 0) {
        gDPPipeSync(gDisplayListHead++);
        gSPSetGeometryMode(gDisplayListHead++, G_ZBUFFER);
    }

    for (i = 0; i < GFX_NUM_MASTER_LISTS; i++) {
        if ((currList = node->listHeads[i]) != NULL) {
            gDPSetRenderMode(gDisplayListHead++, modeList->modes[i], mode2List->modes[i]);
            while (currList != NULL) {
                if (currList->splat) gfx_sp_splat_marker(1);
                gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(currList->transform),
                          G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
                gSPDisplayList(gDisplayListHead++, currList->displayList);
                if (currList->splat) gfx_sp_splat_marker(0);
                currList = currList->next;
            }
        }
    }
    if (enableZBuffer != 0) {
        gDPPipeSync(gDisplayListHead++);
        gSPClearGeometryMode(gDisplayListHead++, G_ZBUFFER);
    }
}

/**
 * Appends the display list to one of the master lists based on the layer
 * parameter. Look at the RenderModeContainer struct to see the corresponding
 * render modes of layers.
 */
static void geo_append_display_list(void *displayList, s16 layer) {

#ifdef F3DEX_GBI_2
    gSPLookAt(gDisplayListHead++, &lookAt);
#endif
    if (gCurGraphNodeMasterList != 0) {
        struct DisplayListNode *listNode =
            alloc_only_pool_alloc(gDisplayListHeap, sizeof(struct DisplayListNode));

        listNode->transform = gMatStackFixed[gMatStackIndex];
        // HFR: the master list will point at this matrix, so register it for
        // interpolation together with the previous tick's transform.
        hfr_record(listNode->transform, gMatStackPrev[gMatStackIndex]);
        listNode->displayList = displayList;
        listNode->splat = gSplatCurrentObject || (gfx_splat_target == 3);
        listNode->next = 0;
        if (gCurGraphNodeMasterList->listHeads[layer] == 0) {
            gCurGraphNodeMasterList->listHeads[layer] = listNode;
        } else {
            gCurGraphNodeMasterList->listTails[layer]->next = listNode;
        }
        gCurGraphNodeMasterList->listTails[layer] = listNode;
    }
}

/**
 * Process the master list node.
 */
static void geo_process_master_list(struct GraphNodeMasterList *node) {
    s32 i;
    UNUSED s32 sp1C;

    if (gCurGraphNodeMasterList == NULL && node->node.children != NULL) {
        gCurGraphNodeMasterList = node;
        for (i = 0; i < GFX_NUM_MASTER_LISTS; i++) {
            node->listHeads[i] = NULL;
        }
        geo_process_node_and_siblings(node->node.children);
        // The first Z-buffered master list is the perspective 3D scene (the
        // skybox/pre-render background has no Z buffer). Record where it starts
        // so the shadow pass can skip the background and 2D UI.
        if (gGfxShadowStart == NULL && (node->node.flags & GRAPH_RENDER_Z_BUFFER)) {
            gGfxShadowStart = gDisplayListHead;
        }
        geo_process_master_list_sub(node);
        gCurGraphNodeMasterList = NULL;
    }
}

/**
 * Process an orthographic projection node.
 */
static void geo_process_ortho_projection(struct GraphNodeOrthoProjection *node) {
    if (node->node.children != NULL) {
        Mtx *mtx = alloc_display_list(sizeof(*mtx));
        f32 left = (gCurGraphNodeRoot->x - gCurGraphNodeRoot->width) / 2.0f * node->scale;
        f32 right = (gCurGraphNodeRoot->x + gCurGraphNodeRoot->width) / 2.0f * node->scale;
        f32 top = (gCurGraphNodeRoot->y - gCurGraphNodeRoot->height) / 2.0f * node->scale;
        f32 bottom = (gCurGraphNodeRoot->y + gCurGraphNodeRoot->height) / 2.0f * node->scale;

        guOrtho(mtx, left, right, bottom, top, -2.0f, 2.0f, 1.0f);
        gSPPerspNormalize(gDisplayListHead++, 0xFFFF);
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(mtx), G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);

        geo_process_node_and_siblings(node->node.children);
    }
}

/**
 * Process a perspective projection node.
 */
static void geo_process_perspective(struct GraphNodePerspective *node) {
    if (node->fnNode.func != NULL) {
        node->fnNode.func(GEO_CONTEXT_RENDER, &node->fnNode.node, gMatStack[gMatStackIndex]);
    }
    if (node->fnNode.node.children != NULL) {
        u16 perspNorm, prevPerspNorm;
        Mtx *mtx = alloc_display_list(sizeof(*mtx));
        Mtx *prevMtx = alloc_display_list(sizeof(*prevMtx));
        f32 prevFov;

        // Real canvas aspect (root dims are the fixed 320x240 game resolution,
        // which forced a 4:3 projection into a 16:9 view).
        f32 aspect = gfx_current_dimensions.aspect_ratio;
        if (aspect < 0.1f) {
            aspect = (f32) gCurGraphNodeRoot->width / (f32) gCurGraphNodeRoot->height;
        }

        // HFR: the fov is animated by the camera (zoom out at speed), so it
        // has to be interpolated as well.
        prevFov = hfr_prev_valid(node->prevTimestamp) ? node->prevFov : node->fov;
        node->prevFov = node->fov;
        node->prevTimestamp = gGlobalTimer;

        guPerspective(mtx, &perspNorm, node->fov, aspect, node->near, node->far, 1.0f);
        guPerspective(prevMtx, &prevPerspNorm, prevFov, aspect, node->near, node->far, 1.0f);

        // Remember the projection so the Android screen-space effects can
        // linearise the depth buffer and rebuild view-space positions.
        gfx_cam_near = node->near;
        gfx_cam_far = node->far;
        gfx_cam_tan_half_fov_y = tanf(node->fov * 3.14159265f / 360.0f);
        gSPPerspNormalize(gDisplayListHead++, perspNorm);

        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(mtx),
                  G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
        hfr_record(mtx, prevMtx);

        gCurGraphNodeCamFrustum = node;
        geo_process_node_and_siblings(node->fnNode.node.children);
        gCurGraphNodeCamFrustum = NULL;
    }
}

/**
 * Process a level of detail node. From the current transformation matrix,
 * the perpendicular distance to the camera is extracted and the children
 * of this node are only processed if that distance is within the render
 * range of this node.
 */
static void geo_process_level_of_detail(struct GraphNodeLevelOfDetail *node) {
    // The fixed point Mtx type is defined as 16 longs, but it's actually 16
    // shorts for the integer parts followed by 16 shorts for the fraction parts
    s16 *mtx = (s16 *) gMatStackFixed[gMatStackIndex];
    s16 distanceFromCam = -mtx[14]; // z-component of the translation column

    if (node->minDistance <= distanceFromCam && distanceFromCam < node->maxDistance) {
        if (node->node.children != 0) {
            geo_process_node_and_siblings(node->node.children);
        }
    }
}

/**
 * Process a switch case node. The node's selection function is called
 * if it is 0, and among the node's children, only the selected child is
 * processed next.
 */
static void geo_process_switch(struct GraphNodeSwitchCase *node) {
    struct GraphNode *selectedChild = node->fnNode.node.children;
    s32 i;

    if (node->fnNode.func != NULL) {
        node->fnNode.func(GEO_CONTEXT_RENDER, &node->fnNode.node, gMatStack[gMatStackIndex]);
    }
    for (i = 0; selectedChild != NULL && node->selectedCase > i; i++) {
        selectedChild = selectedChild->next;
    }
    if (selectedChild != NULL) {
        geo_process_node_and_siblings(selectedChild);
    }
}

/* ---------------- Realtime shadow map: camera/light matrices -------------
 * Row-vector 4x4 matrices (v' = v * M, translation in row 3), the same
 * convention as the N64 matrices and gfx_pc.c.
 *
 * gfx_cam_inv_mtx maps view space -> world space (inverse of the camera view),
 * gfx_shadow_light_vp maps world space -> light clip space. The shadow map is
 * rendered by gfx_pc.c replaying the scene range with
 *   clip = object * modelview * cam_inv * light_vp
 * and the screen-space post pass reconstructs world positions from depth with
 * the same cam_inv, then samples the map.
 */
float gfx_cam_inv_mtx[4][4];
float gfx_shadow_light_vp[4][4];
float gfx_shadow_light_dir[3];   // world-space direction towards the light
int gfx_shadow_matrices_ready = 0;

extern int gfx_shadow_enabled;
extern int gfx_shadow_yaw_deg;
extern int gfx_shadow_pitch_deg;
extern int gfx_shadow_range;

static void shadow_make_camera_inv(Mat4 dest, Mat4 viewMtx, Vec3f cameraPos) {
    // Inverse of a rigid view matrix: transpose the rotation, keep the camera
    // position as the translation.
    dest[0][0] = viewMtx[0][0];
    dest[0][1] = viewMtx[1][0];
    dest[0][2] = viewMtx[2][0];
    dest[0][3] = 0.0f;

    dest[1][0] = viewMtx[0][1];
    dest[1][1] = viewMtx[1][1];
    dest[1][2] = viewMtx[2][1];
    dest[1][3] = 0.0f;

    dest[2][0] = viewMtx[0][2];
    dest[2][1] = viewMtx[1][2];
    dest[2][2] = viewMtx[2][2];
    dest[2][3] = 0.0f;

    dest[3][0] = cameraPos[0];
    dest[3][1] = cameraPos[1];
    dest[3][2] = cameraPos[2];
    dest[3][3] = 1.0f;
}

static void shadow_make_ortho(Mat4 dest, f32 left, f32 right, f32 bottom, f32 top,
                              f32 near, f32 far) {
    mtxf_identity(dest);
    dest[0][0] = 2.0f / (right - left);
    dest[1][1] = 2.0f / (top - bottom);
    dest[2][2] = -2.0f / (far - near);
    dest[3][0] = -(right + left) / (right - left);
    dest[3][1] = -(top + bottom) / (top - bottom);
    dest[3][2] = -(far + near) / (far - near);
}

static void shadow_update_light_matrices(Vec3f cameraPos) {
    Vec3f center;
    Vec3f lightPos;
    Mat4 lightView;
    Mat4 lightProj;
    f32 yaw = (f32)gfx_shadow_yaw_deg * 3.14159265f / 180.0f;
    f32 pitch = (f32)gfx_shadow_pitch_deg * 3.14159265f / 180.0f;
    f32 range = (f32)gfx_shadow_range;
    f32 dist = 6000.0f;

    // Follow Mario so the map's limited resolution is spent around the action.
    if (gMarioObject != NULL) {
        vec3f_copy(center, gMarioObject->header.gfx.pos);
        center[1] += 400.0f;
    } else {
        vec3f_copy(center, cameraPos);
    }

    // dir points from the scene towards the light (elevation above the horizon).
    gfx_shadow_light_dir[0] = sinf(yaw) * cosf(pitch);
    gfx_shadow_light_dir[1] = sinf(pitch);
    gfx_shadow_light_dir[2] = cosf(yaw) * cosf(pitch);

    lightPos[0] = center[0] + gfx_shadow_light_dir[0] * dist;
    lightPos[1] = center[1] + gfx_shadow_light_dir[1] * dist;
    lightPos[2] = center[2] + gfx_shadow_light_dir[2] * dist;

    mtxf_lookat(lightView, lightPos, center, 0);
    shadow_make_ortho(lightProj, -range, range, -range, range, 100.0f, 12000.0f);
    mtxf_mul(gfx_shadow_light_vp, lightView, lightProj);
}

/**
 * Process a camera node.
 */
static void geo_process_camera(struct GraphNodeCamera *node) {
    Mat4 cameraTransform;
    Mtx *rollMtx = alloc_display_list(sizeof(*rollMtx));
    Mtx *mtx = alloc_display_list(sizeof(*mtx));
    Vec3f prevPos;
    Vec3f prevFocus;

    if (node->fnNode.func != NULL) {
        node->fnNode.func(GEO_CONTEXT_RENDER, &node->fnNode.node, gMatStack[gMatStackIndex]);
    }
    mtxf_rotate_xy(rollMtx, node->rollScreen);

    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(rollMtx), G_MTX_PROJECTION | G_MTX_MUL | G_MTX_NOPUSH);

    // HFR: the view matrix is baked into every modelview matrix below, so
    // interpolating the previous-tick stack also smooths the camera.
    if (hfr_prev_valid(node->prevTimestamp)) {
        vec3f_copy(prevPos, node->prevPos);
        vec3f_copy(prevFocus, node->prevFocus);
        // A big jump is a camera cut (warp, door, cutscene), not motion.
        if (hfr_dist_sq(prevPos, node->pos) > 500000.0f) {
            vec3f_copy(prevPos, node->pos);
            vec3f_copy(prevFocus, node->focus);
        }
    } else {
        vec3f_copy(prevPos, node->pos);
        vec3f_copy(prevFocus, node->focus);
    }
    vec3f_copy(node->prevPos, node->pos);
    vec3f_copy(node->prevFocus, node->focus);
    node->prevTimestamp = gGlobalTimer;

    // Cinematic DOF auto-focus: distance to Mario's centre, pulled a little
    // closer to the camera by his hitbox radius so his front surface is sharp.
    if (gfx_cam_mode == 4) {
        /* First person: Mario is the camera, so focus on whatever the view
         * ray hits instead.  TOEAUTO and the reticle depth use this value. */
        Vec3f dir, hit;
        struct Surface *surf = NULL;
        f32 dx = node->focus[0] - node->pos[0];
        f32 dy = node->focus[1] - node->pos[1];
        f32 dz = node->focus[2] - node->pos[2];
        f32 len = sqrtf(dx * dx + dy * dy + dz * dz);
        f32 focusDist = 1200.0f;

        if (len > 1.0f) {
            dir[0] = dx * (3000.0f / len);
            dir[1] = dy * (3000.0f / len);
            dir[2] = dz * (3000.0f / len);
            find_surface_on_ray(node->pos, dir, &surf, hit);
            if (surf != NULL) {
                focusDist = sqrtf((hit[0] - node->pos[0]) * (hit[0] - node->pos[0])
                                + (hit[1] - node->pos[1]) * (hit[1] - node->pos[1])
                                + (hit[2] - node->pos[2]) * (hit[2] - node->pos[2]));
            }
        }
        if (focusDist < 50.0f) focusDist = 50.0f;
        if (focusDist > 3000.0f) focusDist = 3000.0f;
        gfx_dof_focus_auto = focusDist;
    } else if (gMarioObject != NULL) {
        Vec3f marioCentre;
        f32 focusDist;

        marioCentre[0] = gMarioObject->header.gfx.pos[0];
        marioCentre[1] = gMarioObject->header.gfx.pos[1] + gMarioObject->hitboxHeight * 0.5f;
        marioCentre[2] = gMarioObject->header.gfx.pos[2];
        focusDist = sqrtf(hfr_dist_sq(node->pos, marioCentre)) - gMarioObject->hitboxRadius;
        if (focusDist < 50.0f) {
            focusDist = 50.0f;
        }
        gfx_dof_focus_auto = focusDist;
    }

    mtxf_lookat(cameraTransform, node->pos, node->focus, node->roll);
    /* The post pass needs camera->world for world reconstruction (dynamic
     * lights, probes, motion blur) even when the shadow map is off, so keep
     * this unconditional; only the light matrices are shadow-specific. */
    shadow_make_camera_inv(gfx_cam_inv_mtx, cameraTransform, node->pos);
    if (gfx_shadow_enabled) {
        shadow_update_light_matrices(node->pos);
        gfx_shadow_matrices_ready = 1;
    }
    mtxf_mul(gMatStack[gMatStackIndex + 1], cameraTransform, gMatStack[gMatStackIndex]);
    mtxf_lookat(cameraTransform, prevPos, prevFocus, node->roll);
    mtxf_mul(gMatStackPrev[gMatStackIndex + 1], cameraTransform, gMatStackPrev[gMatStackIndex]);
    gMatStackIndex++;
    mtxf_to_mtx(mtx, gMatStack[gMatStackIndex]);
    gMatStackFixed[gMatStackIndex] = mtx;
    if (node->fnNode.node.children != 0) {
        gCurGraphNodeCamera = node;
        node->matrixPtr = gMatStack[gMatStackIndex];
        node->prevMatrixPtr = gMatStackPrev[gMatStackIndex];
        geo_process_node_and_siblings(node->fnNode.node.children);
        gCurGraphNodeCamera = NULL;
    }
    gMatStackIndex--;
}

/**
 * Process a translation / rotation node. A transformation matrix based
 * on the node's translation and rotation is created and pushed on both
 * the float and fixed point matrix stacks.
 * For the rest it acts as a normal display list node.
 */
static void geo_process_translation_rotation(struct GraphNodeTranslationRotation *node) {
    Mat4 mtxf;
    Vec3f translation;
    Mtx *mtx = alloc_display_list(sizeof(*mtx));

    vec3s_to_vec3f(translation, node->translation);
    mtxf_rotate_zxy_and_translate(mtxf, translation, node->rotation);
    mtxf_mul(gMatStack[gMatStackIndex + 1], mtxf, gMatStack[gMatStackIndex]);
    mtxf_mul(gMatStackPrev[gMatStackIndex + 1], mtxf, gMatStackPrev[gMatStackIndex]);
    gMatStackIndex++;
    mtxf_to_mtx(mtx, gMatStack[gMatStackIndex]);
    gMatStackFixed[gMatStackIndex] = mtx;
    if (node->displayList != NULL) {
        geo_append_display_list(node->displayList, node->node.flags >> 8);
    }
    if (node->node.children != NULL) {
        geo_process_node_and_siblings(node->node.children);
    }
    gMatStackIndex--;
}

/**
 * Process a translation node. A transformation matrix based on the node's
 * translation is created and pushed on both the float and fixed point matrix stacks.
 * For the rest it acts as a normal display list node.
 */
static void geo_process_translation(struct GraphNodeTranslation *node) {
    Mat4 mtxf;
    Vec3f translation;
    Mtx *mtx = alloc_display_list(sizeof(*mtx));

    vec3s_to_vec3f(translation, node->translation);
    mtxf_rotate_zxy_and_translate(mtxf, translation, gVec3sZero);
    mtxf_mul(gMatStack[gMatStackIndex + 1], mtxf, gMatStack[gMatStackIndex]);
    mtxf_mul(gMatStackPrev[gMatStackIndex + 1], mtxf, gMatStackPrev[gMatStackIndex]);
    gMatStackIndex++;
    mtxf_to_mtx(mtx, gMatStack[gMatStackIndex]);
    gMatStackFixed[gMatStackIndex] = mtx;
    if (node->displayList != NULL) {
        geo_append_display_list(node->displayList, node->node.flags >> 8);
    }
    if (node->node.children != NULL) {
        geo_process_node_and_siblings(node->node.children);
    }
    gMatStackIndex--;
}

/**
 * Process a rotation node. A transformation matrix based on the node's
 * rotation is created and pushed on both the float and fixed point matrix stacks.
 * For the rest it acts as a normal display list node.
 */
static void geo_process_rotation(struct GraphNodeRotation *node) {
    Mat4 mtxf;
    Vec3s prevRotation;
    Mtx *mtx = alloc_display_list(sizeof(*mtx));

    // HFR: angles are interpolated at the source (shortest arc) so the blend
    // never takes the long way round.
    if (hfr_prev_valid(node->prevTimestamp)) {
        vec3s_copy(prevRotation, node->prevRotation);
    } else {
        vec3s_copy(prevRotation, node->rotation);
    }
    vec3s_copy(node->prevRotation, node->rotation);
    node->prevTimestamp = gGlobalTimer;

    mtxf_rotate_zxy_and_translate(mtxf, gVec3fZero, node->rotation);
    mtxf_mul(gMatStack[gMatStackIndex + 1], mtxf, gMatStack[gMatStackIndex]);
    mtxf_rotate_zxy_and_translate(mtxf, gVec3fZero, prevRotation);
    mtxf_mul(gMatStackPrev[gMatStackIndex + 1], mtxf, gMatStackPrev[gMatStackIndex]);
    gMatStackIndex++;
    mtxf_to_mtx(mtx, gMatStack[gMatStackIndex]);
    gMatStackFixed[gMatStackIndex] = mtx;
    if (node->displayList != NULL) {
        geo_append_display_list(node->displayList, node->node.flags >> 8);
    }
    if (node->node.children != NULL) {
        geo_process_node_and_siblings(node->node.children);
    }
    gMatStackIndex--;
}

/**
 * Process a scaling node. A transformation matrix based on the node's
 * scale is created and pushed on both the float and fixed point matrix stacks.
 * For the rest it acts as a normal display list node.
 */
static void geo_process_scale(struct GraphNodeScale *node) {
    UNUSED Mat4 transform;
    Vec3f scaleVec;
    Mtx *mtx = alloc_display_list(sizeof(*mtx));

    vec3f_set(scaleVec, node->scale, node->scale, node->scale);
    mtxf_scale_vec3f(gMatStack[gMatStackIndex + 1], gMatStack[gMatStackIndex], scaleVec);
    mtxf_scale_vec3f(gMatStackPrev[gMatStackIndex + 1], gMatStackPrev[gMatStackIndex], scaleVec);
    gMatStackIndex++;
    mtxf_to_mtx(mtx, gMatStack[gMatStackIndex]);
    gMatStackFixed[gMatStackIndex] = mtx;
    if (node->displayList != NULL) {
        geo_append_display_list(node->displayList, node->node.flags >> 8);
    }
    if (node->node.children != NULL) {
        geo_process_node_and_siblings(node->node.children);
    }
    gMatStackIndex--;
}

/**
 * Process a billboard node. A transformation matrix is created that makes its
 * children face the camera, and it is pushed on the floating point and fixed
 * point matrix stacks.
 * For the rest it acts as a normal display list node.
 */
static void geo_process_billboard(struct GraphNodeBillboard *node) {
    Vec3f translation;
    Mtx *mtx = alloc_display_list(sizeof(*mtx));

    gMatStackIndex++;
    vec3s_to_vec3f(translation, node->translation);
    mtxf_billboard(gMatStack[gMatStackIndex], gMatStack[gMatStackIndex - 1], translation,
                   gCurGraphNodeCamera->roll);
    mtxf_billboard(gMatStackPrev[gMatStackIndex], gMatStackPrev[gMatStackIndex - 1], translation,
                   gCurGraphNodeCamera->roll);
    if (gCurGraphNodeHeldObject != NULL) {
        mtxf_scale_vec3f(gMatStack[gMatStackIndex], gMatStack[gMatStackIndex],
                         gCurGraphNodeHeldObject->objNode->header.gfx.scale);
        mtxf_scale_vec3f(gMatStackPrev[gMatStackIndex], gMatStackPrev[gMatStackIndex],
                         gCurGraphNodeHeldObject->objNode->header.gfx.scale);
    } else if (gCurGraphNodeObject != NULL) {
        Vec3f *prevScale = &gCurGraphNodeObject->prevScale;

        mtxf_scale_vec3f(gMatStack[gMatStackIndex], gMatStack[gMatStackIndex],
                         gCurGraphNodeObject->scale);
        if (!hfr_prev_valid(gCurGraphNodeObject->prevScaleTimestamp)) {
            prevScale = &gCurGraphNodeObject->scale;
        }
        mtxf_scale_vec3f(gMatStackPrev[gMatStackIndex], gMatStackPrev[gMatStackIndex], *prevScale);
    }

    mtxf_to_mtx(mtx, gMatStack[gMatStackIndex]);
    gMatStackFixed[gMatStackIndex] = mtx;
    if (node->displayList != NULL) {
        geo_append_display_list(node->displayList, node->node.flags >> 8);
    }
    if (node->node.children != NULL) {
        geo_process_node_and_siblings(node->node.children);
    }
    gMatStackIndex--;
}

/**
 * Process a display list node. It draws a display list without first pushing
 * a transformation on the stack, so all transformations are inherited from the
 * parent node. It processes its children if it has them.
 */
static void geo_process_display_list(struct GraphNodeDisplayList *node) {
    if (node->displayList != NULL) {
        geo_append_display_list(node->displayList, node->node.flags >> 8);
    }
    if (node->node.children != NULL) {
        geo_process_node_and_siblings(node->node.children);
    }
}

/**
 * Process a generated list. Instead of storing a pointer to a display list,
 * the list is generated on the fly by a function.
 */
static void geo_process_generated_list(struct GraphNodeGenerated *node) {
    if (node->fnNode.func != NULL) {
        Gfx *list = node->fnNode.func(GEO_CONTEXT_RENDER, &node->fnNode.node,
                                     (struct AllocOnlyPool *) gMatStack[gMatStackIndex]);

        if (list != 0) {
            geo_append_display_list((void *) VIRTUAL_TO_PHYSICAL(list), node->fnNode.node.flags >> 8);
        }
    }
    if (node->fnNode.node.children != NULL) {
        geo_process_node_and_siblings(node->fnNode.node.children);
    }
}

/**
 * Process a background node. Tries to retrieve a background display list from
 * the function of the node. If that function is null or returns null, a black
 * rectangle is drawn instead.
 */
static void geo_process_background(struct GraphNodeBackground *node) {
    Gfx *list = NULL;

    if (node->fnNode.func != NULL) {
        list = node->fnNode.func(GEO_CONTEXT_RENDER, &node->fnNode.node,
                                 (struct AllocOnlyPool *) gMatStack[gMatStackIndex]);
    }
    if (list != 0) {
        geo_append_display_list((void *) VIRTUAL_TO_PHYSICAL(list), node->fnNode.node.flags >> 8);
    } else if (gCurGraphNodeMasterList != NULL) {
        Gfx *gfxStart = alloc_display_list(sizeof(Gfx) * 8);
        Gfx *gfx = gfxStart;

        gDPPipeSync(gfx++);
        gDPSetCycleType(gfx++, G_CYC_FILL);
        gDPSetFillColor(gfx++, node->background);
        gDPFillRectangle(gfx++, GFX_DIMENSIONS_RECT_FROM_LEFT_EDGE(0), BORDER_HEIGHT,
        GFX_DIMENSIONS_RECT_FROM_RIGHT_EDGE(0) - 1, SCREEN_HEIGHT - BORDER_HEIGHT - 1);
        gDPPipeSync(gfx++);
        gDPSetCycleType(gfx++, G_CYC_1CYCLE);
        gSPEndDisplayList(gfx++);

        geo_append_display_list((void *) VIRTUAL_TO_PHYSICAL(gfxStart), 0);
    }
    if (node->fnNode.node.children != NULL) {
        geo_process_node_and_siblings(node->fnNode.node.children);
    }
}

// Walks one entry of the animation stream and returns the part's local
// translation/rotation. The stream position (animAttribute) advances, so the
// previous tick and the current tick must each be walked from the same start.
static void anim_process(Vec3f translation, Vec3s rotation, u8 *animType, s16 animFrame,
                         u16 **animAttribute) {
    if (*animType == ANIM_TYPE_TRANSLATION) {
        translation[0] += gCurAnimData[retrieve_animation_index(animFrame, animAttribute)]
                          * gCurAnimTranslationMultiplier;
        translation[1] += gCurAnimData[retrieve_animation_index(animFrame, animAttribute)]
                          * gCurAnimTranslationMultiplier;
        translation[2] += gCurAnimData[retrieve_animation_index(animFrame, animAttribute)]
                          * gCurAnimTranslationMultiplier;
        *animType = ANIM_TYPE_ROTATION;
    } else {
        if (*animType == ANIM_TYPE_LATERAL_TRANSLATION) {
            translation[0] +=
                gCurAnimData[retrieve_animation_index(animFrame, animAttribute)]
                * gCurAnimTranslationMultiplier;
            *animAttribute += 2;
            translation[2] +=
                gCurAnimData[retrieve_animation_index(animFrame, animAttribute)]
                * gCurAnimTranslationMultiplier;
            *animType = ANIM_TYPE_ROTATION;
        } else {
            if (*animType == ANIM_TYPE_VERTICAL_TRANSLATION) {
                *animAttribute += 2;
                translation[1] +=
                    gCurAnimData[retrieve_animation_index(animFrame, animAttribute)]
                    * gCurAnimTranslationMultiplier;
                *animAttribute += 2;
                *animType = ANIM_TYPE_ROTATION;
            } else if (*animType == ANIM_TYPE_NO_TRANSLATION) {
                *animAttribute += 6;
                *animType = ANIM_TYPE_ROTATION;
            }
        }
    }

    if (*animType == ANIM_TYPE_ROTATION) {
        rotation[0] = gCurAnimData[retrieve_animation_index(animFrame, animAttribute)];
        rotation[1] = gCurAnimData[retrieve_animation_index(animFrame, animAttribute)];
        rotation[2] = gCurAnimData[retrieve_animation_index(animFrame, animAttribute)];
    }
}

/**
 * Render an animated part. The current animation state is not part of the node
 * but set in global variables. If an animated part is skipped, everything afterwards desyncs.
 */
static void geo_process_animated_part(struct GraphNodeAnimatedPart *node) {
    Mat4 matrix;
    Vec3s rotation;
    Vec3s prevRotation;
    Vec3f translation;
    Vec3f prevTranslation;
    Mtx *matrixPtr = alloc_display_list(sizeof(*matrixPtr));
    u16 *animAttribute = gCurrAnimAttribute;
    u8 animType = gCurAnimType;

    vec3s_copy(rotation, gVec3sZero);
    vec3f_set(translation, node->translation[0], node->translation[1], node->translation[2]);
    vec3s_copy(prevRotation, rotation);
    vec3f_copy(prevTranslation, translation);

    // HFR: walk the same stream for the previous frame first, then for the
    // current one (which also advances the real stream state as before).
    anim_process(prevTranslation, prevRotation, &animType, gPrevAnimFrame, &animAttribute);
    anim_process(translation, rotation, &gCurAnimType, gCurrAnimFrame, &gCurrAnimAttribute);

    mtxf_rotate_xyz_and_translate(matrix, translation, rotation);
    mtxf_mul(gMatStack[gMatStackIndex + 1], matrix, gMatStack[gMatStackIndex]);
    mtxf_rotate_xyz_and_translate(matrix, prevTranslation, prevRotation);
    mtxf_mul(gMatStackPrev[gMatStackIndex + 1], matrix, gMatStackPrev[gMatStackIndex]);
    gMatStackIndex++;
    mtxf_to_mtx(matrixPtr, gMatStack[gMatStackIndex]);
    gMatStackFixed[gMatStackIndex] = matrixPtr;
    if (node->displayList != NULL) {
        geo_append_display_list(node->displayList, node->node.flags >> 8);
    }
    if (node->node.children != NULL) {
        geo_process_node_and_siblings(node->node.children);
    }
    gMatStackIndex--;
}

/**
 * Initialize the animation-related global variables for the currently drawn
 * object's animation.
 */
void geo_set_animation_globals(struct GraphNodeObject_sub *node, s32 hasAnimation) {
    struct Animation *anim = node->curAnim;

    if (hasAnimation != 0) {
        node->animFrame = geo_update_animation_frame(node, &node->animFrameAccelAssist);
    }
    node->animTimer = gAreaUpdateCounter;
    if (anim->flags & ANIM_FLAG_HOR_TRANS) {
        gCurAnimType = ANIM_TYPE_VERTICAL_TRANSLATION;
    } else if (anim->flags & ANIM_FLAG_VERT_TRANS) {
        gCurAnimType = ANIM_TYPE_LATERAL_TRANSLATION;
    } else if (anim->flags & ANIM_FLAG_6) {
        gCurAnimType = ANIM_TYPE_NO_TRANSLATION;
    } else {
        gCurAnimType = ANIM_TYPE_TRANSLATION;
    }

    gCurrAnimFrame = node->animFrame;
    // HFR: the previous frame is only usable if it belongs to the same
    // animation and the previous tick (otherwise the blend would smear
    // across an animation change).
    if (node->prevAnimPtr == anim && node->prevAnimID == node->animID
        && hfr_prev_valid(node->prevAnimFrameTimestamp)) {
        gPrevAnimFrame = node->prevAnimFrame;
    } else {
        gPrevAnimFrame = node->animFrame;
    }
    node->prevAnimPtr = anim;
    node->prevAnimID = node->animID;
    node->prevAnimFrame = node->animFrame;
    node->prevAnimFrameTimestamp = gGlobalTimer;
    gCurAnimEnabled = (anim->flags & ANIM_FLAG_5) == 0;
    gCurrAnimAttribute = segmented_to_virtual((void *) anim->index);
    gCurAnimData = segmented_to_virtual((void *) anim->values);

    if (anim->unk02 == 0) {
        gCurAnimTranslationMultiplier = 1.0f;
    } else {
        gCurAnimTranslationMultiplier = (f32) node->animYTrans / (f32) anim->unk02;
    }
}

/**
 * Process a shadow node. Renders a shadow under an object offset by the
 * translation of the first animated component and rotated according to
 * the floor below it.
 */
static void geo_process_shadow(struct GraphNodeShadow *node) {
    Gfx *shadowList;
    Mat4 mtxf;
    Vec3f shadowPos;
    Vec3f prevShadowPos;
    Vec3f animOffset;
    f32 objScale;
    f32 shadowScale;
    f32 sinAng;
    f32 cosAng;
    struct GraphNode *geo;
    Mtx *mtx;

    if (gCurGraphNodeCamera != NULL && gCurGraphNodeObject != NULL) {
        if (gCurGraphNodeHeldObject != NULL) {
            get_pos_from_transform_mtx(shadowPos, gMatStack[gMatStackIndex],
                                       gCurGraphNodeCamera->matrixPtr);
            shadowScale = node->shadowScale;
        } else {
            vec3f_copy(shadowPos, gCurGraphNodeObject->pos);
            shadowScale = node->shadowScale * gCurGraphNodeObject->scale[0];
        }

        objScale = 1.0f;
        if (gCurAnimEnabled != 0) {
            if (gCurAnimType == ANIM_TYPE_TRANSLATION
                || gCurAnimType == ANIM_TYPE_LATERAL_TRANSLATION) {
                geo = node->node.children;
                if (geo != NULL && geo->type == GRAPH_NODE_TYPE_SCALE) {
                    objScale = ((struct GraphNodeScale *) geo)->scale;
                }
                animOffset[0] =
                    gCurAnimData[retrieve_animation_index(gCurrAnimFrame, &gCurrAnimAttribute)]
                    * gCurAnimTranslationMultiplier * objScale;
                animOffset[1] = 0.0f;
                gCurrAnimAttribute += 2;
                animOffset[2] =
                    gCurAnimData[retrieve_animation_index(gCurrAnimFrame, &gCurrAnimAttribute)]
                    * gCurAnimTranslationMultiplier * objScale;
                gCurrAnimAttribute -= 6;

                // simple matrix rotation so the shadow offset rotates along with the object
                sinAng = sins(gCurGraphNodeObject->angle[1]);
                cosAng = coss(gCurGraphNodeObject->angle[1]);

                shadowPos[0] += animOffset[0] * cosAng + animOffset[2] * sinAng;
                shadowPos[2] += -animOffset[0] * sinAng + animOffset[2] * cosAng;
            }
        }

        // HFR: the shadow position is not part of the node hierarchy, so it
        // needs its own previous-tick value.
        if (gCurGraphNodeHeldObject != NULL) {
            vec3f_copy(prevShadowPos, shadowPos);
        } else {
            if (hfr_prev_valid(gCurGraphNodeObject->prevShadowPosTimestamp)) {
                vec3f_copy(prevShadowPos, gCurGraphNodeObject->prevShadowPos);
            } else {
                vec3f_copy(prevShadowPos, shadowPos);
            }
            vec3f_copy(gCurGraphNodeObject->prevShadowPos, shadowPos);
            gCurGraphNodeObject->prevShadowPosTimestamp = gGlobalTimer;
        }

        shadowList = create_shadow_below_xyz(shadowPos[0], shadowPos[1], shadowPos[2], shadowScale,
                                             node->shadowSolidity, node->shadowType);
        if (shadowList != NULL) {
            mtx = alloc_display_list(sizeof(*mtx));
            gMatStackIndex++;
            mtxf_translate(mtxf, shadowPos);
            mtxf_mul(gMatStack[gMatStackIndex], mtxf, gCurGraphNodeCamera->matrixPtr);
            mtxf_to_mtx(mtx, gMatStack[gMatStackIndex]);
            gMatStackFixed[gMatStackIndex] = mtx;
            mtxf_translate(mtxf, prevShadowPos);
            mtxf_mul(gMatStackPrev[gMatStackIndex], mtxf, gCurGraphNodeCamera->prevMatrixPtr);
            if (gShadowAboveWaterOrLava == 1) {
                geo_append_display_list((void *) VIRTUAL_TO_PHYSICAL(shadowList), 4);
            } else if (gMarioOnIceOrCarpet == 1) {
                geo_append_display_list((void *) VIRTUAL_TO_PHYSICAL(shadowList), 5);
            } else {
                geo_append_display_list((void *) VIRTUAL_TO_PHYSICAL(shadowList), 6);
            }
            gMatStackIndex--;
        }
    }
    if (node->node.children != NULL) {
        geo_process_node_and_siblings(node->node.children);
    }
}

/**
 * Check whether an object is in view to determine whether it should be drawn.
 * This is known as frustrum culling.
 * It checks whether the object is far away, very close / behind the camera,
 * or horizontally out of view. It does not check whether it is vertically
 * out of view. It assumes a sphere of 300 units around the object's position
 * unless the object has a culling radius node that specifies otherwise.
 *
 * The matrix parameter should be the top of the matrix stack, which is the
 * object's transformation matrix times the camera 'look-at' matrix. The math
 * is counter-intuitive, but it checks column 3 (translation vector) of this
 * matrix to determine where the origin (0,0,0) in object space will be once
 * transformed to camera space (x+ = right, y+ = up, z = 'coming out the screen').
 * In 3D graphics, you typically model the world as being moved in front of a
 * static camera instead of a moving camera through a static world, which in
 * this case simplifies calculations. Note that the perspective matrix is not
 * on the matrix stack, so there are still calculations with the fov to compute
 * the slope of the lines of the frustrum.
 *
 *        z-
 *
 *  \     |     /
 *   \    |    /
 *    \   |   /
 *     \  |  /
 *      \ | /
 *       \|/
 *        C       x+
 *
 * Since (0,0,0) is unaffected by rotation, columns 0, 1 and 2 are ignored.
 */
extern int gfx_stereo_enabled;
extern int gfx_stereo_nocull;
extern float gfx_stereo_shear;

static int obj_is_in_view(struct GraphNodeObject *node, Mat4 matrix) {
    s16 cullingRadius;
    s16 halfFov; // half of the fov in in-game angle units instead of degrees
    struct GraphNode *geo;
    f32 hScreenEdge;

    if (node->node.flags & GRAPH_RENDER_INVISIBLE) {
        return FALSE;
    }
    if (gfx_stereo_nocull) {
        return TRUE;
    }

    geo = node->sharedChild;

    // ! @bug The aspect ratio is not accounted for. When the fov value is 45,
    // the horizontal effective fov is actually 60 degrees, so you can see objects
    // visibly pop in or out at the edge of the screen.
    halfFov = (gCurGraphNodeCamFrustum->fov / 2.0f + 1.0f) * 32768.0f / 180.0f + 0.5f;

    hScreenEdge = -matrix[3][2] * sins(halfFov) / coss(halfFov);
    // -matrix[3][2] is the depth, which gets multiplied by tan(halfFov) to get
    // the amount of units between the center of the screen and the horizontal edge
    // given the distance from the object to the camera.

    hScreenEdge *= GFX_DIMENSIONS_ASPECT_RATIO;

    if (gfx_stereo_enabled) {
        // The display list is replayed for both eyes with a clip-space shear;
        // culling must include the union of both eye frusta, otherwise geometry
        // pops out at the edges while panning.
        float inflate = 1.0f + fabsf(gfx_stereo_shear);
        hScreenEdge *= inflate;
    }

    if (geo != NULL && geo->type == GRAPH_NODE_TYPE_CULLING_RADIUS) {
        cullingRadius =
            (f32)((struct GraphNodeCullingRadius *) geo)->cullingRadius; //! Why is there a f32 cast?
    } else {
        cullingRadius = 300;
    }

    // Don't render if the object is close to or behind the camera
    if (matrix[3][2] > -100.0f + cullingRadius) {
        return FALSE;
    }

    //! This makes the HOLP not update when the camera is far away, and it
    //  makes PU travel safe when the camera is locked on the main map.
    //  If Mario were rendered with a depth over 65536 it would cause overflow
    //  when converting the transformation matrix to a fixed point matrix.
    if (matrix[3][2] < -20000.0f - cullingRadius) {
        return FALSE;
    }

    // Check whether the object is horizontally in view
    if (matrix[3][0] > hScreenEdge + cullingRadius) {
        return FALSE;
    }
    if (matrix[3][0] < -hScreenEdge - cullingRadius) {
        return FALSE;
    }
    return TRUE;
}

/**
 * Process an object node.
 */
static int splat_object_selected(struct Object *node) {
    switch (gfx_splat_target) {
        case 1:  // Mario's model
            return node == gMarioObject;
        case 2:  // any tree object
            return node->behavior == segmented_to_virtual(bhvTree);
        case 3:  // every object (level geometry untouched)
            return 1;
        default:
            return 0;
    }
}

static void geo_process_object(struct Object *node) {
    Mat4 mtxf;
    s32 hasAnimation = (node->header.gfx.node.flags & GRAPH_RENDER_HAS_ANIMATION) != 0;
    Vec3f prevPos;
    Vec3s prevAngle;
    Vec3f prevScale;

    if (node->header.gfx.unk18 == gCurGraphNodeRoot->areaIndex) {
        // HFR: this tick's transform is stored for the next tick to blend
        // from. A teleport (respawn, warp, object recycling) is snapped
        // instead, otherwise objects would streak across the level.
        if (hfr_prev_valid(node->header.gfx.prevTimestamp)) {
            vec3f_copy(prevPos, node->header.gfx.prevPos);
            vec3s_copy(prevAngle, node->header.gfx.prevAngle);
            if (hfr_dist_sq(prevPos, node->header.gfx.pos) > HFR_SNAP_DIST_SQ) {
                vec3f_copy(prevPos, node->header.gfx.pos);
                vec3s_copy(prevAngle, node->header.gfx.angle);
            }
        } else {
            vec3f_copy(prevPos, node->header.gfx.pos);
            vec3s_copy(prevAngle, node->header.gfx.angle);
        }
        vec3f_copy(node->header.gfx.prevPos, node->header.gfx.pos);
        vec3s_copy(node->header.gfx.prevAngle, node->header.gfx.angle);
        node->header.gfx.prevTimestamp = gGlobalTimer;

        if (node->header.gfx.throwMatrix != NULL) {
            Mat4 *throwMtx = (Mat4 *) node->header.gfx.throwMatrix;

            mtxf_mul(gMatStack[gMatStackIndex + 1], *throwMtx, gMatStack[gMatStackIndex]);
            if (hfr_prev_valid(node->header.gfx.prevThrowMatrixTimestamp)) {
                mtxf_mul(gMatStackPrev[gMatStackIndex + 1], node->header.gfx.prevThrowMatrix,
                         gMatStackPrev[gMatStackIndex]);
            } else {
                mtxf_mul(gMatStackPrev[gMatStackIndex + 1], *throwMtx, gMatStackPrev[gMatStackIndex]);
            }
            mtxf_copy(node->header.gfx.prevThrowMatrix, *throwMtx);
            node->header.gfx.prevThrowMatrixTimestamp = gGlobalTimer;
        } else if (node->header.gfx.node.flags & GRAPH_RENDER_CYLBOARD) {
            mtxf_cylboard(gMatStack[gMatStackIndex + 1], gMatStack[gMatStackIndex],
                           node->header.gfx.pos, gCurGraphNodeCamera->roll);
            mtxf_cylboard(gMatStackPrev[gMatStackIndex + 1], gMatStackPrev[gMatStackIndex],
                           prevPos, gCurGraphNodeCamera->roll);
        } else if (node->header.gfx.node.flags & GRAPH_RENDER_BILLBOARD) {
            mtxf_billboard(gMatStack[gMatStackIndex + 1], gMatStack[gMatStackIndex],
                           node->header.gfx.pos, gCurGraphNodeCamera->roll);
            mtxf_billboard(gMatStackPrev[gMatStackIndex + 1], gMatStackPrev[gMatStackIndex],
                           prevPos, gCurGraphNodeCamera->roll);
        } else {
            mtxf_rotate_zxy_and_translate(mtxf, node->header.gfx.pos, node->header.gfx.angle);
            mtxf_mul(gMatStack[gMatStackIndex + 1], mtxf, gMatStack[gMatStackIndex]);
            mtxf_rotate_zxy_and_translate(mtxf, prevPos, prevAngle);
            mtxf_mul(gMatStackPrev[gMatStackIndex + 1], mtxf, gMatStackPrev[gMatStackIndex]);
        }

        if (hfr_prev_valid(node->header.gfx.prevScaleTimestamp)) {
            vec3f_copy(prevScale, node->header.gfx.prevScale);
        } else {
            vec3f_copy(prevScale, node->header.gfx.scale);
        }
        vec3f_copy(node->header.gfx.prevScale, node->header.gfx.scale);
        node->header.gfx.prevScaleTimestamp = gGlobalTimer;

        mtxf_scale_vec3f(gMatStack[gMatStackIndex + 1], gMatStack[gMatStackIndex + 1],
                         node->header.gfx.scale);
        mtxf_scale_vec3f(gMatStackPrev[gMatStackIndex + 1], gMatStackPrev[gMatStackIndex + 1],
                         prevScale);
        node->header.gfx.throwMatrix = gMatStack[++gMatStackIndex];
        node->header.gfx.cameraToObject[0] = gMatStack[gMatStackIndex][3][0];
        node->header.gfx.cameraToObject[1] = gMatStack[gMatStackIndex][3][1];
        node->header.gfx.cameraToObject[2] = gMatStack[gMatStackIndex][3][2];

        // FIXME: correct types
        if (node->header.gfx.unk38.curAnim != NULL) {
            geo_set_animation_globals(&node->header.gfx.unk38, hasAnimation);
        }
        if (obj_is_in_view(&node->header.gfx, gMatStack[gMatStackIndex])) {
            Mtx *mtx = alloc_display_list(sizeof(*mtx));

            mtxf_to_mtx(mtx, gMatStack[gMatStackIndex]);
            gMatStackFixed[gMatStackIndex] = mtx;
            if (node->header.gfx.sharedChild != NULL) {
                int splat = splat_object_selected(node);
                u8 prevSplat = gSplatCurrentObject;
                gCurGraphNodeObject = (struct GraphNodeObject *) node;
                node->header.gfx.sharedChild->parent = &node->header.gfx.node;
                if (splat) gSplatCurrentObject = 1;
                geo_process_node_and_siblings(node->header.gfx.sharedChild);
                gSplatCurrentObject = prevSplat;
                node->header.gfx.sharedChild->parent = NULL;
                gCurGraphNodeObject = NULL;
            }
            if (node->header.gfx.node.children != NULL) {
                geo_process_node_and_siblings(node->header.gfx.node.children);
            }
        } else {
            // Not drawn this tick: don't interpolate when it comes back.
            node->header.gfx.prevTimestamp = 0;
            node->header.gfx.prevScaleTimestamp = 0;
            node->header.gfx.prevThrowMatrixTimestamp = 0;
        }

        gMatStackIndex--;
        gCurAnimType = ANIM_TYPE_NONE;
        node->header.gfx.throwMatrix = NULL;
    }
}

/**
 * Process an object parent node. Temporarily assigns itself as the parent of
 * the subtree rooted at 'sharedChild' and processes the subtree, after which the
 * actual children are be processed. (in practice they are null though)
 */
static void geo_process_object_parent(struct GraphNodeObjectParent *node) {
    if (node->sharedChild != NULL) {
        node->sharedChild->parent = (struct GraphNode *) node;
        geo_process_node_and_siblings(node->sharedChild);
        node->sharedChild->parent = NULL;
    }
    if (node->node.children != NULL) {
        geo_process_node_and_siblings(node->node.children);
    }
}

/**
 * Process a held object node.
 */
void geo_process_held_object(struct GraphNodeHeldObject *node) {
    Mat4 mat;
    Vec3f translation;
    Mtx *mtx = alloc_display_list(sizeof(*mtx));

#ifdef F3DEX_GBI_2
    gSPLookAt(gDisplayListHead++, &lookAt);
#endif

    if (node->fnNode.func != NULL) {
        node->fnNode.func(GEO_CONTEXT_RENDER, &node->fnNode.node, gMatStack[gMatStackIndex]);
    }
    if (node->objNode != NULL && node->objNode->header.gfx.sharedChild != NULL) {
        s32 hasAnimation = (node->objNode->header.gfx.node.flags & GRAPH_RENDER_HAS_ANIMATION) != 0;

        translation[0] = node->translation[0] / 4.0f;
        translation[1] = node->translation[1] / 4.0f;
        translation[2] = node->translation[2] / 4.0f;

        mtxf_translate(mat, translation);
        mtxf_copy(gMatStack[gMatStackIndex + 1], (void *) gCurGraphNodeObject->throwMatrix);
        gMatStack[gMatStackIndex + 1][3][0] = gMatStack[gMatStackIndex][3][0];
        gMatStack[gMatStackIndex + 1][3][1] = gMatStack[gMatStackIndex][3][1];
        gMatStack[gMatStackIndex + 1][3][2] = gMatStack[gMatStackIndex][3][2];
        mtxf_mul(gMatStack[gMatStackIndex + 1], mat, gMatStack[gMatStackIndex + 1]);
        mtxf_scale_vec3f(gMatStack[gMatStackIndex + 1], gMatStack[gMatStackIndex + 1],
                         node->objNode->header.gfx.scale);
        // HFR: the held object follows the interpolated parent.
        mtxf_mul(gMatStackPrev[gMatStackIndex + 1], mat, gMatStackPrev[gMatStackIndex]);
        mtxf_scale_vec3f(gMatStackPrev[gMatStackIndex + 1], gMatStackPrev[gMatStackIndex + 1],
                         node->objNode->header.gfx.scale);
        if (node->fnNode.func != NULL) {
            node->fnNode.func(GEO_CONTEXT_HELD_OBJ, &node->fnNode.node,
                              (struct AllocOnlyPool *) gMatStack[gMatStackIndex + 1]);
        }
        gMatStackIndex++;
        mtxf_to_mtx(mtx, gMatStack[gMatStackIndex]);
        gMatStackFixed[gMatStackIndex] = mtx;
        gGeoTempState.type = gCurAnimType;
        gGeoTempState.enabled = gCurAnimEnabled;
        gGeoTempState.frame = gCurrAnimFrame;
        gGeoTempState.translationMultiplier = gCurAnimTranslationMultiplier;
        gGeoTempState.attribute = gCurrAnimAttribute;
        gGeoTempState.data = gCurAnimData;
        gGeoTempState.prevFrame = gPrevAnimFrame;
        gCurAnimType = 0;
        gCurGraphNodeHeldObject = (void *) node;
        if (node->objNode->header.gfx.unk38.curAnim != NULL) {
            geo_set_animation_globals(&node->objNode->header.gfx.unk38, hasAnimation);
        }

        geo_process_node_and_siblings(node->objNode->header.gfx.sharedChild);
        gCurGraphNodeHeldObject = NULL;
        gCurAnimType = gGeoTempState.type;
        gCurAnimEnabled = gGeoTempState.enabled;
        gCurrAnimFrame = gGeoTempState.frame;
        gCurAnimTranslationMultiplier = gGeoTempState.translationMultiplier;
        gCurrAnimAttribute = gGeoTempState.attribute;
        gCurAnimData = gGeoTempState.data;
        gPrevAnimFrame = gGeoTempState.prevFrame;
        gMatStackIndex--;
    }

    if (node->fnNode.node.children != NULL) {
        geo_process_node_and_siblings(node->fnNode.node.children);
    }
}

/**
 * Processes the children of the given GraphNode if it has any
 */
void geo_try_process_children(struct GraphNode *node) {
    if (node->children != NULL) {
        geo_process_node_and_siblings(node->children);
    }
}

/**
 * Process a generic geo node and its siblings.
 * The first argument is the start node, and all its siblings will
 * be iterated over.
 */
void geo_process_node_and_siblings(struct GraphNode *firstNode) {
    s16 iterateChildren = TRUE;
    struct GraphNode *curGraphNode = firstNode;
    struct GraphNode *parent = curGraphNode->parent;

    // In the case of a switch node, exactly one of the children of the node is
    // processed instead of all children like usual
    if (parent != NULL) {
        iterateChildren = (parent->type != GRAPH_NODE_TYPE_SWITCH_CASE);
    }

    do {
        if (curGraphNode->flags & GRAPH_RENDER_ACTIVE) {
            if (curGraphNode->flags & GRAPH_RENDER_CHILDREN_FIRST) {
                geo_try_process_children(curGraphNode);
            } else {
                switch (curGraphNode->type) {
                    case GRAPH_NODE_TYPE_ORTHO_PROJECTION:
                        geo_process_ortho_projection((struct GraphNodeOrthoProjection *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_PERSPECTIVE:
                        geo_process_perspective((struct GraphNodePerspective *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_MASTER_LIST:
                        geo_process_master_list((struct GraphNodeMasterList *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_LEVEL_OF_DETAIL:
                        geo_process_level_of_detail((struct GraphNodeLevelOfDetail *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_SWITCH_CASE:
                        geo_process_switch((struct GraphNodeSwitchCase *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_CAMERA:
                        geo_process_camera((struct GraphNodeCamera *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_TRANSLATION_ROTATION:
                        geo_process_translation_rotation(
                            (struct GraphNodeTranslationRotation *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_TRANSLATION:
                        geo_process_translation((struct GraphNodeTranslation *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_ROTATION:
                        geo_process_rotation((struct GraphNodeRotation *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_OBJECT:
                        geo_process_object((struct Object *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_ANIMATED_PART:
                        geo_process_animated_part((struct GraphNodeAnimatedPart *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_BILLBOARD:
                        geo_process_billboard((struct GraphNodeBillboard *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_DISPLAY_LIST:
                        geo_process_display_list((struct GraphNodeDisplayList *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_SCALE:
                        geo_process_scale((struct GraphNodeScale *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_SHADOW:
                        geo_process_shadow((struct GraphNodeShadow *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_OBJECT_PARENT:
                        geo_process_object_parent((struct GraphNodeObjectParent *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_GENERATED_LIST:
                        geo_process_generated_list((struct GraphNodeGenerated *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_BACKGROUND:
                        geo_process_background((struct GraphNodeBackground *) curGraphNode);
                        break;
                    case GRAPH_NODE_TYPE_HELD_OBJ:
                        geo_process_held_object((struct GraphNodeHeldObject *) curGraphNode);
                        break;
                    default:
                        geo_try_process_children((struct GraphNode *) curGraphNode);
                        break;
                }
            }
        } else {
            if (curGraphNode->type == GRAPH_NODE_TYPE_OBJECT) {
                ((struct GraphNodeObject *) curGraphNode)->throwMatrix = NULL;
            }
        }
    } while (iterateChildren && (curGraphNode = curGraphNode->next) != firstNode);
}

/**
 * Process a root node. This is the entry point for processing the scene graph.
 * The root node itself sets up the viewport, then all its children are processed
 * to set up the projection and draw display lists.
 */
void geo_process_root(struct GraphNodeRoot *node, Vp *b, Vp *c, s32 clearColor) {
    UNUSED s32 unused;

    if (node->node.flags & GRAPH_RENDER_ACTIVE) {
        Mtx *initialMatrix;
        Vp *viewport = alloc_display_list(sizeof(*viewport));

        gDisplayListHeap = alloc_only_pool_init(main_pool_available() - sizeof(struct AllocOnlyPool),
                                                MEMORY_POOL_LEFT);
        initialMatrix = alloc_display_list(sizeof(*initialMatrix));
        gMatStackIndex = 0;
        gCurAnimType = 0;
        hfr_reset();
        vec3s_set(viewport->vp.vtrans, node->x * 4, node->y * 4, 511);
        vec3s_set(viewport->vp.vscale, node->width * 4, node->height * 4, 511);
        if (b != NULL) {
            clear_frame_buffer(clearColor);
            make_viewport_clip_rect(b);
            *viewport = *b;
        }

        else if (c != NULL) {
            clear_frame_buffer(clearColor);
            make_viewport_clip_rect(c);
        }

        mtxf_identity(gMatStack[gMatStackIndex]);
        mtxf_identity(gMatStackPrev[gMatStackIndex]);
        mtxf_to_mtx(initialMatrix, gMatStack[gMatStackIndex]);
        gMatStackFixed[gMatStackIndex] = initialMatrix;
        gSPViewport(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(viewport));
        gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(gMatStackFixed[gMatStackIndex]),
                  G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
        gCurGraphNodeRoot = node;
        if (node->node.children != NULL) {
            geo_process_node_and_siblings(node->node.children);
        }
        gCurGraphNodeRoot = NULL;
        if (gShowDebugText) {
            print_text_fmt_int(180, 36, "MEM %d",
                               gDisplayListHeap->totalSpace - gDisplayListHeap->usedSpace);
        }
        main_pool_free(gDisplayListHeap);
    }
}
