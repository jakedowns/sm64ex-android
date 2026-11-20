/* Box3D (github.com/erincatto/box3d, MIT) prototype:
 * a stack of dynamic cubes in the PTC annex and a ball blaster.
 *
 * N64 units are treated as centimetres: 1 unit = 0.01 m.  Physics positions
 * and velocities are metres; rendering converts back to units. */

#include <ultra64.h>
#include <math.h>

#include "sm64.h"
#include "game/camera.h"
#include "game/level_update.h"
#include "game/mario.h"
#include "game/memory.h"
#include "game/print.h"
#include "game/game_init.h"
#include "pc/gfx/gfx_pc.h"
#include "audio/external.h"
#include "seq_ids.h"
#include "engine/math_util.h"
#include "box3d/box3d.h"
#include "boxphys.h"

#define BP_SCALE       0.01f
#define BP_CUBE_HALF   0.60f          /* 120-unit cube */
#define BP_CUBE_GAP    0.61f
#define BP_BALL_R      0.25f
#define BP_BALL_SPEED  26.0f
#define BP_MAX_BALLS   24
#define BP_MAX_BOXES   40
#define BP_PYRAMID_X   18.0f          /* 1800 units */
#define BP_PYRAMID_Z   4.0f           /*  400 units */

static b3WorldId s_world;
static b3BodyId s_boxes[BP_MAX_BOXES];
static b3BodyId s_marioBody;
static int s_boxCount;
static b3BodyId s_balls[BP_MAX_BALLS];
static u32 s_ballTime[BP_MAX_BALLS];
static int s_ballCount;
static int s_equipped;
static u32 s_tick;
static int s_inited;
static int s_onResetPad;
static u32 s_lastAction;

static void bp_box_half(b3BoxHull *h) {
    *h = b3MakeBoxHull(BP_CUBE_HALF, BP_CUBE_HALF, BP_CUBE_HALF);
}

static void bp_add_box(float x, float y, float z) {
    b3BodyDef bd = b3DefaultBodyDef();
    b3ShapeDef sd = b3DefaultShapeDef();
    static b3BoxHull hull;   /* must outlive the shape if Box3D references it */

    if (s_boxCount >= BP_MAX_BOXES) return;
    bd.type = b3_dynamicBody;
    bd.position = (b3Pos){ x, y, z };
    bd.linearDamping = 0.05f;
    bd.angularDamping = 0.05f;
    b3BodyId body = b3CreateBody(s_world, &bd);
    bp_box_half(&hull);
    sd.density = 80.0f;           /* light: punches and balls launch cubes */
    sd.baseMaterial.friction = 0.6f;
    sd.baseMaterial.restitution = 0.05f;
    b3CreateHullShape(body, &sd, &hull.base);
    s_boxes[s_boxCount++] = body;
}

static void bp_pyramid(void) {
    int layer, ix, iz;
    for (layer = 0; layer < 4; layer++) {
        int n = 4 - layer;
        for (ix = 0; ix < n; ix++) {
            for (iz = 0; iz < n; iz++) {
                float x = BP_PYRAMID_X + (ix - (n - 1) * 0.5f) * BP_CUBE_GAP * 2.0f;
                float z = BP_PYRAMID_Z + (iz - (n - 1) * 0.5f) * BP_CUBE_GAP * 2.0f;
                float y = BP_CUBE_HALF + layer * (BP_CUBE_HALF * 2.02f);
                bp_add_box(x, y, z);
            }
        }
    }
}

void boxphys_reset(void) {
    b3WorldDef wd;

    if (s_world.index1 != 0) {
        b3DestroyWorld(s_world);
        s_world = (b3WorldId){0};
    }
    s_boxCount = s_ballCount = 0;
    s_tick = 0;

    wd = b3DefaultWorldDef();
    wd.gravity = (b3Vec3){ 0.0f, -9.8f, 0.0f };
    wd.workerCount = 1;
    wd.enableSleep = true;
    wd.enableContinuous = true;
    s_world = b3CreateWorld(&wd);

    {
        b3BodyDef bd = b3DefaultBodyDef();
        b3ShapeDef sd = b3DefaultShapeDef();
        static b3BoxHull hull;
        hull = b3MakeBoxHull(9.0f, 0.5f, 9.0f);
        bd.type = b3_staticBody;
        bd.position = (b3Pos){ BP_PYRAMID_X, -0.5f, BP_PYRAMID_Z };
        b3BodyId ground = b3CreateBody(s_world, &bd);
        sd.density = 0.0f;
        sd.baseMaterial.friction = 0.9f;
        b3CreateHullShape(ground, &sd, &hull.base);
    }
    {
        /* Kinematic proxy so Mario pushes cubes when he runs into them. */
        static b3BoxHull mhull;
        b3BodyDef bd = b3DefaultBodyDef();
        b3ShapeDef sd = b3DefaultShapeDef();
        bd.type = b3_kinematicBody;
        bd.position = (b3Pos){ 0.0f, 1.0f, 0.0f };
        s_marioBody = b3CreateBody(s_world, &bd);
        mhull = b3MakeBoxHull(0.30f, 0.80f, 0.30f);
        sd.density = 0.0f;
        sd.baseMaterial.friction = 0.4f;
        b3CreateHullShape(s_marioBody, &sd, &mhull.base);
    }
    bp_pyramid();
    s_inited = 1;
}

void boxphys_init(void) {
    if (!s_inited || s_world.index1 == 0) {
        boxphys_reset();
    }
}

void boxphys_equip_toggle(void) {
    s_equipped = !s_equipped;
    if (s_equipped) {
        boxphys_init();
    }
}

int boxphys_equipped(void) {
    return s_equipped;
}

static void boxphys_fire_dir(Vec3f orig, Vec3f dir) {
    float len;
    b3BodyDef bd;
    b3ShapeDef sd;
    b3Sphere sphere;
    b3BodyId body;

    if (s_world.index1 == 0 || s_ballCount >= BP_MAX_BALLS) return;
    len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (len < 0.001f) return;
    dir[0] /= len; dir[1] /= len; dir[2] /= len;

    bd = b3DefaultBodyDef();
    bd.type = b3_dynamicBody;
    bd.position = (b3Pos){
        (orig[0] + dir[0] * 40.0f) * BP_SCALE,
        (orig[1] + dir[1] * 40.0f) * BP_SCALE,
        (orig[2] + dir[2] * 40.0f) * BP_SCALE,
    };
    bd.linearVelocity = (b3Vec3){
        dir[0] * BP_BALL_SPEED, dir[1] * BP_BALL_SPEED, dir[2] * BP_BALL_SPEED,
    };
    bd.isBullet = true;
    bd.linearDamping = 0.05f;
    bd.angularDamping = 0.05f;
    body = b3CreateBody(s_world, &bd);

    sd = b3DefaultShapeDef();
    sd.density = 3000.0f;         /* heavy projectile: ~196 kg */
    sd.baseMaterial.friction = 0.4f;
    sd.baseMaterial.restitution = 0.3f;
    sphere.center = (b3Vec3){ 0.0f, 0.0f, 0.0f };
    sphere.radius = BP_BALL_R;
    b3CreateSphereShape(body, &sd, &sphere);

    s_balls[s_ballCount] = body;
    s_ballTime[s_ballCount] = s_tick;
    s_ballCount++;
    play_sound(SOUND_ACTION_THROW, gDefaultSoundArgs);
}

void boxphys_fire(void) {
    Vec3f dir, orig;

    if (!s_equipped || gMarioState == NULL) return;
    orig[0] = gLakituState.pos[0];
    orig[1] = gLakituState.pos[1];
    orig[2] = gLakituState.pos[2];
    dir[0] = gLakituState.focus[0] - orig[0];
    dir[1] = gLakituState.focus[1] - orig[1];
    dir[2] = gLakituState.focus[2] - orig[2];
    boxphys_fire_dir(orig, dir);
}

int boxphys_test_fire(void) {
    Vec3f orig, dir;
    boxphys_init();
    if (s_world.index1 == 0) return 0;
    orig[0] = gLakituState.pos[0];
    orig[1] = gLakituState.pos[1];
    orig[2] = gLakituState.pos[2];
    dir[0] = BP_PYRAMID_X / BP_SCALE - orig[0];
    dir[1] = 80.0f - orig[1];
    dir[2] = BP_PYRAMID_Z / BP_SCALE - orig[2];
    boxphys_fire_dir(orig, dir);
    return 1;
}

/* Highest upward-facing cube face.  Requires dot(worldNormal, up) >= 0.55 so
 * Mario only lands on a face that actually points up, as cubes tumble. */
static int bp_up_face(b3BodyId id, Vec3f center, Vec3f uax, Vec3f vax, float *topOut) {
    static const b3Vec3 axis[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    b3Pos p = b3Body_GetPosition(id);
    b3Quat q = b3Body_GetRotation(id);
    float best = 0.55f;
    int bestAxis = -1, bestSign = 1, a, sign;

    for (a = 0; a < 3; a++) {
        for (sign = -1; sign <= 1; sign += 2) {
            b3Vec3 n = b3RotateVector(q, (b3Vec3){
                axis[a].x * sign, axis[a].y * sign, axis[a].z * sign });
            if (n.y > best) {
                best = n.y;
                bestAxis = a;
                bestSign = sign;
            }
        }
    }
    if (bestAxis < 0) return 0;

    {
        b3Vec3 n = b3RotateVector(q, (b3Vec3){
            axis[bestAxis].x * bestSign, axis[bestAxis].y * bestSign,
            axis[bestAxis].z * bestSign });
        b3Vec3 u = b3RotateVector(q, axis[(bestAxis + 1) % 3]);
        b3Vec3 v = b3RotateVector(q, axis[(bestAxis + 2) % 3]);
        float lift = BP_CUBE_HALF * (fabsf(u.y) + fabsf(v.y));

        center[0] = (p.x + n.x * BP_CUBE_HALF) / BP_SCALE;
        center[1] = (p.y + n.y * BP_CUBE_HALF) / BP_SCALE;
        center[2] = (p.z + n.z * BP_CUBE_HALF) / BP_SCALE;
        uax[0] = u.x; uax[1] = u.y; uax[2] = u.z;
        vax[0] = v.x; vax[1] = v.y; vax[2] = v.z;
        *topOut = center[1] + lift / BP_SCALE;
    }
    return 1;
}

/* Radial shockwave: shove nearby cubes outward and up. */
static void bp_slam(float x, float z, float radius, float power) {
    int b;
    for (b = 0; b < s_boxCount; b++) {
        b3Pos p = b3Body_GetPosition(s_boxes[b]);
        float dx = p.x / BP_SCALE - x;
        float dz = p.z / BP_SCALE - z;
        float d2 = dx * dx + dz * dz;
        if (d2 > radius * radius) continue;
        {
            float d = sqrtf(d2);
            float k = 1.0f - d / radius;
            float inv = 1.0f / (d + 1.0f);
            b3Body_ApplyLinearImpulseToCenter(s_boxes[b],
                (b3Vec3){ dx * inv * power * k, power * 0.85f * k,
                          dz * inv * power * k }, true);
        }
    }
}

void boxphys_slam_test(void) {
    if (gMarioState == NULL) return;
    boxphys_init();
    bp_slam(gMarioState->pos[0], gMarioState->pos[2], 340.0f, 1600.0f);
    play_sound(SOUND_ACTION_TERRAIN_HEAVY_LANDING, gDefaultSoundArgs);
}

void boxphys_update(void) {
    int i;

    if (gCurrLevelNum != 38) return;
    boxphys_init();
    if (s_world.index1 == 0) return;

    /* Green reset pad: landing on it rebuilds the pyramid. */
    if (gMarioState != NULL) {
        struct MarioState *m = gMarioState;
        int onPad = m->pos[0] > 1120.0f && m->pos[0] < 1380.0f
                 && m->pos[2] > 620.0f && m->pos[2] < 880.0f
                 && m->pos[1] > 25.0f && m->pos[1] < 70.0f;
        if (onPad && !s_onResetPad) {
            s_onResetPad = 1;
            boxphys_reset();
            return;
        }
        s_onResetPad = onPad;
    }

    if (gMarioState != NULL && s_marioBody.index1 != 0) {
        b3Body_SetTransform(s_marioBody, (b3Pos){
            gMarioState->pos[0] * BP_SCALE,
            (gMarioState->pos[1] + 80.0f) * BP_SCALE,
            gMarioState->pos[2] * BP_SCALE,
        }, b3Quat_identity);
        b3Body_SetLinearVelocity(s_marioBody, (b3Vec3){
            gMarioState->vel[0] * BP_SCALE,
            gMarioState->vel[1] * BP_SCALE,
            gMarioState->vel[2] * BP_SCALE,
        });
    }

    /* Punch / kick impact: a fresh attack action shoves nearby cubes. */
    {
        static int s_attackCooldown;
        u32 a = gMarioState != NULL ? gMarioState->action : 0;
        int attacking = (a == ACT_PUNCHING || a == ACT_MOVE_PUNCHING
                      || a == ACT_JUMP_KICK || a == ACT_SLIDE_KICK);
        if (s_attackCooldown > 0) s_attackCooldown--;
        if (attacking && s_attackCooldown == 0 && gMarioState != NULL) {
            float fx = sins(gMarioState->faceAngle[1]);
            float fz = coss(gMarioState->faceAngle[1]);
            int b;
            for (b = 0; b < s_boxCount; b++) {
                b3Pos bp = b3Body_GetPosition(s_boxes[b]);
                float dx = bp.x / BP_SCALE - gMarioState->pos[0];
                float dz = bp.z / BP_SCALE - gMarioState->pos[2];
                float d2 = dx * dx + dz * dz;
                if (d2 < 210.0f * 210.0f) {
                    float inv = 1.0f / sqrtf(d2 + 1.0f);
                    if (dx * inv * fx + dz * inv * fz > -0.25f) {
                        b3Body_ApplyLinearImpulseToCenter(s_boxes[b],
                            (b3Vec3){ fx * 1400.0f + dx * 2.0f, 360.0f,
                                      fz * 1400.0f + dz * 2.0f }, true);
                    }
                }
            }
            s_attackCooldown = 10;
        }
    }

    s_tick++;
    b3World_Step(s_world, 1.0f / 30.0f, 6);


    /* Stand on an upward-facing cube face.  Mario's static collision knows
     * nothing about the physics cubes, so this runs after his action step and
     * pins him to the face: idle action (no fall pose), no sliding, and a
     * floorHeight that keeps the camera/HUD stable. */
    if (gMarioState != NULL) {
        struct MarioState *m = gMarioState;
        int b;
        for (b = 0; b < s_boxCount; b++) {
            Vec3f c, u, v;
            float top, du, dv, margin = 60.0f + 16.0f;

            if (!bp_up_face(s_boxes[b], c, u, v, &top)) continue;
            du = (m->pos[0] - c[0]) * u[0] + (m->pos[1] - c[1]) * u[1]
               + (m->pos[2] - c[2]) * u[2];
            dv = (m->pos[0] - c[0]) * v[0] + (m->pos[1] - c[1]) * v[1]
               + (m->pos[2] - c[2]) * v[2];
            if (fabsf(du) > margin || fabsf(dv) > margin) continue;
            if (m->pos[1] > top + 16.0f || m->pos[1] < top - 55.0f) continue;
            if (m->vel[1] > 20.0f) continue;

            if (m->action == ACT_GROUND_POUND) {
                /* Land the slam on the cube top; the shockwave runs below. */
                m->forwardVel = 0.0f;
                m->vel[0] = m->vel[2] = 0.0f;
                set_mario_action(m, ACT_GROUND_POUND_LAND, 0);
            } else if (m->action & ACT_FLAG_AIR) {
                /* First contact: kill the fall/slide carry-over so he lands
                 * instead of skidding in the falling pose. */
                m->forwardVel = 0.0f;
                m->vel[0] = m->vel[2] = 0.0f;
                set_mario_action(m, ACT_IDLE, 0);
            } else {
                m->forwardVel = 0.0f;
                m->vel[0] = m->vel[2] = 0.0f;
            }
            m->pos[1] = top;
            m->vel[1] = 0.0f;
            m->floorHeight = top;
            m->floorAngle = 0;
            if (m->marioObj != NULL) {
                m->marioObj->header.gfx.pos[1] = top;
            }
            break;
        }
    }

    /* Ground-pound shockwave: fires once on the landing tick, whether Mario
     * hit the floor or the support code just landed him on a cube. */
    if (gMarioState != NULL) {
        struct MarioState *m = gMarioState;
        if (m->action == ACT_GROUND_POUND_LAND && s_lastAction != ACT_GROUND_POUND_LAND) {
            bp_slam(m->pos[0], m->pos[2], 320.0f, 1500.0f);
            play_sound(SOUND_ACTION_TERRAIN_HEAVY_LANDING, gDefaultSoundArgs);
        }
        s_lastAction = m->action;
    }

    /* Expire old balls. */
    for (i = 0; i < s_ballCount; ) {
        b3Pos p = b3Body_GetPosition(s_balls[i]);
        if (s_tick - s_ballTime[i] > 300 || p.y < -5.0f) {
            b3DestroyBody(s_balls[i]);
            s_balls[i] = s_balls[s_ballCount - 1];
            s_ballTime[i] = s_ballTime[s_ballCount - 1];
            s_ballCount--;
        } else {
            i++;
        }
    }

    if (s_equipped && gPlayer1Controller != NULL
        && (gPlayer1Controller->buttonPressed & Z_TRIG)) {
        boxphys_fire();
    }
}

/* ------------------------------------------------------------- rendering */

static void bp_fill(Vtx *v, float x, float y, float z, u8 r, u8 g, u8 b) {
    v->v.ob[0] = (s16)x;
    v->v.ob[1] = (s16)y;
    v->v.ob[2] = (s16)z;
    v->v.flag = 0;
    v->v.tc[0] = 0;
    v->v.tc[1] = 0;
    v->v.cn[0] = r;
    v->v.cn[1] = g;
    v->v.cn[2] = b;
    v->v.cn[3] = 255;
}

static void bp_draw_box(b3BodyId id, int layer) {
    static const u8 cols[4][3] = {
        { 235, 140, 60 }, { 80, 190, 110 }, { 90, 140, 230 }, { 230, 205, 80 },
    };
    b3Pos pos = b3Body_GetPosition(id);
    b3Quat q = b3Body_GetRotation(id);
    float cx[8], cy[8], cz[8];
    float x[8], y[8], z[8];
    Vtx *v;
    int i, idx = 0;
    static const float lx[8] = { -1, 1, 1, -1, -1, 1, 1, -1 };
    static const float ly[8] = { -1, -1, 1, 1, -1, -1, 1, 1 };
    static const float lz[8] = { -1, -1, -1, -1, 1, 1, 1, 1 };

    v = alloc_display_list(sizeof(Vtx) * 24);
    if (v == NULL) return;

    for (i = 0; i < 8; i++) {
        b3Vec3 p = b3RotateVector(q, (b3Vec3){
            lx[i] * BP_CUBE_HALF, ly[i] * BP_CUBE_HALF, lz[i] * BP_CUBE_HALF });
        x[i] = (pos.x + p.x) / BP_SCALE;
        y[i] = (pos.y + p.y) / BP_SCALE;
        z[i] = (pos.z + p.z) / BP_SCALE;
        cx[i] = x[i]; cy[i] = y[i]; cz[i] = z[i];
    }
    /* Six faces with the outward winding of the unit cube.  Per-corner
     * shading gives a bottom-to-top gradient so the faces are not flat. */
    {
        static const int f[6][4] = {
            {0,3,2,1}, {4,5,6,7}, {0,1,5,4}, {3,7,6,2}, {0,4,7,3}, {1,2,6,5},
        };
        float top = -1e9f, bottom = 1e9f;
        for (i = 0; i < 8; i++) {
            if (cy[i] > top) top = cy[i];
            if (cy[i] < bottom) bottom = cy[i];
        }
        for (i = 0; i < 6; i++) {
            int k;
            for (k = 0; k < 4; k++) {
                int ci = f[i][k];
                float t = (cy[ci] - bottom) / (top - bottom + 0.001f);
                float shade = 0.55f + 0.45f * t;
                bp_fill(&v[idx++], cx[ci], cy[ci], cz[ci],
                        (u8)(cols[layer & 3][0] * shade),
                        (u8)(cols[layer & 3][1] * shade),
                        (u8)(cols[layer & 3][2] * shade));
            }
        }
    }
    gSPVertex(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(v), 24, 0);
    for (i = 0; i < 6; i++) {
        int b = i * 4;
        gSP1Triangle(gDisplayListHead++, b, b + 1, b + 2, 0);
        gSP1Triangle(gDisplayListHead++, b, b + 2, b + 3, 0);
    }
}

static void bp_draw_ball(b3BodyId id) {
    b3Pos p = b3Body_GetPosition(id);
    Vtx *v = alloc_display_list(sizeof(Vtx) * 6);
    float cx = p.x / BP_SCALE, cy = p.y / BP_SCALE, cz = p.z / BP_SCALE;
    float r = BP_BALL_R / BP_SCALE;
    if (v == NULL) return;
    bp_fill(&v[0], cx, cy + r, cz, 235, 235, 245);
    bp_fill(&v[1], cx, cy - r, cz, 200, 200, 215);
    bp_fill(&v[2], cx + r, cy, cz, 220, 220, 235);
    bp_fill(&v[3], cx, cy, cz + r, 220, 220, 235);
    bp_fill(&v[4], cx - r, cy, cz, 200, 200, 215);
    bp_fill(&v[5], cx, cy, cz - r, 220, 220, 235);
    gSPVertex(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(v), 6, 0);
    gSP1Triangle(gDisplayListHead++, 0, 2, 3, 0);
    gSP1Triangle(gDisplayListHead++, 0, 3, 4, 0);
    gSP1Triangle(gDisplayListHead++, 0, 4, 5, 0);
    gSP1Triangle(gDisplayListHead++, 0, 5, 2, 0);
    gSP1Triangle(gDisplayListHead++, 1, 3, 2, 0);
    gSP1Triangle(gDisplayListHead++, 1, 4, 3, 0);
    gSP1Triangle(gDisplayListHead++, 1, 5, 4, 0);
    gSP1Triangle(gDisplayListHead++, 1, 2, 5, 0);
}

void boxphys_render(void) {
    Mat4 view;
    if (gCurrLevelNum != 38) return;
    Mtx *mtx;
    int i;

    if (s_world.index1 == 0) return;
    mtxf_lookat(view, gLakituState.pos, gLakituState.focus, 0);
    mtx = alloc_display_list(sizeof(*mtx));
    if (mtx == NULL) return;
    mtxf_to_mtx(mtx, view);
    gSPMatrix(gDisplayListHead++, VIRTUAL_TO_PHYSICAL(mtx),
              G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);

    gSPSetGeometryMode(gDisplayListHead++, G_ZBUFFER | G_SHADE | G_CULL_BACK);
    gSPClearGeometryMode(gDisplayListHead++, G_LIGHTING);
    gDPSetCombineMode(gDisplayListHead++, G_CC_SHADE, G_CC_SHADE);
    gDPSetRenderMode(gDisplayListHead++, G_RM_AA_ZB_OPA_SURF, G_RM_AA_ZB_OPA_SURF2);

    if (gfx_splat_target == 3) gfx_sp_splat_marker(1);
    for (i = 0; i < s_boxCount; i++) {
        bp_draw_box(s_boxes[i], i / 8);
    }
    if (gfx_splat_target == 3) gfx_sp_splat_marker(0);
    for (i = 0; i < s_ballCount; i++) {
        bp_draw_ball(s_balls[i]);
    }

    gDPPipeSync(gDisplayListHead++);
    gDPSetCombineMode(gDisplayListHead++, G_CC_MODULATERGB, G_CC_MODULATERGB);
    gSPSetGeometryMode(gDisplayListHead++, G_LIGHTING);
    gSPClearGeometryMode(gDisplayListHead++, G_ZBUFFER);
}
