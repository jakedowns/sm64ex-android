# NINJA TRAILER — cinematic plan for the chained smoke test

## One-line verifiable goal

**One continuous ~30 s screen recording of the ninja run in level 38 shows every beat framed — cube-pyramid impact, wall-run, grapple, jetpack, ceiling-portal drop, both stars, goomba lemming loop — while the run passes `flags=0x7FF`, `star=1`, `star2=1` and goomba crossings > 0.**

That is checkable from (a) the final `cmd dump` and (b) the recorded video / timed
screenshots, without a human guessing what “looks good” means.

## Vision

Turn the smoke test into an **epic teaser**: one take, big readable beats,
deliberate camera language, impact moments, and no dead air.  The existing run
proves the systems; this plan makes it *look* like a trailer.

## Beat sheet (one continuous take)

| # | beat | action | camera language |
| --- | --- | --- | --- |
| 1 | Cube hero | ball blast the top of a 4-layer Box3D pyramid, punch a cube, jump kick, ground-pound the base | low 3/4 wide → side angle → low wide on the slam; FOV widens on impact |
| 2 | Lift-off | jetpack up to the wall-run pad | camera below/behind, looking up; FOV wide |
| 3 | Wall-run | jump into the wall, 52-tick run south | side-tracking camera west of the wall, trailing in Z; speed sells the run |
| 4 | Grapple | aim at the N3 anchor, attach, swing across the gap | wide shot with the gap, anchor and rope in frame; focus the anchor |
| 5 | Star 1 | collect the N4 star | close side orbit; FOV tightens |
| 6 | Jetpack 2 | climb east to N5 | low angle from N5's edge, tracking |
| 7 | Portal | shoot the N7 portal + the ceiling portal, run in, fall out of the ceiling | show the portal; pan up to the ceiling portal for the drop |
| 8 | Star 2 | collect the N6 star | close orbit |
| 9 | Lemming finale | wall portal in front of a goomba + ceiling portal; goomba falls in/out in a loop | low north-side spectator, focus the landing |

## Camera path (implemented)

`ptc_chain_camera()` in `app/jni/src/src/game/ptc.c` drives one eased freelook
camera per chain phase (position, focus, FOV).  It glides between shots with a
per-phase ease factor, so there are **no cuts**.  `WALL NOCLIP` keeps walls out
of the way and hides the one between camera and Mario.

| chain phase | camera | look-at | FOV % |
| --- | --- | --- | --- |
| BOX_BALL | (-200, 400, -500) | ball target / pyramid | 130 |
| PUNCH | (-100, 380, -560) | pyramid | 125 |
| KICK | (-320, 380, -620) | pyramid | 125 |
| SLAM | (-450, 240, -680) | pyramid base | 165 |
| JET_L1 | (-120, 260, -1900) | Mario | 145 |
| WALLRUN | (-730, 300, Mario.z+230) | Mario | 150 |
| GRAPPLE_AIM | (-1120, 640, -2260) | N3 anchor | 175 |
| GRAPPLE_HOLD | (-980, 560, -2360) | Mario | 160 |
| STAR1 | (700, 520, -2680) | Mario | 95 |
| JET_N5 | (620, 340, -2280) | Mario | 165 |
| PORTAL_RUN | (1550, 1260, -2600) | N7 portal | 112 |
| PORTAL_FALL | (430, 1560, -2980) | (-600, 1980, -2980) | 168 |
| STAR2 | (250, 1620, -2980) | (-700, 1650, -3080) | 130 |
| GOOMBA | 780-radius arc, north -> east, y=1900 | (-640, 1620, -3000) | 135 |

FOV % is a percentage of the game's base FOV (100 = 45°).

## Work plan

### Done
- [x] Continuous scripted run with all systems; one camera (`cmode=3`).
- [x] Cinematic camera track per phase (above), eased, no cuts.
- [x] WALL NOCLIP + occluding-wall hide so wide shots stay readable.
- [x] Green per-phase note; UI trigger (ENH → PORTAL → RUN NINJA CHAIN).
- [x] Fast-forward harness so timing passes can be checked in ~2 s.
- [x] Cinematic camera integrated with the chain: fast run PASS 8/8
      (`flags=0x7ff`, both stars, crossings 41, cmode=3).
- [x] **Cube-pyramid beat**: `boxphys_test_pyramid` spawns the real 30-cube
      pyramid at the course start; the ball blaster gets a separate lone cube so
      the pyramid survives for the punch/kick/slam.  Found and fixed the root
      cause of the cubes vanishing: the Box3D static ground was only 18×18 m at
      (1800,400), so the course cubes fell into the void - it is now a
      64×56 m slab covering the chamber.
- [x] Beat frame sweep on device: pyramid hero shot, cube impact, wall-run,
      jetpack, N4 star, N5 landing, goomba (t_5 shows Mario + the goomba).
- [x] **Accepted continuous take**: 34 s screenrecord, HUD hidden, all eight
      beats framed, `flags=0x7ff`, `star=1`, `star2=1`, 27 goomba crossings,
      `cmode=3`.  Slow fall (13 u/tick) makes the ceiling drop readable;
      three staggered goombas + a north-side drifting camera carry the finale.
- [x] **Seamless portal crossing** (portal-seam subagent, same tree): wall,
      back and ceiling crossings PASS with 8.8/10.0/9.8 px screen jumps,
      height ratio 1.00, orientation delta < 0.3 deg; handoff is skipped for
      the chain's freelook camera.  Fast chain PASS 8/8, crossings 117.

### Take 5 - final take (accepted)

Files:

- `/tmp/opencode/ninja-take5.mp4` - raw 33.97 s screenrecord (1600x1000).
- `/tmp/opencode/ninja-teaser-30s.mp4` - **deliverable**: pre-roll-trimmed
  **30.0 s** continuous take (stream copy).
- `/tmp/opencode/ninja-beats-30s.png` - 10-beat contact sheet from the 30 s
  file; `/tmp/opencode/ninja-beats-final.png` is the same sheet from the raw
  take.

Device dump at the end of the take:

```
chain=26,406,0x7ff,27,0    (goomba loop, flags 0x7ff, 27 crossings)
star=1  star2=1  cmode=3
```

Beat timings measured from the take:

| t | beat | framed? |
|---|------|---------|
| 0.0-1.0 | start, cube pyramid hero shot | yes |
| 1.0-6.5 | ball, punch, kick, ground-pound into the 30-cube pyramid | yes, cubes fly |
| 7.0-8.5 | wall-run along the chamber wall | yes |
| 10.0-11.5 | grapple swing to N4 + star 1 | yes |
| 12.0-14.5 | jetpack climb, N4/N5 | yes |
| 15.0-16.8 | run into the N7 gate portal (blue fan) | yes |
| 17.0-19.6 | ceiling drop: orange ceiling portal, slow float down, N6 star below | yes |
| 19.6-21.0 | N6 star 2 pick-up | yes |
| 21.0-30.0 | goomba lemming loop, three staggered goombas, north-side drifting finale camera (no level edge in frame) | yes |

HUD: `cmd hud --ei v 1` hid the touch overlay and the tuning bar for the take.

Functional acceptance on the same build:

```
scripts/ninja-chain-test.py --fast --section chain --ticks 2400
RESULT: PASS chain (8/8 checks)
  flags=0x7ff  star=1  star2=1  crossings=118  cmode=3
  2400 ticks in 147 ms (544x real time)
```

Known cosmetic gaps (not blocking): the portal relay is drawn as its flat fan
(blue/orange) because the through-view renders black over open air; the cube
beat has one fast camera transition at 6.5 s.  The finale holds on a mostly
green wall for the last few seconds (safe composition rather than a wide shot
over the level edge).

### Feedback v2 (reviewer notes) - fixed

| note | fix |
| --- | --- |
| ground pound happened in front of the pyramid, blocks clipped | beat order is now ball -> guided **jetpack hop onto the top cube -> ground pound** -> step off -> punch -> kick. Attacks target the nearest cube via `boxphys_nearest_box`; impulse reduced (slam 1200, punch 900) and Box3D substeps raised 6 -> 10 so cubes no longer interpenetrate. |
| no jetpack flames | 5-quad nozzle plume plus a 26-slot ageing exhaust trail, drawn camera-facing under the pack. |
| grapple hooked the green wall and vanished | grapple now anchors on the **room ceiling** (-150, 2780, -3000) with a scripted rising reel (330 -> 1900) and close tracking camera; the white rope stays attached for the whole climb. |
| power meter covered the frame | chain clears `HUD_DISPLAY_FLAG_CAMERA_AND_POWER`/`EMPHASIZE_POWER` for the take and restores it on stop. |
| punch/kick too far away | close tracking cameras (punch from +X, kick from -X) follow Mario and the cube he targets. |

Take: `/tmp/opencode/ninja-feedback-30s.mp4` (30.0 s, HUD-free), full capture
`/tmp/opencode/fb-take4.mp4`; beat sheet `/tmp/opencode/ninja-beats-feedback.png`.
Fast chain on this build: **PASS 8/8** (flags=0x7ff, stars 1/1, crossings 117).

### Feedback v3 (presentation) - in progress

| note | change |
| --- | --- |
| Mario floated on an invisible platform in the goomba finale | Root cause: the ninja-course display lists (`ptc_dl_l1`, `ptc_dl_n1..n8`) existed but were never referenced by the level geo. They are now GEO_DISPLAY_LIST nodes, so L1/N1-N8 (including N6) are visible. |
| goomba section should be a portal timing gag | One goomba walks at Mario; the chain shoots a **floor portal** between them just before it arrives and a **ceiling portal** above it. The goomba drops through the floor, comes out of the ceiling and dies on the ~1290-unit fall, then the next one respawns. New two-shot finale camera: stand-off, then tilt up for the drop. |
| wall-run looked like running in mid-air | ACT_WALLRUN now rolls Mario's model onto the wall plane (faceAngle[2]/gfx.angle[2] = side * 90 deg) and resets on exit, so his feet read as planted. |
| section captions mid-video | `ptc_chain_hud_text()` prints short labels in the game's HUD lettering via `print_text_centered` from `render_hud`: BOX3D PYRAMID, GROUND POUND, PUNCH, KICK, ROCKET (J has no glyph), WALL RUN, CEILING GRAPPLE, PORTALS, STAR, GOOMBA LOOP. |
| grapple should swing from the ceiling like Spider-Man | Scripted pendulum under the ceiling anchor: R 2450 -> 2070, angle -6 -> +16 deg, wide side camera; releases onto N4/star 1. |
| sound | Not capturable with current tooling: Android 12, `screenrecord` has no audio, no aarch64 scrcpy build, and shell RECORD_AUDIO + tinycap still writes a header-only wav. The deliverable stays silent. |
| next video portrait | Superseded by the user: no portrait crop; re-encode to 60 fps only. |

### Take 10 - presentation cut (landscape, 60 fps)

- `/tmp/opencode/g-take10.mp4` - raw 38 s screenrecord (1600x1000, ~95 fps).
- `/tmp/opencode/ninja-final-60fps.mp4` - 37.1 s, **1600x1000, 60 fps** re-encode
  (no portrait crop): https://gofile.io/d/4hzBk3J0
- Run proof: `chain=26,469,0x7ff,3,0`, `star=1 star2=1 cmode=3`.
- Includes: visible L1/N1-N8 platforms, ground pound on the pyramid then
  punch/kick, ROCKET/WALL RUN/CEILING GRAPPLE/PORTALS/GOOMBA LOOP captions in
  the HUD lettering, wall-run body roll, goomba floor->ceiling portal gag with
  the two-shot finale camera.
- Audio: none. Android 12 screenrecord has no audio; no aarch64 scrcpy build;
  shell RECORD_AUDIO + tinycap writes a header-only wav.

### Take 14 - audio + polish cut

- `/tmp/opencode/g-take14.mp4` + `g-take14.wav` (38 s) -> 
  `/tmp/opencode/ninja-final-audio-60fps.mp4`: 37.15 s, 1600x1000, **60 fps,
  AAC stereo audio**: https://gofile.io/d/LXhOBCUf
- Fixes in this cut:
  - goombas render again (the chain rig was using `bhvGoomba`; the AI
    moved/despawned them while the dump still showed a live object. Switched to
    `bhvStaticObject` with manual placement).
  - goomba-loop camera is one continuous follow shot derived from the goomba's
    live position (k=0.07), no more snap between floor/ceiling framings.
  - captions are half-size and uncropped: the caption pass doubles the
    per-pixel S/T step (`8<<10, 2<<10`) so the whole 16x16 glyph fits the 8x8
    rect; SWING / RETRACT / RELEASE label the grapple phases.  RELEASE
    evidence: `/tmp/opencode/release_caption.png` (phase 17 captured with
    `cmd pause` + `cmd step`).
  - L1/N1-N8 moved into the occluder fade system (GEO_ASM 30-38, mask bits
    20-28, ray boxes), so course geometry can ghost instead of blocking.
    Evidence: `ptc fadetest` puts N8 between the camera and Mario;
    `/tmp/opencode/fade_off.png` (N8 opaque, Mario hidden) vs
    `/tmp/opencode/fade_on.png` (N8 hidden, Mario visible).  Debug actions
    `fadeon`/`fadeoff` toggle the system for before/after captures.
  - audio: in-app PCM tap (`cmd audiorec --ei v 1|0`, `audio_sdl_play` writes
    `files/audio_rec.wav`), muxed with the 60 fps video. Device volume was not
    raised (see TESTING.md 1b).
- Run proof: `chain=26,589,0x7ff,3,0`, `star=1 star2=1 cmode=3`; audio levels
  mean -24.4 dB / max -0.5 dB.

### Take 17 - griefing loop + splat 360 finale

- `/tmp/opencode/g-take17.mp4` + `g-take17.wav` -> `/tmp/opencode/ninja-final-splat-60fps.mp4`
  (43.2 s, 1600x1000, 60 fps, AAC; mean -25.4 dB / max -0.5 dB). Not uploaded
  (third-party uploads paused).
- N6 widened to x -1500..-300, z -3200..-2600 (2.4x area) and the level regenerated.
- Goomba: one goomba waddles in, the blue ceiling portal appears above the orange
  floor portal, it loops through **8 accelerating passes**, then the blue exit is
  shifted one portal width sideways (`SHIFT! BULLET`) and the next pass fires it
  into the hard ground beside the floor portal where it splats and fades.
- Portal fans are now orange on the floor / blue on the ceiling, with a 0.10/tick
  scale-up reveal on placement.
- Grapple: retract is 2x longer (105-tick hold), the rope shoots from Mario's
  hand to the ceiling and whips back into his hand on release.
- Finale: `SPLAT 360` - 360-degree orbit of the chamber with splat rendering on
  all objects, density lerped 20 -> 100 over the arc, ellipse splats switching to
  paint strokes at the 180-degree mark; the orbit holds at the end.
- Run proof: `chain=27,572,0x7ff,9,0`, `star=1 star2=1 cmode=3`.

### Next
1. **Impact feel** (polish): short slow-mo/time-scale dip on the ground-pound,
   grapple apex and the two star grabs; a decaying camera shake on the slam.
2. **Presentation polish**: optional letterbox bars; tidy the 6.5 s box-beat
   camera transition.
3. **Portal window**: the relay through-view is black over open air, so the
   take uses flat fans.  Fix the relay (clip-plane / virtual-camera culling)
   and re-shoot the portal run and ceiling drop with real windows.
4. Re-record only after a visual change; the current take is the accepted one.

## Take 1 review (30 s, `/tmp/opencode/ninja-teaser.mp4`)

Decoded at 2 fps with the static ffmpeg (`/tmp/opencode/ffmpeg-7.0.2-arm64-static/ffmpeg`).

- 0.0-6.5 s cube pyramid + ball/punch/kick/slam: framed, impact visible.
- 7.0-8.5 s wall-run: framed but the camera was still settling.
- 9.0-11.0 s grapple: too wide, Mario small.
- 11.5-14.5 s star1 + jetpack: good.
- 15.0-16.5 s portal run: Mario small; portal window rendered **black**.
- 16.5-17.0 s portal drop: lost behind a grey slab (camera mid-swing).
- 17.0 s star2: collected during the transition - never framed.
- 17.5-30 s goomba loop: one static low shot, empty green field, black portal
  hole; no goomba for long stretches.

Fixes applied for take 2:

- Portal run camera closer behind Mario, gate portal on the right.
- Portal fall camera moved east & low, focused on the falling Mario so the
  ceiling portal face and N6 below are both in frame.
- Star2 camera three-quarter on N6; 50-tick hold after the pickup.
- Three staggered goomba lemmings and a slow north->east arc for the finale.
- Relay through-view disabled for the take (`portal_view_set(0)`) so portals
  draw their flat fan instead of a black hole; the stop action restores it.
- `cmd hud --ei v 1`: hides the touch overlay and collapses the tuning panel.

## Progress log

- 2026-11-21: cinematic camera track + `NINJA-TRAILER.md` (0528a9d).
- 2026-11-21: ENH PORTAL 3 columns + RUN NINJA button (7bcb2a4).
- 2026-11-21: real 30-cube pyramid at the course start; Box3D ground enlarged
  so the cubes no longer fall through; ball target split to a lone cube; box
  camera held wide; slower attack pacing.  Fast chain still PASS 8/8.
- Device beat sweep: key frames in `/tmp/opencode/e_{1,3}.png` (pyramid +
  impact), `s_2.png` (wall-run), `s_4.png` (N4 star), `s_5.png` (jetpack),
  `t_5.png` (goomba + Mario).
- 2026-11-21: take 1 recorded (29.96 s, `screenrecord`, 1600x1000) with
  flags=0x7ff, star=1, star2=1, crossings=9, cmode=3.  Frame review found the
  late beats unframed and the portal relay black; take 2 changes above.
- 2026-11-21: takes 2 and 3 recorded and frame-reviewed.  Final late-beat
  camera table, three staggered goombas, star2 hold, flat portal fans and
  `cmd hud` landed here.
- 2026-11-21: take 4 recorded; finale arc swung south of N6 and showed a
  black level edge, so the arc was constrained north of the platform.
- 2026-11-21: **take 5 accepted** - 33.97 s, HUD-free, all beats framed, no
  void in the finale; dump flags=0x7ff, star=1, star2=1, crossings=27,
  cmode=3.  Trimmed to the 30.0 s deliverable
  `/tmp/opencode/ninja-teaser-30s.mp4`.
- 2026-11-21: fast chain re-verified on the take-5 build: **PASS 8/8**
  (flags=0x7ff, stars 1/1, crossings 118, cmode=3, 544x speedup).
- 2026-11-21: portal-seam subagent landed the camera-handoff fix (rotation
  around Mario instead of a room-crossing swoop) and verified wall/back/ceil
  crossings at 8.8/10.0/9.8 px; fast chain PASS 8/8, crossings 117.  The
  changes are in the working tree (uncommitted) and were part of take 5.

## Verification

```bash
# functional acceptance (fast)
scripts/ninja-chain-test.py --fast --section chain --ticks 2400
# expected: PASS 8/8, flags=0x7ff, star=1, star2=1, crossings>0, cmode=3

# visual acceptance
adb shell screenrecord --time-limit 30 /sdcard/ninja.mp4   # pull afterwards
# or timed captures at each beat; check Mario + action are in frame
```

A run is only “trailer-ready” when the recording shows each beat framed **and**
the dump still passes.  Functional regressions win over beauty: never trade the
0x7FF/stars/crossings checks for a shot.

## References

- `NINJA-CHAIN.md` — course layout, phases, per-section commands, evidence.
- `TESTING.md` — framing toolbox (distance/FOV/noclip), capture and lock rules.
- `faster-iteration-idea.md` — `runticks`/trace harness and the host-build option.
