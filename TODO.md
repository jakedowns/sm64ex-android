# TODO — open work, checked in

Session-wide list, separate from `BACKLOG.md` (which holds the long-form notes and
the *why*). This file is the short "what is left" that survives a lost chat.
Update it in the same commit as the work that closes a line.

Status: 2026-11-26. Landed this session: rays, emissive/bloom/jetpack light,
occlusion punch-through, slow-mo sub-frame interpolation, HFR held-object fix,
touch layout editor, camera sensitivity band + FREE-mode fix, feedback outbox,
wall-run stick + release window, grading/LUT, paint-ripple interpolation,
grapple targeting view, cam-stick curve/heatmap/C-bits/R-follow, panel
compaction + docking, Java captions, vertical dock, and the three perf rewrites
(DOF −53 %, SSGI −77 %, MXAO −67 %).

## Blocked on hardware (tablet at 1 % — needs a charge)

- [ ] Panel stills: VERT short page vs tall page (+ scroll), HORIZ pixel-diff
      baseline, sub-strips both docks, collapsed, restart persistence.
- [ ] Cam-stick: heatmap still, dragged-point response, C-bit table, the three
      R-follow clips.
- [ ] Wall-run: six-case `ptc wrjump` rig, the chain harness, and the 90° corner
      smoke test (`scripts/wallrun-corner-test.py`).
- [ ] Paint-ripple frame-alternation metric (30/60/120, `fx 350` on/off).
- [ ] Grading: operator/look/LUT stills + the cost row.
- [ ] Captions in 3D (Java overlay over the weave) + a GAME FONT A/B still.
- [ ] Emissive: floor-light before/after and a bloom still on a healthy panel.
- [ ] Occlusion: re-shoot the four mode stills with the radius slider shown.

## Bugs / loose ends

- [ ] Motion-vector heatmap renders a flat blue frame (`fx 31 9`, `149 2`).
- [ ] A failed FX shader link silently falls back to the legacy path — add
      `fxerr=<program>` to the dump so it cannot look like a no-op optimisation.
- [ ] App sometimes "refuses to die" on relaunch (`ProcessRecord … refused to
      die` in dropbox); find the thread that survives `am force-stop`.
- [ ] Bloom is added after the FX pass, so emissive pixels are not rolled off by
      the tonemap (move bloom before the grade when both are on).
- [ ] Slow-mo still cannot interpolate limb animation (root + camera only).
- [ ] Cycle the camera to FREE and back after a warp: label now correct, but the
      inherited-pose rejection seeds a behind-Mario pose — verify it reads well.
- [ ] Wall-run rig pass window: measure and document the timing window at a 90°
      corner, and whether an early/late jump should bounce rather than fall.

## Performance, next (biggest levers left)

- [ ] **Half-resolution FX/post pass** — the sweep's biggest remaining win: `set
      12` only shrinks the scene raster, so a 76.9 ms 3D stack drops just 13 %.
- [ ] GI at 1/8 res (`div 8` is one line); DOF at `div 4` as a shipping default;
      bokeh at 32 taps.
- [ ] Cheapen the reduced-res upsamples (DOF's bilateral stop is ~2 ms/eye).
- [ ] MXAO: drop the normal prepass when the reject dial is off; `RGBA16F` gather
      field so 6 bilinear taps replace 12 nearest.
- [ ] LOW/MED/HIGH presets from `findings/fx-sweep-11-25-26.md`, re-measured on
      the new DOF/SSGI/MXAO code.

## Not started (from the session, still wanted)

- [ ] Crepuscular rays hero demo (`ptc raysdemo` parking a light low behind a
      pillar); rays from emissive sources (star/1-up/nozzle).
- [ ] Per-eye 6DoF rig (Phase C) — head-tracked parallax exists, tracker z axis
      unvalidated.
- [ ] Portal phase 3: through-portal camera relay + Mario clone in the exit.
- [ ] Independent voice/gender per project for audio (preamble identifies the
      speaker instead).
- [ ] Skybox fine-tune dials surfaced on the panel as sliders of their own
      (currently `370`-`373` plus the collapsible group).
- [ ] Debug text in Java beyond captions: the `gShowDebugText` "BUF" line is
      still drawn with the game font.

## Tooling / process

- [ ] Shared `ccache`/`sccache` for the ndk-build translation units (needs
      install; would remove most of the per-worktree build cost).
- [ ] A `scripts/worktree-new.sh` that provisions a worktree completely
      (`NDK_HOST_PYTHON=/usr/bin/python3` + copy `app/jni/src/build/us_pc`).
- [ ] Cap build-heavy parallel worktrees at 3 in practice (documented, not
      enforced by the lock).
- [ ] `device-quick` adoption by the agent scripts that only read state.
