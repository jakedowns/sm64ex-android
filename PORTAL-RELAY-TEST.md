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
