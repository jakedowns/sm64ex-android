# Efficient debugging loop

This project builds slowly and shares one tablet between agents. The default
loop — edit a constant, rebuild, install, boot, look — costs minutes per data
point. Almost every tuning question can be answered in seconds instead.

Read this before starting a tuning session.

## The rule

**Runtime config beats recompilation. One batch beats many loops.**

- If you are changing a **number**, use an `fx` key (or a panel slider) and
  measure. No rebuild.
- If you are changing **behaviour**, change code once, add the key that will let
  you tune it, and then sweep at runtime.
- If you are **measuring**, write the whole matrix first and run it in one
  device-lock hold.

## What a step actually costs here (measured, Lume Pad 2)

| step | cost |
| --- | --- |
| `cmd fx --ei k ... --ei v ...` (one dial) | ~0.1 s |
| one sweep step in `scripts/fx-sweep.sh` (settle + dump + screenshot) | ~3 s |
| incremental `assembleDebug` (warm, no daemon) | 20–50 s |
| `adb install -r` | 20–60 s |
| app start + PTC level load | ~20 s |
| `scripts/quick-dev.sh` capture on a live session (no install) | ~3 s |
| `scripts/quick-dev.sh` restart into PTC (QUICK DEV on / off) | ~7 s / ~12 s |
| device-lock default hold limit | 300 s |

So a rebuild-install-look cycle is **2–4 minutes**, while a dial change is
**0.1 s** — a factor of a thousand. `scripts/quick-dev.sh` is the one-command
version of the look step: it installs only when the APK changed, waits for the
level instead of sleeping, captures state + screenshot + log, and prints the
measured wall time. Ten data points by rebuild costs 30 minutes;
the same ten in one sweep costs 30 seconds.

## Required shape of a new feature

Every new effect or tunable should ship with:

1. an `fx` key (set/get in `gfx_opengl_ssfx_set/get` or equivalent),
2. a panel control on the relevant page,
3. a field in `cmd dump` / the PTC status line,
4. a line in the README FX key table.

That is not bureaucracy — it is what makes the *next* session able to tune
without a compiler. A feature without dials forces a rebuild for every
experiment.

## Running a sweep

```bash
# 1. describe the matrix once (name | fx settings | settle | caption)
$EDITOR scripts/plans/myfeature.sweep

# 2. check the plan without touching the tablet
scripts/fx-sweep.sh scripts/plans/myfeature.sweep --dry-run

# 3. run the whole plan in ONE device-lock hold
AGENT_TABLET_MUTE=1 scripts/agent-lock.sh device --reason "myfeature sweep" -- \
    scripts/fx-sweep.sh scripts/plans/myfeature.sweep --extract dn
```

Output: `results.tsv` (fps, ms, requested fields) plus one PNG per step, under
`/media/share/tmp/artifacts/sm64/sweeps/<stamp>/`. The script refuses to run
without the device lock unless you pass `--allow-unlocked`.

## Probe before you build

Before implementing a technique, measure the thing it depends on with dials that
already exist. Two examples from this repo:

- God rays: the quarter-res march turned out to be nearly free (flat from 8 to
  32 samples); the fixed cost was the blur passes and the extra FX sampler. That
  changed the tuning target before any UI was written.
- Denoiser: a temporary `glFinish` probe showed the à-trous chain was 4.3 ms and
  the composite 2.9 ms, which is why the reduced-resolution default was chosen
  rather than guessed.

## Anti-patterns (all of these happened here)

- Editing a constant, rebuilding, and repeating for each value of that constant.
- Looking at the wrong metric: `rate=` in `cmd dump` is a main-loop EMA, not a
  frame time; use `fps=` (`1000/fps`). Denoiser numbers were misread for days
  because of it.
- Trusting a stubbed clock: `osGetTime()` returns 0 in this port, so scripted
  orbits were frozen at phase 0 and looked like "the effect does nothing".
- Taking the device lock once per command instead of once per batch, which
  starves the other agents waiting on it. Short read-only probes have their own
  shared lock now (`device-quick`), so they neither queue behind a recording nor
  need a full batch.
- Running adb unlocked by accident. `fx-sweep.sh` now refuses; write other device
  scripts the same way (`AGENT_DEVICE_LOCK_HELD=1` is exported by the lock).
- Re-deriving numbers that are already written down: check `BACKLOG.md`,
  `MOTION-VECTORS.md`, `DENOISER-RATE-LIMIT.md` and the README tables first.

## Checklist before any rebuild

1. Is there already a dial for this? (Check the FX key table.)
2. Am I changing a number, or behaviour? Numbers never need a rebuild.
3. Have I batched all the edits I want in this build?
4. Have I written the measurement matrix, and does it fit in one lock hold?
5. Does the change add a dial + status field so the next experiment is free?

## Parallelism cap (learned the hard way, 2026-11-25)

Nine build-heavy worktrees at once do not scale on this VM. Measured from the
lock event log over one hour:

| symptom | number |
| --- | --- |
| longest single build hold | 1568 s (26 min) for a build that takes 20-60 s alone |
| waits that hit the timeout | 3600 s, 3103 s, 2889 s, 2288 s, 1800 s, 1500 s, 900 s |
| totals | every worktree spent ~20-50 min queued for ~10 min of work |

The lock was doing its job; the machine was not. Nine gradle JVMs plus nine
NDK compiles under the qemu toolchain oversubscribe the CPU, so every build
takes 25x longer and the queue turns into timeouts. The lock is the symptom,
not the cause.

Rules that follow:

- **Cap build-heavy parallel worktrees at ~3.** More than that and the wall
  time is worse than doing them in sequence.
- **Do not retry a timed-out build in a loop** while others are queued; you are
  competing with yourself. Wait for the queue to drain, or hand the build to the
  parent to run once.
- **Design-only work parallelizes fine** (no build, no device); a wave can be
  ten agents if only three of them need to compile.
- A shared `ccache`/`sccache` for the ndk-build translation units would remove
  most of this, but neither is installed here yet.

