# Rate limiting + alternative denoised rendering path

Date: November 23, 2026
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
  to the scene colour before the effects: 1-3 à-trous passes, radius 1-4,
  intensity 0-100, frames 1-8 (accumulation weight), bounces 1-4 (reserved for
  the effect budget; exposed in the UI/captions).
- Turning the denoiser off in the alternative path shows the unfiltered input,
  which is the A/B for the smoke test.
- The main path does not read any denoise uniforms, so it cannot regress.

### Controls

| adb key | meaning |
| --- | --- |
| `fx 210` | path: 0 MAIN, 1 ALTERNATIVE |
| `fx 211` | denoiser on/off |
| `fx 212` | intensity 0-100 |
| `fx 213` | passes 1-3 |
| `fx 214` | radius 1-4 |
| `fx 215` | frames 1-8 |
| `fx 216` | bounces 1-4 |
| `fx 217` | test captions on/off |

The LIGHT debug tab exposes the same controls.

### On-screen captions

`ptc.c` draws two live lines from the current state:

```
PATH ALTERNATIVE | DENOISER ON | INTENSITY 70
FRAMES 2 | BOUNCES 2 | RATE 60 FPS
```

They update every frame, so every recorded segment is self-labelling.

## 4. Smoke test

`scripts/denoise-rate-test.sh` (device lock; muted via `AGENT_TABLET_MUTE=1`):

1. MAIN / denoiser off / 60 fps
2. ALTERNATIVE / denoiser off / 60 fps
3. ALTERNATIVE / on / intensity 30 / 1 pass / frames 1 / bounces 1
4. ALTERNATIVE / on / intensity 70 / 2 passes / frames 2 / bounces 2
5. ALTERNATIVE / on / intensity 100 / 3 passes / frames 4 / bounces 4
6. ALTERNATIVE / on / 30 fps rate limit

Each segment is ~5 s with a dump before/after (`dn=`, `rate=`). The script
records the session and keeps the dumps next to the video.

## 5. Results / observations

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

Interpretation:

- The main path is unchanged and ~40 fps in the light-test scene on this
  Adreno 660. The alternative path at **1 pass is ~30 ms (≈33 fps)** and is the
  only setting that fits a 30-60 fps target; 2-3 passes are quality modes
  (≈9-10 fps) and show the cost of full-resolution filtering.
- Rate limiting works: target 60/30 is applied (see `rate=60` / `rate=30` in
  the dumps). It cannot create headroom: when the frame work exceeds the
  budget the pacer simply does not sleep and the measured interval stays at
  the work time (105-119 ms in the last segments).
- The research recommendation (à-trous at 1/4 effect-buffer resolution) is the
  next optimization; this slice filters the beauty image at full resolution,
  which explains the 2-3 pass cost. Pass 1 is the recommended default.
- Captions: `PATH ALT | DN ON | I30 P1 F1 B1 | 60FPS` etc. were visible in
  every segment (`03_...png`, `04_...png`), at quarter HUD size.

Known limitations of this first slice:

- The denoiser filters the beauty colour, not a separate noisy GI buffer; the
  alternative path is therefore a filtered-render path rather than a
  path-traced denoiser. A follow-up can move it to a 1/4-res effect buffer as
  the research recommends.
- "Frames" is an accumulation weight (no history texture yet).
- "Bounces" is reserved for the effect budget and shown in the captions; it
  does not change geometry.
