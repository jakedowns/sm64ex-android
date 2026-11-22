# Faster iteration idea — what we chose and what I recommend now

## The original suggestion (quoted)

> Short answer: don't reimplement the simulation in Python — the things we're
> tuning (wall-run trigger thresholds, input edges, collision radii) are exactly
> where a re-implementation would quietly lie, and you'd end up tuning the
> Python. Keep the real engine and remove the iteration overhead around it.
>
> 1. In-engine fast-forward: `cmd runticks N` / scenario runner using
>    `gfx_test_uncapped` (one 30 Hz tick per loop, no present/audio), plus a
>    per-tick trace.
> 2. Native Linux build of the same C engine (SDL2 + OpenGL under Xvfb/OSMesa),
>    `--scenario` CLI, JSONL + PNG out.
> 3. Python only as the host harness/planner: one device-lock session, parse the
>    dump, assert flags/stars/crossings, save checkpoints.

## What we actually went with

**Option 3, plus batching on the device.** No Python simulation, no host engine
build. `scripts/ninja-chain-test.py` now does one `agent-lock.sh device`
session: unlock → optional APK install → restart → load level 38 → run a chain
section → poll `cmd dump` every ~0.6 s → parse `chain=`, flags, stars, teleport
count, goomba crossings → save 3 screenshots → print PASS/FAIL and exit
non-zero on failure. First real result:

```
PASS  wall-run: flag 0x20 or action 0x0300089A
      flags=0x160  star=1  cmode=3
RESULT: PASS chainwall
```

That harness immediately paid for itself: it found the wall-run failure and,
after the probe radius fix, proved `ACT_WALLRUN 0x0300089A` + flag `0x20` and the
N4 star (`0x100`). Iterations are still gated by real time, but manual adb loops
are gone and the result is machine-checkable.

**Also done since:** option 1 — `cmd runticks --ei n N` + `cmd trace`, the
`ptc_trace.c` engine and the harness `--fast` mode (subagent).  A 2400-tick
full-chain run takes ~150 ms of game-thread time instead of 80 s (≈526×), with
`PASS chain (8/8)`.  **Not done:** the native host build (option 2).

## What I recommend now

**Option 1 is done** (see above).  If visual iteration becomes the bottleneck,
do option 2 next.

Design:

1. New debug command `cmd runticks --ei n 600` (and `ptc trace <section>`):
   - enable `gfx_test_uncapped`, then call `produce_one_frame()` /
     `game_loop_one_iteration()` in a loop for N ticks, without presenting;
   - every ~15 ticks append a compact line to a preallocated trace buffer,
     e.g. `t=120 ph=12 act=0300089A pos=-408,509,-1536 fl=0x020 star=0 gx=0`;
   - on completion, flush the trace to logcat (one `PTC trace` block) and
     restore `gfx_test_uncapped = 0`;
   - optionally stop early on a phase/flags mask so a section can say "run
     until flags & 0x7FF or 900 ticks".
2. Extend `scripts/ninja-chain-test.py` with `--fast`: use `runticks` and assert
   on the returned trace; keep the real-time path for screenshots.
3. Screenshot checkpoints stay in normal mode: run uncapped to tick K, turn it
   off for one rendered frame, `exec-out screencap`, continue.

Caveats: uncapped skips `gfx_run` (no portal FBO/decal/3D rendering), but the
simulation, input hook, camera update and grapple aiming all still run, which is
what the chain assertions need. Visual evidence still comes from normal mode.

Effort: ~2–4 hours including the harness `--fast` mode. Payoff: a 20–30 s chain
becomes a ~2 s adb call, and tuning the remaining stages (portal/star2/goomba)
stops being a real-time waiting game.

**Then option 2** (native Linux build) if we keep iterating on *visuals* as
well: it removes install (5–10 s) and level load (8 s), allows pixel/PNG
regression, and keeps the real engine. Keep Android for final validation.

Do **not** implement a Python simulator of Mario's movement. The wall-run
finding in this session is the example: the trigger probe radius (32/34) was
smaller than Mario's collision distance (~50), so the wall-run could never be
entered or maintained. A Python model would have been tuned around that same
wrong assumption unless it copied the engine exactly — at which point it is the
engine, just slower and less trustworthy.

## Current chain status (for context)

Verified on device (harness traces): box3d ball/punch/kick/slam (`0x00f`),
jetpack (`0x010`), wall-run `ACT_WALLRUN` + flag (`0x020`), grapple (`0x040`),
N4 star (`0x100`), portal teleport + N6 landing (`0x080`), custom camera
(`cmode=3`). Remaining to verify end-to-end: N6 star (`0x200`), goomba crossings
(`>0`), and one continuous full `chain` run with `0x7FF` + both stars.
