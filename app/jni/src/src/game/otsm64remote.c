/* otsm64remote.c - opt-in telemetry + one-tick remote input bridge.
 *
 * Two game-thread hooks, both single calls:
 *
 *   otsm64remote_tick_begin()  after read_controller_inputs(), applies a
 *                              queued "jump" as one tick of A_BUTTON;
 *   otsm64remote_capture()     at the end of execute_mario_action(), copies
 *                              gMarioState pos/yaw/action plus the animation
 *                              id/frame that set_mario_animation() wrote.
 *
 * The capture path only *reads* state that the normal game tick already
 * computed, so no simulation is duplicated and no value feeds back into
 * gameplay. Samples go into a fixed 8-slot single-producer/single-consumer
 * ring with release/acquire counters; a full ring drops the newest sample so
 * the game thread never waits. The Java worker drains it through JNI.
 *
 * Nothing here opens a socket or allocates.
 */

#include <ultra64.h>

#include "sm64.h"
#include "mario.h"
#include "level_update.h"
#include "game_init.h"
#include "otsm64remote.h"

#include <stdio.h>
#include <string.h>

/* Wall clock in ms, defined next to the other Android/SDL helpers
 * (gfx_sdl2.c). Matches what `cmd dump` timing uses. */
extern unsigned int gfx_uptime_ms(void);

/* gPlayer1Controller lives in game_init.c. */
extern struct Controller *gPlayer1Controller;

/* A remote press older than this is dropped instead of applied: the relay is
 * gone, so a late input must not teleport out of nowhere. */
/* (OTSM64REMOTE_INPUT_TTL_MS lives in the header next to the protocol.) */

static volatile int s_enabled;

/* --- lock-free ring: one producer (game thread), one consumer (JNI) ------- */

static struct Otsm64RemoteSample s_ring[OTSM64REMOTE_SLOTS];
static unsigned int s_head; /* written by the producer only */
static unsigned int s_tail; /* written by the consumer only */
static unsigned int s_seq;

/* Diagnostics; races with the reader are harmless (plain counters). */
static volatile unsigned int s_captured;
static volatile unsigned int s_dropped;

/* --- one-tick remote input ---------------------------------------------- */

static volatile int s_input_pending;
static volatile unsigned int s_input_queued_ms;
static volatile unsigned int s_input_queued_count;
static volatile unsigned int s_input_applied_count;
static volatile unsigned int s_input_expired_count;
static volatile unsigned int s_input_last_apply_ms;

void otsm64remote_enable(int on) {
    s_enabled = on ? 1 : 0;
    if (!s_enabled) {
        /* Keep the ring from replaying stale state when re-enabled. */
        s_tail = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
        s_input_pending = 0;
        s_input_queued_ms = 0;
    }
}

int otsm64remote_enabled(void) {
    return s_enabled;
}

void otsm64remote_queue_jump(void) {
    if (!s_enabled) {
        return;
    }
    s_input_pending = 1;
    s_input_queued_ms = gfx_uptime_ms();
    s_input_queued_count++;
}

void otsm64remote_clear_input(void) {
    s_input_pending = 0;
    s_input_queued_ms = 0;
}

void otsm64remote_tick_begin(void) {
    unsigned int now;
    int pending;

    if (!s_enabled) {
        return;
    }
    pending = s_input_pending;
    if (!pending) {
        return;
    }
    /* Always consume: a command is worth exactly one tick, never a repeat. */
    s_input_pending = 0;

    now = gfx_uptime_ms();
    if (s_input_queued_ms != 0 && (unsigned int) (now - s_input_queued_ms) > OTSM64REMOTE_INPUT_TTL_MS) {
        s_input_expired_count++;
        s_input_queued_ms = 0;
        return;
    }
    s_input_queued_ms = 0;

    /* Exactly the same field the pad writes; the next read_controller_inputs()
     * recomputes it, so this press cannot survive into a second tick. */
    if (gPlayer1Controller != NULL) {
        gPlayer1Controller->buttonPressed |= A_BUTTON;
        gPlayer1Controller->buttonDown |= A_BUTTON;
    }
    s_input_last_apply_ms = now;
    s_input_applied_count++;
}

/* --- capture ------------------------------------------------------------ */

/* Short, stable labels for the actions a viewer actually sees. The fallback
 * is the ACT_GROUP name, so "action" is never empty. */
static const struct {
    int id;
    const char *name;
} s_action_names[] = {
    { 0x001, "idle" },        { 0x003, "sleeping" },   { 0x020, "crouch" },
    { 0x040, "walk" },        { 0x045, "braking" },    { 0x050, "slide_start" },
    { 0x052, "butt_slide" },  { 0x053, "belly_slide" },{ 0x056, "dive_slide" },
    { 0x059, "crouch_slide" },{ 0x070, "jump_land" },  { 0x071, "land" },
    { 0x072, "djump_land" },  { 0x078, "tjump_land" }, { 0x080, "jump" },
    { 0x081, "double_jump" }, { 0x082, "triple_jump" },{ 0x083, "backflip" },
    { 0x086, "wallkick" },    { 0x087, "sideflip" },  { 0x088, "long_jump" },
    { 0x08A, "dive" },        { 0x08C, "freefall" },  { 0x0A9, "ground_pound" },
    { 0x0C0, "water_idle" },  { 0x0D1, "swim_end" },  { 0x105, "reading" },
    { 0x106, "dialog" },      { 0x108, "sign" },      { 0x111, "dying" },
    { 0x143, "climbing" },    { 0x149, "hanging" },   { 0x180, "punch" },
    { 0x09A, "wallrun" },
};

static const char *action_label(int action) {
    int id = action & ACT_ID_MASK;
    unsigned int i;

    for (i = 0; i < sizeof(s_action_names) / sizeof(s_action_names[0]); i++) {
        if (s_action_names[i].id == id) {
            return s_action_names[i].name;
        }
    }
    switch (action & ACT_GROUP_MASK) {
        case ACT_GROUP_STATIONARY: return "stationary";
        case ACT_GROUP_MOVING:     return "moving";
        case ACT_GROUP_AIRBORNE:   return "air";
        case ACT_GROUP_SUBMERGED:  return "submerged";
        case ACT_GROUP_CUTSCENE:   return "cutscene";
        case ACT_GROUP_AUTOMATIC:  return "automatic";
        case ACT_GROUP_OBJECT:     return "object";
    }
    return "other";
}

void otsm64remote_capture(void) {
    struct MarioState *m = gMarioState;
    struct Otsm64RemoteSample *s;
    unsigned int head, tail;
    const char *name;

    if (!s_enabled || m == NULL || m->marioObj == NULL) {
        return;
    }

    head = s_head;
    tail = __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE);
    if ((unsigned int) (head - tail) >= OTSM64REMOTE_SLOTS) {
        /* Full: drop the newest. The consumer is behind, and blocking here
         * would stall the simulation. */
        s_dropped++;
        return;
    }

    s = &s_ring[head & (OTSM64REMOTE_SLOTS - 1)];
    s->seq = ++s_seq;
    s->t_ms = gfx_uptime_ms();
    /* -1 means "no animation yet" (init_mario) or "before the first frame";
     * relays commonly validate anim/frame >= 0, so mirror 0 instead of
     * letting a whole push be rejected. */
    s->anim = m->marioObj->header.gfx.unk38.animID;
    s->frame = m->marioObj->header.gfx.unk38.animFrame;
    if (s->anim < 0) s->anim = 0;
    if (s->frame < 0) s->frame = 0;
    s->action = m->action;
    /* Units are centimetres and the simulation is 30 Hz, so 0.3 converts
     * units/tick to metres/second. */
    s->speed = sqrtf(m->vel[0] * m->vel[0] + m->vel[2] * m->vel[2]) * 0.3f;
    s->p[0] = m->pos[0] * 0.01f;
    s->p[1] = m->pos[1] * 0.01f;
    s->p[2] = m->pos[2] * 0.01f;
    /* s16 angle: 65536 units per turn. */
    s->yaw = (float) m->faceAngle[1] * (6.283185307179586f / 65536.0f);
    name = action_label(m->action);
    strncpy(s->action_name, name, OTSM64REMOTE_ACTION_LEN - 1);
    s->action_name[OTSM64REMOTE_ACTION_LEN - 1] = '\0';

    s_captured++;
    __atomic_store_n(&s_head, head + 1, __ATOMIC_RELEASE);
}

int otsm64remote_poll_json(char *out, int cap) {
    const struct Otsm64RemoteSample *s;
    unsigned int tail, head;
    int n;

    if (out == NULL || cap <= 0) {
        return 0;
    }
    head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
    tail = s_tail;
    if (head == tail) {
        return 0;
    }
    s = &s_ring[tail & (OTSM64REMOTE_SLOTS - 1)];
    n = snprintf(out, (size_t) cap,
                 "{\"t\":%u,\"anim\":%d,\"frame\":%d,\"action\":\"%s\","
                 "\"speed\":%.2f,\"p\":[%.2f,%.2f,%.2f],\"yaw\":%.3f}",
                 s->t_ms, s->anim, s->frame, s->action_name, s->speed,
                 s->p[0], s->p[1], s->p[2], s->yaw);
    __atomic_store_n(&s_tail, tail + 1, __ATOMIC_RELEASE);
    if (n < 0) {
        return 0;
    }
    if (n >= cap) {
        return cap - 1;
    }
    return n;
}

int otsm64remote_stat(int which) {
    unsigned int now;

    switch (which) {
        case 0: return s_enabled;
        case 1: return (int) s_input_queued_count;
        case 2: return (int) s_input_applied_count;
        case 3: return (int) s_input_expired_count;
        case 4: return (int) s_captured;
        case 5: return (int) s_dropped;
        case 6: return s_input_pending ? 1 : 0;
        case 7:
            now = gfx_uptime_ms();
            if (s_input_last_apply_ms == 0) {
                return -1;
            }
            return (int) (now - s_input_last_apply_ms);
        case 8:
            return (int) (__atomic_load_n(&s_head, __ATOMIC_ACQUIRE)
                          - __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE));
    }
    return 0;
}
