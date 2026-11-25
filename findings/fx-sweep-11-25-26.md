# Screen-space FX performance sweep — 2026-11-25

Measured on the tablet, no rebuild, every dial set at runtime through the debug
channel. Raw data and stills:
`/media/share/tmp/artifacts/sm64/fx-perf/`.

## 1. Method

**Device** Lume Pad 2 (Snapdragon 888 / Adreno 660), GLES2 path, panel
2560×1600 @ 120 Hz. `cmd dump` reports `drawable=2560x1534 eyeW=1280`, so a
per-eye buffer is **1280×1534 ≈ 1.96 MP**; stereo renders two of them.

**Build** measured as installed, never rebuilt or reinstalled by this task. The
main checkout is shared with three other agents and the APK was reinstalled
under me twice, so the exact stamp is per batch (`dumpsys package
com.vdavid003.sm64port | grep lastUpdateTime`, local time = UTC−5):

| batch | installed at | repo HEAD seen |
| --- | --- | --- |
| 2D core + stacks (`2d/`, `c2/`) | 15:03:10 | 6f9c1e1 |
| 3D core + stacks (`3d/`, `c3/`) | 15:09:28 | 6f9c1e1 |
| denoise + MB orbit (`g/`) | 15:15:52 | 7c65b02 |
| profile check (`h/`) | 15:20:23 | 7c65b02 |
| tier fill + HIGH repeat (`i/`, `j/`) | 15:25:31 | b3ecdf4 at report time |

**Scene** Portal Test Chamber, level 38, `ptc fadetest` vantage: custom camera
pinned at `(-100, 1500, -3000)` looking at Mario at `(-600, 1500, -3000)`.
Identical geometry, depth complexity and auto-focus distance in every row.
Mario is idle, so the static rows have near-zero motion vectors; the motion-blur
rows are backed by a second matrix in the moving `ptc blurshow` orbit.

**Modes**

| label | how | what runs |
| --- | --- | --- |
| 2D | `cmd stereo 1` + `cmd set 21 1` (FLAT) | one eye at 1280×1534, FX pass once |
| 3D | `cmd stereo 1` + `cmd set 21 0` | two eyes, FX pass twice |

There is no useful "stereo off" 2D measurement: with `cmd stereo 0` the whole
per-eye path (and therefore the FX chain) is bypassed. FLAT is the mode users
get from the 2D button, and it is what "2D" means below.

**Sampling** each row applies the complete target state (every key used in any
plan is reset, so a row can never inherit the previous row's value), waits
1.2–2.6 s, then takes 3 dumps 0.55 s apart; the first is discarded. `fps=` from
`cmd dump` is the only metric (`ms = 1000/fps`); `rate=` was not used. Values
are the median of the remaining samples; min–max is in `results.tsv`. The
on-device EMA window is 0.5 s, so consecutive dumps often report the same
window — identical samples mean "stable", not "one sample".

**Pacer** `fx 200 0` (rate limiter off) for every row. Two consequences worth
stating because they contradict the usual assumption:

- There is **no 120 fps cap** in these runs. The FLAT baseline measures 358 fps
  (2.8 ms), so the swap chain is not vsync-limited when the pacer is off.
  With the pacer on (`200 1`, `201 120`) — the shipping default — anything
  faster than 8.33 ms is simply presented at 120.
- `cmd fps` (30/60/120) changes how often the 30 Hz display list is
  re-presented, **not the cost per present**. Measured on the same
  AO8+DOF12+rays8 row: 18.18 ms at 120 mode, 18.18 ms at 60 mode, and 33.33 ms
  at 30 mode only because the mode itself refuses to present faster than 30
  (fps=30 is the ceiling, not the cost). The budgets below are per-present budgets.

**Caveats**

- Run-to-run drift on this device is ±5–10% (thermal/other agents). The same
  AO-12 row measured 12.35, 12.50 and 13.33 ms in three sessions; the FLAT
  baseline 2.77 / 2.81 / 3.03 ms. Ordering is stable, absolute numbers are not.
- The scene is a still camera: motion-blur rows understate the per-pixel gather
  cost (empty vectors early-out) and are backed by the orbit matrix.
- Two things to know about the FX key table: parallel worktrees have renumbered
  it at least once this week, and two keys (`31` debug view, `250` rays) behaved
  differently earlier the same day before an APK reinstall. Keys below are the
  ones that worked on the installed build at 15:20; re-check `cmd dump`'s
  `fxdbg=` / `rays=` before trusting a key after a merge.

## 2. Individual effect cost

`base` = every effect off, post pass skipped. `floor` = the pass running with
no effect (debug view 2, which also disables the letterbox early-out, so it is
a worst-case floor). Δ is against `floor`, because every effect row includes
the pass.

### 2D (one eye, 1280×1534)

| row | settings | ms | ms range | Δ over floor |
| --- | --- | --- | --- | --- |
| base | all off, no post pass | **2.81** | 2.75–2.87 | — |
| floor | post pass, no effect | **4.72** | 4.69–4.74 | +1.9 vs base |
| ao | AO 12 samples, r120, 100% | **12.35** | 12.35–12.35 | **+7.63** |
| ao32 | AO 32 samples | 22.73 | 22.73 | +18.0 |
| dof | DOF 16 samples, max blur 12 px | **14.08** | 14.08 | **+9.36** |
| dofbokeh | DOF 16 + bokeh kernel, highlight 150% | 19.61 | 19.61 | +14.9 (+5.5 for bokeh) |
| gi | SSGI 6 steps × 4 rays, r150 | **18.18** | 18.18 | **+13.46** |
| gimax | SSGI 12 steps × 8 rays | 58.82 | 58.82 | +54.1 |
| ssrtx | SSRTX 20 steps × 1 ray, 600 units | **15.15** | 14.93–15.15 | **+10.43** |
| ssrtxmax | SSRTX 48 steps × 4 rays, rough 50 | **142.86** | 142.86 | +138.1 |
| mbcam | MB on, camera-only vectors (still scene) | 5.08 | 5.05–5.08 | +0.36 |
| mbvec | MB on, per-vertex vectors, `240 0` | 4.98 | 4.93–5.05 | +0.26 |
| mbvmax2 | MB + 3×3 tile-max bound | 8.20 | 8.20–8.26 | +3.48 |
| light3 | 3 dynamic lights | 5.81 | 5.78–5.85 | +1.09 |
| light3sh | + 6-step per-light screen-space shadows | 6.94 | 6.94 | +2.22 |
| probe16 | 16 probe spheres (debug visualisation) | **9.26** | 9.26–9.35 | **+4.54** |
| rays8 | crepuscular rays, 8 march samples | 6.17 | 6.13–6.25 | +1.45 |
| rays32 | rays, 32 samples | 6.67 | 6.67 | +1.95 |
| shadow1024 | shadow map 1024, 9 PCF taps | 6.37 | 6.33–6.45 | +1.65 |
| shadowt1 | shadow map 1024, 1 PCF tap | 5.56 | 5.52–5.56 | +0.84 |
| shadowi4 | shadow map every 4th frame | 6.10 | 6.10–6.13 | +1.38 |

### 3D (two eyes, same scene, same rows)

| row | ms | Δ over floor | 3D/2D Δ ratio |
| --- | --- | --- | --- |
| base | **4.15** | — | 1.48 |
| floor | **9.35** | +5.2 vs base | 1.98 |
| ao | **26.32** | **+16.97** | 2.2× |
| ao32 | 52.63 | +43.3 | 2.4× |
| dof | **30.30** | **+20.95** | 2.2× |
| dofbokeh | 41.67 | +32.3 | 2.2× |
| gi | **40.00** | **+30.65** | 2.3× |
| gimax | 142.86 | +133.5 | 2.5× |
| ssrtx | **21.74** | **+12.39** | 1.2× |
| ssrtxmax | **250.00** | +240.7 | 1.7× |
| mbcam | 10.00 | +0.65 | 1.8× |
| mbvec | 13.51 | +4.16 | n/m |
| mbvmax2 | 18.18 | +8.83 | 2.5× |
| light3 | 14.08 | +4.73 | 4.3× |
| light3sh | 20.41 | +11.06 | 5.0× |
| probe16 | **21.74** | **+12.39** | 2.7× |
| rays8 | 14.29 | +4.94 | 3.4× |
| rays32 | 15.87 | +6.52 | 3.3× |
| shadow1024 | 18.52 | +9.17 | **5.6×** |
| shadowt1 | 12.20 | +2.85 | 3.4× |
| shadowi4 | 18.18 | +8.83 | 6.4× |

The stereo multiplier is **not** a flat 2×: fill-bound per-eye loops land at
2.2–2.5×, but the shadow map, the per-light shadow march, the rays sampler and
the per-vertex velocity path are 3.4–5.6× because they add a per-eye fetch or
pass on top of a replay that is already paid once.

### Denoiser path (2D)

| row | settings | ms | Δ vs denoise off |
| --- | --- | --- | --- |
| alt_off | `210 1`, denoiser off | 3.04 | — (same as base: ALT alone is free) |
| alt_dn4 | `210 1 211 1`, 1/4 res, I60 P2 F1 B1 | **7.41** | **+4.37** |
| alt_dn2 | divisor 2 | 10.00 | +6.96 |
| alt_dn1 | divisor 1 (in-shader full-res) | 12.20 | +9.16 |
| alt_dn_p3 | 3 passes + 3 bounces at 1/4 | 8.85 | +5.81 |
| alt_dn_i100 | intensity 100 at 1/4 | 7.75 | +4.71 |

This reproduces the 11-25 reduced-resolution decision: 1/4 res is 1.65× faster
than the full-res in-shader filter, and extra passes at 1/4 res cost ~0.7 ms
each — spend quality there, not on resolution.

### Motion blur with real motion (`ptc blurshow` orbit, 2D)

Bracketed by two "off" rows (3.06 and 3.04 ms), so the scene itself is stable.

| row | ms | Δ vs off |
| --- | --- | --- |
| off | 3.06 | — |
| camera-only vectors (`140 0`) | 6.17 | +3.11 |
| per-vertex, `240 0` (no bound pass) | 6.06 | +3.00 |
| per-vertex + 1-tap bound (`240 1`) | 8.06 | +5.00 |
| per-vertex + 3×3 bound (`240 2`) | 9.62 | +6.56 |
| velocity at 1/8 res (`144 8`) | 7.87 | +4.81 |
| velocity at full res (`144 1`) | 9.01 | +5.95 |
| 8 sub-steps (`142 8`) | 9.43 | +6.37 |

The dominant cost is the **velocity replay** (one extra scene pass at 1/4 res,
~3 ms), not the blur gather (4 extra taps: +1.15 ms from 4 to 8 sub-steps).
Per-vertex and camera-only are within 0.1 ms in 2D; in 3D the per-vertex path is
*worse* (13.51 vs 10.00 ms, +3.5 ms) because it pays the replay plus a per-eye
velocity fetch. `240 2` (3×3 tile max) adds 1.56 ms over `240 1` for a slightly
better bound — keep the default 1.

## 3. Stacked combinations

| stack | 2D ms | 3D ms |
| --- | --- | --- |
| AO12 | 12.50 | 26.32 |
| AO12 + DOF16 | 21.74 | — |
| AO12 + rays32 | 14.08 | — |
| DOF16 + rays32 | 15.62 | — |
| AO12 + 3 lights | 12.99 | — |
| AO8 | 10.87 | 23.81 |
| AO8 + rays8 | 11.63 | 26.32 |
| AO8 + DOF12 | 17.24 | — |
| AO8 + DOF12 + rays8 | 18.18 | 43.48 |
| AO16+DOF16+rays16+3 lights+shadow4 taps | 27.78 | 76.92 |
| same without the shadow map | — | 71.43 |
| same with `set 12` (scene half-res) | — | 66.67 |

**Effects are additive.** Predicted from `floor + Σ(effect − floor)` vs
measured: AO+DOF 21.9/21.74, AO+rays 14.5/14.08, DOF+rays 16.0/15.62, AO+3
lights 13.6/12.99 — every pair inside 0.6 ms (≤4%). The one stack that came out
above its sum is AO8 + 2 lights + shadow (predicted 12.8, measured 14.49), so
budget ~2 ms of slack on a 4-effect stack. This matters twice: a preset can be
composed from the single-effect table without measuring every combination, and
the "one huge shader → register pressure" theory for the high stack cost is
**ruled out** on this build. The whole stack costs ~1.9× from 2D to 3D
(AO8+DOF12+rays8 18.18→43.48, HIGH 29.4→58.8), while the lightweight LOW tier
is 2.2× (5.52→12.20).

## 4. Biggest offenders

Ranked by 3D ms, because 3D is the real user case.

1. **SSGI at high settings — 142.86 ms 3D (17× the 120 Hz budget, 4.3× the 30 Hz one).**
   `21 1 24 12 25 8` runs 12 steps × 8 rays = up to 96 full-res depth taps per
   eye per pixel, for a low-frequency colour bleed. Even the default (6×4) is
   **+30.7 ms in 3D**. Evidence: `3d/results.tsv` rows `gi`, `gimax`.
2. **SSRTX — 21.7 ms at default, 250 ms at 48×4.** Steps × rays is a straight
   multiplier on a full-res march with no sky/far early-out. `ssrtxmax` is 7.5×
   the 30 Hz budget by itself.
3. **DOF — 20.9 ms 3D at 16 samples** (30.30 total), plus 5.5 ms for the bokeh
   polygon kernel in 2D. A full-res spiral gather whose *output is defocused by
   definition* — the most downsampleable pass in the set.
4. **AO — 17.0 ms 3D at 12 samples.** 7.6 ms in 2D for an effect whose whole job is a 120-unit contact shadow;
   cost is linear in samples (32 samples → 18.0 ms 2D).
5. **Shadow PCF taps, not resolution — 9.17 ms 3D total.** 9 taps cost 6.4 ms
   more than 1 tap; the shadow-map *render* is nearly free (re-rendering it
every 4th frame saves only ~0.3 ms). Default `57 9` is the expensive part.
6. **Probe visualisation — 12.4 ms 3D.** 16 debug spheres, a per-pixel
   `probeEnv()` loop and 16 draws, for a debug view. Disproportionate; it should
   not be reachable in a shipping profile.
7. **Per-light shadows — +6.3 ms 3D** for 3 lights at 6 steps (an 8-tap screen
   march per light per pixel), when the shadow map is already bound.
8. **Bokeh polygon kernel — +5.5 ms 2D** for the aperture shape: `atan`, `cos`
   and a `continue` per sample, re-evaluated for every tap.

## 5. Missed wins

Each with the measurement that points at it and a one-line proposal. **Nothing
here is implemented.**

1. **There is no post-pass resolution lever, and that is the biggest single
   win on the table.** `cmd set 12 1` ("half-res per eye") only shrinks the
   *scene* raster inside a still-full-res eye buffer: the FX pass keeps running
   at 1280×1534. Measured on the 76.9 ms 3D HIGH stack, `set 12` gives 66.7 ms
   (−13%), not the −75% that quartering the pixels should give. `set 3` (eye
   width 1280/960/640) moved the scene-only baseline by less than the noise
   (4.13 / 4.10 / 4.49 ms) — I could not confirm from logcat that it resized the
   eye FBO (the `SM64FBO eye … fbo=` line only logs on creation and the probe
   came back empty), so read that as "no effect measured", not "proven inert".
   *Proposal:* render the FX pass into a `scene_res/2` target and upsample in a
   dedicated pass (the denoiser's bilateral upsample is the proven pattern;
   BACKLOG already records that putting the upsample inside the big shader cost
   ~30 ms).
2. **SSGI should run at 1/4 res.** It is a screen-space colour bleed with a
   150-unit radius; nothing in it is high-frequency. +30.7 ms in 3D at 1/4 the
   pixels would be ~8 ms. *Proposal:* quarter-res GI prepass + bilateral
   upsample, same shape as the denoiser.
3. **DOF should run at half res.** 20.9 ms in 3D for a pass whose output is
   blurred. *Proposal:* half-res colour gather at 2× the CoC radius, then the
   existing full-res bilinear composite for the in-focus region.
4. **AO at half res + skip far/sky pixels.** 17.0 ms 3D, and every pixel pays
   12 taps even on sky. *Proposal:* half-res AO buffer, and `if (z > far*0.95)
   ao = 1.0` before the loop.
5. **Shadow taps 9 → 4 as the default, and reuse the shadow map for the
   per-light march.** 9 taps cost 6.4 ms more than 1 tap in 3D, and the cost
   is per-tap, so 4 taps lands near +2 ms by interpolation between the two
   measured points. The per-light
   `lightVis()` march (8 depth taps per light) duplicates what the shadow map
   already knows. *Proposal:* default `57 4`; replace `lightVis()` with one
   shadow-map sample when `gfx_shadow_enabled`.
6. **The camera-only velocity path is evaluated even when motion blur is off.**
   The FX shader computes `mvPx` unconditionally: when `uVelOn == 0` it takes the
   `else` branch (two `worldToView`, two `viewToNdc`, three matrix multiplies)
   for every pixel of every FX pass, blur or no blur. The measured size is small
   — per-vertex MB with no motion (4.98 ms) is 0.26 ms *below* camera-only MB
   (5.08 ms) and only 0.26 ms above the pass floor — but it is pure waste on
   every frame that does not blur. *Proposal:* guard the `mvPx` block with
   `if (uMBOn > 0.5 || uDebug > 8.5)`.
7. **The tile-max bound pass is optional and already documented as a win**
   (BACKLOG / MOTION-VECTORS): `240 0` skips a whole velocity-sized pass
   (+1.6 ms for 3×3 vs off in the orbit, +3.5 ms static). Keep default 1, but
   expose 0 in a performance profile.
8. **`fx 102` (MB samples) is a dead dial** — the shader uses `uMBSubSteps`
   (142). Anyone tuning "samples" with 102 will see nothing happen. Either wire
   it or remove it from the panel/README.

## 6. LOW / MED / HIGH profiles

Targets: **120 Hz = 8.33 ms, 60 Hz = 16.67 ms, 30 Hz = 33.33 ms** per presented
frame. Every number below is measured for that exact key list (rows `i/`, `h/`
and `j/`), not predicted. The presets are only FX keys — ship with the pacer on
(`200 1`, `201` 0 so it inherits the render mode).

### LOW — 120 Hz in 2D, 60 Hz in 3D

```
250=1 255=8 251=80 103=2
```
Crepuscular rays (8 march samples, 80%) + 2 dynamic lights. AO/DOF/GI/SSRTX
off.

| | measured | fps | target | verdict |
| --- | --- | --- | --- | --- |
| 2D | **5.52 ms** (5.29–7.46) | 181 | 120 (8.33) | fits, 2.8 ms spare |
| 3D | **12.20 ms** (11.63–12.66) | 82 | 60 (16.67) | **fits**, 4.5 ms spare |

Visual cost: no ambient occlusion, no depth of field, no GI, no reflections.
The scene is the raw game with light shafts and coloured lights. This is the
only tier that clears 60 Hz in 3D.

### MED — 60 Hz in 2D, 30 Hz in 3D

```
1=1 4=8 103=2 50=1 51=1024 57=4 250=1 255=8 251=80
```
LOW + AO 8 samples + shadow map at 1024 with 4 PCF taps.

| | measured | fps | target | verdict |
| --- | --- | --- | --- | --- |
| 2D | **15.87 ms** (15.62–15.87) | 63 | 60 (16.67) | fits, 0.8 ms spare |
| 3D | **30.30 ms** (29.41–30.30) | 33 | 30 (33.33) | fits, 3.0 ms spare |

Visual cost: adds contact AO and real shadows; rays and 3 lights as LOW. Note
the 2D headroom is thin — any extra effect drops it under 60.

### HIGH — 30 Hz in 2D, 17 fps in 3D

```
1=1 4=12 11=1 15=12 14=12 250=1 255=16 103=3 104=1 50=1 51=1024 57=4
```
AO 12 + DOF 12 samples at 12 px max blur + rays 16 + 3 shadowed lights +
shadow map.

| | measured | fps | target | verdict |
| --- | --- | --- | --- | --- |
| 2D | **29.41 ms** (28.57–30.30) | 34 | 30 (33.33) | fits, 3.9 ms spare |
| 3D | **58.82 ms** (55.56–58.82) | 17 | 30 (33.33) | **misses by 25 ms** |

Three repeats of the 2D row measured 27.78 / 29.41 / 30.30 ms and one outlier at
24.39 ms; treat ~29 ms as the number. Trimming max blur from 16 to 12 px
(`14 16`→`14 12`) does **not** move it (29.41 vs 30.30), because the DOF cost is
dominated by the sample count and the fraction of pixels out of focus, not by
the radius. To buy margin, drop DOF samples (`15 12`→`15 8`) or rays to 8.

Visual cost: everything — AO, cinematic DOF, shafts, shadowed colour lights.
**In 3D this tier is a 17 fps showcase, not a playable profile.** The gap is
DOF (30.3 ms of the 58.8 in 3D) plus the per-eye multiplier.

### What the tiers mean

| tier | 2D (one eye) | 3D (both eyes) | what it buys visually |
| --- | --- | --- | --- |
| LOW | 120 Hz @ 5.5 ms | 60 Hz @ 12.2 ms | shafts + coloured lights |
| MED | 60 Hz @ 15.9 ms | 30 Hz @ 30.3 ms | + AO + shadows |
| HIGH | 30 Hz @ 29.4 ms | ~17 fps @ 58.8 ms | + DOF + 3 shadowed lights |

The stereo multiplier is ~1.9× for AO/DOF/GI stacks but only ~2.2× for the LOW
tier (rays + lights), which is why LOW still clears 60 Hz in 3D. The 3D ceiling
is set by the per-eye fill cost of the post pass: 4.15 ms of scene + 9.35 ms of
empty pass leaves 3.2 ms for effects at 60 Hz. The way out is missed win #1
(post pass below eye resolution), not more dial tuning.

Optional extras, measured on 2D HIGH: `70=1` (SSRTX default) → 41.67 ms / 24 fps
(breaks the 30 Hz budget); probes (`110=1`) → +4.5 ms 2D / +12.4 ms 3D and are
a debug view, not a profile feature.

### Copy-paste

```bash
B="am broadcast -n com.vdavid003.sm64port/.DebugReceiver -a com.vdavid003.sm64port.CMD"
send() { for kv in "$@"; do adb shell $B --es cmd fx --ei k "${kv%%=*}" --ei v "${kv##*=}"; done; }

# LOW
send 250=1 255=8 251=80 103=2
# MED
send 250=1 255=8 251=80 103=2 1=1 4=8 50=1 51=1024 57=4
# HIGH
send 250=1 255=16 251=80 103=3 104=1 1=1 4=12 11=1 15=12 14=12 50=1 51=1024 57=4
```

Shadow keys are also reachable as `cmd shadow --ei k 0 --ei v 1` (enable),
`--ei k 1 --ei v 1024` (resolution), `--ei k 7 --ei v 4` (taps).

## 7. What to measure next

1. **A half-res FX target with its own upsample.** Predict 0.6–0.7× on every
   fill-bound effect; measure HIGH 2D/3D before and after. This is the only
   change that moves the 3D HIGH tier from 17 fps toward 30.
2. **GI and DOF at reduced resolution** (separately), with the denoiser's
   bilateral upsample. Predicted −20 to −22 ms 3D for GI.
3. **AO at half res + far-plane skip**, and AO samples 8 vs 12 at the same
   radius — is 8 visibly worse in stereo?
4. **Per-light shadow march vs one shadow-map tap** (`lightVis()`), measured
   with 3 lights and shadows on.
5. **HIGH with fewer DOF samples (`15 12`→`15 8`) or rays 8 instead of 16** —
   max-blur trimming was measured and does not help; sample count is the dial
   that should buy 3-5 ms of 30 Hz margin in 2D.
6. **A 3D 60 Hz candidate with AO**: LOW is 12.20 ms in 3D; adding AO8 costs
   +17 ms, so a half-res AO (missed win #4) is what would make "60 Hz 3D with
   contact shadows" possible. Measure after the resolution work.
7. **Rebuild-free key audit after the merge queue lands.** Three worktrees are
   renumbering `fx` keys (`27x` occlusion, glow keys); the key table in this
   document is from the 15:20 build and two keys (`31`, `250`) already behaved
   differently earlier in the same day.
8. **Thermal drift**: repeat the LOW/MED/HIGH rows cold and after 10 minutes of
   load to quantify the ±10%.

## Raw data

All under `/media/share/tmp/artifacts/sm64/fx-perf/`:

| path | content |
| --- | --- |
| `2d/results.tsv` | individual effects, 2D (21 rows) |
| `3d/results.tsv` | individual effects, 3D (21 rows) |
| `c2/results.tsv` | additivity pairs + 2D stacks, with stills `c2/<row>.png` |
| `c3/results.tsv` | 3D stacks, stills `c3/<row>.png` |
| `g/results.tsv` | ALT/denoiser matrix + moving-camera MB matrix |
| `h/results.tsv` | AO-based presets (AO8/AO8+rays/AO12+DOF), stills `h/<row>.png` |
| `i/results.tsv` | the final LOW/MED/HIGH key lists (120 Hz 2D, 60 Hz 3D, HIGH-safe), stills `i/<row>.png` |
| `j/results.tsv` | HIGH at max blur 12, 2D and 3D (the 24.39 outlier and its repeats) |
| `*/steps/<row>.lines.txt` | raw `cmd dump` lines per sample (DUMP + PTC) |
| `driver.sh`, `plans/*.sweep` | the harness and every plan, rerunnable |

Each row's raw status lines are kept so any number can be re-derived
(`fps=` → ms) or re-run: `AGENT_TABLET_MUTE=1 scripts/agent-lock.sh device --
/media/share/tmp/artifacts/sm64/fx-perf/driver.sh <plan> <out> --mode 2d|3d`.

Driver gotchas worth knowing before reusing it: it applies BASE + the row
tokens keyed by key (not by string, which bit the first pass), takes 3 samples
and discards the first, verifies the state the app reports and re-applies once
on a mismatch, aborts if the scene leaves level 38, and by default restarts the
app only when it is not already in the PTC.
