# sm64ex-android — Lume Pad 2 fork

A fork of [sm64ex-android](https://github.com/MegaBitmap/sm64ex-android) that turns
the native Super Mario 64 port into a **glasses-free 3D, high-framerate** game for
the **Leia Lume Pad 2** (2560x1600 lenticular tablet, 8-view lightfield panel).

No emulation: this is the decompiled game built as a native arm64 Android app
(ROM is only used at build time to produce the asset segments). Rendering goes
through SDL -> OpenGL ES 2 -> a Leia `Surface`, which the Leia SDK weaves onto
the lenticular panel.

Everything below was added on top of upstream in this fork.

## Latest branch additions (`ragdoll-ppcam-shadows`)

- **Death and hit ragdolls**, ported from Epic0522's 3DS Ultimate, runtime
  toggles on the new **ENH** tab (default on).
- **Puppycam+**: 70-unit camera wall margin and a Mario close-up fade (default
  on).
- **Realtime dynamic shadow map** with a dedicated **SHADOW** quality/perf tab
  (default on).
- **SSRTX**: screen-space ray-traced reflections with a dedicated **SSRTX** tab
  (opt-in, default off).
- **Debug level select**: ENH button, `cmd level`, or **L + R + START** in the
  pause menu/title screen.
- **KILL MARIO (TEST)** button and `cmd kill` for testing the ragdoll path.
- Headless tooling: `cmd shadow`, `cmd kill`, `cmd pad`, plus new
  `RAGDOLL/HITRAG/PPCAM/SHADOW/SHADOWDBG/SSRTX` toggles.

Defaults for the gameplay features are on; SSRTX and the pre-existing MXAO /
C.DOF / SSGI effects stay off so the base look and frame rate are unchanged
until you enable them.

---

## Build

Headless build (no Android Studio). This branch builds the native side through
Gradle's `ndkBuild` task, so no separate `ndk-build` + `EXCLUDE_NATIVE_LIBS`
dance is needed:

```sh
export JAVA_HOME=/home/ubuntu/opencode/jdk
export ANDROID_HOME=/home/ubuntu/opencode/sdk
export ANDROID_SDK_ROOT=$ANDROID_HOME
export ANDROID_NDK_HOME=$ANDROID_HOME/ndk/27.2.12479018
export GRADLE_USER_HOME=/home/ubuntu/opencode/gradle-home
export GNUMAKE=/usr/bin/make
export PATH="$JAVA_HOME/bin:/media/share/lp2-test/toolchain/hostbin:/media/share/lp2-test/toolchain/x86bin:$PATH"

./scripts/agent-lock.sh build --reason "assembleDebug" -- \
    ./gradlew assembleDebug --no-daemon
```

- `app/build.gradle` pins `ndkVersion "27.2.12479018"`. NDK r26 has no aarch64
  host build (`Unknown host CPU architecture`); the shared headless toolchain
  patches r27's x86_64 tools to run under qemu. `hostbin/uname` and
  `GNUMAKE=/usr/bin/make` are needed for the NDK's own host detection/make.
- The only supported target is **arm64-v8a** (Lume Pad 2). Other ABIs are opt-in
  with `-PSM64_ABIS=arm64-v8a,x86_64,...`.
- APK output: `app/build/outputs/apk/debug/app-debug.apk`.
- Multi-agent edits should use `scripts/agent-lock.sh build|device`, which
  serialises Gradle/NDK builds and `adb` access across worktrees.

Quick smoke test after installing:

```sh
B="am broadcast -n com.vdavid003.sm64port/.DebugReceiver -a com.vdavid003.sm64port.CMD"
adb shell $B --es cmd dump          # feature state + FX/shadow knobs
adb shell $B --es cmd kill          # death ragdoll test
```

---

## Display / stereo

| Feature | Notes |
|---|---|
| Leia interlaced 3D output | SDL renders into a Leia video `Surface` (`CreateEmptySurfaceForVideo` + `SDLMutableSurface` override); the SDK weaves it. Full 2560x1600. |
| Per-eye framebuffers | One colour + depth FBO per eye, composited into a 2x1 SBS texture. Independent glViewport per eye, so neither eye inherits the other's viewport. |
| Clip-space stereo shear | IPD, convergence and toe are applied as projection shears, so the game's own camera/projection stays untouched. |
| Depth buffer semantics | N64 depth range/ordering handled explicitly; Z buffer can be disabled per render mode without writing depth. |
| Aspect | Projection uses the real canvas aspect ratio (fixes the 4:3-stretched-to-16:9 look) with an adjustable override. |
| Frustum culling | Culling accounts for the stereo shear (union of both eye frusta), so geometry no longer pops at the edges while panning. |
| Adjustable stereo | IPD (+/-300 px), zero parallax, depth %, toe in/out, FOV %, in-scale %, eye width, near/far per eye, convergence mode (2D). |
| 2D mode | Zero-parallax single view. |
| Debug views | Raw depth (ZBUF), wireframe, per-eye tint, SBS "same both eyes", half-res (PERF2x). |

### Convergence and toe

- **Zero parallax** moves the zero-parallax plane: both eyes shift *towards each
  other* rather than the pair being trucked sideways.
- **Toe in/out** cants the eyes in **mirrored** directions (they converge/divergence
  around the centre instead of both looking the same way).
- **TOEAUTO** (default on) aims both eyes at the DOF autofocus target
  (`theta = atan((ipd/2) / d)`, `d` = camera to Mario's front), so the pair toed
  in more as he gets closer and is nearly parallel at long range.
- **UI depth** gives the UI layer its own perceived Z, independent of the scene.

---

## High framerate rendering (HFR)

Simulation stays at a fixed **30 Hz**; rendering runs at **30 / 60 / 120 /
uncapped** (default **120 Hz**, panel refresh matched on resume).

- Fixed-step accumulator (clamped so a resume cannot spiral); the game clock,
  physics and audio cadence are unchanged.
- The 30 Hz display list is retained and replayed by the extra frames.
- A parallel **previous-tick matrix stack** is built in lockstep with the normal
  one; every matrix that reaches the display list is blended between the two
  ticks (`prev + (cur - prev) * alpha`).
- Interpolated: camera position/focus, perspective FOV, object position, angle
  (shortest arc), scale, attached-object matrices, skeletal animation (the
  animation stream is walked twice: previous frame and current frame), rotation
  nodes, and object shadows.
- Snap guards: `gGlobalTimer` discontinuities (warps, area loads),
  object teleports, animation changes and culled frames snap instead of
  streaking.
- HFR modes render one tick behind so frame spacing is even; 30 Hz mode adds no
  latency.
- `INTERP` toggle for A/B comparison; `hfr=N/1` in the debug dump reports how
  many matrices were interpolated in the last built display list.

---

## Screen-space effects

A per-eye post pass runs after the scene is rendered (options tabs **MXAO**,
**C.DOF**, **SSGI**, **SSRTX**):

- **MXAO-style ambient occlusion** — intensity, radius, samples, bias, power.
- **Cinematic DOF** — focus, aperture, max blur, samples, CoC debug view.
  **AUTO FOCUS** (default on) keeps the focusing plane on Mario: distance to his
  centre minus his hitbox radius, i.e. the front of his body, so the plane sits
  a little closer than his middle.
- **SSGI-lite** — intensity, radius, steps, rays (screen-space colour bleed).
- **SSRTX** — screen-space ray-traced reflections (ReShade SSR/SSRT-style):
  intensity, max ray distance, steps, rays, depth thickness, roughness cone,
  edge fade, and a reflection debug view. Opt-in (off by default) because it is
  the most expensive post effect.
- Debug views: AO buffer, CoC, GI, SSRTX reflections.

### SSRTX controls (SSRTX tab)

| Control | Effect |
|---|---|
| `SSRTX ENABLE` | master toggle (default **off**) |
| `Intensity` | reflection contribution % |
| `Distance` | max ray length in world units |
| `Steps` | depth-buffer march samples per ray |
| `Rays` | jittered rays per pixel (1–8); averaging softens |
| `Thickness` | world-space depth window that counts as a hit |
| `Roughness` | cone spread for multi-ray soft reflections |
| `Edge fade` | fade reflections out near the screen borders |
| `SHOW SSRTX` | debug view of the reflection buffer |

The march reconstructs view-space position/normals, reflects the view vector,
projects each sample back through the **forward** per-eye stereo
shear/toe/flip, and blends the hit colour with a Schlick Fresnel term. Headless
keys are `cmd fx` `70`–`77` (`70` enable, `71` intensity, `72` distance,
`73` steps, `74` rays, `75` thickness, `76` roughness, `77` edge fade);
`cmd fx --ei k 31 --ei v 4` shows the reflection buffer.

SSRTX cost scales with `Steps × Rays × pixels × eyes`; at 200 % intensity and
24 steps the 120 Hz cap dropped to ~56 fps in the castle grounds, so leave it
off unless you want the effect and tune down from the defaults.

- Everything is a live knob (panel sliders or `cmd fx`), defaults off so the
  base look is unchanged.

---

## UI after the post pass (UIPOST)

The frame's display list is split in `render_game()`: the 3D scene up to the HUD
scissor, then all 2D UI (HUD, cannon reticle, dialogs, Peach letter, pause menu,
text labels, transitions, native touch controls, borders).

- Scene -> eye FBO -> FX pass; UI -> a second per-eye RGBA buffer (own depth).
- Composite: `ui.rgb + scene.rgb * (1 - ui.a)` with premultiplied coverage, so
  AO/DOF/GI neither blur the HUD nor bleed HUD colours into the scene.
- The UI range skips the stereo shear, so the HUD is screen-locked by default
  (`UIPAR` restores the old sheared UI).
- `UIBUF` shows the captured UI buffer for verification.
- Pipeline state caches (shader, viewport, scissor, depth, blend) are invalidated
  after the post pass so the next frame re-applies them.

---

## Camera

The free camera (bettercamera / puppycam) is compiled in and usable from the
touch overlay:

- **X** toggles between the vanilla camera and the free camera (button turns
  solid green while the free camera is active).
- **Y** recentres the free camera (or resets the vanilla view when free cam is
  off).
- The right-hand control can be switched from C-buttons to an analog camera
  stick (**C / ANA** toggle); the stick drives the free camera.
- Free camera parameters (sensitivity, aggression, pan, degradation) are read
  from the existing bettercam config keys.

---

## Controls

- Floating left thumbstick (appears where the thumb lands), A/B/Z/R, C-button
  cluster or analog right stick, START, plus the camera X/Y buttons.
- The controls are a Java view layered above the Leia view (never per-eye, never
  letterboxed into the game canvas), drawn above the interlaced output.
- **Ctl opacity** fades the whole control layer (replaces the old Ctl X/Y/scale
  sliders).
- The in-game N64 touch sprites are hidden; the native overlay feeds the game's
  controller state directly.

---

## Gameplay

### Runtime feature toggles at a glance

| Feature | Default | Where |
|---|---|---|
| Death ragdoll | **on** | ENH tab / `cmd toggle --es name RAGDOLL` |
| Hit (knockback) ragdoll | **on** | ENH tab / `cmd toggle --es name HITRAG` |
| Puppycam+ wall margin + Mario fade | **on** | ENH tab / `cmd toggle --es name PPCAM` |
| Realtime dynamic shadows | **on** | SHADOW tab / `cmd toggle --es name SHADOW` |
| SSRTX reflections | **off** (opt-in) | SSRTX tab / `cmd toggle --es name SSRTX` |
| MXAO / C.DOF / SSGI | off | their tabs |

- **Stay in level after a star** (default on): normal stars take the vanilla
  no-exit path (star dance -> save prompt -> idle) instead of the star-exit warp.
  100-coin stars already behaved this way. Persisted as `stay_in_level` in
  `sm64config.txt`; toggle with **STAY** or `cmd toggle --es name STAY`.

### Death / hit ragdolls (ENH tab, default on)

Ported from Epic0522's *Super Mario 64 3DS port — Ultimate*
(`enhancements/death_ragdoll.inc.c`, `hit_ragdoll.inc.c`). Mario becomes a
14-limb physics body when he dies, and gets a recoverable knockback ragdoll on
non-lethal hits. Both are runtime toggles:

- `DEATH RAGDOLL` (`death_ragdoll_enabled`, default on)
- `HIT RAGDOLL` (`hit_ragdoll_enabled`, default on)

The ragdoll renders its own limb display lists (`death_ragdoll_geo_render`) via
a switch case in `mario_geo`, so the normal Mario model is hidden while it runs.

### Puppycam+ (ENH tab, default on)

Two newer-Puppycam behaviours on top of `bettercamera`:

- **Wall margin** — the camera stops 70 units short of a wall hit instead of
  snapping to the surface.
- **Close-up fade** — Mario fades out when the camera is forced within ~250
  units, so the view never clips through his model.

Toggle with `PUPPYCAM+` / `cmd toggle --es name PPCAM`.

### Realtime dynamic shadows (SHADOW tab, default on)

`gfx_pc.c` replays the frame's 3D scene range once per frame from a directional
sun into a depth-only shadow map (packed RGBA depth + a depth renderbuffer), and
the per-eye post pass reconstructs world positions from depth, projects them
into light space and applies PCF shadowing. The HUD/UI range is excluded, so
only scene geometry casts.

The **SHADOW** tab exposes the quality/performance trade-offs live:

| Control | Effect |
|---|---|
| `SHADOW ENABLE` | master toggle |
| `RES` | 512 / 1024 / 2048 px map |
| `Strength` | shadow darkening % |
| `Bias` / `Normal bias` | acne vs. peter-panning |
| `Softness` / `PCF taps` | 1 / 4 / 9 / 16 tap filter radius |
| `Range` | ortho extent around Mario |
| `Sun yaw` / `Sun pitch` | light direction |
| `Update every` | refresh the map every 1–4 frames |
| `SHADOW VIEW` | off / shadow factor / raw depth map |

Headless: `cmd shadow --ei k 1 --ei v 2048` sets the resolution, and
`cmd toggle --es name SHADOW` / `SHADOWDBG` flip the master/debug state
(`k` = 0 enable, 1 res, 2 interval, 3 strength, 4 bias, 5 normal bias,
6 softness, 7 taps, 8 range, 9 yaw, 10 pitch, 11 debug view).

The shadow matrices are built in `rendering_graph_node.c` from the camera node
(`gfx_cam_inv_mtx`, `gfx_shadow_light_vp`); the depth pass runs before the eye
passes and is view-independent, so both eyes share one map.

### Debug level select (ENH tab / `cmd level` / L+R+START)

The decomp debug level select (`src/menu/level_select_menu.c`) is wired up:

- **ENH → LEVEL SELECT**, or `cmd level` with no `n`, opens the menu.
- `cmd level --ei n <id>` jumps straight to that level id.
- **L + R + START** opens it from the pause menu or the title screen.
- Navigate with the left stick, **A/B** (±1) or the C-button cluster
  (up/down ±1, left/right ±10). **START** warps; the vanilla quit combo is
  `Z + START + L-C + R-C`.
- While active, exiting a course returns to the level select instead of the
  castle (the original `gDebugLevelSelect` behaviour).

```sh
adb shell $B --es cmd level              # open the menu
adb shell $B --es cmd level --ei n 24    # warp straight to level 24
```

This is the fastest way to test a specific course with the stereo/shadow/FX
tabs after changing a setting.

### Debug kill (testing)

The **ENH** tab has a **KILL MARIO (TEST)** button, and the debug channel has
`cmd kill`. It triggers the death ragdoll when `DEATH RAGDOLL` is on, otherwise
vanilla standing death; the request waits for a valid gameplay floor, so it is
safe to fire from a menu.

```sh
adb shell $B --es cmd kill
```

This is the fastest way to verify the ragdoll path: on the castle grounds Mario
immediately collapses into the 14-limb pose, then the normal death warp and
respawn follow.

---

## Debug tooling

A broadcast channel drives everything over adb:

```sh
B="am broadcast -n com.vdavid003.sm64port/.DebugReceiver -a com.vdavid003.sm64port.CMD"
adb shell $B --es cmd dump                      # full state line
adb shell $B --es cmd set --ei k 0 --ei v 300   # stereo knob (see table)
adb shell $B --es cmd toggle --es name MXAO     # MXAO / CDOF / SSGI / SSRTX / STAY /
                                                # CAM / TOEAUTO / UIPOST / UIPARALLAX /
                                                # UIBUF / FLAT / 3D / ZBUF / WIRE /
                                                # RAGDOLL / HITRAG / PPCAM /
                                                # SHADOW / SHADOWDBG / ...
adb shell $B --es cmd fx --ei k 2 --ei v 250    # screen-space FX knob
adb shell $B --es cmd shadow --ei k 1 --ei v 1024  # shadow knob (res here)
adb shell $B --es cmd kill                      # kill Mario (ragdoll if enabled)
adb shell $B --es cmd level                     # open debug level select
adb shell $B --es cmd level --ei n 24           # warp straight to level id 24
adb shell $B --es cmd pad --ei sx 0 --ei sy 0 --ei buttons 4096 --ei ms 250
adb shell $B --es cmd fps --ei v 2              # 0=30 1=60 2=120 3=uncapped
adb shell $B --es cmd busy --ei v 1             # debug line red ("hands off") / 0 green
adb shell $B --es cmd reinit | reset
```

Stereo knobs (`cmd set`): `0` IPD px, `1` zero parallax px, `2` depth %,
`3` eye width, `4` swap eyes, `5` flip Y, `6` same both, `7` tint, `8/9/10`
legacy touch offsets, `11` source width %, `12` half-res, `13` fit, `14`
in-scale %, `15` ZBUF, `16` wireframe, `17` toe %, `18` aspect %, `19` no cull,
`20` FOV %, `21` 2D, `30` STAY, `35` UIPOST, `36` UIPARALLAX, `37` UIBUF,
`38` UI depth px, `40` TOEAUTO.

FX knobs (`cmd fx`): AO `1`-`6`, DOF `11`-`15` (`16` auto focus, `17` reports
the auto distance), SSGI `21`-`25`, SSRTX `70`-`77`, debug view `31`
(0 off, 1 AO, 2 CoC, 3 GI, 4 SSRTX), normal flip `32`.

`cmd pad` injects raw overlay input for headless menu/gameplay driving:
`--ei sx/-127..127 --ei sy/-127..127 --ei buttons <N64 pad bits> --ei ms <hold>`.
Handy bits: `4096` START, `32768` A, `16384` B, `8192` Z, `16` R.

The on-screen tuning panel is collapsed by default (**SHOW**/**HIDE**) so it does
not cover the controls; it has tabs for STEREO / MXAO / C.DOF / SSGI / SSRTX /
CAMERA / ENH / SHADOW, a toggle row, and a live status line plus FPS meter. The status line is **green (`FREE`)**
when the app is free to touch and **red (`BUSY - hands off`)** while an automated
agent is driving it.

---

## Known gaps / roadmap

- HUD, dialogs, menus and screen transitions still step at 30 Hz (UI-after-post
  keeps them out of the FX but they are not interpolated yet).
- Skybox rotation, particles and paintings are not interpolated yet.
- Ragdoll limbs are rebuilt at 30 Hz and are not part of the HFR interpolation
  table, so they can look one logical tick behind the camera at 120 Hz.
- Dynamic shadows need the UIPOST scene split and `GL_OES_depth_texture`; the
  camera inverse is sampled once per 30 Hz tick, and head/gyro tracking is not
  compensated, so shadows can shift slightly while those optional tracking
  modes are active.
- SSRTX is a single-pass, screen-space effect: off-screen geometry cannot
  reflect, there is no denoiser, and high `Steps`/`Rays` settings are
  expensive on the Lume Pad 2.
- Resume from background can leave the Leia surface black (surface teardown
  without recreate).
- Slider values (stereo, FX, SSRTX, shadows) are not persisted across launches;
  the ragdoll/Puppycam/shadow master defaults are.
- Head/eye-tracked 6DoF (Phase C) is not implemented; the panel currently shows
  fixed-offset stereo.

---

## Credits

Upstream: sm64ex / sm64-port / sm64ex-android. This fork adds the Lume Pad 2
display path, stereo controls, HFR interpolation, screen-space FX, UI-after-post
compositing, the touch overlay and the debug tooling described above.

Death/hit ragdoll physics and the original Puppycam improvements come from
Epic0522's *Super Mario 64 3DS port — Ultimate* (Puppycam / Better Camera
authors credited upstream). The Lume Pad 2 dynamic shadow map and SSRTX
reflections are original GLES implementations; SSRTX is inspired by the
ReShade SSR/SSRT-style shaders.
