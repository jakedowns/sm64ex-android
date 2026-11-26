# Backlog — open work and things worth not forgetting

Last reviewed: **2026-11-25**. This is the durable list for work that is
planned, half-finished, or easy to lose when a chat is gone. Detail belongs in
the linked docs; this file is the index.

Update it in the same commit as the change that makes a line stale. A thread
that is finished gets deleted here, not ticked.

---

## In progress

### Camera below the ground: COLLIDE / CLIP (worktree `w3-cam-ground`)
Code-complete 2026-11-26, not yet built or device-verified (the parent applies
the patch to the main tree and builds centrally).

The custom orbit camera used to collide with everything: the pull-in ray
(`find_surface_on_ray`) shortened the orbit distance for a floor or ceiling and
a final `find_floor` clamp lifted it 80 units above the floor. **GROUND** on
CAMERA > SETTINGS, directly under **WALL NOCLIP**, is now a persisted
`COLLIDE` / `CLIP` picker:

- `COLLIDE` (default) is the historical behaviour, unchanged.
- `CLIP` identifies floors and ceilings by surface normal (`|normal.y| > 0.7`,
  not object type, so PTC geometry works too), re-casts past each horizontal
  hit until it finds a wall, and skips both floor clamps: the custom camera's
  own and `update_lakitu`'s (`camera.c`, gated on mode 3 + CLIP). Walls and
  steep slopes still collide; `gfx_cam_noclip` keeps its own "hide the wall"
  meaning and still wins when on.

Dial: `fx 360` (0 COLLIDE, 1 CLIP); `cmd dump` ends with
`camclip=collide|clip`. FREE/puppycam are untouched (FREE never collided).

Stills to capture after the parent's build, from one fixed pose: `fx 360 0`
(camera pulled up above the floor) vs `fx 360 1` (camera under the floor with
the world still rendering correctly), plus one wall case with `fx 360 1` to
show the camera still pulling in. Artifacts:
`/media/share/tmp/artifacts/sm64/cam-ground/`.

### Wall-run 90° corner smoke test (worktree `w3-corner-test`, 2026-11-26)
Code-complete, not built and not device-run (the parent builds centrally). This
is the code side of the `TODO.md` line "Wall-run rig pass window: measure and
document the timing window at a 90° corner".

`ptc corner0..7` runs a deterministic corner chain on the chamber's own walls —
wall A the room west wall (`x=-2600`, normal +X, run -Z), wall B the room south
wall (`z=-3200`, normal +Z), inner corner `(-2600,-3200)` — and `cornerliveN`
is the real-time variant for stills/clips.  Mario starts airborne at
`(-2574, 900, -700)` with `vel (-14,0,-42)` and the stick held at `0x8000`
(along A, towards B); only the jump press is timed, as a distance `d` before
the corner.  In batch mode the rig freezes the start pose until `cmd runticks`
begins, so adb latency cannot shift the timing, and each tick emits a `WRC`
trace line (position, action, wall normal) alongside the dump's
`wrc=<case>,<stage>,<outcome>,<re>,<done>,<adv>,<ymin>,<corner>,<djump>,<jy>,
<maxstall>,<bonk>`.

`scripts/wallrun-corner-test.py` runs all eight timings in one device-lock
hold (it refuses without `AGENT_DEVICE_LOCK_HELD=1`), cross-checks trace and
dump, saves one still per timing, a mid-corner still and a clip of the passing
timing, prints the table + window, and exits non-zero unless the nominal timing
(index 3, `d=1480`) corner-passes.  Dials `fx 410`-`415` retune it live.

Predicted window from the source constants (device run still to do): pass for
`d ≈ 1280-1690`, bounded below by the 18-tick `s_cooldown` after a jump-off
(a late jump reaches wall B inside it, bonks and loses the run) and above by
the ~24-tick airtime from y=900.  That is ~6 ticks / ~200 ms of press
timing.  TESTING.md has the rig table, the acceptance criteria and the table
to replace with the measured one.

Two things to do after the device pass: tick the `TODO.md` corner line, and
answer the open question there — whether an early/late jump should bounce
instead of falling.  Current answer from the code: it already does what the
physics says, late = bonk (jump-off cooldown), early = falls short of the wall;
changing that is a gameplay decision, not a rig bug.


All nine worktrees are applied to the main tree and the combined build is green.
Not yet verified on hardware because the tablet dropped off Wi-Fi adb and needs a
human wake.

| feature | keys | commit |
| --- | --- | --- |
| touch layout editor + profiles | `300`-`309` | merged in the wave |
| camera sensitivity band + FREE-mode black-screen fix | `330`-`332` | `ab4f945` |
| feedback outbox | — | `a8fde17` |
| wall-run stick push-off + release window | `320`-`329` | `685ce6a` |
| tone mapping / looks / LUT | `280`-`289` | `5925b7b` |
| painting ripple at present rate | `350` | `4e2a0c8` |
| grapple targeting view | `310`-`315` | `d9972df` |
| cam-stick curve, heatmap, C-bits, R-follow | `290`-`299` | `93cc92d` |
| panel compaction, glyphs, docking | — | `ca27d21` |

Merge scars worth knowing: each worktree patch had to be applied on top of the
previous ones, and the fx switch cases (290-299 / 310-315 / 320-329 / 330-332 /
350) interleave, so a "keep both sides" resolution twice dropped the closing
brace of the neighbouring case — the compiler caught all of them. The cam-stick
patch also assumed the pre-layout `TouchOverlay` field `stickRadius`, now
`stickRadiusPx(E_RSTICK)`.

**Device pass still to run:** panel stills in both docks + restart persistence,
camera sens/cycler wrap, feedback capture + adb pull/delete, wall-run six-case
harness, paint-ripple alternation metric, grapple stills, cam-stick heatmap and
R-follow clips, grading operators/looks/LUT + cost.

### Painting ripple at the present rate (unmerged, worktree `w2-paint-ripple`)
`paintings.c` regroups the mesh and relights it once per 30 Hz sim tick and
bakes the result into the display list Vtx array, so the extra 60/120 Hz
presents replayed it unchanged. The ripple now keeps the two most recent meshes
and `gfx_patch_frame()` rewrites the baked vertex height + normal for the frame
being presented, the same way display-list matrices are rewritten. The state
machine, `rippleTimer` and `currRippleMag` are untouched, so the transition
still takes the same number of ticks; the entry-ripple dwell and the real warp
duration were measured identical with `fx 350 0` / `fx 350 1`.
Dials: `fx 350` (1 = interpolate, default; 0 = old stepping for A/B), status
field `paint=<on>,<blocks>,<verts>,<alpha*1000>` appended to `ptc_format_status`.
PTC rig: `ptc paint` (fixed camera on the Bob painting + re-armed entry ripple),
`ptc paintwarp` (real warp transition for the duration comparison), `ptc
paintoff`, `ptc paintdump` (per-present `SM64Paint` telemetry: alpha and the
presented ripple height). Clips and frame strips:
`/media/share/tmp/artifacts/sm64/paint-ripple/`.

### Captions as a Java overlay (worktree `w3-caption-java`, 2026-11-26)
Code-complete, not yet built or device-verified. `print_caption*` now also
publishes the main and small lines for four JNI getters, and
`render_text_labels()` skips those two lines when Java owns the text; what a
caption says and when it appears is unchanged. A `CaptionOverlay` view draws
them with a bold monospace typeface at the native position (~15 Hz poll), above
the Leia view and below the touch overlay and debug panel, never taking input.
DEBUG > SYS > CAPTIONS toggles JAVA (default) / GAME FONT with persisted size
and opacity sliders; `cmd captions --ei v 1|0` is the adb path and `cmd dump`
logs `CAPTIONS mode=...,main="...",small="..."`. The `QUICK DEV L<n>` marker
stays native in both modes because it has to annotate a GL readback. Consequence
to remember: the feedback `shot.png` is a GL readback, so JAVA captions are not
in it (GAME FONT is; a screen `screencap` shows both). The overlay is screen
space at zero parallax, so it does not follow the UI-depth / UI-size / UI-box
stereo sliders the way the native caption inside the UI pass did; at the
default 0 px / 100 % they coincide.

**3D follow-up (2026-11-26).** A 3D capture showed the game font again while
captions were set to JAVA. The 3D shots came from a different APK than the 2D
one (branch label `HEAD` vs `master`) and had no `CAPTIONS` dump line at all,
i.e. that run did not have the feature; but three real weak points were fixed
anyway: `caption_java` was written with `apply()` (a force-stop right after the
command could lose it, so a restart read the old GAME FONT), the native flag was
only set in `onCreate` (a Leia/3D surface change or recreated engine could leave
it at 0), and the overlay poll now compares `nativeGetCaptionMode()` against its
own mode every 66 ms and re-asserts it — so the two can never disagree. The
mode is also re-applied in `onResume`, in `setStereoMode` and at the end of
`initLeia`, the overlay re-attaches if it is no longer in the live layout, `cmd
captions` commits the pref, and `cmd dump`'s first line is now unconditional:
`CAPTIONS mode=… native=… stereo3d=… build=… overlay=… vis=… idx=…/… parent=…
leiaIdx=… panelIdx=… main="…" small="…"`.

Still to capture on hardware: JAVA vs GAME FONT at one fixed caption (PTC level
38, e.g. `ptc light show` or `ptc momentum`), each in 2D and over the Leia 3D
view, plus one panel-open still to prove the overlay sits below the panel.
### C.DOF cost: reduced-resolution gather (worktree `w3-dof-perf`, 2026-11-26)
The sweep measured C.DOF at **30.30 ms in 3D** (Δ +20.95 over the 9.35 ms
empty FX pass) and 14.08 ms in 2D, plus +5.5 ms for the polygonal bokeh. Cause:
the gather runs at full eye resolution (1280×1534 per eye) and each of the 16
taps does **one full-res colour fetch + one full-res depth fetch + linZ + a CoC
division**, for an output that is defocused by definition; the bokeh path adds
`atan`/`cos`/`mod` per tap.

New path (fx **380-389**, defaults pick it; dial 380=0 is the legacy gather):

| key | default | meaning |
| --- | --- | --- |
| `380` | 1 | 0 = legacy in-shader full-res gather, 1 = reduced-res prepass |
| `381` | 2 | prepass divisor 2-4 (scene_res/2 default) |
| `382` | 1 | near field: 0 none, 1 CoC-aware (no depth fetch), 2 low-res z test |
| `383` | 0 | prepass tap count (0 = key `15`) |
| `384` | 1 | 0 bilinear upsample, 1 depth-aware bilateral |
| `385` | 1 | 0 recompute CoC per tap (A/B), 1 precomputed CoC in colour alpha |
| `386` | 1 | 0 in-shader atan blade mask, 1 precomputed taps+mask |
| `387` | 1 | 0 point taps, 1 bilinear taps |
| `388` | 0 | 1 = log `DOFSTAGE`/`FXSTAGE` ms (glFinish probes) |
| `389` | 100 | in-focus sharpness pass-through % |

`cmd dump` reports `dof=<380>,<381>,<382>,<383>`. Same shape as the denoiser
(`s_dn_*`): prepare colour+CoC and packed z at eye/div, gather there, then a
full-res bilateral upsample as its own pass. Result is fed to the FX shader as
`uColor` with its DOF block off, so everything else (AO/GI/shadows) is
unchanged. Still to do: automatic "no near field in frame" detection needs a
CoC-max reduction plus a throttled 1×1 readback; only the forced dial exists.

### Reduced-resolution SSGI (worktree `w3-ssgi-perf`, `fx` 400-405)
SSGI was the most expensive effect measured (+30.7 ms in 3D at the default 6x4,
142.9 ms at 12x8) for output that is pure low-frequency colour bleed with a
150-unit radius, so the gather now runs at `canvas_res/div` (default 4) in its
own chain and is added back by a dedicated full-res pass — the same shape as the
denoiser (`s_dn_*`), because adding a sampler to the big FX shader cost ~30 ms
once already.

Chain, per eye: downsample colour -> gather the ray/tap loop at 1/16 the pixels
-> edge-aware 3x3 blur there -> bilateral (depth-stopped) upsample that is added
into the FX output with `GL_ONE/GL_ONE`. Linear depth and view-space normals are
precomputed once per low-res texel, so taps never reconstruct them. Tap offsets
are canvas-UV (derived from the same NDC offset), so the 150-unit world radius
stays 150 units at every divisor; only the pixel count changes. Rotated-grid
per-pixel offsets plus per-frame jitter replace the samples the resolution
removes.

Dials: `400` divisor (1 = today's full-res in-shader loop, for A/B), `401`
low-res blur passes (default 1), `402` rotated-grid/temporal jitter, `403`
low-res debug view (1 GI, 2 depth, 3 normals; adb only), `404` far/sky skip
percent (default 95), `405` normal-aware tap reject. Status field
`gi=<on>,<div>,<blur>,<norm>`; panel controls on the SSGI tab.

GI with intensity 0 now costs nothing at all: `stereo_fx_any()` ignores GI when
`22=0`, so the whole post pass is skipped.

Measured (PTC level 38, `ptc annex`, pacer off, `fps=`, median of 3, Δ over the
empty FX pass): 6 steps × 4 rays goes **26.32 → 11.90 ms in 2D** (+18.5 → +4.1)
and **52.63 → 23.26 ms in 3D** (+38.3 → +9.0); 12 × 8 goes 111.1 → 20.4 ms (2D)
and 166.7 → 31.3 ms (3D). Half res is *not* a win on this GPU (27.0 ms 2D,
same as full res — the gather's working set stops fitting the texture cache),
so 1/4 is the only useful divisor. Stage split at the default in 2D: 1.6 ms
fixed (downsample + blur + upsample/composite) and 2.5 ms for the 24 taps;
per tap 1/4 res is 7.4× cheaper than full res, not 16×. Quality against today's
full res on one fixed camera: mean |diff| 2.6/255 at 1/2 and 3.2/255 at 1/4, and
the residue is edge-local (silhouettes, surface junctions). Raw numbers, stills
and `RESULTS.md`: `/media/share/tmp/artifacts/sm64/ssgi-quarter/`.

Two traps found on the way, worth remembering: **Adreno rejects integer `%` in
a fragment shader** ("supported in pack/unpack shaders only") and the failure is
invisible from outside — the chain silently falls back to the legacy full-res
loop, so every divisor measures identically until the compile log is read; and
the **idle chase camera is not static** for stills (check `cam=` and take all
rows in one settle window, with `s:41=0 s:42=0`).

Remaining wins on this thread, in order:
1. **Drop the normal prepass when it is not used.** Mode 1 exists only for
   `405` (default off) and the `403 3` debug view, and it reads three full-res
   depth texels per low-res texel — the cache-hostile third of the fixed cost.
2. **Allow divisor 8.** The taps are 2.5 ms of the 4.1 ms default in 2D; at 1/8
   that falls to ~0.6 ms and the 1.6 ms fixed chain dominates. One line.
3. **Cheapen the full-res composite.** It is the biggest single piece of the
   fixed cost: 4 depth fetches + a GI fetch per pixel per eye. Either sample the
   GI bilinearly with one depth tap, or fold it into the existing bloom
   upsample when both are on.

### MXAO cost: reduced-resolution chain (worktree `w3-mxao-perf`, 2026-11-26)
MXAO was **30.5 ms in 3D** (17.96 over the 12.5 ms empty FX pass) and 15.8 ms in
2D at 12 taps, radius 120. Cause: everything ran at full eye resolution and the
AO loop was 13 depth fetches per pixel per eye (1 centre + 2 for the normal +
12 taps), each re-deriving `linZ` with a divide and `viewPos` with two more,
plus a `sin`/`cos` pair per tap; there was no blur at all, no sky early-out and
no skip at intensity 0. Cost was exactly linear in taps (1.33 ms/tap in 3D,
0.66/tap/eye), i.e. fetch-bound; the normal reconstruction was already free.

Now three small per-eye passes at `scene_res/2` (fx **390-396**, defaults pick
it): prepass `(packed linear z, octahedral view normal)` → 12-tap spiral gather
reading only that field → depth-aware 4-tap apply that multiplies the scene
colour, so the FX shader keeps no AO code and no extra sampler. The chain runs
after the denoise and C.DOF prepasses, and when AO is the only effect the big
FX pass is skipped entirely (that alone was 8.4 ms of the 30.5).

**3D 30.5 → 12.1 ms (2.5×), 2D 15.8 → 6.8 ms**; the whole chain (6.3 ms 3D) is
cheaper than the empty post pass it removes, and `LOW + AO8` in 3D went 35.8 →
28.2 ms, so 30 Hz with contact AO now fits. Half res keeps the look (AO ~2 px
softer at silhouettes, no halos); quarter res stair-steps thin geometry
(Mario's cap brim) and is a dial, not the default. `392`'s per-pixel rotation
is the textbook fix for banding but costs 1.9-3.1 ms in 3D and leaves speckle
on flat walls, so the default keeps the legacy one-direction-per-frame pattern.
Keys, measurements, stills and the rerun recipe:
[`findings/mxao-perf-11-26.md`](findings/mxao-perf-11-26.md). Still open:
quarter-res as a performance mode, an `RGBA16F` gather field so 6 bilinear taps
replace 12 NEAREST ones, and a half-res 3×3 depth-aware blur that would make 8
taps plus a static pattern viable.

### Shipped this wave (merged 2026-11-25)
Rays (`6c55b1e`), the reduced-res denoiser, slow-motion interpolation, the
emissive/bloom/GLOW work, the occluded-Mario punch-through and the HFR held-object
fix are all in the main tree; the per-thread details are below and in the linked
docs. What is genuinely still open is in "Planned, not started" and
"Known bugs and limits".

### In-app feedback outbox (2026-11-25, worktree `w2-feedback`)
**FEEDBACK** under the FPS meter captures one item per tap without text
(long press adds a note) into
`/sdcard/Android/data/com.vdavid003.sm64port/files/feedback/<timestamp>/`:
`feedback.txt`, `shot.png` (game frame), `state.txt` (debug-report header +
full PTC status line) and `log.txt` (256 KB app-log tail). Items accumulate
until an agent pulls and deletes them; the button label is the pending count and
`cmd feedback` triggers a capture over adb. Pull/clear commands are in
`AGENTS.md`. Known limits: the shot is the renderer's SBS frame (not the woven
panel output), so in-app HUD/panel chrome is not in it, Java-overlay captions
are not in it either (see the CAPTIONS entry above), and only the newest frame
at the tap is captured (no history).

### Held-object HFR scale pulse (Bowser tail spin) — fixed
Investigated and fixed 2026-11-25 in worktree `bowser-scale`.
Two defects: the held object's previous matrix came from the local hand stack
instead of the holder root (0.25 scale in the holding pose → a 25%–100% pulse
every tick at HFR), and the element-wise 4×4 blend shrinks fast rotations by
`cos(delta/2)` (2.6% residual). Both are fixed; telemetry and the
`ptc bowser` / `ptc hfron` rig are described in
[`HFR-HELD-OBJECTS.md`](HFR-HELD-OBJECTS.md).

### Camera: FREE-mode black screen + sensitivity picker (worktree `w2-camera-fixes`)
Code-complete 2026-11-25, not yet compiled or device-verified.

The top-bar `CAM …` cycler did not go black on the vanilla wrap. The third press
(vanilla → custom → first person → **FREE**) showed a black view *and* labelled
itself `CAM: VANILLA`, because `camModeName()` had no `case 5`. Two defects:

- The black was the free camera's own entry pose. `custom_camera_freecam_update`
  converted the inherited look direction with `atan2s(dx, dz)` and
  `atan2s(dy, horiz)`, but this engine's convention is `(z, x)` and
  `(horizontal, vertical)` (see `vec3f_get_dist_and_angle`). Any inherited level
  view therefore came back as pitch `0x4000`, i.e. **straight up**, and the
  freecam rendered the void above the room.
- `CAM: VANILLA` while in FREE made that look like "the cycler wrapped and the
  screen went black" rather than "a new mode is active".

Fixes: correct the conversion, reject an inherited pose that is inside Mario or
more than 67° off level and seed a behind-Mario pose instead, add the `FREE`
label, log mode transitions and the freecam entry pose, clear pending
pinch/drag deltas when leaving FREE, and show a "drag to look / pinch to move"
hint in FREE. Re-entering FREE now always starts from a usable view.

CAMERA > SETTINGS also gained a persisted **SENSITIVITY** picker at the top:
LOW/MED/HIGH = 0.6x/1.0x/1.6x band, the existing `Cam sensitivity` slider
trims inside it, and the effective percent is printed. Keys: `330` band,
`331` trim, `332` effective readback; the panel slider stays `49`; both are in
`cmd dump` as `sens=band,trim,effective`. See `README-lumepad-2-fork.md`.

Still to do after the parent's build: device stills for every mode plus the
wrap, and a check that the picker's persisted values survive a relaunch.

### Dithered punch-through for an occluded Mario
Started 2026-11-25 in worktree `occlusion-fade` (not merged).

Today an occluder just wins: `gfx_cam_noclip` hides the PTC wall and the
per-object occluder fade (`ptc.c`) makes specific platforms vanish; there is no
general "let me see Mario" behaviour. The thread adds four modes — OFF, FULL
DROPOUT, PARTIAL ALPHA (dithered see-through), CIRCULAR CUTOUT (soft radial
hole with a dithered feather) — as a `discard` in the generated scene shader,
restricted to fragments nearer than Mario, with a per-eye centre and depth so
it lands correctly in 3D. **Mario is never hidden by any of these modes:**
"dropout" always means the occluding geometry dropping out, which is why mode 1
is the hard-edged baseline the dithered modes improve on. Controls: OCCLUSION
group in the panel (mode, radius, feather, dither, depth bias), `fx 27x`.

### Cam stick: curve, heatmap, vanilla C-stick, R-follow
Implemented 2026-11-25 in worktree `w2-camstick` (`fx 290`-`299`, CAMERA >
CAM STICK). Five-point monotone-cubic response curve with a draggable editor and
a radial heatmap in the on-screen stick circle, vanilla-mode digital C-stick
mapping, and three R-shoulder follow behaviours (toggle/hold/snap+release) with
spring/damping sliders. See README "Cam stick: response curve, radial heatmap
and R-shoulder follow". Open follow-ups: the heatmap is rebuilt only on curve
changes but the view is still invalidated at 2 Hz by the debug readout (a cached
bitmap, so ~0 cost); the curve applies to the on-screen stick only, not to a
paired gamepad's right stick; and the C-up edge that a full push generates in
vanilla mode is intentional (a real pad does the same) but surprising.

**Device proof still to capture.** The build is in and
`scripts/camstick-curve-check.py` proves the shipped curve is monotone and that
a dragged point changes the effective rate (in=56: 48 -> 120 of 127), but the
panel dropped off Wi-Fi adb during the first device run (30 s screen timeout —
raise it first, `scripts/camstick-test.sh` now does), so the heatmap still, the
C-bit table and the three R-follow clips are not captured yet. Run
`scripts/camstick-test.sh` under the device lock on a woken tablet; artifacts
to `/media/share/tmp/artifacts/sm64/camstick/`.

### Merge notes (2026-11-25)
Merging the three worktrees into the main tree needed real conflict resolution,
not a fast-forward, and the lessons are worth keeping:

- **Keys collide across worktrees.** Rays took `250`-`260`, the glow work started
  at `260` (renumbered to `261`-`273`), and the occlusion work independently
  took `261`-`265` (renumbered to `274`-`278`). Always reserve a range in
  `BACKLOG.md` before starting a feature with dials.
- **Interleaved C merges are worse than additive ones.** Both `gfx_opengl.c`
  additions were self-contained functions, but git aligned their shared tails
  and produced function definitions inside function bodies. The working fix was
  to rebuild the region from each side's own copy (glow block, then rays block)
  rather than patching the conflicted text.
- **Status-line format strings** (`ptc_format_status`) and the Java page strips
  conflict every time: append new fields at the end of the format family and
  build new pages last.
- **One worktree at a time per file.** The parallel approach still paid for
  itself, but the merge cost should be budgeted.

### Crepuscular rays: quality follow-ups
Shipped 2026-11-25 (quarter-res screen-space shafts, RAYS tab, `fx 250`-`260`;
see README "Crepuscular rays"). Still open:

- **Shadow-map occlusion as a quality dial.** Today the march only tests the
  depth buffer against the pixel's own depth plane, so shafts need a visible
  silhouette and thin/off-screen occluders contribute nothing. Sampling
  `uShadowMap` at each march step would let geometry outside the view block the
  light; keep it behind a dial and measure before making it the default.
- **Emissive sources.** Shafts from the star, a 1-up or the jetpack nozzle need
  the emissive/bloom work below; the current `threshold` dial only gates on
  scene brightness, it does not identify a source object.
- **Hero demos.** The PTC lightshow lights sit above the frame, so the demo
  stills are subtle. A `ptc` action that parks one light low behind a pillar
  would show the shafts off better than the free-look orbit.

### Emissive particles + bloom — **implemented** (2026-11-25)Emissive draw-batch tag (`GFX_OP_EMISSIVE_MARKER`, display-list marker), a
per-eye half-res emissive buffer, a threshold/blur/additive-upsample bloom chain
that reads only that buffer, and a per-frame world-space light registry (jetpack
nozzle + nearby flames). Dial it from the FX > LIGHT > **GLOW** tab (`fx`
261-273); debug views `268 1` (emissive buffer) and `268 2` (bloom buffer).
Bloom is off by default. See `README-lumepad-2-fork.md` for the key table and
measured cost. The god-ray pass below should now shaft from this buffer.

### Tone mapping + colour grading (GRADE tab) — **implemented** (2026-11-25)
Exposure/white point, four analytic tonemap operators (linear, Reinhard, ACES
fit, Hable filmic), six analytic looks plus an optional 2D stripe LUT, and
saturation/contrast/tint on top (`fx 280`-`289`, off by default). See the GRADE
section of `README-lumepad-2-fork.md` for the LUT format and path. Open:
- **Bloom is added after the tonemap** (the glow pass writes into the FX output),
  so a bright emissive pixel is not rolled off by ACES/filmic. Moving bloom into
  the scene buffer before the FX pass would fix it; not done here.
- **No LUT interpolation between slices** beyond bilinear on the strip; a
  32-slice LUT (`512x16`) is the cheap upgrade if 16 bands are visible.
- **Cel look is per-pixel only**; there is no edge detect, so it does not draw
  ink lines.

### Occluder punch-through: quality follow-ups
Shipped 2026-11-25 (scene-shader circular discard, CAMERA > SETTINGS
OCCLUSION group, `fx 274`-`278`; see README "Occluder punch-through"). Limits
worth revisiting:

- **Radius is in eye pixels, not a fraction of the eye height.** The 2D/3D
  panel anamorphic stretch is compensated (`eye_width / full_width`), so one
  value looks the same in both, but it is still absolute: another resolution or
  panel would need retuning.
- **Screen-space dither can alias** when the occluder moves fast; it is static
  per pixel by design (no crawl when still). A 2x2 rotated-grid hash would
  soften it at the cost of a more visible pattern.
- **Only the colour scene pass is punched.** The portal relay image and the
  shadow map still see the occluder, so a portal seen through the hole or a
  shadow cast through it is not corrected.
- **PTC per-object ghosting is a separate mechanism.** The wall-hide is
  suppressed when OCCLUSION is on, but `s_ptc_fade` object ghosting still
  layers on top in the cube/wall demos.
- **No per-material opt-out.** Everything nearer than Mario is eligible,
  including translucent geometry and the skybox if it were ever in front.

**Still to confirm on hardware:** the bloom glow and the nozzle light were
measured (frame cost) and light registration is confirmed from the status line
(`glow=...,1` while thrusting), but the visual before/after stills were not
captured. The later investigation showed the "black 3D layer" was the tablet
being locked/asleep (see Known bugs), not the renderer — re-run
`scripts/emissive-glow-test.sh` on an unlocked panel and compare `a_baseline`
vs `f_light_on` / `c_bloom`.

---

### Grapple targeting debug view — shipped 2026-11-25 (worktree `w2-grapple-debug`)
One resolver in `movekit.c` (`grapple_aim_resolve`) now feeds both the attach
and a world-space debug view: targeting fan, resolved surface point + normal,
distance, and a green/amber/red validity colour, plus `grp=` in `cmd dump`
(`fx 310`-`315`, off by default). The axis ray is cast first and wins, so every
pre-existing attach point is unchanged; the fan is only a fallback for misses.
Open follow-ups:

- **The attach ray starts at the camera, not at the gun.** `gLakituState.pos`
  is the origin, so the hook can attach to something to the side of Mario's
  muzzle, and the drawn fan apex is behind his head. The view draws the hand →
  point rope leg next to it, which makes the offset obvious. Moving the origin
  to the muzzle (the portal gun's approach) is a gameplay change and was not
  made.
- **The fan is one ring pair, not a solid cone.** Two rings (½ angle, full
  angle) at `312` samples each; a surface between spokes can still be missed.
- **Range is measured from the camera.** `314` tunes the attach range, so it is
  a gameplay dial, not just a view option.
- **Device verification is still outstanding.** The first pass built cleanly
  (`libmain.so` carries the rig + HUD strings) but the tablet's Wi-Fi adb link
  dropped before the capture batch (`adb devices` empty, host could not route
  to `192.168.1.207`). Run
  `AGENT_TABLET_MUTE=1 AGENT_NO_SLEEP=1 scripts/agent-lock.sh device --timeout 900
  --reason "grapple debug" -- scripts/grapple-debug-test.sh`
  and check `results.tsv` for `a_wall_valid`/`d_floor_valid` = `grp=1,1,...`,
  `b_out_of_range` = `grp=1,2,...`, `c_sky_no_surface` = `grp=1,0,...`. The
  b/d pair is the same pose with `fx 314` changed, so the colour change is
  isolated.

### Reserved fx key ranges (registered 2026-11-25)

Parallel agents must not invent ranges. Claim one here before adding dials, and
keep a feature inside its block:

| range | owner |
| --- | --- |
| `250`-`260` | crepuscular rays (taken) |
| `261`-`273` | emissive / bloom / jetpack light (taken) |
| `274`-`278` | occluder punch-through (taken) |
| `280`-`289` | tone mapping + colour grading |
| `290`-`299` | cam-stick curve + heatmap |
| `300`-`309` | on-screen layout editor + profiles (taken — see README) |
| `310`-`319` | grapple targeting debug view |
| `320`-`329` | wall-run stick vector + cooldown |
| `330`-`339` | camera sensitivity picker + FREE-mode cycler fix (taken: `330` band, `331` trim, `332` effective readback) |
| `340`-`349` | feedback outbox (reserved, likely unused) |
| `350`-`359` | painting ripple presentation-time interpolation (taken) |
| `360`-`369` | camera vs ground COLLIDE/CLIP (taken: `360` mode, `361`-`369` free) |
| `370`-`379` | UI layout box + skybox fine-tune (taken: `370` scale X %, `371` scale Y %, `372` offset X %, `373` offset Y %; `374`-`379` free) |
| `380`-`389` | reduced-resolution C.DOF (taken, see the C.DOF section: `380` path, `381` divisor, `382` near field, `383` taps, `384` upsample, `385` CoC, `386` bokeh, `387` bilinear, `388` stage log, `389` sharpness) |
| `390`-`399` | reduced-resolution MXAO (taken: `390` path, `391` divisor, `392` rotation, `393` upsample, `394` sky skip, `395` tap trig, `396` stage probe; `397`-`399` free) |
| `400`-`409` | reduced-resolution SSGI (taken: `400` divisor, `401` blur, `402` roto jitter, `403` debug view, `404` sky skip, `405` normal reject) |
| `410`-`419` | wall-run 90° corner rig, worktree `w3-corner-test` (taken: `410` nominal jump distance, `411` timing spacing, `412` re-entry deadline ticks, `413` entry height, `414` entry speed, `415` corner-stuck ticks; `416`-`419` free) |

## Known bugs and limits

- **"Black 3D layer" screenshots are usually a locked/asleep tablet.**
  `screencap` on a locked device returns pure black (mean ~1.5/255) with no
  error, and on the lock screen the HUD is visible while the app's surface is
  not. Wake and unlock before capturing
  (`/media/share/lp2-test/scripts/unlock.sh`). If a capture is black, check
  `dumpsys power | grep mWakefulness` before suspecting the renderer.
- **…but waking from screen-off can leave the 3D layer genuinely black**
  (found 2026-11-25, worktree `w2-camera-fixes`). With a healthy scene, sleep
  the panel (`adb shell input keyevent 26`), wait a few seconds and wake it
  (`unlock.sh`): the camera/HUD overlay keeps drawing but the game layer is
  pure black, while `cmd dump` still reports `surfaceValid=true`, a sane
  `cam=`/`focus=` pair and `cmode=0`. `cmd reinit` does **not** restore it;
  `HOME` followed by reopening the task does. This is independent of the camera
  modes and is the most likely source of "the app went black" reports after the
  panel's 30 s timeout. Suspect the Leia/SDL override surface not re-binding on
  resume (the "never hide/show the view" workaround covers activity restarts,
  not sleep); do not "fix" it by restarting the interlacer.
- **The app sometimes "refuses to die" on relaunch.** Dropbox repeatedly logs
  `ProcessRecord{... com.vdavid003.sm64port} <pid> refused to die, but we need to
  launch ...` — the old process does not exit promptly when the activity is
  restarted, so the system has to kill it. Likely a native thread that does not
  observe the shutdown (SDL/Leia), and it is why relaunch cycles can stall.
  Worth a proper look: reproduce with `am start` after `am force-stop`, watch
  `dumpsys dropbox --print` for a fresh entry, and check which thread survives.
- **Wi-Fi adb + device sleep is a deadlock.** The device lock sleeps the tablet
  on release; sleeping drops the Wi-Fi adb link; waking it needs adb. Mitigation
  is documented (AGENTS.md): `AGENT_NO_SLEEP=1` during a multi-agent session and
  sleep only at the end. Never break the loop with `adb reboot`.

- **A failed FX shader link silently falls back to the legacy path.** The SSGI
  perf work found that Adreno's GLES2 compiler rejects the integer `%` operator,
  so the new GI shader never linked and every measurement quietly ran the old
  full-res loop (the log line existed, but nothing surfaced it in the dump).
  worth adding a `fxerr=<program>` field to the status line so a broken shader
  is visible from a script instead of looking like "the optimisation did
  nothing".
- **Motion-vector heatmap renders a flat blue frame.** `fx 31 9` (VIEW VECTORS)
  with `fx 149 2` shows solid blue even with the blur pass on, so the debug view
  itself is broken, not the vectors. It is the only visibly wrong tile in the
  sweep deliverables. See `MOTION-VECTORS.md`.
- **Slow motion cannot interpolate limb animation.** The root transform and the
  camera interpolate (`324fd03`); Mario's pose steps at the sim rate because
  animation is graph-node driven. Effects drawn outside the master display list
  do not register with `hfr_record` and jump per sim step. The camera-only
  fallback blur pulses by design.
- **Denoiser has no temporal reprojection.** `frames` is only a blend weight;
  `bounces` means extra à-trous iterations. See `DENOISER-RATE-LIMIT.md`.
- **HUD, menus and transitions step at 30 Hz**, and the skybox and some particles
  are not interpolated (also listed in the README roadmap). Rippling paintings
  were on this list; they now interpolate at the present rate (`fx 350`), so the
  remaining steppy cases are the HUD/menus, the skybox and some particles.
- **Portal phase 3** (through-portal camera relay, Mario clone in the exit
  portal) is not implemented; portals are decal + instantaneous teleport.

---

## Environment / tooling facts that cost time to rediscover

- **The 2026-11-26 outage was a battery shutdown, not an agent.**
  `getprop sys.boot.reason` reported `shutdown,battery` after the tablet came
  back. The device lock now records the boot id, boot reason and battery level
  on every hold, so a real reboot (`reboot,shell` from an `adb reboot`) is
  attributed to the previous holder, and an `adb` shim refuses power verbs
  inside a device hold (exit 97, logged as `power_blocked`).

- **Adreno's GLES2 compiler rejects integer `%` in a fragment shader**
  (`ERROR: 0:126: '%' : supported in pack/unpack shaders only`, 2026-11-26).
  Integer `/` happens to be accepted. The dangerous part is not the error but
  the silence: a failed optional chain just falls back to its legacy path, so
  every dial keeps reading back correctly while nothing changes. Spell 2-D
  offsets out as comparisons, and make any measurement script abort if the
  program did not build.
- **The idle chase camera is not static.** Two screencaps 2 s apart on an idle
  Mario shift by ~1 px, which is a mean |diff| of ~20/255 on a high-contrast
  scene — more than the effect being compared. Take still rows in one settle
  window, verify `cam=` (and `focus=`) are unchanged, and turn head
  tracking/gyro off (`cmd set --ei k 41 --ei v 0`, `42 0`) for stills.
- **`osGetTime()` is stubbed to `0` in this port.** Anything time-based must use
  `gfx_wall_time_sec()` (`gfx_sdl2.c`, SDL_GetTicks). The scripted camera orbits
  were frozen at phase 0 for weeks because of this (`c626ce1`).
- **`rate=` in `cmd dump` is not a frame time.** It is a main-loop EMA that
  counts variable composited frames; use `fps=` (`1000/fps` = ms). The old
  "105–118 ms" denoiser numbers were this mistake.
- **The upsample must be its own GL pass.** Doing it inside the big FX shader,
  or adding a sampler to it, cost ~30 ms. Worth remembering for any new
  half-res effect.
- **This VM is behind QEMU user-mode NAT** (`10.0.2.15`, gateway `10.0.2.2`).
  The host Mac cannot open connections into the guest without a `hostfwd` at the
  hypervisor level, so a server in the VM is not reachable from the Mac. Use the
  9p share (`/media/share` ↔ `/mnt/macos`) or a reverse SSH tunnel.
- **A fresh worktree has no generated asset tree.** `app/jni/src/build/us_pc`
  (textures as `*.rgba16.inc.c`, skybox/animation/demo C, sound banks, ~134 MB,
  git-ignored) is produced by the asset build, not checked in. Without it
  ndk-build fails on the first missing `actors/.../*.inc.c`. Copy it from a
  built checkout (or regenerate textures from a ROM) before the first build.
- **Where results live:** reproducible artifacts in `test-artifacts/` (committed
  or regenerated by script); published clips/sheets in
  `/media/share/tmp/artifacts/sm64/`; durable audio replies in
  `/media/share/tmp/audio/sm64/`. Never leave a referenced file in `/tmp`.
- **Device and build work go through `scripts/agent-lock.sh`** (see `AGENTS.md`),
  with `AGENT_TABLET_MUTE=1`. The device lock marks the app BUSY and sleeps the
  tablet on release.
- **Audio reply preferences live in `.dev/audiomode.jsonc`, which is
  git-ignored.** On a fresh clone there is no audio mode until the user asks.
  The spoken preamble (`"<session title>, here: <msg>"`) comes from that file
  and from `audioq.py` in `/media/share/opencode-audio-tool`.

---

## Deferred on purpose

- Slow-mo limb/pose interpolation (low payoff for the work).
- Temporal denoiser reprojection (bigger project; 1/4-res default is at budget).
- Per-eye 6DoF rig (tracker z axis still unvalidated on hardware).
- Independent voice/gender per project for audio (the preamble identifies the
  speaker instead; revisit only if the preamble proves insufficient).
