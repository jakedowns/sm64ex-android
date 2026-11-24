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

`scripts/motionblur-perf-test.sh` drives the PTC light loop (counter-rotating
camera = fast vectors) through profiles and records a video plus a frame-time
table:

| segment | profile |
| --- | --- |
| 01_off | motion blur off (baseline) |
| 02_cheap | 1 sub-step, strength 60, velocity 1/8, update every 2 frames, no jitter |
| 03_balanced | 4 sub-steps, strength 110, velocity 1/4, every frame, jitter 100 |
| 04_quality | 8 sub-steps, strength 160, velocity 1/2, shutter 320, max 48 px |
| 05_camera_vectors | balanced with the camera-only fallback (fx 140 0) |
| 06_vertex_vectors | balanced with per-vertex vectors (fx 140 1) |
| 07_heatmap | motion-vector heatmap (view 9, direction style) |
| 08_off_again | back to off |

Each segment saves a still and a dump; `results.txt` records EMA frame ms and
fps. The on-screen caption line (second caption, quarter size) shows
`PATH/DN/rate | MB s<strength> x<sub-steps> r<velocity divisor>` and appends
`VIEW VECTORS` in the heatmap segment, so every recorded segment is
self-labelling even while the LIGHT LOOP label is on screen.
