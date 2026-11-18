/**
 * Portal gun: pickup prop, two placed portals, hitscan placement and teleport.
 *
 * Portals are stored as an oriented ellipse on a collision surface (centre,
 * surface normal pointing out of the wall, tangent/bitangent). The renderer
 * draws them as a coloured fan after the scene, and the simulation tick checks
 * whether Mario's centre of mass crossed a portal plane inside the ellipse.
 *
 * Clone / through-portal camera relay is deliberately not implemented here yet.
 */

#include <ultra64.h>
#include <math.h>

#include <string.h>

#include "sm64.h"
#include "portal.h"
#include "engine/math_util.h"
#include "engine/surface_collision.h"
#include "game/camera.h"
#include "game/level_update.h"
#include "game/mario.h"
#include "game/memory.h"
#include "game/object_helpers.h"
#include "game/rendering_graph_node.h"
#include "game/game_init.h"
#include "game/print.h"
#include "audio/external.h"
#include "game/area.h"

#define PORTAL_RX        70.0f     // ellipse half width (world units)
#define PORTAL_RY        110.0f    // ellipse half height
#define PORTAL_SEGMENTS  24
#define PORTAL_RAY_DIST  4000.0f   // hitscan length
#define PORTAL_EXIT_PUSH 40.0f     // spawn this far out of the exit portal
#define PORTAL_COOLDOWN  20        // ticks before the next teleport
#define PORTAL_PICKUP_R  150.0f

#define GUN_HALF_X 18.0f
#define GUN_HALF_Y 10.0f
#define GUN_HALF_Z 55.0f

struct Portal {
    int active;
    Vec3f pos;
    Vec3f normal;   // out of the surface
    Vec3f tan;      // in-plane, roughly horizontal
    Vec3f bitan;    // in-plane, roughly vertical
    float rx, ry;
};

static struct Portal s_portal[2];
// Last placement result, for headless debugging: 0 none, 1 placed, 2 bad
// surface, 3 could not fit the ellipse, 4 no gun.
int g_portal_debug = 0;
static int s_hasGun = 0;
static int s_gunSpawned = 0;
static Vec3f s_gunPos = {0.0f, 0.0f, 0.0f};
static float s_gunSpin = 0.0f;
static int s_cooldown = 0;
static Vec3f s_prevCom;
static int s_havePrevCom = 0;
static int s_lastLevelArea = -1;

/* ------------------------------------------------------------------ math */

static float vdot3(const Vec3f a, const Vec3f b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void vcross3(Vec3f out, const Vec3f a, const Vec3f b) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static void vnorm3(Vec3f v) {
    float len = sqrtf(vdot3(v, v));
    if (len > 0.0001f) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

// Row-vector 4x4 multiply: res = a * b (this engine stores matrices transposed
// relative to GL, with translation in row 3).
static void mat4_mul_rm(float res[4][4], const float a[4][4], const float b[4][4]) {
    float tmp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j]
                      + a[i][3] * b[3][j];
        }
    }
    memcpy(res, tmp, sizeof(tmp));
}

/* ------------------------------------------------------- portal placement */

static int portal_surface_ok(struct Surface *surf) {
    if (surf == NULL) return 0;
    // Object-owned surfaces: NPCs, coins, question blocks, breakable bricks,
    // cork boxes, signs, doors... all excluded.
    if (surf->object != NULL) return 0;
    switch (surf->type) {
        case SURFACE_WATER:
        case SURFACE_BURNING:
        case SURFACE_DEATH_PLANE:
        case SURFACE_VANISH_CAP_WALLS:
        case SURFACE_INSTANT_QUICKSAND:
        case SURFACE_INSTANT_MOVING_QUICKSAND:
        case SURFACE_FLOWING_WATER:
        case SURFACE_DEEP_MOVING_QUICKSAND:
            return 0;
        default:
            return 1;
    }
}

// Builds the ellipse basis from a surface normal.
static void portal_basis_from_normal(const Vec3f n, Vec3f tan, Vec3f bitan) {
    Vec3f up = {0.0f, 1.0f, 0.0f};
    Vec3f worldX = {1.0f, 0.0f, 0.0f};

    vcross3(tan, up, n);
    if (vdot3(tan, tan) < 0.01f) {
        // Floor / ceiling: use world X as the reference.
        vcross3(tan, worldX, n);
    }
    vnorm3(tan);
    vcross3(bitan, n, tan);
    vnorm3(bitan);
}

// Terrain collision is tessellated per triangle, so "same surface" cannot mean
// the same Surface pointer. Compare the plane instead: same normal and the hit
// point lies on the plane of the original hit.
static int portal_rim_ok(const Vec3f centre, const Vec3f n, const Vec3f tan, const Vec3f bitan,
                         float rx, float ry, const Vec3f planePos) {
    static const float ang[8] = { 0.0f, 0.7854f, 1.5708f, 2.3562f, 3.1416f, 3.9270f, 4.7124f, 5.4978f };

    for (int i = 0; i < 8; i++) {
        Vec3f p;
        Vec3f dir;
        struct Surface *hit = NULL;
        Vec3f hitPos;

        p[0] = centre[0] + (tan[0] * cosf(ang[i]) * rx) + (bitan[0] * sinf(ang[i]) * ry);
        p[1] = centre[1] + (tan[1] * cosf(ang[i]) * rx) + (bitan[1] * sinf(ang[i]) * ry);
        p[2] = centre[2] + (tan[2] * cosf(ang[i]) * rx) + (bitan[2] * sinf(ang[i]) * ry);
        // Lift the probe off the surface, then look back at it.
        p[0] += n[0] * 8.0f;
        p[1] += n[1] * 8.0f;
        p[2] += n[2] * 8.0f;
        dir[0] = -n[0] * 32.0f;
        dir[1] = -n[1] * 32.0f;
        dir[2] = -n[2] * 32.0f;
        find_surface_on_ray(p, dir, &hit, hitPos);
        if (hit == NULL) {
            return 0;
        }
        {
            Vec3f hn;
            float dn, dp;
            hn[0] = hit->normal.x;
            hn[1] = hit->normal.y;
            hn[2] = hit->normal.z;
            vnorm3(hn);
            dn = vdot3(hn, n);
            dp = (hitPos[0] - planePos[0]) * n[0]
               + (hitPos[1] - planePos[1]) * n[1]
               + (hitPos[2] - planePos[2]) * n[2];
            if (dn < 0.99f || fabsf(dp) > 6.0f) {
                return 0;
            }
        }
    }
    return 1;
}

void portal_shoot(int which) {
    Vec3f orig, dir;
    struct Surface *surf = NULL;
    Vec3f hit;
    Vec3f n, tan, bitan;
    Vec3f centre;

    if (which < 0 || which > 1 || gMarioState == NULL) {
        return;
    }
    if (!s_hasGun) {
        g_portal_debug = 4;
        return;
    }

    vec3f_copy(orig, gLakituState.pos);
    vec3f_copy(dir, gLakituState.focus);
    vec3f_sub(dir, orig);
    vnorm3(dir);
    dir[0] *= PORTAL_RAY_DIST;
    dir[1] *= PORTAL_RAY_DIST;
    dir[2] *= PORTAL_RAY_DIST;

    find_surface_on_ray(orig, dir, &surf, hit);
    if (!portal_surface_ok(surf)) {
        g_portal_debug = 2;
        return;
    }

    vec3f_set(n, surf->normal.x, surf->normal.y, surf->normal.z);
    vnorm3(n);
    portal_basis_from_normal(n, tan, bitan);

    vec3f_copy(centre, hit);
    // Nudge the portal off the surface a little so it does not z-fight.
    centre[0] += n[0] * 1.5f;
    centre[1] += n[1] * 1.5f;
    centre[2] += n[2] * 1.5f;

    // Edge fitting: push the ellipse away from edges until every rim probe
    // still lands on the same surface; shrink it if that is not enough.
    float rx = PORTAL_RX;
    float ry = PORTAL_RY;
    int fitted = 0;

    for (int shrink = 0; shrink < 3 && !fitted; shrink++) {
        for (int iter = 0; iter < 8 && !fitted; iter++) {
            if (portal_rim_ok(centre, n, tan, bitan, rx, ry, hit)) {
                fitted = 1;
                break;
            }
            // Move the centre away from the failing side: try the four cardinal
            // directions and keep the first that improves things.
            Vec3f cand;
            int improved = 0;
            static const float dirs[4][2] = { {1, 0}, {-1, 0}, {0, 1}, {0, -1} };
            for (int d = 0; d < 4 && !improved; d++) {
                vec3f_copy(cand, centre);
                cand[0] += (tan[0] * dirs[d][0] + bitan[0] * dirs[d][1]) * rx * 0.35f;
                cand[1] += (tan[1] * dirs[d][0] + bitan[1] * dirs[d][1]) * ry * 0.35f;
                cand[2] += (tan[2] * dirs[d][0] + bitan[2] * dirs[d][1]) * rx * 0.35f;
                if (portal_rim_ok(cand, n, tan, bitan, rx, ry, hit)) {
                    vec3f_copy(centre, cand);
                    fitted = 1;
                    improved = 1;
                }
            }
            if (!improved) {
                vec3f_copy(centre, hit);
                centre[0] += n[0] * 1.5f;
                centre[1] += n[1] * 1.5f;
                centre[2] += n[2] * 1.5f;
            }
        }
        if (!fitted) {
            rx *= 0.75f;
            ry *= 0.75f;
        }
    }
    if (!fitted) {
        g_portal_debug = 3;
        return;   // too small / too close to an edge: leave the old portal
    }

    s_portal[which].active = 1;
    vec3f_copy(s_portal[which].pos, centre);
    vec3f_copy(s_portal[which].normal, n);
    vec3f_copy(s_portal[which].tan, tan);
    vec3f_copy(s_portal[which].bitan, bitan);
    s_portal[which].rx = rx;
    s_portal[which].ry = ry;
    s_havePrevCom = 0;
    g_portal_debug = 1;
}

/* ------------------------------------------------------------- simulation */

static void portal_clear(void) {
    s_portal[0].active = 0;
    s_portal[1].active = 0;
    s_havePrevCom = 0;
    s_cooldown = 0;
}

static void portal_rotate_vec(const struct Portal *from, const struct Portal *to,
                              const Vec3f in, Vec3f out) {
    float x = vdot3(in, from->tan);
    float y = vdot3(in, from->bitan);
    float z = vdot3(in, from->normal);
    // Mirror through the portal plane (180 degrees about the local up axis).
    x = -x;
    z = -z;
    out[0] = to->tan[0] * x + to->bitan[0] * y + to->normal[0] * z;
    out[1] = to->tan[1] * x + to->bitan[1] * y + to->normal[1] * z;
    out[2] = to->tan[2] * x + to->bitan[2] * y + to->normal[2] * z;
}

static void portal_teleport(int srcIdx) {
    struct Portal *src = &s_portal[srcIdx];
    struct Portal *dst = &s_portal[1 - srcIdx];
    Vec3f rel, newCom, vel, newVel;
    float height = gMarioState->marioObj != NULL ? gMarioState->marioObj->hitboxHeight : 160.0f;

    rel[0] = s_prevCom[0] - src->pos[0];
    rel[1] = s_prevCom[1] - src->pos[1];
    rel[2] = s_prevCom[2] - src->pos[2];

    float x = vdot3(rel, src->tan);
    float y = vdot3(rel, src->bitan);
    float z = vdot3(rel, src->normal);
    x = -x;
    z = -z;

    newCom[0] = dst->pos[0] + dst->tan[0] * x + dst->bitan[0] * y + dst->normal[0] * z;
    newCom[1] = dst->pos[1] + dst->tan[1] * x + dst->bitan[1] * y + dst->normal[1] * z;
    newCom[2] = dst->pos[2] + dst->tan[2] * x + dst->bitan[2] * y + dst->normal[2] * z;
    // Push out of the exit portal so we do not immediately re-enter it.
    newCom[0] += dst->normal[0] * PORTAL_EXIT_PUSH;
    newCom[1] += dst->normal[1] * PORTAL_EXIT_PUSH;
    newCom[2] += dst->normal[2] * PORTAL_EXIT_PUSH;

    gMarioState->pos[0] = newCom[0];
    gMarioState->pos[1] = newCom[1] - height * 0.5f;
    gMarioState->pos[2] = newCom[2];
    if (gMarioState->marioObj != NULL) {
        vec3f_copy(gMarioState->marioObj->header.gfx.pos, gMarioState->pos);
        gMarioState->marioObj->oPosX = gMarioState->pos[0];
        gMarioState->marioObj->oPosY = gMarioState->pos[1];
        gMarioState->marioObj->oPosZ = gMarioState->pos[2];
    }

    // Momentum: rotate the velocity through the same portal transform.
    vel[0] = gMarioState->vel[0];
    vel[1] = gMarioState->vel[1];
    vel[2] = gMarioState->vel[2];
    portal_rotate_vec(src, dst, vel, newVel);
    vec3f_copy(gMarioState->vel, newVel);

    // Facing: turn by the difference of the two portals' tangents.
    {
        s16 fromYaw = atan2s(src->tan[2], src->tan[0]);
        s16 toYaw = atan2s(dst->tan[2], dst->tan[0]);
        s16 delta = (s16)(toYaw - fromYaw) + (s16)0x8000;
        gMarioState->faceAngle[1] = (s16)(gMarioState->faceAngle[1] + delta);
        gMarioState->marioObj->oMoveAngleYaw = gMarioState->faceAngle[1];
    }

    // Snap the camera to the new position.
    gCameraMovementFlags |= CAM_MOVE_INIT_CAMERA;
    s_cooldown = PORTAL_COOLDOWN;
    s_havePrevCom = 0;
    play_sound(SOUND_MENU_CAMERA_ZOOM_IN, gDefaultSoundArgs);
}

static int portal_point_inside(const struct Portal *p, const Vec3f point) {
    Vec3f rel;
    rel[0] = point[0] - p->pos[0];
    rel[1] = point[1] - p->pos[1];
    rel[2] = point[2] - p->pos[2];
    float x = vdot3(rel, p->tan) / p->rx;
    float y = vdot3(rel, p->bitan) / p->ry;
    return (x * x + y * y) <= 1.0f;
}

void portal_update(void) {
    static int lastArea = -1;
    int area = gCurrLevelNum * 100 + gCurrAreaIndex;

    if (area != lastArea) {
        lastArea = area;
        portal_clear();
        s_gunSpawned = 0;
    }
    if (s_cooldown > 0) {
        s_cooldown--;
    }

    s_gunSpin += 0x0300;

    // Gun pickup: spawn the prop next to Mario in the castle grounds.
    if (!s_hasGun && !s_gunSpawned && gCurrLevelNum == 16 && gCurrAreaIndex == 1 && gMarioState != NULL) {
        vec3f_copy(s_gunPos, gMarioState->pos);
        s_gunPos[0] += 200.0f;
        s_gunPos[2] -= 200.0f;
        s_gunPos[1] += 40.0f;
        s_gunSpawned = 1;
    }
    if (!s_hasGun && s_gunSpawned && gMarioState != NULL) {
        Vec3f rel;
        rel[0] = gMarioState->pos[0] - s_gunPos[0];
        rel[1] = (gMarioState->pos[1] + 60.0f) - s_gunPos[1];
        rel[2] = gMarioState->pos[2] - s_gunPos[2];
        if (vdot3(rel, rel) < PORTAL_PICKUP_R * PORTAL_PICKUP_R) {
            s_hasGun = 1;
            s_gunSpawned = 0;
            play_sound(SOUND_MENU_CHANGE_SELECT, gDefaultSoundArgs);
        }
    }

    if (gMarioState == NULL || gMarioState->marioObj == NULL) {
        return;
    }

    {
        Vec3f com;
        float height = gMarioState->marioObj->hitboxHeight;
        com[0] = gMarioState->pos[0];
        com[1] = gMarioState->pos[1] + height * 0.5f;
        com[2] = gMarioState->pos[2];

        if (s_portal[0].active && s_portal[1].active && s_cooldown == 0 && s_havePrevCom) {
            for (int i = 0; i < 2; i++) {
                struct Portal *p = &s_portal[i];
                float dPrev = vdot3(s_prevCom, p->normal);
                float dPrevRel = dPrev - vdot3(p->pos, p->normal);
                float dNow = vdot3(com, p->normal) - vdot3(p->pos, p->normal);
                if (dPrevRel > 0.0f && dNow <= 0.0f && portal_point_inside(p, s_prevCom)) {
                    portal_teleport(i);
                    break;
                }
            }
        }
        vec3f_copy(s_prevCom, com);
        s_havePrevCom = 1;
    }
}

/* -------------------------------------------------------------- rendering */

static void emit_model_matrix(const Vec3f pos, const Vec3f tan, const Vec3f bitan,
                              const Vec3f n, const Mat4 view) {
    float model[4][4];
    float mv[4][4];
    Mtx *mtx = alloc_display_list(sizeof(*mtx));

    model[0][0] = tan[0];  model[0][1] = tan[1];  model[0][2] = tan[2];  model[0][3] = 0.0f;
    model[1][0] = bitan[0];model[1][1] = bitan[1];model[1][2] = bitan[2];model[1][3] = 0.0f;
    model[2][0] = n[0];    model[2][1] = n[1];    model[2][2] = n[2];    model[2][3] = 0.0f;
    model[3][0] = pos[0];  model[3][1] = pos[1];  model[3][2] = pos[2];  model[3][3] = 1.0f;

    mat4_mul_rm(mv, model, view);
    mtxf_to_mtx(mtx, mv);
    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(mtx),
              G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
}

static void fill_vtx(Vtx *v, float x, float y, float z, u8 r, u8 g, u8 b, u8 a) {
    v->v.ob[0] = (s16)(x * 4.0f);
    v->v.ob[1] = (s16)(y * 4.0f);
    v->v.ob[2] = (s16)(z * 4.0f);
    v->v.flag = 0;
    v->v.tc[0] = 0;
    v->v.tc[1] = 0;
    v->v.cn[0] = r;
    v->v.cn[1] = g;
    v->v.cn[2] = b;
    v->v.cn[3] = a;
}

static void draw_portal(const struct Portal *p, u8 r, u8 g, u8 b, const Mat4 view) {
    Vtx *v = alloc_display_list(sizeof(Vtx) * (PORTAL_SEGMENTS + 2));

    if (v == NULL) {
        return;
    }

    emit_model_matrix(p->pos, p->tan, p->bitan, p->normal, view);

    fill_vtx(&v[0], 0.0f, 0.0f, 0.0f, r, g, b, 0xB8);
    for (int i = 0; i <= PORTAL_SEGMENTS; i++) {
        float a = (float)i * (2.0f * 3.14159265f) / (float)PORTAL_SEGMENTS;
        fill_vtx(&v[i + 1], cosf(a) * p->rx, sinf(a) * p->ry, 0.0f, r, g, b, 0xC8);
    }

    gSPVertex(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(v), PORTAL_SEGMENTS + 2, 0);
    for (int i = 0; i < PORTAL_SEGMENTS; i++) {
        gSP1Triangle(gDisplayListHead++, 0, i + 1, i + 2, 0);
    }
}

static void draw_gun(const Mat4 view) {
    Vtx *v = alloc_display_list(sizeof(Vtx) * 8);
    Vec3f tan, bitan, up;
    s16 yaw = (s16)s_gunSpin;

    if (v == NULL) {
        return;
    }

    // Spin around world Y.
    tan[0] = coss(yaw);
    tan[1] = 0.0f;
    tan[2] = -sins(yaw);
    up[0] = 0.0f;
    up[1] = 1.0f;
    up[2] = 0.0f;
    vcross3(bitan, up, tan);

    emit_model_matrix(s_gunPos, tan, bitan, up, view);

    // A chunky barrel: placeholder until there is a real gun mesh.
    fill_vtx(&v[0], -GUN_HALF_X, -GUN_HALF_Y, -GUN_HALF_Z, 0x60, 0x60, 0x70, 0xFF);
    fill_vtx(&v[1],  GUN_HALF_X, -GUN_HALF_Y, -GUN_HALF_Z, 0x60, 0x60, 0x70, 0xFF);
    fill_vtx(&v[2],  GUN_HALF_X,  GUN_HALF_Y, -GUN_HALF_Z, 0x90, 0x90, 0xA0, 0xFF);
    fill_vtx(&v[3], -GUN_HALF_X,  GUN_HALF_Y, -GUN_HALF_Z, 0x90, 0x90, 0xA0, 0xFF);
    fill_vtx(&v[4], -GUN_HALF_X, -GUN_HALF_Y,  GUN_HALF_Z, 0x60, 0x60, 0x70, 0xFF);
    fill_vtx(&v[5],  GUN_HALF_X, -GUN_HALF_Y,  GUN_HALF_Z, 0x60, 0x60, 0x70, 0xFF);
    fill_vtx(&v[6],  GUN_HALF_X,  GUN_HALF_Y,  GUN_HALF_Z, 0x90, 0x90, 0xA0, 0xFF);
    fill_vtx(&v[7], -GUN_HALF_X,  GUN_HALF_Y,  GUN_HALF_Z, 0x90, 0x90, 0xA0, 0xFF);

    gSPVertex(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(v), 8, 0);
    gSP1Triangle(gDisplayListHead++, 0, 1, 2, 0);
    gSP1Triangle(gDisplayListHead++, 0, 2, 3, 0);
    gSP1Triangle(gDisplayListHead++, 4, 6, 5, 0);
    gSP1Triangle(gDisplayListHead++, 4, 7, 6, 0);
    gSP1Triangle(gDisplayListHead++, 0, 4, 5, 0);
    gSP1Triangle(gDisplayListHead++, 0, 5, 1, 0);
    gSP1Triangle(gDisplayListHead++, 3, 2, 6, 0);
    gSP1Triangle(gDisplayListHead++, 3, 6, 7, 0);
    gSP1Triangle(gDisplayListHead++, 0, 3, 7, 0);
    gSP1Triangle(gDisplayListHead++, 0, 7, 4, 0);
    gSP1Triangle(gDisplayListHead++, 1, 5, 6, 0);
    gSP1Triangle(gDisplayListHead++, 1, 6, 2, 0);
}

void portal_render(void) {
    Mat4 view;

    if (gCurrentArea == NULL) {
        return;
    }
    if (!s_portal[0].active && !s_portal[1].active && !s_gunSpawned) {
        return;
    }

    mtxf_lookat(view, gLakituState.pos, gLakituState.focus, 0);

    gSPSetGeometryMode(gDisplayListHead++, G_ZBUFFER);
    gSPClearGeometryMode(gDisplayListHead++, G_LIGHTING);
    gDPSetCombineMode(gDisplayListHead++, G_CC_SHADE, G_CC_SHADE);
    gDPSetRenderMode(gDisplayListHead++, G_RM_AA_ZB_XLU_SURF, G_RM_AA_ZB_XLU_SURF2);

    if (s_portal[0].active) {
        draw_portal(&s_portal[0], 0x30, 0x60, 0xFF, view);   // blue
    }
    if (s_portal[1].active) {
        draw_portal(&s_portal[1], 0xFF, 0x80, 0x20, view);   // orange
    }
    if (s_gunSpawned) {
        draw_gun(view);
    }

    gSPClearGeometryMode(gDisplayListHead++, G_ZBUFFER);
}

void portal_give_gun(void) {
    s_hasGun = 1;
    s_gunSpawned = 0;
}

int gfx_portal_field(int which, int field) {
    if (which < 0 || which > 1 || field < 0 || field > 5) {
        return 0;
    }
    if (field < 3) {
        return (int) s_portal[which].pos[field];
    }
    return (int) (s_portal[which].normal[field - 3] * 1000.0f);
}

int gfx_mario_pos(int axis) {
    if (gMarioState == NULL || axis < 0 || axis > 2) {
        return 0;
    }
    return (int) gMarioState->pos[axis];
}

int portal_gun_has(void) {
    return s_hasGun;
}

int portal_count(void) {
    return (s_portal[0].active ? 1 : 0) + (s_portal[1].active ? 1 : 0);
}
