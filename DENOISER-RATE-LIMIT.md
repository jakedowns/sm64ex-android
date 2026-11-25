# Rate limiting + alternative denoised rendering path

Date: November 23, 2026 (reduced-resolution default added 2026-11-25)
Status: implemented, smoke-tested (see results below)

This is the implementation/decision record. The hardware and model survey with
sources is in `DENOISER-RESEARCH.md`; this file documents what shipped and how
to reproduce it.

## 1. Target hardware (summary)

| Item | Value |
| --- | --- |
| Device | Leia Lume Pad 2 (12.4", 2560x1600, 120 Hz lenticular) |
| SoC / GPU | Snapdragon 888 (SM8350) / Adreno 660 |
| Graphics API | GLES 3.2 driver; this port renders through its **GLES2** path |
| Memory / OS | 8 GB / Android 12 |
| Capture | 1600x1000, 30-60 fps; stereo per-eye FBOs ~1280x1534 |

The original brief assumed Snapdragon 8 Gen 1 / Adreno 730. On-device
`ro.soc.model`, `dumpsys SurfaceFlinger` and UL's 3DMark listing all say
SM8350 / Adreno 660. The plan was adjusted to the real device.

## 2. Rate limiting

Implementation: `gfx_sdl2.c` (`gfx_rate_*`).

- **Deadline pacer**: sleeps to an absolute next-frame deadline with
  `nanosleep`; an overrun resets the deadline instead of bursting to catch up.
- **Target**: 0 = inherit the render mode, 1-240 fps otherwise.
- **Adaptive** (optional): if the measured frame time stays above 1.4x the
  budget for ~45 frames, the effective target drops by 5 fps (floor 24); after
  ~180 fast frames it climbs back toward the configured target.
- **Stats**: EMA frame interval and EMA jitter, in microseconds, exposed in the
  dump (`rate=<target>,<adaptive>,<ema_us>`).
- The 30 Hz simulation accumulator is untouched; pacing only controls the
  present/render cadence, so gameplay timing stays deterministic.

Controls:

| adb key | meaning |
| --- | --- |
| `fx 200` | rate limiting on/off |
| `fx 201` | target fps (0 = mode) |
| `fx 202` | adaptive on/off |
| `fx 203` | reset the pacer/effective target |

## 3. Denoiser

### Selection

The device has no small, permissively licensed, real-time neural denoiser that
fits a GLES2 pipeline. From `DENOISER-RESEARCH.md`:

| Option | Verdict |
| --- | --- |
| Intel OIDN (Apache-2.0) | 0.25-0.9 s CPU + readback stall; not real-time |
| OpenCV NLM (Apache-2.0) | ~400 ops/px, spatially incoherent in stereo |
| CNN via NCNN/TFLite | ~2 TFLOP/frame at 1.6 MP; only viable at 1/8-1/16 res |
| NVIDIA NRD / QNN | proprietary licenses, compute-only |
| **SVGF / à-trous (shipped)** | GPU fragment cost, no new dependency |

**Shipped model**: a GLES2 edge-aware à-trous / bilateral filter inspired by
SVGF (Schied et al. 2017, "Spatiotemporal Variance-Guided Filtering"), with a
depth-edge weight and a colour-similarity weight. It is original code in this
repo; the algorithm is cited, no third-party code is linked. The same idea as
SVGF's wavelet passes, simplified for ES2 (no compute, no history buffer in
this first slice).

### Path architecture

- `MAIN` remains the default and is unchanged: full-resolution FX, no denoise.
- `ALTERNATIVE` is selectable at runtime (`fx 210 1`) and applies the denoiser
  to the scene colour before the effects: 1-6 à-trous iterations
  (`passes` + `bounces` - 1, capped), radius 1-4 in filter texels,
  intensity 0-100, frames 1-8 (accumulation weight).
- **Reduced-resolution default (`fx 220`, default 4)**: when the divisor is
  > 1 the denoise runs offscreen before the FX pass, following
  `DENOISER-RESEARCH.md` §4.1:
  1. downsample the eye scene colour to `scene_res/div` (4 bilinear taps into
     an RGBA8 target, 2-deep ping-pong per eye);
  2. downsample linear z into an RGBA8 target as 8.8 fixed point
     (`z/far`, r = high byte, g = low byte);
  3. run 1-3 edge-aware à-trous passes there (5x5 B3 kernel = 25 taps,
     depth weight `exp(-|dz| * 0.10/div)`, colour weight
     `exp(-|dc| * 2)`, step doubling each pass);
  4. bilaterally upsample in a **dedicated full-res pass** with four low-res
     taps weighted by coverage and depth difference (centre depth taken from
     the reduced-res depth field, which is cache-resident), mixing the result
     with the sharp scene colour by the intensity/frames weight so silhouettes
     keep their full-res edge. The FX pass then receives the mixed colour as
     its `uColor` and its in-shader denoise block stays off.
  `fx 220 1` keeps the original full-resolution in-shader `denoiseTap()` path
  available for A/B. At divisor 4 the filter covers the same fraction of the
  buffer as the full-res path and costs a fraction of its fragment work.
- Turning the denoiser off in the alternative path shows the unfiltered input,
  which is the A/B for the smoke test.
- The main path does not read any denoise uniforms, so it cannot regress.
- The denoise now also implies the FX pass (`stereo_fx_any()`), so ALT+DENOISER
  works with every other effect off.

### Controls

| adb key | meaning |
| --- | --- |
| `fx 210` | path: 0 MAIN, 1 ALTERNATIVE |
| `fx 211` | denoiser on/off |
| `fx 212` | intensity 0-100 |
| `fx 213` | passes 1-3 |
| `fx 214` | radius 1-4 |
| `fx 215` | frames 1-8 |
| `fx 216` | bounces 1-4 (extra à-trous iterations) |
| `fx 217` | test captions on/off |
| `fx 220` | denoise resolution divisor: 1 = full res, 2/3/4 = `scene_res/n` (default **4**) |

The LIGHT debug tab exposes the same controls (RATE/DN page, "Denoise res 1/n").

### On-screen captions

`ptc.c` draws two live lines from the current state:

```
PATH ALTERNATIVE | DENOISER ON | INTENSITY 70
FRAMES 2 | BOUNCES 2 | RATE 60 FPS
```

The first line now also carries the resolution divisor, e.g.
`PATH ALT | DN ON | I70 P2 F2 B2 R4 | 60FPS`. The `dn=` field of `cmd dump`
gained the same value as its seventh element (`path,on,intensity,frames,bounces,passes,div`).

They update every frame, so every recorded segment is self-labelling.

## 4. Smoke test

`scripts/denoise-rate-test.sh` (device lock; muted via `AGENT_TABLET_MUTE=1`):

1. MAIN / denoiser off / 60 fps
2. ALTERNATIVE / denoiser off / 60 fps
3. ALTERNATIVE / on / intensity 30 / 1 pass / frames 1 / bounces 1
4. ALTERNATIVE / on / intensity 70 / 2 passes / frames 2 / bounces 2
5. ALTERNATIVE / on / intensity 100 / 3 passes / frames 4 / bounces 4
6. ALTERNATIVE / on / 30 fps rate limit
7. ALTERNATIVE / on / intensity 70 / 2 passes / divisor 1 (full res)
8. same at divisor 2
9. same at divisor 4 (the default)
10. divisor 4 / 3 passes
11. divisor 4 / denoiser off (the A/B still)

Each segment is ~5 s with a dump before/after (`dn=`, `rate=`, `fps=`). The
script records the session and keeps the dumps next to the video.

## 5. Results / observations

### 5.1 First slice (2026-11-23, full-resolution filter only)

Smoke run: `test-artifacts/denoise-20261123-055437/` (`denoise.mp4`, 31.1 s;
`results.txt`; one still + dump per segment).

| segment | path | denoise | intensity / passes / frames / bounces | target | measured frame (EMA) |
| --- | --- | --- | --- | --- | --- |
| 01_main_off | MAIN | off | - | 60 | 24.5 ms |
| 02_alt_off | ALT | off | - | 60 | 24.9 ms |
| 03_alt_i30_p1 | ALT | on | 30 / 1 / 1 / 1 | 60 | 29.8 ms |
| 04_alt_i70_p2 | ALT | on | 70 / 2 / 2 / 2 | 60 | 105.7 ms |
| 05_alt_i100_p3 | ALT | on | 100 / 3 / 4 / 4 | 60 | 118.0 ms |
| 06_alt_rate30 | ALT | on | 100 / 3 / 4 / 4 | 30 | 119.5 ms |

### 5.2 Reduced-resolution default (2026-11-25)

This is the follow-up the research recommended: the filter now runs offscreen
at `scene_res / divisor` (default 4) and is upsampled bilaterally, with the
full-resolution in-shader filter kept as `fx 220 1`.

Measurement method: PTC light test scene (`ptc lighttest`, 2 lights + shadows +
blur + probe grid), denoise `I70 / radius 1 / frames 2`, 60 fps pacer, three
dumps per configuration, `fps=` read from `cmd dump` (`gfx_stereo_fps`, i.e.
composited frames per second) and converted with `1000 / fps`.

| configuration | fps (3 samples) | ms / composited frame |
| --- | --- | --- |
| MAIN path (no denoise) | 28.3, 29.6, 26.5 | 35.6 |
| ALT, denoiser off | 33.4, 32.0, 27.8 | 32.2 |
| ALT, divisor **1** (full-res in-shader), 2 passes | 15.9, 15.4, 13.7 | **66.7** |
| ALT, divisor **2**, 2 passes | 21.6, 20.9, 22.8 | **45.9** |
| ALT, divisor **4**, 2 passes | 24.5, 27.7, 26.8 | **38.0** |
| ALT, divisor 4, 3 passes | 26.7, 27.4, 26.0 | 37.5 |
| ALT, divisor 4, 1 pass | 29.2, 28.7, 27.5 | 35.1 |
| ALT, divisor 4, denoiser off (repeat) | 30.0, 27.6, 31.2 | 33.8 |

A second identical run reproduced every row within ~1 fps (divisor 1: 15.3 fps,
divisor 2: 22.7, divisor 4: 26.4, denoiser off: 30.7).

A temporary `glFinish`+`clock_gettime` probe (removed again) put the offscreen
chain itself at **4.3 ms** (downsample colour + depth + 2 à-trous passes) plus
**2.9 ms** (bilateral upsample/composite) at divisor 4, and 9.0 + 3.4 ms at
divisor 2, with the FX pass unchanged at ~31 ms. That matches the research
estimate of 6-10 ms/frame for à-trous at 1/4 effect-buffer resolution.

Interpretation:

- **The 1/4-res default costs ~6 ms over denoise-off** (38.0 vs 32.2 ms) and is
  **1.76x faster than the full-resolution filter** (66.7 ms). The second and
  third à-trous passes are nearly free at 1/4 res (37.5 ms for 3 passes), so
  `passes` can be spent on quality there.
- Rate limiting still works (`rate=60` in the dumps) but cannot create
  headroom: when the work exceeds the budget the pacer simply does not sleep.
- **Metric note:** the `rate=<target>,<adaptive>,<ema>` field times the main
  loop iteration, which in the 120 Hz HFR mode emits a variable number of
  composited frames and includes the 30 Hz simulation cadence. It is therefore
  *not* a frame time (it read ~195 ms for the full-res path whose composited
  frame rate was 15 fps). Measure with `fps=` and use `1000 / fps`; the 24-120
  ms `rate=` numbers in 5.1 (and in the earlier iteration of this doc) are on
  that other metric and are not comparable to 5.2.
- Visual check (stills at divisor 4 vs denoiser off): the denoised image is
  visibly smoother (wall texture, shading gradients) while cube edges, Mario's
  silhouette and the HUD text keep their edges - no blocking, halo or
  depth-bleed artefacts were visible. See the `s_r4` / `s_r1` / `s_off` stills
  of the 2026-11-25 run.

Implementation notes that mattered:

- The bilateral upsample is its **own full-resolution pass**; doing it inside
  the big FX shader cost ~30 ms more, and adding another sampler there slowed
  the whole post chain down.
- The upsample's centre depth comes from the **1/4-res depth field** rather
  than a full-res depth fetch: four full-res depth taps are cache misses and
  were an order of magnitude more expensive than the entire à-trous chain.
- `fx 220 1` restores the original full-res path; `stereo_denoise_prepass()` is
  then skipped entirely, so the code path still present in the FX shader is the
  same one that shipped in 5.1.

Known limitations:

- The denoiser filters the beauty colour, not a separate noisy GI buffer; the
  alternative path is therefore a filtered-render path rather than a
  path-traced denoiser.
- "Frames" is an accumulation weight (no history texture yet); it scales the
  blend strength.
- "Bounces" adds extra à-trous iterations (effective iterations =
  `passes + bounces - 1`, capped at 6); it does not change geometry.
- The reduced-res path has no temporal component yet: there is no history
  buffer or reprojection, so it cannot use `frames` for real accumulation.
