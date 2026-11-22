# TESTING.md — project testing handbook

Hard-won, this-repo-specific notes for testing rendering/portal work on the
tablet. Read this before you start a debug session or hand a task to a
subagent. General repo rules live in `AGENTS.md`; lock semantics live in
`multi-agent-rules.md`; the chamber layout lives in `PORTAL-TEST-CHAMBER.md`
(note: that doc lags the current generator — `ptc dump` is the source of truth).

## 0. TL;DR

```bash
export JAVA_HOME=/home/ubuntu/opencode/jdk
export ANDROID_HOME=/home/ubuntu/opencode/sdk
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export ANDROID_NDK_HOME="$ANDROID_HOME/ndk/27.2.12479018"
export GRADLE_USER_HOME=/home/ubuntu/opencode/gradle-home
export GNUMAKE=/usr/bin/make
export PATH="$JAVA_HOME/bin:/media/share/lp2-test/toolchain/hostbin:/media/share/lp2-test/toolchain/x86bin:$PATH"

scripts/agent-lock.sh build --reason "what changed" -- ./gradlew assembleDebug --no-daemon

AGENT_NO_SLEEP=1 scripts/agent-lock.sh device --reason "what you test" -- bash -c '
  adb install -r app/build/outputs/apk/debug/app-debug.apk | tail -1
  /media/share/lp2-test/scripts/unlock.sh >/dev/null 2>&1
  B="am broadcast -n com.vdavid003.sm64port/.DebugReceiver -a com.vdavid003.sm64port.CMD"
  adb shell am force-stop com.vdavid003.sm64port
  adb shell am start -n com.vdavid003.sm64port/.sm64portActivity
  sleep 10
  adb shell "$B --es cmd level --ei n 38"
  sleep 7
  adb shell "$B --es cmd ptc --es action povcam"
  sleep 3
  adb exec-out screencap -p > /tmp/opencode/shot.png
'

# finish with a NORMAL device lock (no AGENT_NO_SLEEP) so the tablet sleeps
```

Rules that matter:

- **Every** build and **every** adb call goes through `scripts/agent-lock.sh`.
  Exit code `75` = lock timeout: wait and retry, never bypass.
- One device-lock command = one self-contained script. The lock is held for the
  command's lifetime; the trap clears BUSY and puts the tablet to sleep.
- `AGENT_NO_SLEEP=1` keeps the tablet awake between device-lock calls. Never
  leave it set for the final call.
- Screenshots: `adb exec-out screencap -p > /tmp/opencode/foo.png`. Do not write
  to `/sdcard`.
- The NDK toolchain runs under qemu here and sometimes prints
  `python3: Syntax error` / `llvm-objcopy: error: unknown argument '-o'`.
  Re-run; it is not a code error. Trust the `BUILD SUCCESSFUL` /
  `error:` lines.

## 1. Install canary (do not skip)

`adb install ... | tail -1` has silently hidden a failed install in this repo's
workflow, leaving the **old APK running** and producing "wrong" screenshots.
After installing, prove the new binary is live with a field that only exists in
the new build:

```bash
adb shell "$B --es cmd dump"
adb logcat -d -s SM64Leia:I | grep "PTC level=" | tail -1
```

The dump is a long single line (`PTC level=... knobs ...`). Check for the new
feature's token (for example `pov=600/90/400/1/0`) or a changed knob-dump
format. If it is missing, install again. Also `force-stop` before `am start`;
two app instances make broadcasts land on the wrong one.

## 2. Capture pipeline

- Unlock: `/media/share/lp2-test/scripts/unlock.sh`.
- The panel blanks after 30 s; `screen_off_timeout` is already raised, but a
  long build/test gap can still blank it. Re-run the unlock script.
- Prefer **flat 2D** captures when judging geometry. The Leia stereo weave
  interlaces the eyes and makes screenshots look like a diagonal dither, and
  the HUD gets the same pattern. `ptc povcam` sets `gfx_stereo_2d=1` +
  `gfx_stereo_same_both=1` for you. Do **not** set `gfx_stereo_enabled=0` to
  get a mono shot — that leaves the composite drawing one half of the canvas
  (black half-screen). Use the 2D path (`ptc eyeL` / `eyeR` also work).
- Stereo correctness is a separate check: `ptc eyeL` / `eyeR` / `eyeOff`, or
  the `eye` knob (0 stereo, 1 L, 2 R).
- Keep the menu/drawer out of the way with the `SHOW` text button, or just
  crop the screenshot; the drawer does not block the 3D scene.
- `scripts/portal-relay-test.sh [pattern|scene|color]` automates the older
  portal relay stages (level 37 pattern, level 38 scene, solid-green
  geometry) and writes `/tmp/opencode/portal-*.png`.

## 3. Debug channel cheatsheet

```bash
B="am broadcast -n com.vdavid003.sm64port/.DebugReceiver -a com.vdavid003.sm64port.CMD"

adb shell "$B --es cmd level --ei n 38"        # load PTC (37 = empty test)
adb shell "$B --es cmd dump"                   # one status line to logcat (SM64Leia)
adb shell "$B --es cmd ptc --es action run"    # portal traversal smoke test
adb shell "$B --es cmd ptc --es action povcam" # Mario-in-portal rig, view A
adb shell "$B --es cmd ptc --es action povoff"
adb shell "$B --es cmd set --ei k 21 --ei v 1" # stereo_2d = 1 (flat)
adb shell "$B --es cmd note --es text 'what you are testing / expect'"
```

Quoting matters: `am broadcast ... --es action 'knob pov.dist 400'` needs the
inner quotes so the device shell keeps the spaces.

`cmd` values: `set`, `fx`, `shadow`, `pad`, `kill`, `level`, `toggle`, `fps`,
`portal`, `ptc`, `uncapped`, `runticks`, `trace`, `hz`, `givegun`, `busy`,
`logo`, `reinit`, `reset`, `note`, `dump`.

`cmd note --es text "..."` writes the **green test line** in the Android debug
panel.  Use it to say what is being tested and the expected outcome, e.g.
`"WALLRUN: expect action=0x0300089A for 52 ticks"`.  Scripted tests (the ninja
chain, POV rig, gun-aim test) set it automatically; the chain updates it each
phase.  Grep it from a screenshot or read `nativeGetDebugNote()` in the panel.

PTC actions used most: `povcam`/`povcam2`/`povoff`, `cornerportal`,
`portaltest`/`portalcolor`/`portalalign`/`portalblit`, `goomba`,
`emptytest`, `wallrun`/`jetpack`/`grapple`, `annex`, `resetpad`,
`eyeL`/`eyeR`/`eyeOff`, `knob <name> <value>`, `reload`.

Dump tokens worth grepping:

| token | meaning |
| --- | --- |
| `portals=` | number of placed portals (2 = both) |
| `p0=... n0=...` / `p1=... n1=...` | portal centre and surface normal |
| `view=` / `vsrc=` | active relay view / its source portal |
| `cam=` / `focus=` | `gLakituState` position and focus |
| `mario=` / `action=` | Mario's position / action id |
| `pov=` | POV rig dist/height/mario/freeze/stereo |
| `gun=` | held flag, barrel-vs-reticle error (0.1°), barrel-vs-facing offset (0.1°) |
| `goomba=` | demo active,z,crossings |
| `annex=` | wallrun/jetpack/grapple state |

## 4. Test levels and useful coordinates

- **38 = `LEVEL_PTC`** — the main chamber (big room, annexes, mirror, goomba
  scene). **37 = `LEVEL_PTCTEST`** — runtime-generated empty chamber.
- Level 38 generator: `scripts/gen-ptc-level.py`; generated C in
  `app/jni/src/levels/ptc/`. Level 37 collision: `scripts/gen-empty-test-level.py`.
- Current room: `x -2600..3200`, `z -3200..1250`, `y 0..2800`; floor `y=0`.
  Walls tinted **N +Z white, S -Z green, W -X blue, E +X red**.
- P5 portal platform: `x -600..600`, `z 600..1200`, top `y=800`; its front face
  is `z≈600`. Hitting it from the room side puts portals near `z=564`.
- Main-chamber portal demo targets: shoot from `z=500` toward `z=1249` at
  `x=±90`, `y=140`. Result: portals at `x=±90, y≈140, z≈564`, normal `-Z`.
- Corner test: shoot from `(-2400, y, -3100)` toward `-X` and from
  `(-2500, y, -3000)` toward `-Z`. Result: `(-2596, y, -3100)` normal `+X` and
  `(-2500, y, -3196)` normal `+Z`.
- `portal_shoot_at` runs an edge-fitting pass: the delivered centre can differ
  from the aim point (asking for `y=90` on the corner walls gives `y≈132`).
  Always read `p0/p1` back instead of assuming.
- A 135-wide ellipse cannot sit 20 units from a corner; the rim test rejects it.
  Use ~100 units.

Level 37 runtime scene (`app/jni/src/src/game/ptc_scene.c`): the visible room is
generated every frame from knobs; only its floor/walls are baked into the ROM.
Known gotcha: `gSPMatrix` needs a pooled, fixed-point `Mtx`
(`alloc_display_list` + `mtxf_to_mtx`). Pointing it at a stack `Mat4` renders
nothing (the room looked like a black void).

## 5. Portal relay internals you need while debugging

Read `app/jni/src/src/game/portal.c` (`portal_fill_view`, `portal_view_update`)
and the portal-pass block in `app/jni/src/src/pc/gfx/gfx_pc.c`.

- `m1 = T_src * R * T_dst^-1` (`R = diag(-1,1,-1)`) is stored transposed as
  `c_matrix`. Matrix order means the relay transform is applied **first**, in
  world space.
- **Never** multiply it on the left of `MP` (`C * modelview * P`). That applies
  it in **object space** and shreds every object that has its own matrix —
  Mario's "exploded" geometry. Conjugate instead:
  `K = V^-1 * C * V`, `MP = (modelview * K) * P`.
- For corner and same-facing pairs the relay camera ends up *behind* the
  destination portal, so the destination wall is between it and the content.
  The pass installs an **oblique near plane** at the source portal plane (see
  `gfx_portal_apply_near_plane`); it keeps the mapped destination (behind the
  plane) and drops the mapped room. Without it, corner and same-wall portals
  show nothing but wall. (Facing-away "tunnel" pairs put the content in front
  of the source plane and were already fine.)
- The relay is rendered **per eye** into `[portal][eye]` FBOs; the oval decal
  is sampled with UVs from the real camera projection of the source rim. The
  relay image is a **mirror** (left/right flip) — that is expected, not a bug.
- Stereo shear is baked into each relay pass (`gfx_apply_stereo_shear`), so IPD
  in the window is real; check it with `eyeL`/`eyeR`, not by judging a mono
  screenshot.
- Portal selection: in front of the camera, front side towards it, closest to
  the view axis (`select.maxdeg`, `select.facing`).
- The overlay/decal pass is separate from the relay pass; `relay.solid`,
  `relay.blit`, `fan.src`, and `decal.offset` isolate geometry from content.
- `portals=0` after a `povcam`/`goomba` setup almost always means the hitscan
  hit a platform before the intended wall. Move the ray origin into open space.

## 6. Camera rules of thumb

- The test camera must be in **CUSTOM** (`gfx_cam_mode == 3`) or the vanilla
  Lakitu camera will do its idle/sleep thing. `custom_camera_apply_mode(3)`.
- Use the **freelook** pin (`custom_camera_set_freelook`) for deterministic
  framing: it writes `c->pos/focus` and `gLakituState.pos/focus`, and
  `update_camera()` early-returns before `update_lakitu()` so the camera is not
  smoothed towards the goal over several frames.
- Vanilla zooms FOV 45→30 while Mario is `ACT_SLEEPING`; custom/first-person
  now pin 45 so it does not read as a camera "tuck".
- In the custom camera, **R shoulder** springs the camera behind Mario with a
  racing-game lag.  R is ignored while the grapple is attached (retract button).
- Scripted pans use `custom_camera_set_focus_hint(on, point)`: the camera keeps
  following Mario, only the look-at point eases to the target (no cuts).
- Portal mapping preserves **height relative to the portal centre**. If the
  camera is at Mario's mid-body height (≈90) but the portals are aimed at
  y=200, the relay camera looks over his head and the window shows wall.
  Aim the portals at mid-body height and put the camera at the same height.
- Mapped destination content appears behind the *source* portal plane. A
  person standing close to the destination portal can be only partly inside
  the aperture cone; `pov.mario` (default 400) gives enough room to see the
  whole model.
- Don't put the real Mario between the test camera and the window — he can
  occlude it in the main view. Keep him off the camera→portal axis.

## 7. The Mario-in-portal POV rig

```bash
adb shell "$B --es cmd level --ei n 38"; sleep 7
adb shell "$B --es cmd ptc --es action povcam"    # view portal A
adb shell "$B --es cmd ptc --es action povcam2"   # view portal B
adb shell "$B --es cmd ptc --es action povoff"
```

`povcam` places both portals on the SW corner walls, parks Mario in the
destination space, pins the camera on the viewed portal's axis, freezes the
idle pose, and forces flat 2D. Everything is re-applied every tick, so sleep,
camera lerp and level resets cannot move the framing.

Knobs (`scripts/ptc-knob.sh`, or `ptc knob <name> <value>`):

| knob | default | meaning |
| --- | --- | --- |
| `pov.dist` | 600 | camera distance from the viewed portal plane |
| `pov.height` | 90 | camera height (Mario's mid-body) |
| `pov.mario` | 400 | Mario's distance in front of the destination portal |
| `pov.freeze` | 1 | hold idle frame 0 |
| `pov.stereo` | 0 | 1 = keep stereo, 0 = flat single eye |

Expected result: through the oval you see a complete, upright Mario — cap,
face, overalls, gloves, shoes and his ground shadow — with no missing,
inverted or exploded polygons. Files from the passing run:
`/tmp/opencode/fA.png` (view A) and `/tmp/opencode/fB.png` (view B).

`scripts/ptc-knob.sh` takes the device lock itself. Do **not** wrap it in
another `agent-lock.sh device` call — it will deadlock/time out.

### Gun aim test

```bash
adb shell "$B --es cmd ptc --es action gunaim"     # side view + fixed marker
adb shell "$B --es cmd ptc --es action gunaimoff"
```

`gunaim` pins Mario at `(-600, 0, -900)` facing +Z, gives him the gun, pins a
side camera, and forces the reticle to a fixed marker at `(-560, 240, -700)`.
The marker is drawn as the normal world-space crosshair and the barrel must
point at it. The dump's `gun=held,aimerr,yaw` should read `gun=1,0,N` — aim
error ~0, while `N` is large whenever the aim is not Mario's facing (that is
the point: the gun no longer just follows his yaw). `povcam2` gives
`gun=1,0,718`; `gunaim` gives `gun=1,0,377`.

The reticle normally comes from the camera ray, so in normal play the gun is
always seen roughly end-on. The fixed-marker hook exists so a spectator camera
can show the barrel in profile.

## 8. Framing toolbox (use it for visual inspections)

You control the camera, so frame the evidence deliberately:

| what | how | default |
| --- | --- | --- |
| FOV | `cmd set --ei k 20 --ei v <1..400>` (percent) or CAMERA tab "FOV %" | 100 |
| orbit distance | `cmd set --ei k 46 --ei v <1..1500>` | 700 |
| look-at height | `cmd set --ei k 47 --ei v <0..400>` | 110 |
| recentre speed | `cmd set --ei k 48 --ei v <0..200>` | 50 |
| orbit sensitivity | `cmd set --ei k 49 --ei v <10..300>` | 100 |
| wall pull-in | `cmd set --ei k 52 --ei v 0/1` (CAMERA "WALL NOCLIP") | **on** |

- **WALL NOCLIP (default on)** skips the custom camera's collision pull-in. The
  camera can pass through a room wall and the PTC level hides that wall
  completely, so Mario stays visible instead of filling the frame. With it off
  the camera pulls in to ~90 units near a wall (useful to reproduce the
  gameplay close-up, bad for evidence shots).
- `ptc camwall` is the canonical check: Mario at the west wall, camera aimed
  outside the room. Noclip on → camera at x≈-3082, west wall gone; noclip off →
  camera pulled to x≈-2575 and a close-up. Captures: `camwall_on.png` /
  `camwall_off.png`.
- Scripted framing: `custom_camera_set_focus_hint(on, point)` pans the look-at
  point without moving the camera; `custom_camera_set_yaw_hint(on, yaw)` eases
  the camera to a chosen side (used by the goomba finale). The POV rig has
  `pov.dist` / `pov.height`; the gun-aim test pins a side camera.
- Captures are mono/flat 2D (`gfx_stereo_2d=1`, `gfx_stereo_same_both=1`) —
  the Leia weave makes stills unreadable.
- `runticks` fast-forward renders nothing; take screenshots in normal mode.

## 9. Visual testing principles

- Solid colour fills are not enough. Use well-known shapes and asymmetry:
  Mario himself (cap M, face, overalls, gloves, shadow), a checker grid, or
  text. A mirrored/duplicated/reflected copy is immediately obvious; a red
  patch is not.
- Freeze the pose (`pov.freeze`) instead of relying on screenshot timing;
  random animation frames hide clipped limbs.
- Include the shadow: it is a quick sanity check that the model's ground
  contact and lighting survived the transform.
- Capture one shot per portal. "Both portals in one frame" is the *user's*
  view; the debugging view is better perpendicular to one portal.
- Compare left/right eye captures for stereo parallax; do not judge stereo
  from a flat shot.

## 10. Failure modes → likely cause

| Symptom | Look here |
| --- | --- |
| Mario "exploded"/shards in the window | portal matrix applied on the left of MP (object space); use `K = V^-1 C V` on the right |
| Window shows flat wall (stripes/checker) | destination wall not clipped; oblique near plane missing/disabled |
| Window empty/black | relay FBO not rendered, source selection failed, or camera in the wrong mode |
| `portals=0` | hitscan hit a platform/wall first, or rim fitting rejected the spot |
| Oval in the wrong place / aspect stretched | decal model matrix, `gfx_geo_x_scale`, rim UV projection |
| Camera keeps drifting | vanilla/custom mode not 3, or `update_lakitu` smoothing (freelook should early-return) |
| Camera dives when idle | vanilla sleep FOV zoom (45→30) |
| Room follows Mario / everything static | camera view matrix replaced by identity; use the camera view for overlays |
| Level 37 black void | runtime scene not drawn, or `gSPMatrix` given a stack `Mat4` |
| Screenshot looks dithered/striped | Leia stereo weave; capture in flat 2D |
| Half the screen black | `gfx_stereo_enabled=0` mid-run; use `gfx_stereo_2d` instead |
| "New" build behaves like the old one | install canary failed; old APK still running |

## 11. Pre-commit checklist

1. `BUILD SUCCESSFUL`, and the changed native files actually recompiled.
2. Device run with an install canary (dump field/format changed as expected).
3. Screenshot(s) show the expected thing, not just "something changed".
4. `git status` contains only intended files; no screenshots or logs.
5. Commit message explains the *cause* and the fix, not just the symptom.
6. Final `agent-lock.sh device` call **without** `AGENT_NO_SLEEP` so the tablet
   sleeps.

## 12. Briefing a subagent

Include in the prompt:

- the mandatory lock commands (build + device), the build env exports, and
  "exit 75 = wait/retry, never bypass";
- stdout discipline: `adb install` output, `BUILD SUCCESSFUL`, and the dump line
  are the evidence — do not claim success without them;
- the install canary rule;
- the debug channel and the specific PTC action/knobs to use;
- that screenshot evaluation should use shapes/fiducials, not colour patches;
- "leave changes in the working tree, do not commit" unless you want a commit;
- finish with a **normal** device-lock call so the tablet sleeps.

## 13. Fast-forward runner (`runticks` + `trace`)

The engine can run a batch of simulation ticks back to back on the game
thread, without waiting for real time or presenting every tick.  This is the
first thing to try when tuning chain logic; keep normal mode for visuals.

```bash
B="am broadcast -n com.vdavid003.sm64port/.DebugReceiver -a com.vdavid003.sm64port.CMD"

adb shell "$B --es cmd level --ei n 38"
adb shell "$B --es cmd ptc --es action chainbox"   # start the section normally
sleep 1
adb shell "$B --es cmd runticks --ei n 200"        # 200 ticks, ~6.7 s of gameplay
sleep 1
adb shell "$B --es cmd trace"                      # dump the trace to SM64Leia logcat
```

`runticks` queues the count for the next `produce_one_frame()` call, which then
sets `gfx_test_uncapped=1`, runs exactly N `game_loop_one_iteration()` steps
(no OpenGL pass, no audio buffer), restores `gfx_test_uncapped=0` and renders
one normal frame of the final state.  The app stays responsive afterwards; the
screen shows the fast-forwarded result.

Trace line (one every 15 ticks, plus the final state):

```
t=120 ph=12 fl=0x020 act=0x0300089A pos=-408,509,-1536 st=10 gx=0 cm=3
```

| field | meaning |
| --- | --- |
| `t` | `gGlobalTimer` (30 Hz simulation ticks) |
| `ph` | chain phase (`ptc_chain_state`) |
| `fl` | chain flags bitmask (hex) |
| `act` | Mario action id |
| `pos` | Mario position, rounded to N64 units |
| `st` | star0 / star1 collected as two digits |
| `gx` | goomba portal crossings |
| `cm` | `gfx_cam_mode` (3 = custom camera) |

Footers: `# done ran=<n> sim_ms=<device ms> real30_ms=<n/30 s>` and
`# speedup_x100=<real/sim>`.  The batch completion is also logged as
`PTC runticks done n=<n> ms=<device ms>`.

Caveats:

- No rendering during the batch, so no portal FBO/decal/screenshot evidence;
  screenshots still come from normal mode (or the one frame rendered at the
  end).
- Audio buffers are not produced during the batch; expect a short gap.
- The display-list pools are reset per tick by `config_gfx_pool()`, so N ticks
  do not leak display lists.  Requests are clamped to **2400** ticks to bound
  the time without a present.
- The 16 KB trace keeps its tail: for long runs the oldest lines are dropped.
- `runticks` leaves `gfx_test_uncapped` off, even if it was on before.

Recommended workflow: `scripts/ninja-chain-test.py --fast --section <sec>
--ticks <n>` for logic/flag iteration, then the default real-time mode for
screenshots and the final visual pass.

## Ninja chain (continuous systems showcase)

`NINJA-CHAIN.md` (project root) has the full brief, course table, phases and
per-section commands.  Quick reference:

```bash
adb shell "$B --es cmd level --ei n 38"
adb shell "$B --es cmd ptc --es action chain"        # full continuous run
adb shell "$B --es cmd dump"                         # chain=<phase>,<timer>,<flags>,<gcross>,<cubes>
```

Flags: 1 ball, 2 punch, 4 kick, 8 slam, 16 jetpack, 32 wallrun, 64 grapple,
128 portal, 256 star1, 512 star2, 1024 goomba.  Each section can be started
alone (`chainbox`, `chainwall`, `chaingrapple`, `chainjet`, `chainportal`,
`chaingoomba`) for tuning, or from the tablet: **ENH → PORTAL → RUN NINJA
CHAIN** (loads level 38 then starts after 2.5 s; STOP NINJA CHAIN beside it).
The PORTAL page is 3 columns so it needs little scrolling.  The run always starts in the **custom camera** (`ptc_chain_setup()` calls
`custom_camera_apply_mode(3)`), and the dump prints `cmode=3`; the green panel
line tracks the current step and expected outcome.  R springs the custom camera
back behind Mario (racing lag) unless the grapple rope is attached.
