#ifndef OTSM64REMOTE_H
#define OTSM64REMOTE_H

/* Opt-in telemetry + one-tick remote input bridge ("otsm64remote").
 *
 * The tablet is the authority: this module only mirrors the numeric state of
 * the running game and, at most, presses A once on request. It never changes
 * physics, input mappings or the render loop, and it does nothing at all
 * unless otsm64remote_enable(1) has been called (the Java worker does that
 * only when the config file says enabled = true).
 *
 * No sockets and no allocation live here: the game thread writes fixed-size
 * samples into a lock-free ring and the Java worker (JNI) drains it.
 *
 * Protocol and config: OTSM64REMOTE.md
 */

#define OTSM64REMOTE_SLOTS 8
#define OTSM64REMOTE_ACTION_LEN 16
#define OTSM64REMOTE_JSON_MAX 288
/* A queued remote press older than this is dropped instead of applied. */
#define OTSM64REMOTE_INPUT_TTL_MS 500

/* One game-tick sample. Plain data only: no pointers, no owned strings.
 * Positions are metres (1 SM64 unit = 1 cm), yaw is radians, speed is
 * horizontal metres/second at the 30 Hz simulation rate. */
struct Otsm64RemoteSample {
    unsigned int seq;      /* monotonic capture counter */
    unsigned int t_ms;     /* gfx_uptime_ms() at capture */
    int anim;              /* gfx unk38.animID (what set_mario_animation set) */
    int frame;             /* gfx unk38.animFrame */
    int action;            /* full ACT_* value (id + flags) */
    float speed;           /* horizontal speed, m/s */
    float p[3];            /* position, metres */
    float yaw;             /* face yaw, radians */
    char action_name[OTSM64REMOTE_ACTION_LEN]; /* constant short label */
};

/* Enable/disable. Disabled is the default and makes every hook a no-op. */
void otsm64remote_enable(int on);
int otsm64remote_enabled(void);

/* Game thread, once per tick, right after read_controller_inputs(): applies a
 * pending remote press to gPlayer1Controller for this tick only. */
void otsm64remote_tick_begin(void);

/* Game thread, once per Mario tick: captures a sample into the ring. Drops the
 * newest sample (and counts it) if the ring is full; never blocks. */
void otsm64remote_capture(void);

/* Any thread: queue a one-tick A press / drop a queued press (watchdog). */
void otsm64remote_queue_jump(void);
void otsm64remote_clear_input(void);

/* Worker thread: pop the oldest sample as a JSON object into out.
 * Returns the length written (0 when the ring is empty). */
int otsm64remote_poll_json(char *out, int cap);

/* Remote status counters:
 *   0 enabled        1 queued        2 applied       3 expired
 *   4 captured (samples stored)      5 dropped (ring was full)
 *   6 pending        7 ms since last apply          8 ring depth. */
int otsm64remote_stat(int which);

#endif /* OTSM64REMOTE_H */
