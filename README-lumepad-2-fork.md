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
- **Debug level select**: ENH button, `cmd level`, or **L + R + START** in
  gameplay, the pause menu, or the title screen.
- **Portal Test Chamber** (level 38, level select + ENH shortcut): deterministic
  graybox room, calibrated jump/ledge-grab platforms, debug-only star, and an
  automated portal-traversal regression test. See
  [PORTAL-TEST-CHAMBER.md](PORTAL-TEST-CHAMBER.md).
- **Uncapped test simulation** (ENH tab / `cmd uncapped`): runs normal fixed
  30 Hz sim ticks without display pacing for automated tests; gameplay and
  physics are unchanged.
- **Cheats menu enabled by default** (pause → R → CHEATS); the vanilla triple-L
  unlock still works if the in-menu toggle turns it off.
- **Control overlay opacity** defaults to 33% and persists across app launches.
- **Resume rendering fixed**: the Leia surface is rebound to SDL after background.
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

**Tip (agent builds):** pipe long builds through the progress-tool relay so the
user's progress indicator streams live at zero token cost instead of the agent
sitting on a wall of compiler output:

```sh
progress-relay --id build-1 -- ./scripts/agent-lock.sh build --reason "assembleDebug" -- \
    ./gradlew assembleDebug --no-daemon
# or simply: ./gradlew assembleDebug --no-daemon | progress-relay --id build-1
```

The relay parses OSC 9;4 progress sequences and maps the exit code to
done/error.

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
- Held objects use the holder's **previous root matrix** for the previous tick
  (the old code blended against the local hand stack, which carries a 0.25
  scale in Mario's holding pose, so a held object pulsed 25%→100% every tick).
- Rotation-aware blend: a matrix that rotates more than ~1.6° per tick is
  decomposed into translation + rotation + per-row scale, the rotation is
  slerped and scale/translation are lerped. Blending the 16 floats directly
  would shrink a fast spin by `cos(delta/2)` every tick (2.6% residual on a
  held Bowser spun at 22.5°/tick); see
  [`HFR-HELD-OBJECTS.md`](HFR-HELD-OBJECTS.md).
- Held-object scale telemetry: `ptc hfron` logs one `SM64HFR` line per
  presented frame, `ptc hfrdump` prints a window summary, and
  `ptc hfrslerpoff` restores the old element-wise blend for A/B.
  `scripts/hfr-bowser-capture.sh` + `scripts/hfr-scale-check.py` reproduce and
  measure the tail-spin case.
- HFR modes render one tick behind so frame spacing is even; 30 Hz mode adds no
  latency.
- `INTERP` toggle for A/B comparison; `hfr=N/1` in the debug dump reports how
  many matrices were interpolated in the last built display list.

---

## Screen-space effects

A per-eye post pass runs after the scene is rendered (options tabs **MXAO**,
**C.DOF**, **SSGI**, **SSRTX**, **RAYS**):

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
- **Crepuscular rays** — quarter-resolution screen-space light shafts (below).
- Debug views: AO buffer, CoC, GI, SSRTX reflections, ray shafts/visibility.

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

### Crepuscular rays (RAYS tab)

Screen-space light shafts: for every pixel the pass marches N samples toward the
light's screen position (depth buffer only), accumulates visibility with a
per-step `decay`, and adds the result to the scene colour. Occlusion is "a
sample closer than `z0 · (1 − bias)` blocks"; depth at the far plane (sky) never
occludes. Only bright pixels shaft (luminance gate), and the contribution fades
with screen distance from the source. The march runs at `scene_res/4` per eye,
followed by a 9-tap separable blur; the FX pass samples it bilinearly. Off by
default: no FBO is allocated and no pass runs until you enable it.

Source light `0`-`3` follows the active dynamic lights (`ptc lightshow` /
`ptc lighttest`, or `fx 103`), `4` is a synthetic sun above-left of the camera.
An unavailable index, or a light behind the camera, falls back to that sun.

The additive term is applied in the FX pass next to the dynamic lights (after
AO/DOF/shadow shape the base colour, before the probe-visualisation overlay);
that pass has no tonemap or colour grade, so this is the last point where light
is added. Alpha is untouched.

| Control | Effect |
|---|---|
| `CREPUSCULAR RAYS` | master toggle (default **off**) |
| `Shaft intensity %` | additive contribution to the scene |
| `March length %` | how far of the way to the light the samples reach |
| `Falloff per step %` | per-sample accumulation decay (100 = flat) |
| `Sample weight %` | per-sample weight before the intensity |
| `March samples 8-32` | quality/cost dial |
| `Brightness gate %` | minimum scene luminance that shows shafts |
| `Source light 0-3, 4=sun` | which light casts the shafts |
| `Start jitter %` | interleaved start offset (kills banding) |
| `Occlusion bias %` | depth tolerance before a sample counts as blocked |
| `Debug 0=off 1=shafts 2=vis` | shafts with the composite ramp, or raw visibility |

Headless keys `250`-`260`; `cmd toggle --es name RAYS` flips the master switch
and `cmd dump` reports `rays=on,intensity,samples`. Measured on the PTC light
scene at 2D (`fps=` in `cmd dump`): 10.5 ms with rays off, 11.6 ms at 8 samples,
11.7 at 16, 11.8 at 24, 12.1 at 32; in 3D the chain adds ~4.7 ms for both eyes
(112 -> 74 fps in the same scene). The march itself is cheap at 1/4 res; the
fixed part is the extra FX-pass sampler and the two blur passes. A build with
the feature removed measured 10.1 ms off, so the extra sampler/branch costs
~0.4 ms **only when another FX effect is already running the post pass**; with
every effect off the FX pass is skipped entirely and the cost is zero.

- Everything is a live knob (panel sliders or `cmd fx`), defaults off so the
  base look is unchanged.

### Tone mapping and colour grading (GRADE tab)

The GRADE tab is a single display transform on the assembled scene colour. It
lives at the very end of the FX fragment shader (`gfx_opengl.c`, `s_fx_fs`,
inside `main()`'s final non-debug `else`), **after** AO/DOF/shadow/SSRTX/GI, the
dynamic lights, the crepuscular rays and the probe overlay have all been added
into `outc`, and **after** the debug-view chain, so every debug view still shows
raw linear buffers. Alpha is never touched: the pass writes `vec4(outc, 1.0)`
exactly as before. With `GRADE` off the shader takes one uniform branch and the
MAIN pipeline is unchanged; the pass is not requested at all unless something
else already needs it.

Chain order (all in the FX shader): exposure (`2^(EV)`) → tonemap → white-point
normalisation (`tonemap(W)` mapped to 1.0) → look → saturation → contrast →
tint. The white-point dial only has a shoulder to work with on the non-linear
operators; on LINEAR it is a plain gain.

Operators (analytic, no LUT needed):

| id | operator | curve |
|---|---|---|
| 0 | LINEAR | `c` (exposure/white point only) |
| 1 | REINHARD | `c / (1 + c)` |
| 2 | ACES | Narkowicz fit `(x(2.51x+0.03))/(x(2.43x+0.59)+0.14)` |
| 3 | FILMIC | Hable/Uncharted2 curve, exposed ×2 and normalised by `tm(22.4)` |

Looks (strength dial blends with the tonemapped colour, `Look strength %`):

| id | look | what it does |
|---|---|---|
| 0 | NONE | no look |
| 1 | SEPIA | classic 3×3 sepia matrix |
| 2 | COOL | blue/teal gain + small blue lift |
| 3 | WARM | amber gain + small warm lift |
| 4 | BLEACH | bleach-bypass-ish overlay of luma on colour, 70 % |
| 5 | CEL | per-pixel luminance step ramp (5 hard steps), colour rescaled by `q/l` so hue and chroma edges survive. No spatial filter, so geometry edges are untouched. |
| 6 | LUT | 2D stripe LUT pass (below); falls back to NONE if no valid file |

**LUT slot.** GLES2 has no 3D textures, so the expected file is a horizontal
stripe of 16×16 blue slices: width `16*N`, height `16`, red across a tile,
green down it, blue selects the slice (`256x16` = 16 slices, `512x16` = 32).
Row 0 of the image is green = 0 and column 0 is red = 0 (top-down, i.e. exactly
what an image editor shows). Drop it at:

```
/sdcard/Android/data/com.vdavid003.sm64port/files/luts/grade.png
```

(`SDL_AndroidGetExternalStoragePath()` + `/luts/grade.png`.) PNG/JPG, decoded in
Java with `BitmapFactory` (the embedded-asset build has no stb_image), then
uploaded as RGBA to the renderer on the next FX pass. Dimensions are validated;
anything that is not `16*N × 16` (N = 2–64) is rejected with an explicit
`GRADE LUT REJECTED: WxH is not a 16*N x 16 stripe LUT` log line and look 6
falls back to the analytic path / NONE. A successful load logs the path, size
and slice count. Selecting `fx 284 6` again re-reads the file, so the update
cycle is "push PNG, send one command". The LUT is applied by its own tiny pass
after the FX pass and after bloom, not as a seventh sampler inside the big FX
shader (an extra sampler in that shader has cost ~30 ms here before, per
`BACKLOG.md`); with no file and no look 6 the pass does not run and no FBO is
allocated.

The split debug view (`Split pre/post 0/1`, `fx 289 1`) shows the left half of
the frame before the grade and the right half after, with a magenta divider at
the canvas centre. It only applies to the normal (debug view 0) output.

| Control | Effect |
|---|---|
| `TONE MAP + GRADE` | master toggle (default **off**) |
| `TONEMAP` | LINEAR / REINHARD / ACES / FILMIC |
| `LOOK` | NONE / SEPIA / COOL / WARM / BLEACH / CEL / LUT |
| `Exposure EV x100` | exposure in hundredths of a stop (-300..300) |
| `White point %` | 100 = 1.0 scene white, up to 400 = 4.0 |
| `Look strength %` | blend between tonemapped colour and the look |
| `Saturation %` | 0-200, composes with any look |
| `Contrast %` | -100..100 → 0.5x..2x around mid grey |
| `Tint -cool/+warm` | R/B balance, -100 cool … +100 warm |
| `Split pre/post 0/1` | split-screen debug of the grade |

Headless keys `280`-`289`; `cmd dump` reports `grade=on,op,look,strength`.

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
- **Y** recentres the free camera behind Mario (or resets the vanilla view when
  free cam is off).
- The right-hand control can be switched from C-buttons to an analog camera
  stick (**C / ANA** toggle); the stick drives the free camera.
- Free camera parameters (sensitivity, aggression, pan, degradation) are read
  from the existing bettercam config keys.
- **CAM MODE** in the CAMERA tab cycles *vanilla → custom orbit → first person
  → FREE* (`BETTER` and `PUPPY` are built but hidden from the picker). First
  person puts the camera at Mario's eye (hiding his model), steers with the
  right stick, and eases the yaw back to his facing when the stick is released.
  FREE is a fully manual camera: drag to look, pinch to move, and the left stick
  drives it while **FREE MOVE** is on. It always enters from a usable
  third-person pose (behind Mario) instead of inheriting the first-person eye,
  which used to open onto a black void.
- **SENSITIVITY (CAMERA > SETTINGS, top):** LOW / MED / HIGH pick a multiplier
  band (0.6x / 1.0x / 1.6x) and the `Cam sensitivity` slider trims inside it;
  the effective percent (`trim x band`) is shown under the picker. Both are
  persisted across launches.
- **C-up shortcut:** the first C-up press enters the vanilla look camera; while
  that look camera is active, a second C-up press switches to FIRST PERSON.
  Entering first person also drops the Mario-look action so the left stick runs
  again. C-up or **Select/Back** leaves first person (an on-screen hint shows
  the Back shortcut), and the standard camera C-button sounds are muted while
  first person is active.
- **CAMERA tab:** the mode selector sits above the slider grid; the persistent
  debug header also has a redundant `CAM …` cycler next to `FPS …`.
- **Post-portal handoff:** a teleport still re-aims the camera behind Mario, but
  the rendered position/focus now ease over ~0.8 s (smoothstep) instead of
  cutting. Toggle with
  `adb shell $B --es cmd ptc --es action camblend`.
- **FLAT / SAME:** when 2D/FLAT or the SAME debug view is selected, only one eye
  is rendered and the composite reuses it for both halves.
- **Rumble:** the game now opens the haptic that belongs to the gamepad's
  `InputDevice` instead of the first haptic in the list (which was often the
  tablet's own vibrator). Test with
  `adb shell $B --es cmd ptc --es action rumble`.

### Occluder punch-through (CAMERA > SETTINGS)

Geometry between the camera and Mario normally hides him completely. The
**OCCLUSION** group on CAMERA > SETTINGS (next to **WALL NOCLIP**, the other
"what happens to the thing in front of Mario" control) replaces that with an
optional screen-space hole in the occluder. Mario is never hidden or faded by
this feature in any mode; only the occluder is affected.

| Mode | Button | Behaviour |
|---|---|---|
| 0 | `OFF` | nothing special; occluders render normally and can hide Mario |
| 1 | `DROPOUT` | occluders are discarded 100 % inside a hard-edged circle around Mario |
| 2 | `ALPHA` | occluders are partially dithered inside the circle (default 50 %) |
| 3 | `CUTOUT` | solid hole in the middle, dithered across the radial feather |

The test runs per fragment in the generated scene shader (`gfx_opengl.c`), so
there is no extra pass and mode 0 costs one coherent branch. With `d` the
normalised screen distance from Mario, `cov = 1 - smoothstep(1-feather, 1, d)`
and `hash` a static interleaved-gradient noise on `gl_FragCoord.xy`, mode 2
discards when `hash < cov * cover` and mode 3 when `hash < cov`. Only fragments
**nearer than Mario** are touched (`gl_FragCoord.z < depth - bias`), and HUD
rects (clip z = -1) are excluded, so his own faces, everything behind him, the
UI and the portal image are untouched. The centre and depth are projected per
eye from each eye's view-projection (including shear/toe/flip) once per frame;
on the Leia panel one 1280-wide eye is stretched across the 2560-wide display,
so `x` is compensated by `eye_width / full_width` and the hole is round in both
2D and 3D.

Sliders: radius (`fx 275`, target pixels), feather (`276`, % of radius), dither
coverage (`277`, % for ALPHA) and depth bias (`278`). `ptc_format_status()`
reports `occl=mode,radius,feather`. With WALL NOCLIP on, the PTC demo's
whole-wall hide is suppressed while OCCLUSION is on, so the wall stays visible
and the circular hole is the only mechanism. Proof stills (four modes, stereo
and camwall) live in `/media/share/tmp/artifacts/sm64/occlusion/`.

---

## Controls

- Floating left thumbstick (appears where the thumb lands), A/B/Z/R, C-button
  cluster or analog right stick, START, plus the camera X/Y buttons.
- The controls are a Java view layered above the Leia view (never per-eye, never
  letterboxed into the game canvas), drawn above the interlaced output.
- **Ctl opacity** fades the whole control layer (replaces the old Ctl X/Y/scale
  sliders). Defaults to **33%** and is persisted across launches.
- The in-game N64 touch sprites are hidden; the native overlay feeds the game's
  controller state directly.

---

## Camera tracking (head tracking + gyro)

Two independent view-matrix effects, both off by default, in the **CAMERA** tab:

- **HEAD TRACK** — polls Leia's face tracker (`LeiaSDK.getPrimaryFace()`, ~60 Hz)
  and translates the real view matrix by the tracked head position, so the scene
  behaves like a window. The tracker exposes x, y and z (plus pose angles), so
  leaning in/out dollies the camera rather than zooming it. Strength slider
  0–200 %; the positions are millimetres scaled to game units (1 unit ≈ 1 cm).
- **GYRO CAM** — Android `GAME_ROTATION_VECTOR` (fallback `ROTATION_VECTOR`)
  drives pitch/yaw/roll relative to the pose captured when it was enabled,
  applied as a rotation about a pivot **below the eye midpoint**. The **Pivot
  height** slider (0–200 cm, default 12) moves that pivot from a virtual neck
  joint down to chest height, which changes how the world swings when you tilt
  the tablet. `RECENTRE GYRO` re-captures the reference pose.

Both are applied as a view-space matrix on every modelview load, so they are
real camera motion (independent of the stereo IPD/toe shears) and are skipped
while the UI range is replayed so the HUD does not sway.

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
| Crepuscular rays | **off** (opt-in) | RAYS tab / `cmd toggle --es name RAYS` |
| Occluder punch-through | **off** (mode 0) | CAMERA > SETTINGS / `fx 274` |
| Tone map + colour grade | **off** (opt-in) | GRADE tab / `fx 280` |
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
- **L + R + START** opens it from gameplay, the pause menu, or the title screen.
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

### Cheats menu

The extended pause-menu options are opened with **R** (`R Options` on the pause
screen). The **CHEATS** submenu is enabled by default in this Android build, so
God Mode, Moon Jump, Infinite Lives, Super Speed, Responsive, Exit Anywhere,
Huge/Tiny Mario etc. are available without a command line. Triple-L in the pause
menu still re-enables it if the in-menu toggle was used to hide it.

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

## Portal gun

A Portal-style portal gun (`src/game/portal.c`), enabled by default in the castle
grounds.

- **Pickup** — a spinning prop sits next to Mario's spawn in the castle
  grounds; walk into it to pick it up. The gun is retained across level loads,
  paintings and level select. While held, the game UI draws a centre **aiming
  reticle** (with its own stereo depth from the target distance), and the touch
  overlay shows **P1** (blue) / **P2** (orange); gamepad **LB/RB** fire the same
  portals.
- **Held model** — after pickup, Mario carries a low-poly Portal gun generated
  from the CC0 model *Portal Gun* by 3DESIGNER27 (Tim Gubski), thingiverse
  3022786. `scripts/gen-portal-gun.py` decimates the 47k-triangle STL by vertex
  clustering (currently 1,103 verts / 2,933 tris) and emits a static N64
  display list (`src/game/ptc_gun_model.inc`). It is positioned at Mario's right
  hand and follows his interpolated render position and yaw.
- **Placing** — P1/P2 fire a hitscan from the centre of the camera view (up to
  4000 units). A shot only sticks to solid level geometry: object-owned surfaces
  (NPCs, coins, question blocks, breakable bricks, signs, doors…) and
  water/lava/death-plane/vanish-cap/quicksand surfaces are rejected.
- **Size** — the portal ellipse is now 1.5x larger (135 x 240 units).
- **Audio** — firing plays a throw sound; an accepted surface plays a positive
  select click; an ineligible surface (or a portal that cannot fit) buzzes.
- **Wall self-test** — `adb shell $B --es cmd ptc --es action walltest` fires at
  the four greybox room walls and reports a bitmask in the dump
  (`walltest=0xF` means front/back/left/right all accepted).
- **Orientation** — the decal is an ellipse oriented to the surface: upright on
  walls, world-X aligned on floors and ceilings, offset 1.5 units out of the
  surface to avoid z-fighting.
- **Edge fitting** — eight rim probes must land on the same *plane* (same normal
  and coplanar, so tessellated triangles are fine); otherwise the centre is
  nudged inward and the ellipse shrunk until it fits, and the shot is dropped if
  it cannot. This keeps a portal from spanning a wall/floor edge.
- **Teleport** — when Mario's centre of mass crosses a portal plane inside the
  ellipse he is moved to the other portal: position, velocity and facing are
  transformed by the portal pair's matrix (momentum preserved), he is pushed out
  of the exit portal, the camera is re-initialised, and a 20-tick cooldown
  prevents ping-pong. Walking into a wall portal works; a floor portal is
  entered by falling through it.
- Portals are cleared when the level/area changes; the gun itself is not.
- **Occluder fade (Portal Test Chamber)** — each platform, the ramp and each
  room wall has its own display list. A camera→Mario ray sets a per-object
  bitmask, so only the geometry actually in the way fades; everything else
  stays opaque.

**Through-portal view (experimental, off by default):** toggle with
`adb shell $B --es cmd ptc --es action portalview`. It renders the scene a
second time into an FBO from the portal-mirrored camera
(`C = V * M * V^-1`), then draws the source portal as a textured, depth-tested
ellipse. The first version is mono (both eyes sample the same portal image) and
does not yet do oblique near-plane clipping, so geometry behind the exit portal
can leak in; treat it as a prototype until it has been verified on device.

Not implemented yet: the clone of Mario seen standing in the destination portal.

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
adb shell $B --es cmd givegun                   # grant the portal gun (testing)
adb shell $B --es cmd portal --ei v 0           # fire P1 (0 = blue, 1 = orange)
adb shell $B --es cmd ptc --es action run       # Portal Test Chamber traversal
adb shell $B --es cmd ptc --es action reset     # reset level/portals/star
adb shell $B --es cmd ptc --es action calibrate # measure jump/ledge limits
adb shell $B --es cmd ptc --es action momentum  # floor -> 45-degree ramp launch test
adb shell $B --es cmd uncapped --ei v 1         # debug-only uncapped test ticks
adb shell $B --es cmd hz --ei v 60              # physical panel 60/120 Hz mode
adb shell $B --es cmd feedback                  # capture a feedback item
adb shell $B --es cmd feedback --es text "note"
adb shell $B --es cmd reinit | reset
```

### Feedback outbox

**FEEDBACK** under the FPS meter writes one self-contained item per tap
(`feedback.txt`, `shot.png`, `state.txt`, `log.txt`) to
`/sdcard/Android/data/com.vdavid003.sm64port/files/feedback/<timestamp>/`; the
label shows the pending count and long press adds a note. Short tap is the
no-text path. Pull and delete over adb:

```sh
D=/sdcard/Android/data/com.vdavid003.sm64port/files/feedback
adb shell ls -l $D && adb pull $D/<timestamp>
adb shell rm -rf $D/<timestamp>
```

`cmd feedback [--es text "..."]` does the same remotely. Items are outside the
repository and outside `files/debug-logs`.

Stereo knobs (`cmd set`): `0` IPD px, `1` zero parallax px, `2` depth %,
`3` eye width, `4` swap eyes, `5` flip Y, `6` same both, `7` tint, `8/9/10`
legacy touch offsets, `11` source width %, `12` half-res, `13` fit, `14`
in-scale %, `15` ZBUF, `16` wireframe, `17` toe %, `18` aspect %, `19` no cull,
`20` FOV %, `21` 2D, `30` STAY, `35` UIPOST, `36` UIPARALLAX, `37` UIBUF,
`38` UI depth px, `40` TOEAUTO, `41` HEADTRACK, `42` GYRO, `43` HDT strength %,
`44` gyro strength %, `45` gyro pivot height.

FX knobs (`cmd fx`): AO `1`-`6`, DOF `11`-`15` (`16` auto focus, `17` reports
the auto distance), SSGI `21`-`25`, SSRTX `70`-`77`, crepuscular rays
`250`-`260`, tone map + grading `280`-`289`, wall-run jump-off `320`-`329`,
normal flip `32`.

| group | keys |
| --- | --- |
| debug view `31` | 0 off, 1 AO, 2 CoC, 3 GI, 4 SSRTX, 5 light count, 6 light accumulation, 7 probe visibility, 8 light distance, 9 **motion-vector heatmap** |
| motion blur | `100` on, `101` strength, `102` samples (legacy), `118` heatmap gain (px = full scale) |
| motion vectors | `140` source (1 = per-vertex, 0 = camera only), `141` shutter deg, `142` sub-steps (1-16), `143` max blur px (0-128), `144` velocity res 1/n, `145` update every N frames, `146` sub-step jitter %, `147` depth reject %, `148` velocity clamp px (4-300), `149` heatmap style (0 magnitude, 1 direction, 2 segments), `240` vector-bound mode (0 off, 1 one sample, 2 3x3) |
| dynamic lights | `103` count, `104` shadows, `105` intensity, `106` shadow steps, `120`-`122` / `124`-`126` light 0/1 RGB |
| crepuscular rays | `250` on, `251` intensity %, `252` march length %, `253` decay per step %, `254` sample weight %, `255` samples (8-32), `256` brightness gate %, `257` source light (0-3, 4 = sun), `258` start jitter %, `259` debug (1 shafts, 2 visibility), `260` occlusion bias % |
| occluder punch-through | `274` mode (0 off, 1 dropout, 2 alpha, 3 cutout), `275` radius px, `276` feather %, `277` dither coverage %, `278` depth bias |
| camera sensitivity | `330` band (0 LOW 0.6x, 1 MED 1.0x, 2 HIGH 1.6x), `331` slider trim %, `332` reads back the effective % (`330`/`331` are also stereo-int keys; the panel slider is `49`) |
| light probes | `110` show, `111` spacing, `113` radius, `114` resolution, `115` grid cols (X), `116` grid rows (Y), `117` grid repeats (Z); `112` sets all three axes at once |
| bokeh / DOF | `160` kernel, `161` aspect %, `162` blades, `163` rotation, `164` anamorphic %, `165` highlight %, `166` cat eye % |
| rate limit | `200` on, `201` target fps, `202` adaptive, `203` reset stats |
| slow motion | `241` sim speed `1/n` (1-8, 1 = normal), `242` sub-frame interpolation (1 = re-present the last display list at k/n of the step; 0 = old stop-motion), `243` diagnostic: always present `t=1` (step end) while still re-presenting |
| denoiser | `210` alternative path, `211` on, `212` intensity, `213` passes, `214` radius, `215` frames, `216` bounces, `217` test captions, `220` resolution `1/n` (1 = full res, 2-4 = reduced; default 4) |
| emissive + bloom (GLOW) | `261` bloom on (default **off**), `262` threshold 0-255 (default 48), `263` intensity % (default 120), `264` blur radius (default 3), `265` blur passes 1-3 (default 2), `266` taps 5/9 (default 9), `267` emissive gain % (default 100), `268` debug view (0 off, 1 emissive buffer, 2 bloom buffer), `269` emissive tags on (default on), `270` also bloom scene pixels above threshold (default off) |
| jetpack light | `271` on (default on), `272` intensity % (default 150), `273` radius in world units (default 220) |
| wall-run jump-off | `320` stick blend % (default 65, 0 = the old fixed vector), `321` stick speed as % of run speed (default 120), `322` min outward direction x0.1 (default 40; clamps a stick held into the wall), `323` release window in ticks (default 5 ≈ 167 ms), `324` release window on (default 1), `325` jump-up vy x0.1 (default 100), `326` outward push x0.1 (default 120), `327` tangent speed % of run speed (default 112), `328` stick dead-zone 0-32 (default 8), `329` extra tangential speed when the stick is held along the run direction, % of run speed (default 15) |
| touch layout / profiles | `300` edit mode on/off, `301` RESET layout, `302` select element (0-19, see below), `303` selected x (per-mille of width), `304` selected y (per-mille), `305` selected scale % (40-250), `306` activate profile by index, `307` save current as profile `p<v>`, `308` export active profile, `309` log status |

Element indices for `302`: 0 `stick`, 1 `a`, 2 `b`, 3 `z`, 4 `r`, 5 `s`,
6 `l`, 7 `sel`, 8 `cup`, 9 `cdn`, 10 `clf`, 11 `crt`, 12 `camx`, 13 `camy`,
14 `p1`, 15 `p2`, 16 `tool`, 17 `jet`, 18 `mode`, 19 `rstick`.

String operations (profile save/switch/rename/delete/export) use the debug
channel instead of an integer key:

```bash
B="am broadcast -n com.vdavid003.sm64port/.DebugReceiver -a com.vdavid003.sm64port.CMD"
adb shell $B --es cmd layout --es action save --es profile wide
adb shell $B --es cmd layout --es action switch --es profile default
adb shell $B --es cmd layout --es action rename --es profile wide --es to couch
adb shell $B --es cmd layout --es action delete --es profile couch
adb shell $B --es cmd layout --es action status
```

Layouts are stored as JSON in SharedPreferences (`touch_layout/store`) and
mirrored to `Android/data/com.vdavid003.sm64port/files/layouts/`:
`layouts.json` (all profiles), `active.json` and `share-<profile>.json` (the
active profile). Positions are normalised (0..1) and sizes are multipliers of
a 1600-px reference radius, so a profile survives a different panel resolution.
| tone map + grading (GRADE) | `280` master (default **off**), `281` tonemap (0 linear, 1 Reinhard, 2 ACES, 3 filmic), `282` exposure EV×100, `283` white point % (100-400), `284` look (0 none, 1 sepia, 2 cool, 3 warm, 4 bleach, 5 cel, 6 LUT), `285` look strength %, `286` saturation %, `287` contrast %, `288` tint (-cool/+warm), `289` split pre/post debug |
| painting ripple | `350` interpolate the rippling painting's mesh on presented frames (default **on**); `0` restores the old per-sim-tick stepping for A/B captures |

The denoiser runs at 1/4 resolution by default: the scene colour and depth are
downsampled to `scene_res/4`, 1-3 edge-aware à-trous passes run there, and a
dedicated pass bilaterally (depth-aware) upsamples the result and hands it to
the FX pass as the scene colour. `220 1` restores the original full-resolution
in-shader filter for A/B. Measured on the light-test scene: 38.0 ms/frame at
1/4 res vs 66.7 ms at full res (`fps=` in `cmd dump`). See
`DENOISER-RESEARCH.md` §4 and `DENOISER-RATE-LIMIT.md` §§3, 5.2.

The probe grid is a `cols x rows x repeats` lattice around the probe centre,
capped at 16 probes (4x4x1, 2x2x2, 2x2x4, ...). It is only used for the probe
visualisation/reflection debug view.

### Emissive materials, bloom and the light registry

Two mechanisms, both rebuilt from scratch every frame so nothing leaks:

1. **World-space lights.** Gameplay registers up to four lights (`gfx_light_request`
   in `gfx_pc.c`; flames use the nearest-first auto pool). `gfx_run()` pushes them
   into the FX shader's existing `uLightPos/uLightColor/uLightRadius` slots and
   empties the registry afterwards. The PTC lightshow still calls the legacy
   `gfx_opengl_light_count()` path directly and **wins the slots** while it is
   running (`lightshowstop` releases them again).
   - The jetpack nozzle registers a warm white-orange light at the first flame
     quad (`movekit.c`), radius `273` world units at `272` intensity, with a
     small per-tick flicker, only while thrusting.
   - Level flames (`MODEL_RED_FLAME` / `MODEL_BLUE_FLAME`) register a light when
     they are within 1400 units of the camera; the four nearest win.

2. **Emissive draw batches.** An object or draw range is tagged with a
   renderer-local display-list marker (`GFX_OP_EMISSIVE_MARKER`, opcode `0x3e`,
   mirroring the existing splat marker). The tag carries colour + strength.
   Because it lives *in* the display list it survives the deferred, per-eye
   replay, which a "current object" global would not. `gfx_opengl_draw_triangles`
   copies the tagged vertices, and `gfx_opengl_emissive_flush()` re-draws them
   into a per-eye half-resolution **emissive buffer**. Tagged today: the jetpack
   cone, sparkles (`MODEL_SPARKLES*`), red/blue flames.

**Bloom** (GLOW tab, off by default) reads that emissive buffer only: threshold
→ separable blur (H/V, 5 or 9 taps, 1-3 passes) → additive bilinear upsample as
its own pass into the FX output (the recorded lesson: folding the upsample into
the big FX shader cost ~30 ms). A bright white wall therefore does **not** bloom
unless `270` opts scene brightness in. `268 1` shows the raw emissive buffer and
`268 2` the blurred buffer, which is the proof the source is world-space
geometry and not a screen-space blur of the final image.

Cost on the Lume Pad 2 (level 38, jetpack thrusting, 2D flat presentation so a
still is one image, rate limiter off so `fps=` is a real frame time):

| config | ms/frame (`1000/fps`) |
| --- | --- |
| emissive off, bloom off, no other FX | 2.84 |
| emissive tags on, bloom off | 2.87 |
| bloom on, no other FX (forces the FX pass to run) | 6.29 |
| MXAO on, bloom off | 11.03 |
| MXAO on, bloom on | 12.05 |

So the bloom chain itself is **~+1.0 ms** at half resolution when the FX pass is
already running; the ~3 ms jump from "no FX at all" is the full-screen FX pass
being switched on, not the bloom stages. Emissive capture (one half-res clear +
a handful of tiny draws) is below the run-to-run noise.

`cmd pad` injects raw overlay input for headless menu/gameplay driving:
`--ei sx/-127..127 --ei sy/-127..127 --ei buttons <N64 pad bits> --ei ms <hold>`.
Handy bits: `4096` START, `32768` A, `16384` B, `8192` Z, `16` R.

The on-screen tuning panel is collapsed by default. Its drawer is toggled by
the caret in the top-left (`▾` closed / `▴` open), by tapping a top tab
while closed, or by a swipe up on the bar. Collapsed it is two short rows:
the header (`[caret]` then `3D|2D`, `FPS`, `CAM` on the right) and the tab row
(`FX` `DEBUG` `ABOUT` left, `RESET` right).

Tabs:
- **FX** — second-level strip: MXAO / C.DOF / SSGI / SSRTX / RAYS / SHADOW /
  SPLAT / LIGHT / VIEWS / CAMERA.
  - RAYS: crepuscular-ray toggle, intensity, march length, decay, weight,
    samples, brightness gate, source light, jitter, occlusion bias, debug view.
  - LIGHT: BLUR / LIGHTS / PROBES / LENS / RATE-DN / **GLOW**.
    GLOW holds the emissive/bloom dials and the jetpack nozzle light.
  - VIEWS: all debug views (OFF, AO, CoC, GI, SSRTX, LIGHTS, L-ACCUM,
    PROBE VIS, L-DIST, MOTION VECTORS) plus a small reorderable layer stack
    (ZBUF, WIRE) with per-layer on/off.
  - CAMERA opens a third strip: STEREO / SETTINGS. SETTINGS carries the
    OCCLUSION group (mode 0-3, radius, feather, dither, depth bias) directly
    under WALL NOCLIP.
- **DEBUG** — MOVE / PORTAL / BALL / SYS / LOGS. SYS has SESSION, **QUICK
  DEV**, SYSTEM, DISPLAY, HUD and PERFORMANCE sections. QUICK DEV is the cheap
  device-loop boot (off by default): QUICK DEV, QD LEVEL (default L38, the
  Portal Test Chamber; `cmd level` updates it) and QD MUTE AUDIO (off; audio
  matters for some bugs). When on, the next launch skips the logo, attract
  demo and press-start wait (the title entry advances by itself) and boots the
  configured level through the normal level-select path, with boot presentation
  sleeps capped at 2 frames until the level is live. A small `QUICK DEV L<n>`
  marker is drawn bottom-left and the PTC status line gains
  `qd=<on>,<level>,<mute>`.
  A normal launch is unchanged when it is off. HUD (GAME HUD, DEBUG CAPTIONS,
  TOUCH CONTROLS) and PERFORMANCE
  (PERF MONITOR, FRAME GRAPH, FPS COUNTER — all on by default) follow. LOGS is the
  bug-report page: EXPORT/SHARE the debug report, REFRESH INFO, **LEIA
  VERBOSE** (switches the SDK to Trace logging and restarts so the whole
  launch is captured; the capture then covers every process, including the
  head-tracking service, and the file cap rises to 16 MB) and **RESTART APP**.
  The exported report starts with app/Android/device/firmware versions, a live
  app-state block (FPS mode, rate-limiter frame times, FX/HUD flags, stereo
  values, full PTC status line with lights/denoiser/rate/knobs), the previous
  session's exit status (clean vs unclean/crash/force-stop) and the Leia
  weaver boot markers, then the captured log and logcat snapshots. The last
  three reports are kept in `cache/debug-logs`; SHARE LAST REPORT re-sends the
  newest one without rebuilding it.
- **ABOUT** — GENERAL / CHANGELOG.

Every tab and sub-tab carries a small dot: green when that page has a feature
enabled, grey otherwise. The selected tab is underlined and white. Toggle and
button rows are sized to their labels instead of stretching across the panel,
so they are not one huge touch target. The collapsed drawer is just the header
row plus the status line; opening it animates the black panel between heights
with an ease-in-out curve.

Performance monitor:
- a rolling 120-frame frame-time graph sits under the FPS counter, with a
  reference line at the current target period (green bars under budget, red
  over);
- every FX page has a **MEASURE** button that A/B samples the effect (off for
  ~0.5 s, on for ~0.5 s) and reports the delta, e.g.
  `MXAO: +14.30 ms (off 8.4 / on 22.7)`;
- VIEWS keeps a PERF table of all measurements with REFRESH/CLEAR.

The FPS label shows `∞` in uncapped mode. Camera modes BETTER and PUPPY are
temporarily hidden from both the mode pills and the header CAM switcher.

---

## Known gaps / roadmap

The maintained list of open work — crepuscular rays, emissive particles and
bloom, known bugs, and the environment facts that are easy to lose — lives in
[`BACKLOG.md`](BACKLOG.md). This section is the long-form version of the
user-visible gaps.

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
- Resume from background is fixed: the Leia SurfaceTexture is rebound to SDL's
  native window/EGL surface when the activity comes back.
- Slider values (stereo, FX, SSRTX, shadows) are not persisted across launches;
  the ragdoll/Puppycam/shadow master defaults are. Control opacity is persisted
  separately in Android SharedPreferences.
- Head/eye-tracked 6DoF (Phase C) is partially there: head-tracked parallax and
  gyro canting exist (CAMERA tab), but there is no per-eye 6DoF rig yet and the
  tracker's z axis has not been validated on hardware.
- Portal gun: the through-portal camera relay and the Mario clone in the exit
  portal are not implemented (phase 3); portals are solid coloured decals and
  the teleport is instantaneous.

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
