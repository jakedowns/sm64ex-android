# Plan: Cool Additions

**A skateboard, a Mario Kart, a grapple gun, wall-running, a fuel-limited jetpack, destructible levels, a Dreams-style splat renderer, and P2P multiplayer — feasibility, design, and implementation plan.**

Status: **planning only**. No code changes are described as done. Everything below is grounded in this checkout (`/home/ubuntu/opencode/android-sm64`, an sm64ex-android fork for Lume Pad 2) and in public research done on 2026-11-18.

Working title was `plan-skate-n-kart`; this file is `plan-cool-additions.md` per the rename.

---

## 0. TL;DR

| Feature | Verdict | Main cost | Main risk |
|---|---|---|---|
| Skateboard pickup + Skate-style riding | **Feasible** | Trick input + board physics + grind detection | Animation/feel; SM64 has no rail system |
| MK8-style kart (drift, glider, submarine) | **Feasible** | Vehicle physics + camera + water/air transitions | SM64 levels are not tracks; tuning at SM64 scale |
| Grapple gun | **Feasible, good first win** | Rope constraint + raycast + aim camera | Rope/collision edge cases |
| Titanfall-style wall-run (default on) | **Feasible, good first win** | Wall detection + tangent motion + camera roll | Sticky/cheaty feel; stereo camera roll |
| Jetpack (fuel + capacity/refill upgrades) | **Feasible, low risk** | Fuel model, HUD, upgrade persistence | Trivializes platforming; needs careful tuning |
| Destructible environments + rocket launcher | **Possible, but phased** | Static collision removal + visual hiding | Softlocks and level geometry are static |
| Dreams/BubbleBath-style splat mode | **Feasible as a research feature; prototype first** | Splat generation (albedo) + GLES2 renderer + culling | SM64 collision has **no UVs**; stereo cost doubles |
| P2P WebRTC multiplayer | **Feasible, largest effort** | Netcode, signaling, NAT traversal, object sync | Determinism, NAT/firewalls, scope creep |

**Important research result:** there is **no public decompilation** of Skate 3, Mario Kart 8, or Titanfall 2 that can be ported from. The popular Skate 3 project is a *static recompilation* (`mchughalex/skate3recomp`, ~1.8k stars); MK8 has several recompilations, not decomps. Those projects contain (or link against) the original game's translated code and are not a source of reusable constants. This plan therefore takes **behavior and documented mechanics** from those games, then designs native SM64 implementations. Do not copy recompiled game code into this repo.

**Also important:** the splat renderer's "surface albedo" cannot come from SM64 collision data: `struct Surface` stores vertices, normal and surface type, but **no UVs and no texture ID** (`include/types.h:241`). Albedo must be sampled from the rendered frame (G-buffer), or baked from level display lists/textures at build time.

---

## 1. Scope, constraints, assumptions

### 1.1 Target

- This repo: sm64ex-based Android port, `arm64-v8a`, minSdk 30, built with `ndk-build` (`app/jni/src/Android.mk`).
- Renderer: OpenGL ES 2.0 path (`-DUSE_GLES`, `#version 100` shaders — `app/jni/src/src/pc/gfx/gfx_opengl.c:255`), with custom per-eye FBO/Leia stereo code in `gfx_pc.c` / `gfx_opengl.c`.
- Simulation: SM64 runs at **30 Hz**; 1 game unit ≈ 1 cm (see the comment in `gfx_pc.c`); Mario is ~160 units tall. All constants below are in that scale.
- Controllers: physical gamepad + keyboard + touch overlay. L trigger is effectively free in normal play (only debug/profiler/cheats read it), so it becomes the generic **Use item** button.
- Camera: SM64 camera with `gLakituState.roll` support (`src/game/camera.h:624`) — good for wall-run tilt and drift lean.

### 1.2 Non-goals for the first pass

- No Nintendo assets, models, sounds, or code from Skate 3 / MK8 / TF2 / Dreams.
- No full rigid-body engine. Use SM64's action/step model and hand-tuned arcade physics.
- No real race tracks. Karts are free-roam toys inside existing levels.
- No true 3D Gaussian Splatting. We do surface-aligned 2.5D splats with optional view-dependent shading.
- No dedicated servers for multiplayer. P2P with a tiny signaling service only.

### 1.3 Design rules

1. **Additive, toggleable.** Every feature is behind a config flag and can be disabled without breaking vanilla movement.
2. **Reuse SM64 idioms.** New Mario actions, new behaviors in `src/game/behaviors/`, new entries in `data/behavior_data.c`, `cur_obj_*` helpers, `find_surface_on_ray`, `perform_ground_step`/`perform_air_step`.
3. **No ROM-derived content in the APK.** New art/sounds are authored; ROM assets stay in a user-generated `base.zip` (section 3.1).
4. **Budget for stereo.** Anything that costs 1× in 2D costs ~2× in the Leia mode. Splats especially.
5. **Ship in slices.** Each feature must be playable alone before the next is merged.

---

## 2. Codebase map (what we build on)

| Area | Files | Why it matters |
|---|---|---|
| Mario state | `include/types.h` (`MarioState` at :298, `Object` at :161, `Surface` at :241) | `pos`, `vel`, `forwardVel`, `wall`, `floor`, `ceil`, `riddenObj`, `action`, `actionState`, `actionTimer`, `actionArg` |
| Actions | `include/sm64.h` (`ACT_*`, `ACT_FLAG_*` at :148), `src/game/mario.c:987` (`set_mario_action`), `src/game/mario.c:1776` dispatch, `src/game/mario_actions_*.c` | New `ACT_*` values + switch cases in the matching execute function |
| Existing ride pattern | `src/game/mario_actions_moving.c:1214` (`act_riding_shell_ground`) | Exactly the shape a kart/skate action should take: input → `perform_ground_step()` → `tilt_body_*` → sound |
| Collision | `src/engine/surface_collision.h` / `.c`; `src/engine/surface_load.c:31` (`sSurfacePool`) | `find_floor`, `find_ceil`, `find_wall_collisions`, `f32_find_wall_collision`, **`find_surface_on_ray`** (already exists) |
| Surface pool | `src/engine/surface_load.c` | Contiguous pool → O(1) per-surface state array for destruction |
| Behaviors | `src/game/behavior_actions.c`, `src/game/behaviors/*.inc.c`, `data/behavior_data.c`, `include/behavior_data.h` | New pickups/projectiles/panels/rails; script tables use `BEGIN`, `CALL_NATIVE`, `LOAD_COLLISION_DATA`, etc. |
| Interaction | `src/game/interaction.c`, `include/interaction.h`, `obj_set_hitbox` | Pickups and damage |
| Camera | `src/game/camera.c/h` (`gLakituState.roll` :624) | Chase cam, aim cam, roll, FOV |
| Input | `include/types.h` `Controller`, `src/pc/controller/*` (SDL, keyboard, touchscreen) | Contextual remapping + touch buttons/sticks |
| Config/debug | `src/pc/configfile.c/h`, `src/game/debug.c` | Feature flags, tuning sliders, debug overlays |
| Renderer | `src/pc/gfx/gfx_pc.c` (stereo at :198, `gfx_run` :2102), `gfx_opengl.c` (GLES2 shaders :255) | Splat pass, Leia per-eye integration |
| Build | `app/jni/src/Android.mk` (`EXTERNAL_DATA` at :152), `Makefile` (:695), `src/pc/fs/fs.c:83`, `src/pc/pc_main.c:214` | Runtime assets + first-run extraction |

**Existing external-data path (very relevant):** `EXTERNAL_DATA=1` already switches textures to PNGs loaded through `fs_*` (`gfx_pc.c:667`), sound to files (`src/audio/load.c:874`), and packs them into `res/base.zip`; `fs_init()` mounts a directory or zip. `pc_main.c:214` even moves a dropped `base.zip` into `res/`. This is the hook the BYO-ROM plan uses.

---

## 3. Research findings

### 3.1 Bring-your-own-ROM and on-device extraction (Android)

#### What the linked Conker project does

`agrofubris/conker-3d-stereo` is an **N64Recomp static recompilation**, not a decompilation.

- The maintainer builds the game **with a ROM**; CI fetches it from a private repo via `ROM_REPO` / `ROM_REPO_TOKEN` (`.github/workflows/release.yml`), recompiles the ROM's code, and ships a 22 MB Windows zip containing `ConkerRecomp.exe` + SDL/DXC + menu assets. **No ROM, no game assets.**
- At first run the launcher's **Load ROM** opens a native file dialog (NFD), validates the image, byte-swaps `.n64`/`.v64` to `.z64` if needed, and copies it to the app data folder as `<game_id>.z64` (`N64ModernRuntime/librecomp/src/recomp.cpp:360` `select_rom`). Later runs find the stored copy (`load_stored_rom`).
- **There is no asset extraction.** The runtime reads the ROM live through PI DMA (`recomp::do_rom_read`); the ROM is the game's data source every run. “Remembered” means “stored”, not “converted”.
- ROM hacks that change only assets are accepted by hashing only the code range (`host/src/rom_versions.cpp`).

**Takeaway:** Conker's model works because a recompiler ships recompiled code and streams the ROM's data at runtime. A decomp port (SM64) cannot do that without re-architecting its asset system, because the decomp replaces N64 ROM DMA with extracted host assets.

#### What an SM64 Android port that does exactly your desired UX does

HarbourMasters/**Ghostship** (a libultraship-based SM64 port) ships an APK that is built **without any ROM or ROM secret** and does on-device extraction:

- `android/.../LauncherActivity.kt` is the `MAIN`/`LAUNCHER` activity, in its **own process** (`android:process=":launcher"`), uses `ActivityResultContracts.OpenDocument()` for the picker, verifies SHA-1 (`9bef1128717f958171a4afac3ed78ee2bb4e86ce` US, `8a20a5c83d6ceb0f0506cfc9fa20d8f438cafe51` JP), writes the ROM atomically to the app's external files dir, keeps the screen awake, then calls a JNI entry point.
- `src/port/android/AndroidBridge.cpp` runs **Torch** (libultraship's extractor) over the ROM and writes `sm64.o2r`. `GameAssets.kt` stores it; `LauncherActivity` starts the game if `sm64.o2r` exists, so **later launches never touch the ROM again**.
- `ghostship.o2r` (engine assets) + Torch recipes + TCC runtime are bundled in a zip (`gamedata.zip`); no ROM-derived data ships. `android:allowBackup="false"`.
- Their CI builds the APK with no ROM: `main.yml` `build-android` needs only the generated engine `.o2r`.

**Takeaway:** the exact flow you described is proven on Android for SM64. The difference is that Ghostship uses libultraship + Torch; this repo uses sm64ex's Makefile asset pipeline.

#### What this repo already has

- `EXTERNAL_DATA=1` in both build systems (`app/jni/src/Android.mk:152`, `Makefile:695`).
- A VFS that mounts `res/base.zip` (`src/pc/fs/fs.c:83`), PNG texture loading (`gfx_pc.c:667`), and sound loading from files (`src/audio/load.c:874`).
- Build-time extraction via `app/jni/src/extract_assets.py` + `tools/` (`n64graphics`, `skyconv`, `mio0`, `aifc_decode`, `disassemble_sound.py`, `assemble_sound.py`), producing PNGs, `.aiff`/`.aifc` samples, and packed sound tables.
- Current APK bakes assets into `libmain.so` (the pre-purge APK's `libmain.so` is ~15 MB). The source tree is now mostly purged, **but** 14 ROM-derived skybox PNGs are still tracked (`app/jni/src/textures/skyboxes/*.png`), and `assets/anims/*.inc.c` (upstream decomp data) plus extracted `assets/demos/*.bin` are compiled in.

#### Recommended distribution architecture for this project

**Mode A — personal full build (today):** assets extracted into the tree, compiled into `libmain.so`. Fine for your own device; cannot be distributed.

**Mode B — distributable shell:**
1. Build with `EXTERNAL_DATA=1`, never run `extract_assets.py`, and keep the tree free of extracted PNGs/samples. The APK then contains engine code + authored content only.
2. On first run, a `LauncherActivity` (own process, no storage permissions) takes the ROM via `OpenDocument`, verifies SHA-1, copies it to app-private storage atomically, and runs an **on-device extractor** that writes `res/base.zip` (textures, skyboxes, sound tables/samples) into the external files dir (`Android/data/<pkg>/files/res/base.zip`, where `pc_main.c` expects it).
3. Subsequent runs detect `base.zip` and skip the picker. A “Regenerate assets” action in Settings deletes/rebuilds it.
4. `android:allowBackup="false"` so extracted assets and the ROM never go to cloud backup.

**Extractor implementation options:**

| Option | Pros | Cons |
|---|---|---|
| **A. Port the pipeline to C/C++/Kotlin** | No runtime dependency; fast; small APK | Biggest porting job; sound is the hard part |
| **B. Embed CPython (Chaquopy) + cross-compiled C tools** | Reuses `extract_assets.py`, `disassemble_sound.py`, `assemble_sound.py` nearly verbatim; fastest to working prototype | Adds ~10–25 MB/ABI; Gradle/NDK packaging of CLI tools; license check (Chaquopy is free for open source) |
| **C. Adopt libultraship/Torch** | Proven on-device SM64 extraction (Ghostship) | Means porting this fork onto libultraship; not realistic for the Lume 2 fork |

**Recommendation:** prototype C (or B for speed) after the runtime path works. The textures are straightforward (`n64graphics -e`, `skyconv`); the **sound pipeline** (`disassemble_sound.py` → `.aiff` → AIFC encode → `assemble_sound.py` → ctl/tbl/sequences/bank_sets) is the real work. `assets.json` (2.8 MB of ROM offsets/metadata) is not game data and can ship.

**Legal/practical notes:**
- Never commit the ROM, extracted assets, or `base.zip`. Add a CI check that fails if `actors/**/*.png`, `levels/**/*.png`, `textures/**/*.png`, `sound/**/*.aiff`, or `assets/demos/*.bin` exist at build time.
- Remove the 14 tracked skybox PNGs before calling a release “no game data”; generate them on device.
- Distribution will be sideload/GitHub Releases, not Play Store (IP policy).
- Extraction needs ~2× the asset size free space; use a `.part` file + rename, like Ghostship.

**Consequences for this plan:** all new art (board, kart, hook, rocket, HUD, splat brushes) is authored and can ship in the APK. Splat caches derived from ROM textures must be generated after `base.zip` (or baked at build time only for personal builds).

---

### 3.2 Skate 3: what actually exists

- The popular project is **`mchughalex/skate3recomp`** (~1.8k stars): a static recompilation using XenonRecomp (PowerPC → C), shipping Windows/Linux/macOS builds; it **needs the retail game**. A “Skate 3 Mobile” recomp also exists.
- `Skate-Modding-Team/Skate-Decompilation-Project` exists but is early (≈11 stars, no usable game code).
- `SK8-ENGINE/skate-3-rust-engine` is a Rust/Bevy **rewrite** from reverse-engineering research, not a decomp.
- Therefore: **no portable decomp source or constants.** Use the documented **Flick-It** control scheme and a hand-built arcade model.

**Flick-It (behavior reference):** left stick steers; right stick does tricks by flick direction (down→up = ollie, diagonals = kickflip/heelflip, sideways = shuvit); hold a trigger + flick = grab; hold right stick slightly up/down = manual/nose manual with a balance meter; flick near a rail = grind; RT + left/right on landing = revert; push with A/X; brake with B; combos chain through manuals/reverts. Button labels vary by platform.

**Physics to reproduce (arcade, not simulation):**
- Board heading vs. velocity (drift angle), lean/roll, ground-normal alignment.
- Push cadence adds impulses; rolling friction; slope acceleration.
- Carving converts lean into lateral acceleration; powerslide lowers grip.
- Ollie height from charge; landing tolerance by board angle; bail on bad landing.
- Grind: snap to rail, balance meter, speed retention, exit pop.
- Manual/revert: extend combos, balance mini-game, score multiplier.

**What we will *not* do:** copy recompiled code, EA assets, or exact hidden constants. The model below is original and tunable.

---

### 3.3 Mario Kart 8: what actually exists, and the mechanics worth copying

- **No public MK8 decompilation.** Projects are recompilations/ports: `Vitor-Vaz/MK8DRecomp`, `dougchansan/mk8-recomp`, `c22dev/mush8`, `HelixNX/mk8dx`, etc. They demonstrate underwater + anti-gravity running but are not reusable source.
- Documented mechanics we can implement faithfully:
  - **Drift:** hold drift + steer into a turn → hop, break traction, nose points inward. Charge builds faster with sharper steering. In MK8 at 60 fps the charge counter gains **+5/frame when stick >45° from vertical, +2/frame otherwise**; thresholds at MT stat 1.0 are **280 / 590 / 900** → blue (Mini-Turbo), orange (Super), purple (Ultra). Releasing restores traction and grants a boost; higher tiers last longer.
  - **Glider:** deployed on ramps/air; changes air speed/handling; retracts on landing or water.
  - **Underwater:** propeller pops out; speed/handling/drift stats change; glider cancels on water entry; screen gets a wet effect.
  - **Anti-gravity:** optional stretch goal; changes handling and enables ceiling driving.
  - **Coins:** speed boost in MK8; SM64's existing coins can serve.

**“Submarine mode”:** MK8's underwater mode is seabed driving with a propeller, not a free-swimming submarine. This plan implements **(a)** MK8-style propeller/underwater handling in shallow water, and **(b)** an optional true **Submarine mode** in deep water (3-axis control, buoyancy, propeller thrust). That satisfies “submarine” without pretending MK8 has one.

---

### 3.4 Titanfall 2 wall-run: documented behavior and an implementable model

No public TF2 decomp either. Community measurement/documentation gives the shape:

- Wall-run lasts about **1.75 s**, then the game detaches you.
- Speed on a wall **caps around 28–30 km/h** (TF2 scale); a wall jump at max wall-run speed adds roughly **10–11 km/h**.
- You must hold forward; camera tilts; same-wall regrab is only allowed if you touch **lower** than the previous contact, or the wall curves by **≥~25°**.
- Momentum/“wall jump arc” and coyote-time edge boosts are emergent from the collision + air-strafe model.
- Momentum Mod exposes analogous cvars (`sv_wallrun_accel`, `sv_wallrun_boost`, `sv_wallrun_corner_stick_angle`, anticipation/eye-roll) — a good model for our tunables.

**SM64 mapping (initial values):** 1.75 s duration, entry speed > 30 units/frame, wall-run cap 55 units/frame, wall-jump boost 1.12×, camera roll 10–14°, regrab rule copied conceptually, forward input required, 0.25 s cooldown per wall identity.

---

### 3.5 Dreams' BubbleBath: what it is and what is realistic on GLES2

- Media Molecule's Dreams runs on the **BubbleBath** engine (name chosen by the community in 2015). It renders worlds with **“Flecks”** — camera-facing splat/particle primitives rather than conventional polygons — producing the painterly, impressionistic look.
- A full BubbleBath clone is out of scope. What we can reproduce:
  - a **surface splat** rendering mode: many small, camera-facing, surface-aligned billboards with soft/elliptical footprints;
  - colors sampled from the scene's albedo (runtime G-buffer or build-time bake);
  - density/size/rotation jitter, painterly accumulation;
  - view-dependent shading for “special” materials (metal/water/glass) to evoke the view-dependent color of 3D Gaussian Splatting;
  - smart culling (clusters, frustum, distance LOD, budget allocator).

**Key technical constraint found in this repo:** `struct Surface` has no UV/texture reference, so collision geometry cannot supply albedo. Options are covered in section 5.6.

---

## 4. Shared architecture

### 4.1 Power-up / item framework

Add a small, generic item layer so skateboard, kart, grapple gun, rocket launcher, and jetpack share pickup, HUD, input, and persistence code.

```c
// src/game/powerups.h
enum PowerUpKind {
    POWERUP_NONE,
    POWERUP_SKATEBOARD,
    POWERUP_KART,
    POWERUP_GRAPPLE,
    POWERUP_ROCKET,
    POWERUP_JETPACK,
};

enum PowerUpSlot { SLOT_ITEM = 0, SLOT_COUNT };

struct PowerUpState {
    u8  kind;          // PowerUpKind
    u8  ammo;          // rockets / grapple charges (0 = unlimited for vehicles)
    u8  flags;         // held, aiming, active
    u16 cooldown;      // frames
    f32 fuel;          // jetpack: current fuel
    f32 fuelMax;       // jetpack: capacity after upgrades
    f32 refillRate;    // jetpack: fuel units per second
    u8  upgradeLevel;  // jetpack: 0..JETPACK_MAX_UPGRADES
};

extern struct PowerUpState gPowerUps[SLOT_COUNT];

void powerup_give(enum PowerUpKind kind);
void powerup_use(struct MarioState *m);     // L trigger
void powerup_update(struct MarioState *m);  // per frame
void powerup_render_hud(void);              // authored icons/sprites
```

- **Pickups** are behaviors: `bhvSkateboardPickup`, `bhvKartPickup`, `bhvGrapplePickup`, `bhvRocketPickup`. Use a shared `bhvPowerUpPickup` with `oBehParams2ndByte = kind`, a rotating authored model, and an interaction type (`INTERACT_POWERUP`) handled in `interaction.c`.
- **Use input:** `L_TRIG` tap = use/fire; hold = aim (grapple/rocket); vehicles mount on pickup, dismount with L or a dedicated touch button. Keep `R_TRIG` for the existing camera toggle.
- **HUD:** authored icon + ammo/charge bar + contextual prompts. Touch overlay gets one “Use” button and a second “Dismount” button.
- **Persistence:** config only (which features are enabled). No new save-file fields in v1, to avoid save compatibility work.

### 4.2 Input layer

Contextual mapping is essential; SM64 already has too few buttons. Suggested mapping:

| Context | A | B | Z | R | L | Right stick / C |
|---|---|---|---|---|---|---|
| On foot | jump | punch | crouch/ground pound | camera | **use item** | camera |
| Skateboard | push | brake | crouch/ollie modifier | grab/revert | dismount | **flick tricks** |
| Kart | accelerate | brake/reverse | look back | **drift/hop** | item | camera |
| Grapple aiming | jump/detach | cancel | — | camera | **fire/reel** | aim (camera) |
| Rocket aiming | jump | cancel | — | camera | **fire** | aim |
| Wall-run | jump (wall jump) | — | slide? | camera | item | steer |
| Jetpack | jump | drop | — | camera | **thrust** | steer |

- Add `configKeyUseItem` (default `L`), `configKeyDismount`, and per-context overrides to `configfile.c`.
- Touch: add a right-hand virtual stick for tricks/aim, plus Use/Dismount buttons; keep existing layout configurable (`controller_touchscreen_layouts.inc`).
- Gesture recognizer for skate: quantize right-stick samples into 8 directions + flick speed + hold time; emit `FLICK_UP`, `FLICK_DOWN`, `FLICK_LEFT`, `FLICK_RIGHT`, diagonals, `HOLD_UP`, `HOLD_DOWN`, `CIRCLE_CW/CCW` with a 150–250 ms window.

### 4.3 Actions, steps, and physics helpers

- New action IDs in `include/sm64.h` with appropriate `ACT_FLAG_*` bits (e.g. skate/kart = `ACT_FLAG_MOVING`, wall-run = `ACT_FLAG_AIR`, grapple = `ACT_FLAG_AIR | ACT_FLAG_ATTACKING`).
- Dispatch: add `case ACT_...` to `mario_execute_moving_action` / `_airborne_action` / `_submerged_action` / `_automatic_action` (see `src/game/mario.c:1776`).
- Shared helpers in `src/game/vehicle.c`:
  - `vehicle_ground_step()` — like `perform_ground_step()` but with configurable friction, slope accel, and step-up assist.
  - `vehicle_air_step()` — gravity/drag, ceiling/wall checks.
  - `vehicle_surface_align()` — slerp body angle to floor normal.
  - `vehicle_speed_ratio()`, `vehicle_play_engine_sound()`.
- Collision queries: use `find_floor`, `find_wall_collisions`, `f32_find_wall_collision`, `find_surface_on_ray`; never write new raycast code unless object-vs-ray is needed.
- RNG: use `random_u16()` / `random_float()` for VFX only; gameplay determinism is not required for single-player.

### 4.4 Camera and stereo

- Add camera modes: `CAM_MODE_CHASE` (kart/skate), `CAM_MODE_AIM` (grapple/rocket), `CAM_MODE_WALLRUN` (temporary additive offsets).
- Use `gLakituState.roll` for lean/roll, `gLakituState.yaw` for chase, and the existing FOV shake system for speed boosts.
- Stereo: all gameplay cameras already render twice through the Leia path; keep new effects **view-dependent but per-eye consistent**:
  - camera roll is fine (both eyes share it);
  - aim reticle at screen center needs a depth-aware reticle (place it at the ray hit) or it will read as a flat overlay;
  - splats must be projected per eye (section 5.6).
- Add `CAM_FLAG_NO_COLLISION` / soft-follow for vehicles in tight SM64 corridors.

### 4.5 Assets and BYO-ROM

- Authored assets (models, textures, sounds, HUD) live in the repo/APK and are license-clean.
- ROM-derived assets live only in user-generated `base.zip` (section 3.1).
- If `EXTERNAL_DATA=1`, new authored assets can be compiled in as usual or packed into a separate `mods.zip`; avoid mixing them into `base.zip` so regenerating ROM assets doesn't delete them.

### 4.6 Config and debug

Add to `configfile.c`:
- `wallrun_enabled` (default 1), `wallrun_strength`, `wallrun_camera_roll`;
- `skate_enabled`, `kart_enabled`, `grapple_enabled`, `rocket_enabled`;
- `splat_mode` (0 off, 1 screen-space, 2 world-space), plus splat sliders (section 5.6);
- `net_enabled`, `net_lobby_code`, `net_nickname`.

Debug overlay (`src/game/debug.c` or a new `debug_extras.c`):
- spawn each pickup with a key;
- show action/state, speed, wall normal, splat count, budget, net RTT;
- toggles for invincibility/ammo/AI.

---

## 5. Feature plans

### 5.1 Skateboard pickup (Skate 3-inspired)

**Goal:** touch a board → Mario rides it with momentum, carving, pushing, ollies, flick tricks, grinds, manuals, reverts, bails, and a combo score. Dismount returns to normal Mario.

**States:**

```
ACT_SKATE_IDLE     // standing on board, no/little speed
ACT_SKATE_PUSH     // push animation/impulse cycle
ACT_SKATE_RIDE     // grounded carve/cruise
ACT_SKATE_POWERSLIDE
ACT_SKATE_OLLIE    // charge/release
ACT_SKATE_AIR      // flip/grab/spin in air
ACT_SKATE_GRIND    // locked to a rail
ACT_SKATE_MANUAL   // wheelie/nose manual balance
ACT_SKATE_REVERT   // landing spin transition
ACT_SKATE_BAIL     // ragdoll-ish tumble, lose combo
```

**Physics model (per 30 Hz tick):**

```text
heading      = board yaw
speed        = forward speed along heading
driftAngle   = angle(velocity, heading)          // powerslide
lean         = f(stickX, speed)
grip         = surfaceGrip(floor->type) * (powerslide ? 0.25 : 1.0)

speed       += pushImpulse + slopeAccel - rollingFriction - airDrag
heading     += turnRate * steer * dt * (1 - 0.5*driftAngle)
lateralVel  += lean * speed * grip
pos         += headingVec * speed * dt + lateralVec * lateralVel * dt
```

Initial tunables:

| Parameter | Value | Notes |
|---|---|---|
| Push impulse | 5.5 units/frame | cadence 18 frames |
| Max speed | 50 units/frame | ~15 m/s at 30 Hz |
| Rolling friction | 0.35 units/frame² | asphalt; surface-dependent |
| Turn rate | 5.5°/frame at low speed, 2.5° at max | carve feel |
| Powerslide grip | 0.25× | hold B/Z |
| Ollie charge | 6–18 units | charge 0–20 frames |
| Air gravity | 4.0 units/frame² | SM64 default-ish |
| Landing tolerance | ±30° board pitch, ±25° yaw | else bail |
| Grind snap radius | 45 units | authored rails |
| Grind speed loss | 4%/s | balance meter active |
| Manual balance | ±1.0 meter, 0.03 drift/frame | stick corrects |
| Bail time | 45 frames | combo lost |

**Tricks (gesture → effect):**

| Gesture | Trick | Visual |
|---|---|---|
| Down→Up flick | Ollie | board pop, Mario crouch/extend |
| Down→Up-Right | Kickflip | board roll spin, Mario foot flick |
| Down→Up-Left | Heelflip | opposite roll |
| Left/Right flick | Shuvit / 360 | board yaw spin |
| Hold Up / Down | Nose/tail manual | balance meter |
| Hold trigger + flick | Grab | Mario grab pose, board tilt |
| Flick near rail | Grind | snap + sparks |
| R + left/right on landing | Revert | 180° skid, combo continues |

**Grinds:** v1 uses authored **grind rail objects** (`bhvGrindRail`, a spline of points) because SM64 has no rail system. v2 can auto-detect convex edges from collision triangles (two adjacent surfaces with a sharp dihedral) and generate temporary rails. Rail state stores `railId`, `t`, `speed`, `balance`; exit with A (pop) or at the end.

**Score/combo:** base points per trick × multiplier; multiplier increases with manual/revert/grind links and resets on bail. HUD is authored; no persistence in v1.

**Camera:** chase behind board heading with speed-based FOV, roll into carves, slightly lower height; dismount returns to `CAM_MODE_NORMAL`.

**Edge cases:** water (auto-dismount + swim), death plane, warps, tight corridors, steep slopes, stairs (step-up assist or bail), holding items, riding while small/big, shell riding conflict, pause menu, stereo.

**Assets:** authored board model (~200 tris), wheels, grip texture, 6–8 sounds (roll loop, push, ollie, land, bail, grind), HUD icons.

**Estimate:** 8–12 days for a solid first version, plus tuning.

---

### 5.2 Mario Kart 8-style kart (drift, glider, submarine)

**Goal:** pick up a kart → arcade racing physics in SM64 levels; drift with three-tier mini-turbos, glide, and transition to underwater/submarine.

**States:**

```
ACT_KART_IDLE
ACT_KART_DRIVE
ACT_KART_DRIFT        // actionState = left/right; actionTimer = charge
ACT_KART_BOOST        // actionArg = tier
ACT_KART_AIR
ACT_KART_GLIDE
ACT_KART_WATER        // propeller, shallow
ACT_KART_SUBMARINE    // deep, 3-axis
ACT_KART_SPINOUT      // hit wall/NPC
ACT_KART_FALL         // out of bounds/respawn
```

**Core model:**

| Parameter | Value | Notes |
|---|---|---|
| Top speed | 70 units/frame | ~21 m/s; tune per level |
| Accel | 1.8 units/frame² | approach top speed |
| Reverse | 20 units/frame | |
| Coast drag | 0.6 units/frame² | |
| Turn rate | 3.5°/frame | speed-dependent |
| Drift hop | 6 units up + 2 lateral | on drift press |
| Drift yaw | 5–8°/frame inward | inward-drift option |
| Drift grip | 0.35× | lateral slip |
| Step-up | 16 units | curbs/stairs |
| Wall hit | spinout 40 frames | speed < 50% |

**Drift / mini-turbo (faithful to MK8):**
- On drift press while steering: hop, set `actionState` side, begin charge.
- Charge: at 60 Hz, +5/frame if `abs(steerAngle) > 45°`, else +2/frame. At SM64's 30 Hz, **double the per-tick gain** (10/4) to preserve wall-clock seconds.
- Thresholds (MT stat 1.0): **280 / 590 / 900** → blue/orange/purple. At optimal steering that is ~0.93 s / ~1.97 s / ~3.0 s.
- Release: boost multiplier **1.25 / 1.40 / 1.60** for **0.7 / 1.2 / 1.8 s**, with sparks VFX and FOV punch.
- Steering sharpens charge but widens the line; counter-steer widens the drift.
- Optional “smart steering” assist off by default (it would prevent the top tier, as in MK8).

**Glider:**
- Deploy if airborne and (A held after a ramp, or fall > 40 units, or speed > 55 at ledge).
- While gliding: gravity 1.0, air control 40% of ground, forward speed decays slowly, camera pulls back, glider model unfolds.
- Retract on landing, water entry, ceiling hit, or A release. No glider in tight indoor areas by default.

**Underwater / submarine:**
- Use `find_water_level` + `m->waterLevel`; if submerged depth > 40 units → `ACT_KART_WATER` (propeller, speed 0.8×, handling 0.85×, MK8-style stats).
- If depth > 140 units and `kart_submarine_enabled` → `ACT_KART_SUBMARINE`: full 3-axis movement, buoyancy toward a target depth, propeller thrust on A, pitch with stick, slower but stable; exit by surfacing or pressing B.
- Screen splash/wet overlay; glider auto-retracts; engine sound pitch-shifts.

**Anti-gravity (stretch):** detect ceiling/road underside; flip gravity and align wheels; reuse `m->floor` with inverted normal. Not in v1.

**Camera:** chase with speed FOV, drift roll, look-back on Z, water/underwater color grading (authored post effect), glider pull-back.

**Edge cases:** small doors (kart won't fit), steep slopes, warps, tight platforms, falling into void, NPC collisions, entering buildings, shell/board conflicts, stereo comfort (avoid excessive roll).

**Assets:** authored kart model (Mario-sized), 4 wheels, glider, propeller, drift sparks, engine loop, boost/water sounds.

**Estimate:** 12–18 days, plus tuning.

---

### 5.3 Grapple gun

**Goal:** aim, fire a hook, attach to geometry or objects, reel in, swing, release; pull Mario toward the anchor.

**States:**

```
ACT_GRAPPLE_AIM     // over-shoulder camera, reticle
ACT_GRAPPLE_FIRE    // hook in flight
ACT_GRAPPLE_PULL    // reel toward anchor
ACT_GRAPPLE_SWING   // pendulum, tangential
ACT_GRAPPLE_RELEASE
```

**Targeting:**
- Ray from camera through screen center, or from Mario's eye toward the camera's forward.
- Geometry: `find_surface_on_ray(orig, dir, &surface, hitPos)` (already in `surface_collision.h`).
- Objects: iterate `gObjectPool`, sphere/box test against a ray; skip intangibles; allow NPCs (pull yourself to them) and dedicated anchor objects.
- Max range 1200 units; reject surfaces too close/too steep? Allow any wall/ceiling/floor, but disallow death planes and water surfaces.
- Store `anchorPos`, `anchorSurface`, `anchorObj`, `ropeLength`, `attached`.

**Hook projectile:** spawn `bhvGrappleHook` at Mario's hand, travel 45 units/frame, slight gravity, raycast per step to avoid tunneling; on hit, set anchor and switch Mario action.

**Rope constraint (the important part):**

```text
d      = anchor - pos
dist   = |d|
if (dist > ropeLength) {
    // position correction
    pos += normalize(d) * (dist - ropeLength) * 0.8
    // remove outward radial velocity
    vr = dot(vel, normalize(d))
    if (vr < 0) vel -= normalize(d) * vr
    // centripetal + reel
    vel += normalize(d) * reelAccel
}
vel += gravity * (swinging ? 0.35 : 1.0)
speed = clamp(|vel|, 0, maxGrappleSpeed)
```

- Reel: hold L to shorten `ropeLength` (e.g. 25 units/frame), release to stop; aim reticle shows length.
- Pull mode: if aiming at a point above/forward, use constant acceleration toward anchor until within 60 units, then release with preserved velocity.
- Swing: lock length; steer with stick to pump; jump to detach with a tangential + upward impulse (wall-run/jump synergy).
- Rope visual: 10–16 segment verlet chain for sag (visual only), rendered as a thin authored texture/line; avoid new display-list hacks by using a tiny geo or a chain of sprites.
- Anchor invalidation: if the surface is destroyed or the object dies, detach with a snap sound.
- Cooldown 20 frames; ammo optional (config).

**Camera:** aim mode (over right shoulder, narrow FOV), reticle at hit point (depth-aware), switch back on fire/release. Stereo reticle must sit at the hit point, not on the glass.

**Edge cases:** wrap through geometry, rope through walls (allowed visually; gameplay checks line-of-sight on attach only), pulling into a wall (stop at surface), ceiling anchors, water, death plane, riding vehicles (force dismount), pause.

**Assets:** authored gun/hook/rope textures, fire/attach/reel/release sounds, reticle.

**Estimate:** 5–7 days.

---

### 5.4 Wall-run (Titanfall 2-inspired, enabled by default)

**Goal:** airborne Mario can run along a wall for ~1.75 s, then jump off with a boost; camera tilts; works with existing momentum.

**Detection (every airborne tick):**
1. Query walls at foot and chest heights with `f32_find_wall_collision` / `WallCollisionData` (radius 30, offsets 20 and 80).
2. Pick the wall whose normal is most horizontal and most opposed to movement.
3. Conditions: `m->action & ACT_FLAG_AIR`, horizontal speed > 30, `dot(vel, tangent) > 0`, forward input held, wall normal angle within 60° of horizontal, cooldown expired, wall identity not the recently used one.
4. Wall identity: quantize normal + plane offset + a contact point; regrab allowed only if new contact is lower by > 20 units, or wall normal differs by ≥ 25° (TF2 rule).

**State:** `ACT_WALLRUN` with `actionState = side` (−1 left, +1 right), `actionTimer` = remaining ticks (max 52 ticks ≈ 1.75 s), `actionArg` = wall id hash.

**Physics:**

```text
n   = wall normal (horizontal, normalized)
t   = normalize(cross(up, n)) * sideSign   // choose direction matching velocity
v_t = dot(vel, t)
v_n = dot(vel, n)
speed = clamp(v_t + wallrunAccel, 0, wallrunCap)   // 55 units/frame
vel = t * speed + n * max(v_n, -2)                  // stick to wall
pos += -n * (radius - wallDistance)                  // keep contact
gravity *= 0.35                                     // arc, not float
```

- Entry snap: rotate camera yaw toward tangent; add `camera roll = -side * 10°` (lerp over 6 frames).
- Wall jump: `vel = t * speed * 1.12 + n * 12 + up * 10`; grant 0.15 s coyote time after leaving the wall; reset double-jump? Keep SM64's rules unless config overrides.
- Detach: timer 0, wall ends (no wall query), input away for >6 frames, ceiling/floor collision, or jump.
- Cooldown per wall identity: 20 frames; global 10 frames.

**Tunables:** `wallrun_duration` 1.75 s, `wallrun_min_speed` 30, `wallrun_cap` 55, `wallrun_accel` 0.8/tick, `wallrun_gravity_scale` 0.35, `wallrun_jump_normal` 12, `wallrun_jump_up` 10, `wallrun_jump_tangent` 1.12, `wallrun_camera_roll` 10°, `wallrun_require_forward` true.

**Interactions:** works with grapple (grapple → wall → jump combos); disabled while karting; optional “wallride” on skateboard later (different feel). Default on, toggle in Settings.

**Edge cases:** narrow corridors (two walls), curved walls (re-query normal every tick; detach if normal changes > 30°), moving platforms, slippery walls, water, stereo comfort (clamp roll to ±12°), motion sickness option to reduce camera roll.

**Estimate:** 4–6 days.

---

### 5.5 Destructible environments + rocket launcher

**Goal:** a rocket launcher that damages NPCs and destroys tagged walls, ceilings, props, and *some* elevated floors, without softlocking levels.

This is the hardest feature because SM64 level geometry is static: collision surfaces live in `sSurfacePool` and visuals are baked display lists. Plan in three layers.

#### Layer 1 — Destructible props and NPCs (easy)

- Props: reuse the `breakable_box` pattern (`data/behavior_data.c:1746`): hitbox, `oHealth`, `obj_explode_and_spawn_coins`, respawner optional.
- NPCs: give custom enemies `oHealth` + a damage handler; on death play a authored/borrowed death animation, drop coins, and set a respawn timer. Use `oInteractStatus`/`interact_*` where possible.
- Rocket damage: sphere query over `gObjectPool` (distance + hitbox overlap), apply `damage = base * falloff`, knockback, VFX.

#### Layer 2 — Tagged static surfaces (the core trick)

- `sSurfacePool` is contiguous (`src/engine/surface_load.c:31`), so add a parallel array:

```c
// src/game/destruction.h
struct SurfaceState {
    u8 solid : 1;
    u8 destructible : 1;
    u8 isFloor : 1;
    u8 reserved : 5;
    u16 group;
};
extern struct SurfaceState *gSurfaceState; // [sSurfacePoolSize]
```

- Allocate/reset it in `alloc_surface_pools()` / `load_area_terrain()`; index by `surf - sSurfacePool`.
- Filter candidates in `find_wall_collisions_from_list`, `find_floor_from_list`, `find_ceil_from_list`, and `find_surface_on_ray` with a single `if (!gSurfaceState[surf - sSurfacePool].solid) continue;`. This is cheap and localized.
- **Tagging:** per-level `DestructionVolume` tables (authored):

```c
struct DestructionVolume {
    s16 min[3], max[3];     // AABB in level units
    u8  surfaceMask;        // wall/ceiling/floor
    u8  group;
    u16 health;
    u8  flags;              // FLOOR_ALLOWED, CRITICAL, HIDE_PANEL
};
```

  At area load, after terrain load, mark surfaces whose centroid lies in a volume and whose type/normal matches. This avoids brittle surface indices and survives level edits. A build-time tool can optionally emit exact indices for precision.
- **Visuals:** static walls cannot be removed from the display list. Pragmatic approach: place an authored **panel object** over each tagged wall (slightly in front), matching the level's look as closely as possible (or using an authored material). On destruction: disable collision, remove the panel, spawn debris, and reveal a dark “hole”/interior quad. For ceilings/walls this reads well; for large structural walls, optionally pre-split level geo at build time (advanced, later).
- **Restore:** reset surface state and respawn panels on area reload; do not persist destruction across level exits in v1 (prevents softlocks).

#### Layer 3 — Floors (default immune, narrow exception)

- Floors are indestructible by default.
- `FLOOR_ALLOWED` volumes only apply if:
  1. the surface is an elevated floor (a downward ray from its centroid finds another floor > 120 units below);
  2. it is not under a star/door/warp/spawn/critical trigger (denylist + authored `CRITICAL` flag);
  3. Mario is not currently standing on it (or he is launched/falls with it);
  4. the area still has a reachable path (basic check: a floor exists below within the level bounds).
- On destruction: disable collision, spawn falling debris, optionally spawn a `bhvBreakablePlatform`-style falling chunk; if the check fails, the rocket only scorches it.

#### Rocket launcher

- Pickup → `POWERUP_ROCKET`, ammo 5 (config).
- Hold L to aim (same camera as grapple), release to fire; or tap to fire at reticle.
- Projectile `bhvRocket`: speed 90 units/frame, slight gravity, **swept sphere** raycast each tick (segment vs. surfaces + objects) to avoid tunneling; lifetime 180 ticks.
- Explosion: radius 220, damage 100 at center → 20 at edge, knockback, screen shake, authored VFX/sound; optional rocket jump (self-damage disabled or reduced).
- Damage routing: props → `oHealth`; NPCs → damage table; static → `destruction_damage(group, damage)`; critical surfaces ignore.
- Ammo/cooldown HUD; reload pickup.

**Performance:** debris budget (max 24 chunks/area, pooled), no per-frame allocation, destruction events are O(1) per surface via the state array, panels are normal objects.

**Risks:** softlocks (mitigated by critical flags/restore), visual seams (panels), collision leaks (holes), stereo debris overdraw.

**Estimate:** 10–18 days for layers 1–2 + rockets; +1–2 weeks for a build-time geo-splitting tool if we want real holes.

---

### 5.6 Dreams/BubbleBath-style splat rendering mode

**Goal:** an optional rendering mode that makes SM64 look like a painting made of flecks/splats, with albedo-driven colors, smart culling, view-dependent shading for special materials, and debug controls for stroke uniformity/randomness.

#### 5.6.1 What BubbleBath actually is

Dreams' BubbleBath renders **Flecks** — camera-facing splat/particle primitives — instead of conventional textured polygons, giving the game its impressionistic look. It is not a post-process filter; it is a different way of drawing surfaces. We will imitate the *look and principles*, not the engine.

#### 5.6.2 The albedo problem (read this before estimating)

`struct Surface` (`include/types.h:241`) contains type, force, flags, room, lowerY/upperY, three vertices, normal, originOffset, object — **no UVs, no texture ID**. Therefore:

- You **cannot** sample a surface's texture color from collision data alone.
- Options:
  1. **Runtime G-buffer sampling** (recommended prototype): render the scene normally to a low-res color target; for each candidate world-space splat, project it, sample the color buffer, and cache the color. Depth can come from a depth texture (`OES_depth_texture`) or be approximated from the collision hit. Cheap, automatically matches textures, but colors are view-dependent and need temporal accumulation for stability.
  2. **Build-time bake** (recommended target): a tool that parses level geo/display lists (F3DEX commands), extracts triangles with UVs + texture IDs, samples the actual texture data, and writes per-level splat caches (position, normal, tangent, size, color, material, cluster). Most accurate and stable; requires tooling and ROC assets.
  3. **Post-process painterly filter** (fallback): full-screen splat/stroke filter over the color buffer, no world-space anchoring. Easy, stable, looks good in screenshots, but breaks parallax and stereo depth.

**Plan:** prototype (1) to validate the look and debug UI, then invest in (2) if the look is worth it. Keep (3) as a low-end fallback.

#### 5.6.3 Splat data and generation

```c
struct Splat {
    f32 pos[3];
    s16 normal[3];
    s16 tangent[3];
    u16 size;        // world units, quantized
    u16 rot;         // 0..65535
    u8  color[4];    // albedo + opacity
    u8  material;    // 0 diffuse, 1 metal, 2 water, 3 glass, 4 emissive
    u16 cluster;
}; // ~32 bytes
```

- Density: start at 1 splat per ~8×8 units of surface (tunable). A SM64 area is small; 50k–200k splats/area is plausible (~1.6–6.4 MB).
- Cluster grid: 512-unit cells; each cluster stores AABB, offset/count in a flat array, and a material histogram.
- Pre-sort splats within a cluster by a coarse axis (or keep unsorted and rely on depth test); sort per frame only for translucent clusters.
- Generation from G-buffer: sample visible pixels at a stride, reconstruct world position from depth, project onto the surface (or keep the pixel's world pos), and accumulate into the cluster; merge duplicates by cell; cache colors with a confidence counter; refresh when the camera moves significantly.

#### 5.6.4 Rendering on GLES2

This port uses GLES2 (`#version 100`) and the Leia stereo FBO path. Constraints:

- No compute shaders, no core instancing. Use **point sprites** (`gl_PointSize`, `gl_PointCoord`) for small splats, or batched quads in a dynamic VBO for large/anisotropic ones.
- Point sprites are perfect for flecks: one vertex per splat, per-vertex `size/rot/color/material/normal`; fragment shader rotates `gl_PointCoord`, applies an elliptical Gaussian falloff, and discards outside the footprint.
- Depth test on; alpha blend for soft edges. Sorting is limited: use per-cluster back-to-front order by cluster centroid and depth-test; for the translucent layer, sort within the cluster (small counts).
- Budget: 20k–40k splats per eye, one or few draw calls per cluster; target 1–3 ms. If too slow, lower density (bigger splats) or fall back to screen-space mode.
- Stereo: render the splat pass **once per eye** with that eye's view matrix; sizes are projected per eye; sort per eye if necessary. Do not use a screen-space-only effect as the primary stereo mode.

**Shader sketch (fragment):**

```glsl
vec2 uv = gl_PointCoord - 0.5;
uv = rot2(rot) * uv;
float r2 = dot(uv / halfSize, uv / halfSize);
if (r2 > 1.0) discard;
float a = opacity * exp(-2.5 * r2);       // Gaussian-ish falloff
vec3 c = albedo;
// view-dependent term for special materials
vec3 V = normalize(cameraPos - worldPos);
float fres = pow(1.0 - max(dot(V, normal), 0.0), 4.0);
c = mix(c, skyColor(V, normal), fres * reflectivity);
c += specular * pow(max(dot(reflect(-V, normal), lightDir), 0.0), shininess);
gl_FragColor = vec4(c, a);
```

#### 5.6.5 View-dependent lighting / “Gaussian splat” feel

- True 3DGS stores anisotropic 3D Gaussians + spherical-harmonic colors, projects them to 2D covariances, sorts, and alpha-blends.
- We approximate:
  - **Anisotropic footprint:** elongate the splat along the surface tangent (store tangent + aspect), giving the “smeared paint” look.
  - **View-dependent color:** 2-band approximation `color = c0 + c1 * dot(viewDir, tangent)` (3 floats per channel or a single directional tint) for metals/water; much cheaper than 9 SH coefficients.
  - **Reflective materials:** fresnel + sky gradient reflection + sharp specular; for water, add a scrolling normal perturbation.
  - **Opacity/size jitter:** per-splat seeded randomness; density falloff at distance.

#### 5.6.6 Smart culling

- **Cluster AABB frustum cull** (per eye, because stereo has slightly different frusta).
- **Distance LOD:** keep every Nth splat when a cluster is far; increase size to preserve coverage; fade at the far threshold.
- **Budget allocator:** compute a per-frame splat budget from measured GPU time; distribute across visible clusters proportionally to screen area; drop low-contribution clusters first.
- **Room/area culling:** SM64 areas already separate rooms; use the area/room data to skip off-screen rooms where possible.
- **Occlusion:** optional coarse occlusion from the depth buffer (reject clusters fully behind the depth at their screen AABB); avoid per-splat occlusion.
- **Temporal:** keep the previous frame's visible cluster set and only rebuild changed ones.

#### 5.6.7 Debug controls (the requested “oil paint” knobs)

Expose in a debug panel and `configfile.c`, all live-editable:

| Control | Range | Effect |
|---|---|---|
| `splat_mode` | off / screen / world | pipeline |
| `splat_density` | 0.1–4.0 | samples per area |
| `splat_size_base` | 1–64 units | stroke footprint |
| `splat_size_jitter` | 0–1 | uniformity ↔ randomness |
| `splat_rot_base` | 0–360° | stroke angle |
| `splat_rot_jitter` | 0–1 | rotation randomness |
| `splat_aspect` | 1–8 | elongation/anisotropy |
| `splat_color_jitter` | 0–1 | hue/value variation |
| `splat_opacity` | 0–1 | blend strength |
| `splat_softness` | 0–1 | Gaussian falloff |
| `splat_material_mix` | 0–1 | view-dependent strength |
| `splat_seed` | int | deterministic regeneration |
| `splat_lod_bias` | 0.25–4 | culling aggressiveness |
| `splat_show_clusters` | bool | debug AABBs |
| `splat_budget` | 1k–80k/eye | performance cap |

Add a “freeze/refresh” button and an A/B split view (left half original, right half splats) for tuning. This directly covers the “uniformity/randomness in size/rotation” inspiration from the oil-paint demo.

#### 5.6.8 Milestones

1. Screen-space prototype: full-screen point-splat pass over the color buffer, live sliders, stereo-safe. (3–5 days)
2. World-space G-buffer splats with cluster culling and caching. (1–2 weeks)
3. Build-time bake tool from display lists/textures. (1–2 weeks, only if step 2 proves the look)
4. View-dependent materials + LOD/budget + low-end fallback. (1 week)
5. Polish: temporal stability, stereo comfort, debug overlay. (1 week)

**Risks:** GLES2 performance, stereo cost, temporal shimmer, memory, the bake tool's complexity.

---

### 5.7 Jetpack (fuel, slow refill, capacity/refill upgrades)

**Goal:** a permanent jetpack equipment pickup. Hold **Use** (`L_TRIG` / touch “Thrust”) to fly, watch a fuel gauge drain, and land to let it refill slowly. Collect special fuel-cell items to raise **max capacity** and **refill speed**. Unlike the vehicles, the jetpack is equipment, not a mount: Mario keeps his normal moveset and animation set, with a thruster on his back.

**Design rules:**

- The jetpack is a **one-time unlock per save slot**, not a consumable. Picking up the jetpack equipment enables it permanently; the HUD gauge appears only when equipped.
- Fuel is a **resource with hysteresis**: thrust cuts at 0, and cannot restart until fuel is above 10% (prevents stutter-thrusting at empty).
- Refill happens only when **not thrusting**. Base rate is slow; standing still/grounded is faster, underwater is slower. There is no instant refill on landing.
- Upgrades are **permanent per save slot** and tiered. The “special item” can be a capacity cell, a refill pump, or a combined cell; the plan supports all three variants.
- The jetpack is disabled while driving a kart or riding the skateboard; dismount first. It **pauses fuel drain during a wall-run** (wall-run takes priority) and resumes afterward.
- Dying respawns with 50% fuel (configurable); entering an area does not refill.

**Base tuning (30 Hz; convert from per-second in code):**

| Parameter | Base | Per tick (÷30) | Notes |
|---|---|---|---|
| Max fuel | 100 | — | three upgrade tiers |
| Thrust drain | 12/s | 0.40 | holding Use and ascending |
| Hover drain | 4/s | 0.133 | holding Use with no vertical input |
| Idle drain | 0/s | 0 | no drain when off |
| Refill (air) | 3/s | 0.10 | not thrusting |
| Refill (grounded/idle) | 4.5/s | 0.15 | 1.5× multiplier |
| Refill (underwater) | 1.5/s | 0.05 | 0.5× multiplier |
| Thrust accel | 7.5 u/f² | — | net +3.5 vs. gravity 4.0 |
| Max ascent | 14 u/f | — | clamp |
| Horizontal control | 0.6× air | — | max horizontal 40 u/f |
| Empty restart threshold | 10% | — | hysteresis |
| Low-fuel warning | 25% | — | beep + HUD blink |
| Upgrade tier (×3) | +40 max, +1.2/s refill | +1.33 max, +0.04/tick | final 220 max / 6.6/s |
| Respawn fuel | 50% | — | configurable |

**States:**

```
ACT_JETPACK_GROUND   // equipped, grounded; first thrust does a small hop
ACT_JETPACK_FLY      // airborne; actionState = THRUST | HOVER | EMPTY
```

`actionState` values: `JETPACK_THRUST` (Use held, ascending), `JETPACK_HOVER` (Use held, no vertical input), `JETPACK_EMPTY` (fuel 0, falling, thrust locked out). Use the normal airborne landing path to return to ground.

**Physics/update:**

```text
if (Use held && fuel > 0 && !emptyLockedOut) {
    actionState = stickY != 0 ? JETPACK_THRUST : JETPACK_HOVER;
    fuel -= (actionState == THRUST ? 0.40 : 0.133);
    vel.y += 7.5;                       // gravity is applied by the air step
    vel.y = min(vel.y, 14.0);
    horizontalAccel = 0.6 * airControl;
} else {
    actionState = fuel > 0 ? ... : JETPACK_EMPTY;
    fuel += refillPerTick();            // 0.10 / 0.15 / 0.05 by surface
}
fuel = clamp(fuel, 0, fuelMax);
```

- First thrust from the ground adds a 4-unit hop and switches to `ACT_JETPACK_FLY`.
- Ceiling/wall collisions use the normal air step; no clipping.
- Fuel drain is paused when the game is paused, in cutscenes, or during a wall-run.
- Grapple + jetpack is allowed: the rope constraint runs first, jetpack adds thrust; fuel still drains.
- Landing with fuel > 0 and downward speed > threshold gets a small thrust-assisted landing (no fall damage); landing with 0 fuel uses normal rules.

**Upgrade pickups:**

- `bhvJetpackPickup` — the equipment itself (one-time, persists). Authored model: a small backpack/tank.
- `bhvFuelCellPickup` — the special upgrade item. `oBehParams2ndByte` selects variant and tier:
  - `0` = capacity cell: +40 max fuel;
  - `1` = refill pump: +1.2/s refill;
  - `2` = combined cell: +25 max and +0.8/s (for the “one special item” version).
- Three tiers total; each pickup checks the save slot and refuses/awards accordingly. Spawn as a glowing, rotating authored object with a distinct sound and HUD toast (“Fuel capacity 2/3”).
- Optional: one “master cell” that grants the next tier regardless of variant, for simpler level placement.

**Persistence:**

- Recommended: a small **sidecar** settings file keyed by save slot (e.g. `jetpack.<slot>.cfg` in the user data dir) storing `{unlocked, tierCapacity, tierRefill, fuel}`. This avoids changing the EEPROM save format and its checksum/version.
- Alternative: extend the save struct with a versioned, checksummed block; only if the sidecar proves fragile. Deleting a save slot deletes its sidecar; copying saves must copy both.
- Config can force-unlock the jetpack for testing (`jetpack_unlocked=1`), and reset upgrades.

**HUD/UX:**

- Fuel gauge (bar + numeric), max-capacity ticks, upgrade pips, low-fuel blink, empty “sputter”.
- Context prompt: “Hold Use to thrust” on first equip; “Fuel 0 — land to refill”.
- Touch: the existing Use button doubles as thrust when the jetpack is equipped; add an optional dedicated Thrust button in the touch layout.
- Audio: looping thruster (pitch scales with ascent), low-fuel beep, empty cutoff, pickup/upgrade jingle. Avoid the engine loop used by the kart.
- Camera: slight FOV pull-back and a small downward look offset while flying; no roll (stereo comfort).

**Interactions and edge cases:**

- Vehicles: disabled while karting/skating; auto-dismount prompt rather than silent failure.
- Water: thrust works but is weaker (0.6×), refill is slower, and surfacing is allowed; no underwater jetpack speed exploit.
- Warps/death: cancel thrust, preserve fuel (death sets 50%), respawn cleanly.
- Empty over a void: normal fall, no last-second rescue; this is the intended risk.
- Save/load, pause, cutscenes, stereo, controller hot-plug, touch remap.
- Balance: jetpack can trivialize platforming; keep drain high, refill slow, and cap ascent. Provide a `jetpack_assist` config for younger players (slower drain) but default off.

**Estimate:** 3–5 days including HUD, persistence, and tuning.

---

### 5.8 P2P multiplayer over WebRTC (URL + lobby code)

**Goal:** simple co-op free-roam first, then races/vehicles; host-authoritative; join by URL or 6-character code.

**Transport and stack:**

- **libdatachannel** (C++17, Android NDK-friendly) for WebRTC DataChannels + ICE/STUN/TURN. Alternatives: Google libwebrtc (large), or a UDP socket + manual NAT punch (worse).
- Add `android.permission.INTERNET` and `ACCESS_NETWORK_STATE`.
- Signaling: tiny service (Cloudflare Worker + Durable Object or a small VPS) that stores room offers/answers/ICE candidates for a 6-char code, TTL 2 h. No game data, no accounts.
- TURN fallback (self-hosted coturn or a paid relay) for symmetric NAT; credentials handed out by the signaling service. Without TURN, some mobile networks will fail.
- URL join: `https://<host>/join/ABCDEF` landing page with an `intent://` link to the app, plus app deep link `sm64lumepad://join?code=ABCDEF`. Also a plain in-game “Join by code” field (no web needed).

**Lobby codes:** Crockford base32, 6 chars, excluding ambiguous letters; code maps to a room; host is authoritative; max 4 players for v1.

**Netcode model (host-authoritative):**

- Fixed 30 Hz simulation tick on the host (the existing game loop).
- Clients send **input frames** (`buttons`, `stickX/Y`, `seq`) at 30 Hz over an unreliable, unordered channel; keep the last 3 for redundancy.
- Host sends **snapshots** at 15 Hz: `tick`, player states (pos/vel/action/anims/area), and object deltas for nearby objects (NPCs, pickups, vehicles, projectiles, destruction groups), plus reliable events (star grab, door, level change, rocket explosion).
- Clients render remote players with 100 ms interpolation; local player is predicted and reconciled on snapshot (position/velocity/action).
- Objects: host owns all gameplay state; clients do not simulate NPCs/destruction. Vehicles: the driver is authoritative for their own vehicle; other clients interpolate.
- Determinism is **not** required (host authority + correction). Float divergence is fine.
- Areas/levels: host broadcasts `area_load` with level/area id + seed + object overrides; clients load the same area.
- Splats are local render-only; never networked.

**Packet sketch:**

```c
struct NetInput  { u32 seq; u16 buttons; s8 stickX, stickY; u8 tick; };
struct NetPlayer { u8 id; f32 pos[3]; f32 vel[3]; f32 faceYaw; u32 action; u16 anim; u8 area; };
struct NetSnapshot { u32 tick; u8 count; NetPlayer players[MAX]; u16 eventMask; };
struct NetEvent  { u8 type; u8 a, b; s32 x, y, z; u32 data; };
```

**Phases:**

1. Transport + handshake + lobby code (2 players, one area, ghosts only).
2. Local prediction/interpolation + animation sync.
3. Object sync (coins, NPCs, pickups) + reliable events.
4. Vehicles/projectiles/destruction sync (rockets, walls).
5. Lobby UX (URL/deep link, invite, kick, reconnect), TURN, metrics.
6. Scale to 4 players, bandwidth tuning, mobile network testing.

**Security/cheat:** host validates actions; rate-limit lobby creation; no accounts/PII; code is capability, not auth; optional password.

**Testing:** two devices on Wi-Fi, one on cellular; NAT types (full cone, symmetric); 100–200 ms latency; packet loss 2–5%; reconnect; host migration (not v1).

**Estimate:** 4–8 weeks for a solid co-op MVP; vehicles/destruction sync adds 1–2 weeks.

---

## 6. Milestones and sequencing

| Milestone | Contents | Depends on | Estimate |
|---|---|---|---|
| **M0 — Foundations** | Item slots, contextual input, config/debug flags, shared vehicle helpers, test level/spawn keys | — | 3–5 days |
| **M1 — Grapple + wall-run** | Raycast/aim, rope constraint, wall detection/tangent, camera roll, toggles | M0 | 8–12 days |
| **M1b — Jetpack** | Fuel/thrust/refill, HUD gauge, capacity/refill upgrade pickups, sidecar persistence | M0 | 3–5 days |
| **M2 — Runtime assets (BYO-ROM)** | `EXTERNAL_DATA=1` shell, LauncherActivity, on-device extractor, `base.zip` mount, CI checks | — (parallel) | 2–4 weeks (Chaquopy path shorter) |
| **M3 — Skateboard** | Board pickup, ride/push/carve/ollie, flick tricks, grinds, manual/revert, score, bail | M0 | 8–12 days |
| **M4 — Kart** | Kart pickup, drive/drift tiers, boosts, glider, water/submarine, camera, collisions | M0 | 12–18 days |
| **M5 — Rocket + destruction** | Rocket pickup/projectile/explosion, props/NPC damage, surface registry, panels, floor exception | M0, M1 (raycast) | 10–18 days (+1–2 wks for geo splitting) |
| **M6 — Splat renderer** | Screen prototype → world/G-buffer → culling/LOD → materials/debug | Renderer work; M2 for baked caches | 3–4 weeks |
| **M7 — Multiplayer** | libdatachannel, signaling, lobby codes/URLs, input/snapshots, object sync, vehicles | M0, M3–M5 for full sync | 4–8 weeks |
| **M8 — Polish/release** | Tuning, performance, stereo comfort, docs, CI, BYO-ROM release | all | 2–3 weeks |

**Recommended order:** M0 → M1 → M1b → M3 → M4 → M5 → M6 → M7, with M2 running in parallel as soon as M0 lands (distribution unblocks playtesting). M1b is small and can ship with M1 as one “movement toys” update. If the goal is a demo, do M1 + M6 prototype first for the “wow” factor, then vehicles.

**Why this order:** grapple/wall-run share wall/ray queries and are low-risk; skate/kart share the vehicle framework; destruction needs rockets + surface filtering; splats are renderer-isolated; multiplayer is the largest and benefits from stable state/action definitions.

---

## 7. File-by-file change map

### New gameplay files

| File | Purpose |
|---|---|
| `src/game/powerups.c/.h` | Item slots, pickup kinds, use/update/HUD |
| `src/game/vehicle.c/.h` | Shared ground/air steps, surface alignment, engine audio |
| `src/game/skate.c/.h` + `mario_actions_skate.c` | Board state, tricks, grinds, score |
| `src/game/kart.c/.h` + `mario_actions_kart.c` | Drift charge, boost, glider, water/submarine |
| `src/game/grapple.c/.h` + `mario_actions_grapple.c` | Aim, hook, rope constraint, swing |
| `src/game/wallrun.c/.h` | Wall detection, tangent motion, jump-off, cooldowns |
| `src/game/jetpack.c/.h` + `mario_actions_jetpack.c` | Fuel/thrust/refill model, HUD state, upgrades |
| `src/game/destruction.c/.h` | Surface state array, volumes, damage, restore |
| `src/game/projectile.c/.h` | Rocket/hook swept-sphere movement, explosions |
| `src/game/splat.c/.h` | Splat data, generation, clusters, culling |
| `src/pc/gfx/gfx_splat.c/.h` | GLES2 point-splat pass, shader, VBOs, budget |
| `src/pc/net/net.c/.h`, `protocol.c/.h`, `lobby.c/.h` | WebRTC, packets, signaling client |

### Behavior files (`.inc.c`) + `data/behavior_data.c`

`skateboard_pickup`, `kart_pickup`, `grapple_pickup`, `rocket_pickup` (shared `bhvPowerUpPickup`), `jetpack_pickup`, `fuel_cell_pickup`, `grapple_hook`, `rocket`, `grind_rail`, `destructible_panel`, `breakable_prop`, `falling_chunk`.

### Edits to existing files

- `include/sm64.h` — new `ACT_*` constants and flags.
- `src/game/mario.c` — dispatch cases; action-group flags; initial state setup.
- `src/game/mario_actions_moving.c` / `_airborne.c` / `_submerged.c` — new action handlers.
- `src/engine/surface_collision.c` — solidity filter in the three list walkers + ray.
- `src/engine/surface_load.c` — allocate/reset `gSurfaceState`; apply destruction volumes after terrain load.
- `src/game/interaction.c` / `include/interaction.h` — `INTERACT_POWERUP` + damage routing.
- `src/game/camera.c/h` — chase/aim/wall-run modes, roll, FOV, reticle.
- `src/pc/configfile.c/h` — feature flags + tuning.
- `src/game/debug.c` — spawn/tuning overlay.
- `src/pc/controller/controller_touchscreen.c` + `..._layouts.inc` — Use/Dismount/right stick.
- `src/pc/gfx/gfx_pc.c` / `gfx_opengl.c` — splat pass, per-eye integration.
- `app/jni/src/Android.mk` — new sources, optional `EXTERNAL_DATA=1`.
- `app/src/main/java/...` — `LauncherActivity`, `GameAssets`, extraction JNI.
- `app/src/main/AndroidManifest.xml` — launcher activity/process, `allowBackup="false"`, INTERNET for net.
- `tools/` — splat bake tool, native extractor helpers.

---

## 8. Test plan

### Per feature

- **Skate:** push/carve/ollie on all surface types; trick gesture table; landing tolerance; grind entry/exit/balance; manual/revert; bail; water/death/warp; dismount; stereo camera.
- **Kart:** acceleration/top speed; drift charge tiers and thresholds; boost duration; glider deploy/retract; water/submarine transitions; wall/NPC collisions; stairs/doors; respawn; stereo comfort.
- **Grapple:** hit/miss; wall/ceiling/object anchors; reel/swing/pull; release momentum; anchor destroyed; rope through geometry; aim reticle depth in stereo.
- **Wall-run:** entry conditions; 1.75 s cap; curved walls; two-wall corridors; regrab rule; jump boost; cooldown; camera roll clamp; toggle off.
- **Jetpack:** drain/refill rates on air/ground/water; empty lockout + 10% restart; capacity/refill upgrades tier by tier; HUD gauge/pips; death respawn fuel; save-slot persistence and deletion; disable while karting/skating; wall-run drain pause; ceiling/water/void edge cases.
- **Destruction:** props/NPCs; tagged walls/ceilings; floor exception; critical-surface protection; area reload restore; debris budget; no collision leaks.
- **Splat:** color match; temporal stability; cluster culling; LOD/budget; stereo correctness; low-end fallback; debug sliders; memory.
- **Multiplayer:** lobby code/URL; NAT matrix; latency/loss; prediction/reconciliation; object sync; reconnect; host quit; 4 players; bandwidth.

### Regression

- Vanilla Mario movement, all 15 courses, stars/doors/warps, save/load, pause menu, camera modes, touch controls, controller hot-plug, Leia stereo, performance.
- Ensure every feature can be disabled and the vanilla game still passes.

### CI gates

- No ROM/extracted assets in the tree (fail if `actors/**/*.png`, `levels/**/*.png`, `textures/**/*.png`, `sound/**/*.aiff`, `assets/demos/*.bin` exist).
- Build both `EXTERNAL_DATA=0` (dev) and `EXTERNAL_DATA=1` (release shell).
- Unit-test pure helpers (drift charge, fuel drain/refill + upgrade math, spline math, packet serialization) on host.
- Performance budget check on device: frame time, splat counts, object counts.

---

## 9. Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Scope is enormous (8 large features) | Never ships | Strict milestones; each feature playable alone; cut splat bake/multiplayer first |
| No Skate/MK8/TF2 decomp | Can't copy physics | Original arcade models; documented behavior; tune by feel |
| SM64 collision has no UV/albedo | Splat colors wrong | G-buffer sampling prototype; build-time display-list bake; screen-space fallback |
| GLES2 lacks instancing/compute | Splat performance | Point sprites, static VBOs, clusters, budget, low-end mode |
| Stereo doubles cost | Frame drops/motion sickness | Per-eye budgets, roll clamp, comfort options |
| Static level geometry | Destruction looks fake / softlocks | Panels + collision filtering; critical flags; restore on reload; geo-splitting later |
| Jetpack trivializes platforming | Level shortcuts/softlocks | High drain, slow refill, ascent cap, no instant landing refill; `jetpack_assist` off by default |
| Touch control overload | Unplayable on tablet | Contextual mapping, one Use button, right stick, remap UI |
| ROM extraction complexity | Distribution blocked | EXTERNAL_DATA shell; Chaquopy prototype; Ghostship reference; native port later |
| WebRTC NAT/TURN | Some players can't connect | STUN + TURN, lobby diagnostics, manual SDP fallback |
| Save compatibility | Corrupt saves | Jetpack upgrades in a per-slot sidecar, not EEPROM; no other new save fields in v1 |
| Legal/IP | Takedown | No ROM/assets in repo; authored content; no Play Store |

---

## 10. Open decisions

1. **Distribution target:** GitHub Releases sideload only, or also an F-Droid-style repo? (Play Store is not viable with ROM-derived assets.)
2. **Extractor choice:** Chaquopy prototype vs. native port. Recommend Chaquopy first to validate, native later.
3. **Skate controls on touch:** right-stick flick vs. gesture buttons; how much assist?
4. **Kart scope:** free-roam toy vs. authored race courses/time trials. Courses are a separate content project.
5. **Wall-run default:** on globally or only when a “parkour” flag is enabled? User asked default on; keep it on with a Settings toggle.
6. **Jetpack persistence:** per-slot sidecar file (recommended) vs. a versioned block in the EEPROM save. Sidecar avoids save-format risk but must be copied/deleted with the slot.
7. **Jetpack economy:** how many fuel-cell tiers, and are they per-level or global? Recommend 3 tiers, a handful of authored cells, one “master cell” fallback for level authors.
8. **Floor destruction:** allow only explicitly tagged elevated floors, or never? Recommend tagged-only.
9. **Splat target:** screen-space painterly (safe) vs. world-space cached (ambitious). Recommend prototype then decide.
10. **Multiplayer scope:** co-op only, or races? Co-op first.
11. **Signaling hosting:** Cloudflare Worker vs. small VPS; who pays for TURN?
12. **Asset authorship:** who makes board/kart/grapple/rocket/jetpack models and sounds? Placeholder geometry first.

---

## 11. References

- Conker 3D Stereo: https://github.com/agrofubris/conker-3d-stereo — release workflow (`.github/workflows/release.yml`), launcher ROM flow, `recomp/README.md`.
- N64ModernRuntime ROM handling: `librecomp/src/recomp.cpp` (`select_rom`, `load_stored_rom`), RecompFrontend `recompui/src/base/ui_launcher.cpp`.
- Ghostship (Android on-device SM64 extraction): https://github.com/HarbourMasters/Ghostship — `android/.../LauncherActivity.kt`, `GameAssets.kt`, `src/port/android/AndroidBridge.cpp`, `src/port/GameExtractor.cpp`.
- Skate 3 recompilation: https://github.com/mchughalex/skate3recomp ; decompilation project: https://github.com/Skate-Modding-Team/Skate-Decompilation-Project ; rewrite: https://github.com/SK8-ENGINE/skate-3-rust-engine .
- Skate 3 Flick-It controls/manuals: Pro Game Guides, GameSpot, Operation Sports, skate3online controls (see search results in research notes).
- MK8 drift mechanics: Prima Games driver's manual; Vike's MK8DX drifting guide (charge rates/thresholds); Super Mario Wiki “Underwater driving” and “Drift.”
- MK8 recompilations (not decomps): Vitor-Vaz/MK8DRecomp, dougchansan/mk8-recomp, c22dev/mush8, HelixNX/mk8dx.
- Titanfall 2 movement: community movement guides (wall-run 1.75 s, ~28–30 km/h cap, regrab rules, camera tilt), TF2SR wiki, Momentum Mod parkour cvars (`sv_wallrun_*`).
- Dreams BubbleBath: Wikipedia “Dreams (video game)” (Flecks, BubbleBath), Media Molecule engine-name announcement, Game Developer interview.
- 3D Gaussian Splatting: Kerbl et al., “3D Gaussian Splatting for Real-Time Radiance Field Rendering” (for the view-dependent/alpha-blend principles).
- Oil-paint inspiration: https://patch.gg/blendi/oil-paint (stroke uniformity/randomness idea for debug controls).

---

*End of plan.*
