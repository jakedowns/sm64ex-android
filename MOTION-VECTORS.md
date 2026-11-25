# Motion vectors: implementation notes

Status: shipped. The FX pass no longer guesses motion from depth; a second
scene replay writes a real per-object velocity buffer that drives both the
blur and the debug heatmap.

## Pipeline

1. **Velocity replay** (`gfx_pc.c`) — once per frame, before the eye-0 colour
   pass, the scene range is replayed in `s_vel_pass` mode into a small target.
   `gfx_sp_tri1` then emits opaque, depth-writing triangles as six floats per
   vertex: `x, y, z, w, velx, vely`.
2. **Per-object previous transform.** The display list lives in a per-frame
   pool, so matrix pointers are not stable across frames (measured: 0/18k
   hits). Vertices are transformed on the CPU by the same `rsp.MP_matrix` the
   colour pass uses, so instead the Nth model-view load of a pass is matched
   with the Nth load of the previous pass (`s_vel_seq`), with a 1-2 frame
   window and a velocity scale by the gap, plus a translation sanity check
   (more than 6000 units apart is treated as a despawn/spawn, not motion).
   Measured hit rate: 98-99%.
   `s_vel_prev_mp = prev_modelview * prev_projection` and the per-vertex
   velocity is `0.5 * (prevNdc - curNdc)`.
3. **Target** (`gfx_opengl.c`) — RGBA8 at the eye canvas divided by the
   `Velocity res 1/n` dial, with a 16-bit fixed-point encoding per axis
   (`hi, lo, hi, lo`) so sub-pixel motion survives. The viewport is scaled by
   the same divisor while `s_vel_rendering` is set. Clear value decodes to
   zero motion.
4. **Blur** — `uVelOn` makes the FX shader read the buffer instead of the old
   camera reprojection. The vector is clamped (`Velocity clamp px`), scaled by
   `Shutter deg` and strength, and sampled in `Sub-steps` taps with optional
   per-pixel jitter. Taps whose depth differs by more than `Depth reject %`
   are rejected so foreground/background do not bleed. The kernel is also
   bounded by a tile-max texture (8x8 max velocity, 3x3 neighbourhood), so one
   fast pixel cannot smear the whole screen.
5. **Heatmap** (view 9) — colour from the same buffer, with `Heat style`
   (0 magnitude, 1 direction hue, 2 one-pixel segments) and `Vector heat
   gain`.

## Controls (FX keys)

| key | dial | range | default |
| --- | --- | --- | --- |
| 130 / 140 | vector source: 0 camera-only, 1 per-vertex | 0-1 | 1 |
| 131 / 141 | shutter angle (deg) | 0-360 | 180 |
| 132 / 142 | sub-steps along the vector | 1-8 | 4 |
| 133 / 143 | max blur radius (px) | 0-64 | 16 |
| 134 / 144 | velocity target divisor | 1-8 | 4 |
| 135 / 145 | replay every N frames | 1-4 | 1 |
| 136 / 146 | sub-step jitter (%) | 0-100 | 100 |
| 137 / 147 | depth rejection (%) | 0-100 | 20 |
| 138 / 148 | velocity clamp (px) | 4-200 | 60 |
| 139 / 149 | heatmap style | 0-2 | 2 |
| 118 | heatmap gain (px = full scale) | 20-500 | 120 |

The odd-looking "130 / 140" is intentional: the LIGHT-tab dials use 140-149,
while 130-139 remain accepted on the adb channel for older scripts. (120-135
are per-light colours.)

UI: the old top-level LIGHT tab is now the **LIGHT** page of the **EFFECTS**
sub-bar (GENERIC / MXAO / C.DOF / SSGI / SSRTX / SHADOW / SPLAT / LIGHT).
Its BLUR page carries the motion-vector dials.

## Cost

The replay is CPU vertex transform + a small raster: on the Lume Pad 2 it
costs roughly 10-15 fps in the PTC at 80-120 fps, and only runs when motion
blur is on or the heatmap view is selected.

## Limitations / next steps

- Rigid object motion only: the previous modelview is matched per draw. SM64
  gives each limb its own node matrix, so limb rotation is captured; vertex
  morphs inside a single node are not. Deformation vectors would need previous
  vertex positions (double-buffered Vtx) or a depth-reprojection hybrid.
- Tile-max / neighbour-max is an 8x8 tile pass + 3x3 neighbourhood; a full
  McGuire implementation would also reconstruct scattering, not just bound the
  kernel.
- The velocity target is RGBA8 packed; RG16F would be simpler if
  `EXT_color_buffer_half_float` is confirmed on the driver.

## Smoke test

`scripts/motionblur-perf-test.sh` runs the **blur showcase** (`ptc blurshow`):
a 10 s wall-clock camera orbit around Mario, repeated backflips with the
simulation at 1/3 speed (the renderer keeps presenting and HFR interpolation
smooths it), lights/probes/shadows/AO/DOF/GI/SSRTX disabled, and capture in
2D so the video is a single image. The orbit gives foreground/background
parallax; Mario is the only moving object, so the blur is unambiguous.

Profiles and measured frame time on the Lume Pad 2:

| profile | frame | fps |
| --- | --- | --- |
| 01_off | 8.3 ms | 121 |
| 02_cheap (1 tap, 1/8, stride 2) | 9.3 ms | 108 |
| 03_balanced (4 taps, 1/4, shutter 270) | 9.1 ms | 110 |
| 04_quality (8 taps, 1/2, shutter 360) | 10.8 ms | 93 |
| 05_heatmap (view 9, segments) | 10.3 ms | 97 |
| 06_off | 8.3 ms | 120 |

The slow-motion sim keeps per-frame vectors small, so this scene is cheaper
than the earlier light-loop capture (off 15.5 ms, quality 19.1 ms in 2D).
The heatmap segment is the vector evidence; the quality segment shows the
smear most clearly.

The same matrix in 3D costs roughly twice as much (off 32.7 ms, balanced
96.2 ms, quality 113.1 ms) because the post pass runs per eye.

Reading: one tap is nearly free, and 2D keeps even the quality profile above
50 fps. In 3D the first tap's fixed cost (velocity + tile-max reads) dominates;
the camera-only path at the same tap count was 2.6x cheaper than per-vertex,
which points at the tile-max (9 samples/px) and velocity fetches as the next
optimization targets. The on-screen caption line (second caption, quarter size) shows
`PATH/DN/rate | MB s<strength> x<sub-steps> r<velocity divisor>` and appends
`VIEW VECTORS` in the heatmap segment, so every recorded segment is
self-labelling even while the LIGHT LOOP label is on screen.

## Parameter sweep

`scripts/blur-sweep-test.sh` records the same showcase while sweeping one dial
at a time; each segment is labelled on screen with `ptc note <text>` and the
live `MB s… x… r…` caption. Ceilings were raised for this: strength 0-300,
sub-steps 1-16, max blur 0-128 px, velocity clamp 4-300 px.

| sweep | values | frame time |
| --- | --- | --- |
| strength | 40 / 100 / 160 / 220 / 300 | 8.5 / 12.4 / 10.6 / 11.1 / 11.0 ms |
| sub-steps | 1 / 2 / 4 / 8 / 16 | 8.4 / 10.0 / 10.8 / 10.8 / 10.9 ms |
| shutter | 90 / 180 / 270 / 360 | 10.6 / 11.9 / 10.9 / 10.9 ms |
| max blur px | 8 / 24 / 64 / 128 | 10.8 / 11.2 / 10.0 / 10.0 ms |
| heatmap | view 9 segments | 9.5 ms |

At these settings the showcase stays above 80 fps; the 1-tap profile is
effectively free (8.4 ms). `ptc blurshow` now uses custom camera mode 3 (the
free-look module), so the orbit actually moves.

## Cost knob: vector bound

`fx 240` (LIGHT -> BLUR, "Vector bound 0/1/2") controls the tile-max bound:

| mode | what | measured fps (2D showcase, quality profile, pacer off) |
| --- | --- | --- |
| 2 | 3x3 neighbour max (previous behaviour) | 92-102 |
| 1 | one tile sample (default) | 97-107 |
| 0 | off: skips the tile-max pass and all bound samples | 155-160 |
| blur off | baseline | 357 |

Mode 0 is the cheapest and still clamps the vector at `Velocity clamp px`,
but loses the neighbour dilation that keeps foreground motion from bleeding
over background. Mode 1 (default) keeps a bound for one fetch per pixel.

## Sweep contact sheet

`scripts/blur-sweep-test.sh` keeps, for each sweep step, the still taken at the
fastest point of the scripted orbit: it dumps `cam=` twice and keeps the shot
with the largest camera displacement. `ms`/`fps` in `results.txt` come from the
`fps=` field of the status line (real composited frames, not the `rate=` EMA).

`scripts/contact-sheet.py <artifact-dir> <out.png> [columns]` tiles one still
per step with the sweep label, the frame time and the measured motion:

```
python3 scripts/contact-sheet.py test-artifacts/blursweep-<stamp> sheet.png 5
```

## Slow motion and blur

Simulation slow motion (`fx 241`, sim speed `1/n`) is compatible with the blur
because the renderer interpolates: only tick 0 of each group of `n` rebuilds the
display list, and the frozen ticks re-present it at `k/n` of the step through
the HFR matrix table, so every presented frame carries a `1/n` share of the
motion. Without that (`fx 242 0`, the old behaviour) two of every three frames
at `n = 3` were the same picture: velocity zero, blur off, then one frame with
three steps of motion — the blur/still/blur cadence.

Measured on the blur showcase (`/media/share/tmp/artifacts/sm64/slowmo-interp/`):

| config | still frames (tblend YAVG < 0.03) | presents with zero motion |
| --- | --- | --- |
| `n = 1` | 3.0 % (recorder floor) | 0 / 120 |
| `n = 2`, interp | 4.9 % | 0 / 120 |
| `n = 3`, interp | 5.1 % | 0 / 120 |
| `n = 3`, no interp | 53.1 % | 110 / 120 |

Per-vertex vectors are stamped once per *presented* frame, so they already
describe the sub-frame step and must not be scaled again; only the camera-only
fallback vectors (built from a matrix refreshed per sim step) get `1/n`
(`uVelScale`). Limb animation still steps at the sim rate: the root transform
and the camera interpolate, the animation frame does not.

