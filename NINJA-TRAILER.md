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
| BOX_BALL | (-150, 240, -810) | cube stack | 145 |
| PUNCH | (-70, 185, -940) | cube stack | 125 |
| KICK | (-820, 220, -960) | cube stack | 125 |
| SLAM | (-430, 70, -1000) | impact point | 190 |
| JET_L1 | (-340, 120, -1310) | Mario | 165 |
| WALLRUN | (-730, 300, Mario.z+230) | Mario | 150 |
| GRAPPLE_AIM | (-1120, 640, -2260) | N3 anchor | 175 |
| GRAPPLE_HOLD | (-980, 560, -2360) | Mario | 160 |
| STAR1 | (700, 520, -2680) | Mario | 95 |
| JET_N5 | (620, 340, -2280) | Mario | 165 |
| PORTAL_RUN | (1500, 1160, -2440) | N7 portal | 155 |
| PORTAL_FALL | (-40, 1880, -2600) | ceiling portal | 175 |
| STAR2 | (-300, 1730, -2800) | Mario | 95 |
| GOOMBA | (-660, 1830, -2800) | landing | 135 |

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

### Next
1. **Cube pyramid beat**: spawn the real 4-layer 30-cube Box3D pyramid at the
   course start (`boxphys_test_pyramid`), point the ball/punch/kick/slam at it,
   and verify the cubes visibly fly on the slam.
2. **Impact feel**: short slow-mo/time-scale dip on the ground-pound, grapple
   apex, portal drop and star grab; a decaying camera shake on the slam.
3. **Presentation**: hide HUD/touch overlay for the take, optional letterbox
   bars, and `adb shell screenrecord` (flat 2D) for the actual teaser file.
4. **Goomba finale**: three lemmings in a row + low wide shot; confirm the
   ceiling portal is readable in frame.
5. **Pass**: record 25–35 s, check the dump, review the beats, iterate the
   camera table above.

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
