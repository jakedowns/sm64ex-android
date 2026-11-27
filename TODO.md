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
- [ ] Panel swipe axis (code-complete, needs the charged panel): vertical dock
      left/right toggles and up/down is inert; horizontal dock up/down unchanged;
      a drag started on a page/slider must not move the drawer.
- [ ] Cam-stick: heatmap still, dragged-point response, C-bit table, the three
      R-follow clips.
- [ ] Wall-run: six-case `ptc wrjump` rig + the chain harness (the 90° corner
      test now has a **measured** window, d 1380-1780+, 80/80 checks).
- [ ] Paint-ripple frame-alternation metric (30/60/120, `fx 350` on/off).
- [ ] Grading: operator/look/LUT stills + the cost row.
- [x] Captions in 3D (Java overlay over the weave) — verified 2026-11-26, `mode=java native=1` in 2D and 3D, GAME FONT A/B works.
- [ ] Emissive: floor-light before/after and a bloom still on a healthy panel.
- [ ] Occlusion: re-shoot the four mode stills with the radius slider shown.

## Bugs / loose ends

- [ ] Motion-vector heatmap renders a flat blue frame (`fx 31 9`, `149 2`).
- [x] `fxerr=<programs>` in the dump names any FX shader that failed to link
      (verified `fxerr=none`); a silent fallback can no longer look like a no-op.
- [ ] **Black frame after sleep / resume** (subagent on it now) — the app can come back with the 3D
      layer black (touch overlay and panel draw, game does not) and `HOME` + reopen
      does not always recover it; observed twice today while verifying the swipe
      axis. This blocks visual verification of several items and is the most
      user-visible bug on the list.
- [ ] App sometimes "refuses to die" on relaunch (`ProcessRecord … refused to
      die` in dropbox); find the thread that survives `am force-stop`.
- [ ] Bloom is added after the FX pass, so emissive pixels are not rolled off by
      the tonemap (move bloom before the grade when both are on).
- [ ] Slow-mo still cannot interpolate limb animation (root + camera only).
- [ ] Cycle the camera to FREE and back after a warp: label now correct, but the
      inherited-pose rejection seeds a behind-Mario pose — verify it reads well.
- [ ] Wall-run rig pass window: measure and document the timing window at a 90°
      corner, and whether an early/late jump should bounce rather than fall.
      The window is measured (1380-1780 passes, 1280 and below fails) and the
      sweep now also runs in-engine (`DEBUG > MOVE > RUN CORNER SWEEP` /
      `cmd cornersweep`, dump `csw=`/`cswc=`); device-verify that its verdict
      matches the host harness, then tick this line.

## Performance, next (biggest levers left)

- [ ] **Half-resolution FX/post pass** — the sweep's biggest remaining win: `set
      12` only shrinks the scene raster, so a 76.9 ms 3D stack drops just 13 %.- [ ] GI at 1/8 res (`div 8` is one line); DOF at `div 4` as a shipping default;
      bokeh at 32 taps.
- [ ] Cheapen the reduced-res upsamples (DOF's bilateral stop is ~2 ms/eye).
- [ ] MXAO: drop the normal prepass when the reject dial is off; `RGBA16F` gather
      field so 6 bilinear taps replace 12 nearest.
- [x] **Half-resolution FX/post pass** — `fx 420 n` renders the whole FX shader at
      `eye_res/n` and composites back in its own full-res pass. 3D: grade-only
      floor 10.53→5.95 ms, MED 21.3→12.8, HIGH 28.6→20.0, FX-only 27.8→14.3
      (n=1 vs n=2). Default stays 1; ship 2 for 3D MED/HIGH after a human stereo
      A/B. n=3 adds only ~6 %. `findings/fx-pass-half-res-11-26-26.md`.
- [ ] Fix `fx 103` (dynamic lights): `gfx_pc.c`'s per-frame registry now resets
      the light count unless a caller claims the slots, so the sweep's `103=`
      preset rows silently measure a lightless scene.
- [x] **LOW/MED/HIGH presets** — `fx 430` / `431`, `cmd profile`, the QUALITY row
      on the first FX page, `preset=` in the dump. Re-derived on the new
      DOF/SSGI/MXAO/FX-half-res code: 3D 9.2 / 14.2 / 36.6 ms (109 / 70 / 27 fps).
      `findings/quality-presets-11-26-26.md`.
- [ ] HIGH is 2.7 ms over the 30 Hz budget on the hot run: trim DOF taps
      (`15 12`→`15 8`) or accept 34.7 ms with `57 4`. Also fix `381 3/4`, which
      does not recover in-process (see BACKLOG).

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

## otsm64remote (code landed, awaiting device evidence)

- [x] Panel switch (2026-11-27, code-complete): `DEBUG → SYS → REMOTE` rewrites
      only the `enabled` line of `sm64remote.cfg` (url/token/interval kept
      byte-for-byte) and starts/stops the worker with no relaunch and no adb;
      `cmd remote on|off` is the same setter. Row states: `REMOTE: OFF`,
      `ON (waiting)`, `ON (live)`, `ON (error: …)`.
- [ ] Device: toggle ON with a valid cfg (`pushed=` climbs), toggle OFF (counters
      freeze, one `REMOTE disabled by panel` line, no in-flight POST), and a
      relaunch honours the stored value. A cfg with no url/token must refuse ON
      with the reason on the button.
- [ ] Device run: viewer flips `demo` → `live` on pushes, and a relay
      `{"input":"jump"}` produces a jump in the HUD within ~100 ms. Capture the
      timestamps (`SM64Remote` log + screenshot pair) in
      `/media/share/tmp/artifacts/sm64/otsm64remote/`.
- [ ] Disabled run: with `enabled = false`, one `disabled` line and no pushes
      for a full level load (proves byte-identical behaviour).
- [ ] Decide whether the relay keeps the 12.5 Hz default or 15 Hz reads better
      for the browser's `live` badge.

## Tooling / process

- [ ] Shared `ccache`/`sccache` for the ndk-build translation units (needs
      install; would remove most of the per-worktree build cost).
- [ ] A `scripts/worktree-new.sh` that provisions a worktree completely
      (`NDK_HOST_PYTHON=/usr/bin/python3` + copy `app/jni/src/build/us_pc`).
- [ ] Cap build-heavy parallel worktrees at 3 in practice (documented, not
      enforced by the lock).
- [ ] `device-quick` adoption by the agent scripts that only read state.
