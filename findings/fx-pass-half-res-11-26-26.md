# Reduced-resolution FX/post pass — 2026-11-26

The perf sweep of 2026-11-25 ended on one line: *there is no post-pass
resolution lever, and that is the biggest single win on the table.* `set 12`
only shrinks the scene raster inside a still-full-res eye buffer, so the 76.9 ms
3D HIGH stack dropped 13 % when the scene halved. This is that lever.

Raw data, stills and the harness:
`/media/share/tmp/artifacts/sm64/fx-halfres/`.

## 1. What was built

`fx 420 n` makes the whole per-eye FX fragment shader render into a target at
`eye_res/n` and composites it back into the full-resolution eye target in its
**own pass** — the same shape as the denoiser/DOF/AO/GI reduced-res chains, and
for the same reason (folding an upsample into the big shader measured ~30 ms
once, BACKLOG).

```
scene raster                 -> eye_tex / eye_depth        (full res)
denoise / C.DOF / MXAO / rays / SSGI prepasses             (unchanged)
big FX shader                -> fxp_tex[eye]               (eye_res/n)
bilinear or 4-tap depth-aware upsample -> pp_tex[eye]      (full res)
SSGI add-back, bloom, grade LUT                            (full res, unchanged)
UI-after-post layer + SBS weave                            (full res, unchanged)
```

| key | default | meaning |
| --- | --- | --- |
| `420` | **1** | FX pass divisor: 1 = today's full res, 2/3/4 = eye_res/n |
| `421` | 0 | upsample: 0 = 1-tap bilinear, 1 = 4-tap depth-aware stop |
| `422` | — | read-only: divisor the last frame actually used, 0 = the big FX pass did not run at all |

Panel: **FX → GRADE → POST PASS RESOLUTION** (`Post pass res 1/n`, `Upsample`),
with a hint. `cmd dump` reports `fxr=<div>,<upsample>,<effective>`.

Code: `app/jni/src/src/pc/gfx/gfx_opengl.c` — dials declared next to
`s_fx_debug`, `stereo_ensure_fx_target()`, `stereo_fxu_init()`,
`stereo_fx_upsample()` next to `stereo_ensure_pp()`, the branch in
`stereo_fx_pass()`, and `stereo_fx_upsample()` called after the eye loop and
before the SSGI add-back. Debug views (`fx 31 != 0`) force `n = 1`: their output
*is* the image being inspected. `n = 1` allocates nothing and takes the original
code path unchanged.

## 2. uRes / gl_FragCoord correctness — what actually had to change

**Nothing in the shader.** That is a result, not laziness, and it is worth
writing down because the naive expectation is the opposite:

- the FX fragment shader never reads `gl_FragCoord` (`grep -n gl_FragCoord`
  matches only the *scene* shader's punch-through and the depth-pack shader),
  so there is no fragment-position-derived UV to convert;
- `uRes` and `uCanvas` stay in **eye** pixels, and that is correct at every
  divisor, because every use of them converts a dial expressed in eye pixels
  into UV: `mvPx/uRes`, `cocPx/uRes`, `uCanvas.zw/uRes`, `tx = 1/uRes` for the
  depth-normal step, `canvasUV()/eyeUVfromCanvas()` for the letterbox and the
  ray/SSGI march. The FX target covers the same 0..1 UV range as the eye buffer,
  so a 12 px max-blur stays a 12-eye-pixel blur and the picture keeps its
  framing;
- depth and velocity textures stay bound at full resolution; sampling them is
  UV-addressed, so the reduced pass simply reads fewer of their texels.

The two things that genuinely change at n > 1 are output-pixel-sized details: the
1-texel depth-normal fit and the per-pixel dither/noise grid (`hash12(cuv*uRes)`
in AO/SSGI/SSRTX/MB) become `n`-pixel blocks. That is the visible trade in §4.

## 3. Cost, n = 1 vs n = 2

Lume Pad 2, PTC level 38, `ptc fadetest` fixed camera, eye buffer 1280×1534,
pacer off, `fps=` from `cmd dump` → ms, 3 samples after a 2 s settle, median
(min–max in `results.tsv`). 2D = `stereo 1` + FLAT (`set 21 1`), 3D = `stereo 1`.
Run-to-run drift on this box is ±5–10 %.

| stack | 2D n=1 | 2D n=2 | Δ 2D | 3D n=1 | 3D n=2 | Δ 3D |
| --- | --- | --- | --- | --- | --- | --- |
| grade only (pass floor) | **4.81** | **2.91** | −40 % | **10.53** | **5.95** | **−43 %** |
| HIGH (AO12+DOF12+rays16+shadow) | 10.10–10.42 | **7.69** | −25 % | **28.57** | **20.00** | **−30 %** |
| MED (AO8+rays8+shadow) | 7.46 | **4.69** | −37 % | **21.28** | **12.82** | **−40 %** |
| FX-only (MB+rays+bloom+grade+shadow) | 8.77 | **5.29** | −40 % | **27.78** | **14.29** | **−49 %** |
| HIGH+MB+bloom+grade | 24.39 | **13.16** | −46 % | **34.48** | **23.26** | −33 % |

n = 3 and the depth-aware upsample, HIGH stack:

| row | 2D ms | 3D ms |
| --- | --- | --- |
| n = 1 | 10.10 | 28.57 |
| n = 2, bilinear | **7.69** | **20.00** |
| n = 2, 4-tap depth-aware (`421 1`) | 8.13 | 21.28 |
| n = 3, bilinear | 7.19 | 18.87 |

Reading it:

- the **pass floor itself** (grade only, no other effect) halves: 10.53 → 5.95 ms
  in 3D. That is the fixed cost the sweep said could not be dialled, and it is
  the cleanest proof the lever works.
- the **3D gains are larger than the 2D ones** (−30…−49 % vs −25…−46 %), which is
  what matters: 3D is the real user case and the stereo multiplier was ~2× on
  this pass.
- HIGH 3D goes 28.6 → 20.0 ms (**34 → 50 fps**): still short of 30 Hz, because
  what remains is the *prepasses* (AO/DOF/GI at their own divisors) plus the
  scene, but it is a third of the stack back.
- **n = 3 is only ~6 % better than n = 2** while losing another 44 % of the
  pixels: beyond 2 the pass is no longer fill-bound (it is per-draw/per-pixel
  fixed overhead and the prepasses), so 2 is the sweet spot and 3/4 are not worth
  their quality.
- the **depth-aware upsample costs 0.4 ms in 2D and 1.3 ms in 3D** over bilinear.
  It buys cleaner silhouettes (§4) — worth it at MED/HIGH, optional at LOW.

Not measured in this run: dynamic lights. `fx 103` no longer enables the FX
pass's dynamic lights on this branch, because `gfx_pc.c`'s per-frame gameplay
light registry now calls `gfx_opengl_light_count_auto()` every frame and resets
the count to the number of registered gameplay lights (0 in the PTC) unless a
caller claimed the slots with `gfx_opengl_light_count()` — which `cmd fx` does
not. So the sweep document's `103=` rows in the LOW/MED/HIGH presets are **dead
keys** now; they must move to a `cmd light`/PTC action or the registry must stop
clobbering a manual `fx 103`. `ptc lighttest` / `lightshow` do claim the slots,
but both also change the scene, so they cannot be used for a fixed-camera row.
Everything above is therefore "HIGH without dynamic lights"; the per-light
shadow march would add on top at both divisors.

## 4. Quality, n = 1 vs n = 2

Fixed camera (`ptc fadetest`), FLAT/2D so one eye fills the capture, n=1 and n=2
of the same config back to back, `screencap` of the SBS surface, game area
`y 320…1470` (HUD and bottom bar excluded). Metric = mean |diff| in 0/255 after
the same analysis used for the SSGI resolution work. Stills:
`clean/a_n1.png`, `clean/a_n2.png`, `clean/a_n2d.png`, `clean/b_n1.png`,
`clean/b_n2.png`.

| pair | mean | p99 | > 6/255 | > 16/255 |
| --- | --- | --- | --- | --- |
| HIGH+MB+bloom+grade, **n=1 vs n=1** (noise floor) | 3.44 | 59 | 14.4 % | 2.7 % |
| HIGH+MB+bloom+grade, **n=1 vs n=2** | 13.41 | 81 | 33.8 % | 26.8 % |
| n=2 bilinear vs n=2 depth-aware | 2.94 | 38 | 11.5 % | 2.5 % |

The **noise floor is large** because AO/DOF/rays/MB all dither per frame
(`uFrame`-seeded tap angles), so a single-frame pixel diff between any two
frames is already 3.4/255 mean and 14 % of pixels over 6/255. The n=1→n=2 signal
is ~4× that, and blurring both frames with a 2-pixel Gaussian does **not** shrink
it (10.9 → 9.1 mean), so part of it is real low-frequency change, not just a
re-rolled dither pattern: the half-res pass samples the scene colour at half
density, so any high-contrast micro-detail inside a 2×2 block (a 1-pixel dither,
a 1-pixel line, a text sprite edge) is averaged away before the upsample.

Where it is visible at 1:1 (`clean_pair.png` in the artifact dir):
- **silhouettes and thin geometry** are softer: the N6 sign's frame, the
  checkerboard tile borders and the vertical rope are ~1 px blurrier. Mario's
  outline gains a faint stair-step follow-the-2×2-grid look.
- **flat areas, colour and luminance are unchanged**; the grade, bloom, rays
  composite and shadow are indistinguishable outside edges.
- **UI is untouched** (panel, HUD, touch buttons, feedback bar) — those are the
  Java overlay and the UI-after-post layer, both still full res.
- the 4-tap depth-aware upsample (`421 1`) halves that difference at the
  silhouette (2.94 mean against bilinear n=2, mostly on edges) at the +0.4/+1.3 ms
  cost above.

A 3D-panel judgement is *not* in this report: the captures are 2D stills. The
honest next step before defaulting 2 is a stereo A/B by a human, because the two
eyes see slightly different sub-pixel phases of the same softening.

## 5. One-off corrupted still

One still in the isolation matrix (`iso2d/d2.png`, DOF only at n=2) came out as
the scene at half scale in the bottom-left quarter of the eye with the rest
black — a wrong-viewport signature, not a resolution artefact. It did **not**
reproduce: a fresh-app probe of the same config, a replay of the whole isolation
sequence, five of six other n=2 rows in the same run, and every perf row were
correct. The only unusual thing about that row is that the harness's state check
failed (`fxr eff=0`) and re-sent all ~90 keys as a burst mid-frame. Treat it as
a harness/state-burst transient until proven otherwise, and if `420 > 1` ever
ships, add a "the FX target must be `eye_res/n` when the pass draws" assertion to
the dump (`fxr` already exposes the effective divisor, which is how this was
spotted).

## 6. Recommendation

- **Keep the default at n = 1.** The dial is a lever, not a decision.
- **n = 2 is the right shipping value for the 3D MED/HIGH presets** after a human
  stereo A/B: −40 % / −30 % on the exact stacks that miss their frame budget,
  for edge softness that a 2D still shows and a 3D panel may hide. Pair it with
  `421 1` (depth-aware) at HIGH, where 1.3 ms is affordable and silhouettes
  matter most.
- **Do not bother with n = 3/4**: ~6 % more speed for another quarter of the
  pixels; the pass stops being fill-bound past 2. The same is likely true of the
  other resolution dials, which is worth remembering.
- **What this does not fix**: the remaining 3D HIGH cost is the prepasses
  (AO 8–17 ms, DOF 20 ms in the sweep's numbers) and the scene; the next lever is
  making *those* divisors cheaper (their fixed cost, not their tap counts), not
  pushing this one to 3.
- Fix `fx 103` (dynamic lights) before re-running the LOW/MED/HIGH presets, or
  every light row in `findings/fx-sweep-11-25-26.md` will silently measure a
  lightless scene.

## 7. Reproduce

```bash
# install the worktree APK, then in one device hold:
AGENT_TABLET_MUTE=1 scripts/agent-lock.sh device --timeout 900 --reason "fx half-res" -- \
    /media/share/tmp/artifacts/sm64/fx-halfres/driver.sh \
    /media/share/tmp/artifacts/sm64/fx-halfres/plans/high.sweep \
    /media/share/tmp/artifacts/sm64/fx-halfres/2d --mode 2d
```

`plans/high.sweep` (perf), `plans/stills.sweep` and `plans/noise.sweep` (quality,
`--shots`), `plans/iso.sweep` (per-effect A/B). `still-diff.py A.png B.png
--box 0,320,2560,1470` prints the quality table; the driver's BASE resets
`420`/`421` so a row can never inherit the previous divisor.
