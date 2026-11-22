#ifndef PTC_TRACE_H
#define PTC_TRACE_H

/*
 * Fast-forward runner + per-tick trace for the ninja chain (see
 * NINJA-CHAIN.md).  The simulation stays in the real engine: the debug
 * channel only counts ticks here, and the main loop runs them back to back
 * with gfx_test_uncapped while the render pass is skipped.
 *
 *   cmd runticks --ei n 200   -> ptc_trace_request_ticks(200)
 *   cmd trace                 -> ptc_trace_get()
 */

/* Largest accepted batch.  The trace keeps the most recent ~150 lines, so
 * older lines are dropped for bigger runs; the tick count itself is clamped
 * here to bound the time the game thread spends away from a rendered frame. */
#define PTC_TRACE_MAX_TICKS 2400

/* One trace line every N simulated ticks (plus a final state line). */
#define PTC_TRACE_EVERY 15

/* Queue N ticks.  Safe to call from the JNI/UI thread: the buffer itself is
 * only ever written by the game thread. */
void ptc_trace_request_ticks(int n);

/* Called by the main loop: returns the queued tick count (0 = nothing) and
 * starts a fresh trace buffer. */
int ptc_trace_take_ticks(void);

/* Call once per simulated tick inside the batch. */
void ptc_trace_tick(void);

/* Close the trace after the batch ran.  Returns the elapsed wall time. */
int ptc_trace_finish(int ran);

/* Trace text (NUL-terminated, static).  Empty string before the first run. */
const char *ptc_trace_get(void);

#endif
