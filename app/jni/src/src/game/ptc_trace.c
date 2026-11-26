/**
 * Fast-forward trace for the ninja-chain test runner.
 *
 * The runner queues a tick count (ptc_trace_request_ticks) from the debug
 * channel; the game thread takes it in produce_one_frame(), sets
 * gfx_test_uncapped, runs the ticks back to back and calls ptc_trace_tick()
 * after each one.  Every PTC_TRACE_EVERY ticks one compact line is appended:
 *
 *   t=120 ph=12 fl=0x020 act=0x0300089A pos=-408,509,-1536 st=10 gx=0 cm=3
 *
 * t   gGlobalTimer (simulation tick, 30 Hz)
 * ph  chain phase (ptc_chain_state)
 * fl  chain flags bitmask (hex)
 * act Mario's action id
 * pos Mario's position, rounded to N64 units
 * st  star0/star1 collected flags as two digits
 * gx  goomba portal crossings
 * cm  gfx_cam_mode (3 = custom camera)
 *
 * The buffer is a fixed 16 KB overwrite-safe ring: when it fills, the oldest
 * complete lines are dropped, so a long run keeps its tail instead of
 * failing.  Only the game thread appends; the getter is read-only.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "sm64.h"

#include "game/display.h"
#include "game/level_update.h"
#include "game/ptc.h"
#include "game/ptc_trace.h"

extern int gfx_cam_mode;

#define PTC_TRACE_BUF_SIZE 16384

static char s_buf[PTC_TRACE_BUF_SIZE];
static int s_len;
static int s_tick;
static volatile int s_pending_ticks;
static unsigned long long s_start_ms;

static unsigned long long trace_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long) ts.tv_sec * 1000ULL
         + (unsigned long long) ts.tv_nsec / 1000000ULL;
}

/* Drop the oldest complete line; if the buffer somehow holds no newline,
 * start over.  Keeps the buffer NUL-terminated at all times. */
static void trace_drop_oldest_line(void) {
    char *nl;

    if (s_len <= 0) {
        s_buf[0] = '\0';
        return;
    }
    nl = memchr(s_buf, '\n', (size_t) s_len);
    if (nl == NULL) {
        s_len = 0;
        s_buf[0] = '\0';
        return;
    }
    {
        int drop = (int) (nl - s_buf) + 1;
        memmove(s_buf, s_buf + drop, (size_t) (s_len - drop));
        s_len -= drop;
        s_buf[s_len] = '\0';
    }
}

static void trace_append_line(const char *line) {
    int n = (int) strlen(line);

    while (s_len + n + 2 > PTC_TRACE_BUF_SIZE) {
        trace_drop_oldest_line();
    }
    memcpy(s_buf + s_len, line, (size_t) n);
    s_len += n;
    s_buf[s_len++] = '\n';
    s_buf[s_len] = '\0';
}

static void trace_appendf(const char *fmt, ...) {
    char line[192];
    va_list args;

    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    trace_append_line(line);
}

static void trace_append_state(void) {
    int phase = 0, flags = 0, crossings = 0, star0 = 0, star1 = 0;
    int x = 0, y = 0, z = 0;
    unsigned int action = 0;

    ptc_chain_state(&phase, &flags, &crossings, &star0, &star1);
    if (gMarioState != NULL) {
        action = (unsigned int) gMarioState->action;
        x = (int) gMarioState->pos[0];
        y = (int) gMarioState->pos[1];
        z = (int) gMarioState->pos[2];
    }

    trace_appendf("t=%u ph=%d fl=0x%03x act=0x%08X pos=%d,%d,%d st=%d%d gx=%d cm=%d",
                  (unsigned int) gGlobalTimer, phase, (unsigned int) (flags & 0xFFF),
                  action, x, y, z, star0 ? 1 : 0, star1 ? 1 : 0, crossings,
                  gfx_cam_mode);
}

/* Append one rig-owned line to the ring (no-op string formatting if the rig
 * is not tracing).  Used by the PTC corner rig so its per-tick state lands in
 * the same buffer `cmd trace` dumps after `cmd runticks`. */
void ptc_trace_note(const char *fmt, ...) {
    char line[192];
    va_list args;

    if (fmt == NULL) {
        return;
    }
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    trace_append_line(line);
}

void ptc_trace_request_ticks(int n) {
    if (n <= 0) {
        return;
    }
    if (n > PTC_TRACE_MAX_TICKS) {
        n = PTC_TRACE_MAX_TICKS;
    }
    s_pending_ticks = n;
}

int ptc_trace_take_ticks(void) {
    int n = s_pending_ticks;

    if (n <= 0) {
        return 0;
    }
    s_pending_ticks = 0;

    /* Start a fresh trace; the old one is only reachable through `cmd trace`
     * before the next run, which is how the harness reads it. */
    s_len = 0;
    s_buf[0] = '\0';
    s_tick = 0;
    trace_appendf("# runticks n=%d every=%d", n, PTC_TRACE_EVERY);

    s_start_ms = trace_now_ms();
    return n;
}

void ptc_trace_tick(void) {
    if ((s_tick % PTC_TRACE_EVERY) == 0) {
        trace_append_state();
    }
    s_tick++;
}

int ptc_trace_finish(int ran) {
    unsigned long long ms = trace_now_ms() - s_start_ms;
    unsigned long long real_ms = (unsigned long long) ran * 1000ULL / 30ULL;

    /* The exact final state, even when ran is not a multiple of the interval. */
    if (ran > 0) {
        trace_append_state();
    }
    trace_appendf("# done ran=%d sim_ms=%llu real30_ms=%llu", ran, ms, real_ms);
    if (ms > 0) {
        trace_appendf("# speedup_x100=%llu", real_ms * 100ULL / ms);
    }

    s_tick = 0;
    return (int) ms;
}

const char *ptc_trace_get(void) {
    return s_buf;
}
