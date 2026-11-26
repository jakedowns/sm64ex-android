# MXAO performance: why the AO cost 17 ms in 3D and what the reduced-res chain buys

2026-11-26, worktree `w3-mxao-perf` (measured on `f62dd71`, rebased for merge
onto `60ad7d4`; the AO block sits after the C.DOF *and* SSGI reduced-res
blocks and is self-contained). Follow-up to
[`fx-sweep-11-25-26.md`](fx-sweep-11-25-26.md) §4.4 / missed win #4, which
ranked AO fourth-worst at **26.3 ms in 3D** (+17.0 over the 9.35 ms empty FX
pass) at 12 taps, radius 120, intensity 100 %.

Raw data, plans, stills and the driver:
`/media/share/tmp/artifacts/sm64/mxao/` (`run-025317/` = batch A, `ver3d/`,
`ver2d/` = the shipped-default batch).

## 1. Method

Same rig as the 11-25 sweep so the numbers are comparable: Lume Pad 2
(Adreno 660), GLES2, PTC level 38 **`ptc fadetest`** fixed camera
(`-100,1500,-3000` looking at Mario), `cmd stereo 1`, `cmd fps 2`, and
**`fx 200 0`** (pacer off) in *every* row so `fps=` is the per-present cost.

| mode | how | what runs |
| --- | --- | --- |
| 2D | `cmd set 21 1` (FLAT) | one eye 1280×1534, chain once |
| 3D | `cmd set 21 0` | two eyes, chain twice |

`.sweep` plan rows are applied in order without a reset, so **every row carries
the whole AO state** (including `396` and `200`). Two batches were wasted before
that rule was enforced: one forgot `200=0` (every row under 8.33 ms clamped at
the 120 Hz cap) and one let `396=2` leak into later rows (AO silently not
applied). The plans in the artifacts directory now set all seven AO keys per
row. Sampling: 3 dumps 0.25 s apart after a 3 s settle, median; `fps=` only.

**Drift is the dominant error.** The tablet ran 15–60 % hotter than on 11-25 and
drifted *within* a batch (`off` measured 3.98 then 4.47 in the same 3D run).
Only same-batch, adjacent rows are compared below; the `leg12` and `half`
rows are always adjacent so the ratio is solid, the absolute values are not.

## 2. Why it was expensive

Stage probes used the new chain's `396` dial (1 = reconstruction only, 2 = stop
before the apply). All numbers 3D, both eyes, 12 taps, AO as the only effect:
subtract the `floor` row (the empty FX pass, **12.41–12.53 ms**) for the
in-shader stages, or the `off` row (**3.98 ms**) for the chain.

| stage | resolution | 3D ms | 2D ms |
| --- | --- | --- | --- |
| linear depth + view normal, **old** (in-shader: 3 depth taps + 2 `viewPos` + cross per px) | full | **≈ 0** (11.93 vs 12.53 floor) | ≈ 0 |
| linear depth + view normal, **new** prepass (3 depth taps, oct N, one RGBA8 write) | half | **3.2** (15.58 vs 12.41) | ≈ 0 (8.43 vs 8.47) |
| 12-tap spiral gather, **old** (full-res depth fetch, `linZ` + `viewPos` + `sin/cos` per tap) | full | **18.0** (leg 30.49 − floor 12.53) | **8.9** (15.77 − 6.83) |
| 12-tap spiral gather, **new** (1 fetch of the packed low-res field per tap) | half | **3.8** (19.42 − 15.58) | **1.8** (10.22 − 8.43) |
| blur / denoise | — | **does not exist in either path** | — |
| apply / depth-aware upsample, **new** | full | ≈ 0.5–1.3 (chain − prepass − gather) | ≈ 0.5 |
| composite (`dofColor * ao`) | full | ≈ 0 (one multiply) | ≈ 0 |
| empty FX pass the old path could not skip | full ×2 | **8.4–8.6** (floor − base) | 3.9–5.6 |

Chain totals measured directly (prepass + gather + apply, no other effect):

| | 3D | 2D |
| --- | --- | --- |
| batch A (392 = 1, per-pixel rotation) | **8.10** (12.08 − 3.98) | **3.86** (6.80 − 2.94) |
| shipped default (392 = 0, frame rotation) | **6.33** (10.31 − 3.98) | **2.94** (5.79 − 2.85) |

The 2D probe rows are not additive in batch A (chain 3.86 but
prepass + gather 5.94) — the probe rows keep the big pass alive as a
fallback, so their attribution carries a couple of ms of error. The 3D
split above is self-consistent to ±0.5 ms; the 2D split is the 3D one halved,
which matches the measured totals.

### Which of the usual suspects applied

| suspect | verdict |
| --- | --- |
| per-tap depth fetches instead of one precomputed linear-depth/normal buffer | **yes, the main cost.** 13 depth fetches per pixel per eye (1 centre + 2 for the normal + 12 taps), each re-deriving `linZ` with a divide, plus `viewPos` (2 divides) per tap. |
| tap loop with branches that prevent unrolling | **partly.** `for (i < 32) { if (float(i) >= ns) break; … }` with four `continue`s, plus a `sin`/`cos` pair **per tap** (24 transcendentals per pixel). |
| recomputed matrices per pixel | no (uniforms), but `viewPos`/`linZ` divisions per tap, and the whole per-pixel normal is recomputed even when AO is off. |
| blur running at full res | **there is no blur at all**: the AO is never spatially filtered. The per-pixel golden-angle offset is re-rolled every frame (`uFrame * 0.618`), so the raw 12-tap estimate shimmers instead of being denoised. |
| effect runs when intensity is 0 / AO invisible | **yes on both.** Intensity 0 still ran the full gather; the effect was also the reason `stereo_fx_any()` kept the 9.35 ms pass alive. Sky/far pixels paid all 12 taps. |
| hemisphere taps without bilinear sampling | **yes.** 12 single NEAREST depth fetches; the depth buffer is `DEPTH_COMPONENT16`/NEAREST so there is nothing to interpolate. |

Cost is **perfectly linear in taps**, which is the signature of a fetch-bound
loop (3D, over the floor): 1 tap +2.5, 4 +7.0, 8 +12.6, 12 +18.0, 16 +23.8,
24 +33.1 ms → **1.33 ms per tap for both eyes** (0.66 ms/tap/eye). The 11-25
sweep's "32 samples → 18.0 ms in 2D" is the same slope.

The normal reconstruction being free was the surprise: 3 depth taps plus a
cross product on adjacent texels (L1 hits) cost nothing measurable, so
"precompute linear depth + normal" is a *quality/structure* win, not a
performance one. What actually costs is the *scattered* 12-tap fetch.

## 3. What changed

An 8th-sampler-in-the-big-shader design was rejected up front: BACKLOG records
that adding a seventh sampler there cost ~30 ms, and folding a 4-tap
reduced-res upsample into `s_fx_fs` cost the same. So the chain runs as three
small passes per eye (the `s_dn_*` pattern) and hands the FX pass a colour
buffer that *already has AO in it* — the big shader keeps zero AO code, zero
extra samplers and zero extra fetches on the fast path.

```
mode 0  prepass  full-res depth -> low-res RGBA8 (packed 16-bit linear z, octahedral view normal)
mode 1  gather   low-res 12-tap golden-angle spiral -> low-res RGBA8 (ao, packed z)
mode 2  apply    depth-aware 4-tap upsample, multiplies the full-res scene colour
mode 3  apply    plain bilinear 1-tap (A/B for the 4 taps)
```

- **Half resolution** for everything that costs anything (`391`, default 2).
- **The tap loop reads one precomputed field** (linear z + view normal) instead
  of the depth buffer, so a tap is 1 NEAREST fetch + 2 multiplies, and `linZ`
  is no longer recomputed per tap.
- **Rotation matrix instead of trigonometry**: the spiral is advanced by
  `dir = (dir.x·c − dir.y·s, dir.x·s + dir.y·c)` with `c,s = cos/sin(2.39996)`,
  so a pixel pays one `sin`/`cos` pair instead of 2 N (`395` keeps the old
  per-tap trig for an A/B; worth +0.2 ms 2D / +0.0 ms 3D — the GPU hides it).
- **Sky/far early-out** (`394`): `z > far·0.95` writes `ao = 1` and never
  enters the loop.
- **Skip the whole chain** when AO is off or intensity is 0: `stereo_fx_any()`
  now requires `intensity > 0`, and `1=1 2=0` measures the same as AO off
  (4.49 vs 3.98–4.01 in 3D).
- **Skip the big FX pass when AO is the only effect.** `stereo_fx_other()` is
  `stereo_fx_any()` minus AO; when it is 0 the composite samples the AO'd
  colour directly and the 8.4–8.6 ms (3D) empty pass disappears. This is the
  single biggest part of the win and it is not in the brief.
- The full-res in-shader gather is still there behind `390 0`, and is used
  automatically for the AO debug view and when SSRTX is on (its reflection
  blend weight reads the AO factor).

## 4. New cost

3D, both eyes, AO as the only effect. Batch A ran on the coolest tablet state;
the shipped-default batch ran ~20 % hotter, hence two columns.

| row | batch A 3D | shipped 3D | batch A 2D | shipped 2D |
| --- | --- | --- | --- | --- |
| AO off (no post pass) | 3.98 | 3.98 | 2.94 | 2.85 |
| empty FX pass (floor) | 12.53 | 12.41 | 6.83 | 8.47 |
| **AO full-res 12 (previous)** | **30.49** | **30.86** | **15.77** | **20.62** |
| **AO half 12 (default now)** | **12.08** | **10.31** | **6.80** | **5.79** |
| AO quarter 12 | 7.80 | 7.41 | 4.41 | 4.18 |
| half 12, 1-tap bilinear apply (`393 1`) | 11.57 | 9.69 | 7.87 | 5.50 |
| half 8 | 12.30 | – | 6.96 | – |
| half 16 | 18.18 | – | 9.62 | – |
| AO on, intensity 0 | 4.49 | – | 2.87 | – |
| LOW + AO 8 (half) | 28.17 | – | 15.31 | – |
| LOW + AO 8 (full-res) | 35.84 | – | 18.76 | – |
| LOW without AO | 16.26 | – | 8.81 | – |

**The AO-only chain is now cheaper than the empty post pass it removes**
(6.33 vs 8.4 in 3D; 2.94 vs 3.9–5.6 in 2D), so "AO on" costs *less* than
"post pass on with nothing in it" did before.

- 3D, AO alone: **30.5 → 12.1 ms (batch A), 30.9 → 10.3 ms (shipped)** — 2.5×/3.0×.
- 2D, AO alone: **15.8 → 6.8 ms, 20.6 → 5.8 ms** — 2.3×/3.6×.
- On top of the LOW stack (3D): HIGH-ish AO8 used to add 19.6 ms over the
  stack and now adds 11.9 ms (35.84 → 28.17 total). The chain costs ~3.5 ms
  more when another effect keeps the big pass alive — expected, since the
  AO'd colour then has to be resolved and re-read by the big shader.
- `LOW + AO8` in 3D lands at 28.2 ms: it now **fits 30 Hz with contact AO**
  (11-25: 35.8 ms, miss). 60 Hz in 3D (16.67) still does not fit.

## 5. Quality

Stills, same session and camera, `ver2d/` (`off`, `floor`, `leg12`, `half_def`,
`half_bil`, `quart`), `run-025317/2d/quality_*` and `crop*.png` at 2×.

Mean |Δ| over the game canvas (2560×980, off as reference):

| vs `off` | mean |Δ| | > 4/255 |
| --- | --- | --- |
| full-res 12 | 7.43 | 13.3 % |
| half 12 (default) | 6.71 | 15.4 % |

and on a flat, unoccluded green wall (no legitimate AO there): full-res 0.12
mean / 0.5 % of pixels; half-res 0.53 / 2.7 %. That is the whole difference in
one line: **half-res AO keeps the amount but perturbs flat surfaces 4× more.**

Where it differs, and by how much:

- **Slightly softer contact** — the 2× low-res field blurs AO onto the
  background by up to 2 px. At 1:1 the AO reads as a smoother, lower-frequency
  gradient; the old banding rings become a cleaner falloff.
- **Thin geometry**: Mario's cap brim is the worst case. At half res it is
  softened; at **quarter res it visibly stair-steps** (4-px bands along the
  cap edge, obvious at 2× zoom). That is why quarter res is a dial, not the
  default.
- **Banding, not grain**: with `392 1` (static per-pixel IGN rotation) the
  flat walls get a visible dot/speckle pattern at 1:1 and the chain costs
  +1.9 ms (batch A) / +3.1 ms (shipped batch) in 3D, because neighbouring
  pixels stop sharing tap directions and the fetch pattern goes incoherent.
  With `392 0` the pattern is the same one-direction-per-frame spiral the
  legacy path used, so the stills match the old look (its ring banding
  included) and it is the cheaper of the two. **Default is 0.**
- **No halo or over-darkening at silhouettes**: the apply is a 4-tap
  depth-aware upsample with the same `0.25/div` edge stop the denoiser uses,
  so AO does not bleed across a silhouette.
- `393 1` (1-tap bilinear apply) is 0.5–0.6 ms cheaper and slightly softer at
  edges; `393 0` is the default.

## 6. Keys and defaults

| key | default | meaning |
| --- | --- | --- |
| `390` | 1 | 0 = full-res in-shader (old), 1 = reduced-res chain |
| `391` | 2 | low-res divisor (2 = half, 4 = quarter) |
| `392` | 0 | 1 = static per-pixel IGN rotation (grain, +2–3 ms 3D), 0 = per-frame |
| `393` | 0 | 0 = 4-tap depth-aware apply, 1 = 1-tap bilinear |
| `394` | 1 | skip the gather on sky/far |
| `395` | 0 | 1 = per-tap `sin`/`cos` for the A/B |
| `396` | 0 | stage probe: 1 = prepass only, 2 = stop before the apply |

`cmd dump` appends `aop=path,div,samples,jitter,up,sky,probe`. Panel: MXAO tab →
`AO RESOLUTION (ADVANCED)`. Note that **probe rows keep the big pass alive** as
a fallback (their `s_ao_valid` stays clear), so subtract `floor`, not `base`.

## 7. Top three remaining wins

1. **Quarter res as a "performance" AO mode** (`391 4`): 7.4 vs 10.3 ms in 3D
   (−28 %) for stair-stepping that only shows on thin geometry at 2× zoom.
   The natural follow-up is 1/2 for near geometry and 1/4 past ~1500 units,
   which needs a per-pixel divisor — or simply two shipped presets.
2. **Halve the tap count with a filterable field.** A 12-tap NEAREST gather is
   12 fetches; an `RGBA16F` low-res prepass (`z`, `N.xy` and room for a second
   tap) would make `LINEAR` filtering meaningful, so 6 bilinear taps could
   replace 12 NEAREST ones — the same 4× that half-res bought, again. 8 taps at
   the current precision is *not* the lever (12.30 vs 12.08 at 12 taps: the
   per-pixel fixed cost dominates between 8 and 12).
3. **A 3×3 depth-aware blur on the half-res AO itself** before the apply. It is
   one cheap half-res pass (the denoiser's à-trous shape) and it would let the
   gather stay at 8–10 taps while removing the residual banding/stripes — the
   thing that makes 1/4 res look bad today. It also makes `392 1` viable
   (temporally stable) without the speckle.

Not worth doing: more AO samples (linear 1.33 ms/tap 3D), and any further work
on the apply pass (≤ 1.3 ms of a 10.3 ms total).

## 8. Rerun

```bash
O=/media/share/tmp/artifacts/sm64/mxao/rerun-$(date +%H%M%S)
AGENT_TABLET_MUTE=1 scripts/agent-lock.sh device --timeout 900 --reason "mxao" -- bash -c "
  /media/share/lp2-test/scripts/unlock.sh
  /media/share/tmp/artifacts/sm64/mxao/ao-driver.sh \
      /media/share/tmp/artifacts/sm64/mxao/final2.sweep $O/3d --mode 3d --install --restart
  /media/share/tmp/artifacts/sm64/mxao/ao-driver.sh \
      /media/share/tmp/artifacts/sm64/mxao/final2.sweep $O/2d --mode 2d"
python3 /media/share/tmp/artifacts/sm64/mxao/pngdiff.py $O/2d/off.png $O/2d/half_def.png --crop 0,200,2560,980
```

`--help`-less but small: `--mode 3d|2d`, `--samples N`, `--install`,
`--restart`, `--level`. `pngcrop.py` + `pngdiff.py` are dependency-free
(no PIL/numpy/ImageMagick on this host).
