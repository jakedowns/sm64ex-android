# Backlog — open work and things worth not forgetting

Last reviewed: **2026-11-25**. This is the durable list for work that is
planned, half-finished, or easy to lose when a chat is gone. Detail belongs in
the linked docs; this file is the index.

Update it in the same commit as the change that makes a line stale. A thread
that is finished gets deleted here, not ticked.

---

## In progress

### Crepuscular rays (god rays)
Started 2026-11-25. No code yet; design stage.

- **Approach chosen:** half-resolution screen-space radial light shafts,
  occlusion sampled from the existing shadow map, composited additively.
  Rejected: raymarched world-space shafts (cost on Adreno 660) and shaft
  geometry (level authoring, breaks with moving lights).
- **Infrastructure that already exists and should be reused, not rebuilt:**
  - shadow map: `uShadowMap`, `uShadowVP`, `unpackDepth()` in the FX shader,
    `shadowLookup()` for the 3x3 soft path (`gfx_opengl.c`);
  - four dynamic lights: `uLightPos/uLightPosView/uLightColor/uLightRadius`,
    `fx 103` light count — note only the PTC lightshow drives these today;
  - the half-res prepass + bilateral upsample pattern from the denoiser
    (`stereo_denoise_prepass()`, `s_dn_*`) is the template for a ray buffer;
  - the FX tab system and the A/B MEASURE tool for proving cost.
- **Plan:** probe one half-res radial-blur pass and measure it before building
  the UI; then uniforms for intensity, density, decay, weight, samples,
  threshold, source light and jitter (`fx 250+`); then a RAYS tab under FX;
  then a debug view.
- **Open question:** shafts from the sun/one of the 4 lights, or from bright
  *objects* (star, 1-up, jetpack) — the latter needs the emissive work below.

---

## Planned, not started

### Emissive particles + bloom
Investigated 2026-11-25 at the user's question "can we make jetpack particles
emissive?" — **nothing was implemented**.

State of the renderer:
- **No bloom pass and no emissive concept** (`grep -i 'bloom\|emissive'` finds
  nothing in `gfx_opengl.c`).
- The jetpack nozzle "flame" is five camera-facing quads with vertex colours,
  rebuilt per tick (`app/jni/src/src/game/movekit.c`, "Nozzle flame" ~line 764).
  It emits no light.
- Four dynamic lights exist in the FX shader but **no gameplay object registers
  one**; only `ptc.c` lightshow uses them.

Candidates already in the tree (behaviours directory):
`sparkle_spawn.inc.c` (1-up, coin sparkles), `sparkle_spawn_star.inc.c` (star
sparkle), `water_mist.inc.c`, `flame.inc.c`, `lll_volcano_flames.inc.c`,
`lll_rotating_hex_flame.inc.c`, plus the movekit jetpack cone.

Three tiers:
1. **Emissive tag** — a per-batch emissive strength plus a small glow list so
   chosen draw batches write into a half-res emissive buffer instead of the
   plain colour path.
2. **Bloom** — threshold, downsample, blur, additive upsample, using the
   denoiser's prepass pattern. Budget: ~1–2 ms at 1/2 res; measure with
   A/B MEASURE before shipping.
3. **Feed the rays** — the god-ray pass should shaft from the same bright
   sources, which is why these two threads are adjacent.

Cheap taste (minutes, no bloom): register the nozzle as one of the four lights
so the floor around Mario lights up and the light path is proven on a gameplay
object.

---

## Known bugs and limits

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
