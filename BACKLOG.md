# Backlog — open work and things worth not forgetting

Last reviewed: **2026-11-25**. This is the durable list for work that is
planned, half-finished, or easy to lose when a chat is gone. Detail belongs in
the linked docs; this file is the index.

Update it in the same commit as the change that makes a line stale. A thread
that is finished gets deleted here, not ticked.

---

## In progress

Nothing active.

### Bowser scale pulses under HFR (held/gripped objects)
Reported 2026-11-25, investigating in worktree `bowser-scale`.

On the intro sequence, Bowser's model grows/shrinks frame to frame while Mario
spins him by the tail. Hypothesis: HFR interpolation above 30 Hz, and probably
any time Mario grips or holds an object (`geo_process_held_object`,
`gCurrentGraphNodeHeldObject`). Candidates to prove or dismiss with telemetry:
element-wise lerp of matrices that carry rotation x non-uniform scale; the held
object's previous matrix coming from a different parent chain; `hfr_prev_valid`
gaps making `prev := cur` intermittently; a genuine double application of
`gfx.scale`. Telemetry: `SM64HFR` per-present lines plus a windowed scale
summary (`ptc hfrdump`), parsed by `scripts/hfr-scale-check.py`.

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

### Merge queue for the parallel worktrees
Rays landed in the main checkout (`6c55b1e`). Three worktrees are in flight and
all of them touch `gfx_opengl.c` and the Java panel, so merge order matters:

1. `emissive-bloom` — light registry, emissive batches + buffer, bloom, GLOW tab
2. `occlusion-fade` — discard-based punch-through in the generated scene shader,
   OCCLUSION panel group
3. `bowser-scale` — HFR/held-object analysis (smallest diff; mostly
   `rendering_graph_node.c`, which `emissive-bloom` also edits)

Conflict hotspots to expect: the `s_fx_u` uniform table and `stereo_fx_*` pass
wiring, the FX page list in `sm64portActivity.java` (page order/index bugs —
the RAYS page had to be appended last for exactly this reason), the README FX
key table, and this file. Resolve by keeping each feature's block contiguous,
then rebuild and re-run the device checks for all four features together.

---

## Planned, not started

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

### Emissive particles + bloom — **implemented** (2026-11-25)
Emissive draw-batch tag (`GFX_OP_EMISSIVE_MARKER`, display-list marker), a
per-eye half-res emissive buffer, a threshold/blur/additive-upsample bloom chain
that reads only that buffer, and a per-frame world-space light registry (jetpack
nozzle + nearby flames). Dial it from the FX > LIGHT > **GLOW** tab (`fx`
261-273); debug views `268 1` (emissive buffer) and `268 2` (bloom buffer).
Bloom is off by default. See `README-lumepad-2-fork.md` for the key table and
measured cost. The god-ray pass below should now shaft from this buffer.

**Still to confirm on hardware:** the bloom glow and the nozzle light were
measured (frame cost) and light registration is confirmed from the status line
(`glow=...,1` while thrusting), but the visual before/after stills were not
captured. The later investigation showed the "black 3D layer" was the tablet
being locked/asleep (see Known bugs), not the renderer — re-run
`scripts/emissive-glow-test.sh` on an unlocked panel and compare `a_baseline`
vs `f_light_on` / `c_bloom`.

---

## Known bugs and limits

- **"Black 3D layer" screenshots are usually a locked/asleep tablet.**
  `screencap` on a locked device returns pure black (mean ~1.5/255) with no
  error, and on the lock screen the HUD is visible while the app's surface is
  not. Wake and unlock before capturing
  (`/media/share/lp2-test/scripts/unlock.sh`). If a capture is black, check
  `dumpsys power | grep mWakefulness` before suspecting the renderer.
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
- **HUD, menus and transitions step at 30 Hz**, and the skybox, some particles
  and paintings are not interpolated (also listed in the README roadmap).
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
