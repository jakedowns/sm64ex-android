# Backlog — open work and things worth not forgetting

Last reviewed: **2026-11-25**. This is the durable list for work that is
planned, half-finished, or easy to lose when a chat is gone. Detail belongs in
the linked docs; this file is the index.

Update it in the same commit as the change that makes a line stale. A thread
that is finished gets deleted here, not ticked.

---

## In progress

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
panel output), so in-app HUD/panel chrome is not in it, and only the newest
frame at the tap is captured (no history).

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

### Reserved fx key ranges (registered 2026-11-25)Parallel agents must not invent ranges. Claim one here before adding dials, and
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
