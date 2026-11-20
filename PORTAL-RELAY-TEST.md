# Portal relay automated test

The chamber walls use a regular checkerboard, which is the known pattern for
the relay test.  A working relay shows the checkerboard/room through portal A;
a missing relay shows the flat blue portal fan.

## Run

```bash
scripts/portal-relay-test.sh /tmp/opencode/portal
```

The script:

1. loads the Portal Test Chamber,
2. runs `ptc portaltest` — places A/B, forces A as the relay source and locks a
   fixed camera in front of A,
3. captures the **left eye** with 2D/same-L (`ptc eyeL` defaults),
4. switches to the **right eye** with `ptc eyeR`,
5. analyses both images with `scripts/portal-relay-test.py`.

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
