# Portal relay automated test

The chamber walls use a regular checkerboard, which is the known pattern for
the relay test.  A working relay shows the checkerboard/room through portal A;
a missing relay shows the flat blue portal fan.

## Run

```bash
scripts/portal-relay-test.sh color   # stage 1
scripts/portal-relay-test.sh scene   # stage 2
```

Two stages, one command/capture each:

1. **Color (default)** — `ptc portaltest` places the portals, puts Mario in a
   stable view of A, forces A and fills the relay FBO **solid green** while
   skipping the scene pass. The decal should be a green ellipse exactly on the
   portal. This isolates geometry/UVs from the destination camera.
2. **Scene** — `portalcolor` toggles the scene pass back on; only after stage 1
   passes is it worth debugging the relay content.

You can also switch eyes manually:

```bash
B="am broadcast -n com.vdavid003.sm64port/.DebugReceiver -a com.vdavid003.sm64port.CMD"
adb shell $B --es cmd ptc --es action eyeL
adb shell $B --es cmd ptc --es action eyeR
adb shell $B --es cmd ptc --es action eyeOff
```

## Metrics

- `edge_density` / `std`: confirms there is a texture rather than a flat fan.
- `period_x` / `period_y`: dominant checker spacing (should be close).
- `edge_ratio_hv`: horizontal/vertical edge energy; ~1 for square checks,
  large deviations indicate stretching.
- `pair_mean_abs_diff`: left/right eye difference.
- `flat_blue_distance`: closeness to the old solid portal colour.

`ptc portalblit` shows the relay FBO full-screen; use it to separate a bad
camera transform from an overlay/composite problem.

## Knobs (no rebuild)

The empty test level only bakes a collision floor. The room, floor quadrants
and the checker backdrop are generated at runtime by `src/game/ptc_scene.c`, so
every visual parameter is tunable over adb:

```bash
scripts/ptc-knob.sh                          # print the current values
scripts/ptc-knob.sh scene.chart_z 1200
scripts/ptc-knob.sh scene.chart_cols 24 scene.chart_rows 24
scripts/ptc-knob.sh --reload                 # re-read ptc.cfg
```

| knob | meaning |
| --- | --- |
| `scene.on` | draw the runtime test scene at all |
| `scene.room` / `scene.height` | room half-extent (X/Z) and height |
| `scene.chart_z` / `scene.chart_w` / `scene.chart_h` / `scene.chart_y` | relay backdrop placement |
| `scene.chart_cols` / `scene.chart_rows` | checker density (max 24x24) |
| `decal.offset` | decal nudge off the wall, world units |
| `decal.depth` | 1 = depth-test the relay decal |
| `select.maxdeg` / `select.facing` | portal selection cone / back-face rejection |
| `relay.solid` / `relay.blit` / `fan.src` | debug views (solid relay, FBO blit, magenta source fan) |
| `eye` | 0 stereo, 1 left-eye only, 2 right-eye only |
| `pov.dist` | POV rig: camera distance from the viewed portal plane |
| `pov.height` | POV rig: camera height (default 90, Mario's mid-body) |
| `pov.mario` | POV rig: Mario's distance in front of the destination portal |
| `pov.freeze` | POV rig: hold idle frame 0 for a repeatable silhouette |
| `pov.stereo` | POV rig: 1 keeps stereo, 0 captures flat (single eye) |

`ptc reload` reads `ptc.cfg` from
`/sdcard/Android/data/com.vdavid003.sm64port/files/ptc.cfg` or
`/data/local/tmp/ptc.cfg`, one `name value` pair per line.
`cmd dump` echoes every active knob on the `PTC ... knobs ...` line, so a
screenshot always says how it was configured.

## ENH tab controls

The ENH tab is grouped into sections and drives the same knobs live:

| section | controls |
| --- | --- |
| MOVEMENT / PHYSICS | ragdoll, wallrun, grapple, jetpack, fuel/refill upgrades |
| SESSION | level select, kill Mario |
| RENDER / SIM | uncapped sim, interp, stay in level |
| PORTAL TESTS | PTC warp, smoke/momentum/reset tests, jump calibration, portal gun |
| PORTAL TEST SCENE (LIVE) | empty test level, goomba demo, reload ptc.cfg, and toggles/cycles for `scene.on`, `scene.cubes`, `decal.depth`, `decal.offset`, `select.maxdeg`, `select.facing`, `eye`, `relay.solid`, `relay.blit`, `fan.src` |
| BALL / CUBES | reset to logo, reinit Leia, ball blaster, cube warps/tests |
| SYSTEM | pause game loop |

Knob changes are applied on the next game tick, so they only take effect once a
level is running (not on the title screen).

## Mario-in-the-portal POV rig

`ptc povcam` / `ptc povcam2` put the two portals on the walls of the SW corner,
park Mario in the destination space and pin a freelook camera on the viewed
portal's axis (`pov.dist` back, `pov.height` up). Everything is re-applied every
tick, so the idle/sleep camera and level resets cannot move the framing between
setup and screenshot.

```bash
B="am broadcast -n com.vdavid003.sm64port/.DebugReceiver -a com.vdavid003.sm64port.CMD"
adb shell "$B --es cmd ptc --es action povcam"     # view portal A
adb shell "$B --es cmd ptc --es action povcam2"    # view portal B
adb shell "$B --es cmd ptc --es action 'knob pov.mario 400'"
adb shell "$B --es cmd ptc --es action povoff"     # leave the rig
```

Both windows now show a complete Mario: the relay transform is conjugated into
view space (`K = V^-1 * C * V`, applied on the right of the modelview) so objects
keep their own matrices, and an oblique near plane at the source portal clips the
wall that used to cover the window.
