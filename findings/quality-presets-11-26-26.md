# Quality presets: LOW / MED / HIGH in one tap

2026-11-26, worktree `w4-presets`. Follow-up to the 11-25 sweep's §6 tiers and
to the three reduced-resolution rewrites that landed since (C.DOF −53 %, MXAO
−67 %, SSGI −77 %) plus the half-res FX/post pass (`fx 420`). The 11-25 tiers
are stale: their AO and DOF rows measured the **full-resolution** paths
(AO ~17 ms, DOF ~21 ms in 3D), and their `103=` dynamic-light rows were dead
keys.

Raw data, plans, stills and the drivers:
`/media/share/tmp/artifacts/sm64/presets/`.

## 1. What shipped

- `fx 430 n` applies a named bundle (0 = LOW, 1 = MED, 2 = HIGH) in one call.
  `fx 431` reads back the tier applied last (read-only).
  `cmd profile --es name low|med|high` is the same thing by name; `cmd dump`
  reports `preset=`.
- The first FX page opens with **QUALITY: LOW / MED / HIGH** (pills plus a
  one-line description of the selected tier). The choice is persisted
  (`fx_preset`) and re-applied **after** the panel is built, because the panel's
  own construction pushes its defaults to native and would clobber anything
  applied earlier.
- The bundle is data, not code (`gfx_opengl.c`): a base list that switches
  every optional effect off and resets the reduced-res chains to their shipping
  dials, then a per-tier delta, all fed through `gfx_opengl_ssfx_set()` itself.
  So a preset cannot reference a key that does not exist, adds no shader work,
  keeps every value visible to `cmd fx` and the panel, and a tap is **absolute**
  (HIGH → LOW clears DOF, the 9-tap shadow and the full-res pass).
- Default tier **LOW**. The panel row also routes `cmd fx 430` back through the
  Java side so the MXAO/C.DOF/RAYS/SHADOW toggles and their sliders follow.

## 2. Method

Same rig as the 11-25 sweep and the resolution work: Lume Pad 2, GLES2, **PTC
level 38 `ptc fadetest`** fixed camera (`-100,1500,-3000` → Mario), `cmd
stereo 1`, **`fx 200 0`** (pacer off) in every row, `fps=` from `cmd dump` → ms,
3 samples of each tier, median.

| mode | how | what runs |
| --- | --- | --- |
| 2D | `cmd stereo 0` (= stereo int `21` = 1, FLAT) | one eye 1280×1534, chain once |
| 3D | `cmd stereo 1` | two eyes, chain twice |

`--stereo 0` is not "stereo off": `setStereoMode()` always calls
`nativeSetStereoMode(1)`, so the per-eye path stays on and only the parallax is
zeroed (`gfx_stereo_2d`). The 11-25 note "with `cmd stereo 0` the whole per-eye
path is bypassed" does not hold on this build — the 2D rows measure 45–55 % of
the 3D row, the one-eye/two-eye ratio.

Rows run through `scripts/fx-sweep.sh` from `plans/presets3d.sweep` (15 rows:
three repeats of each tier plus the tuning variants), `plans/presets2d.sweep`,
`plans/clean3d.sweep` (interleaved trims) and `plans/highfix3d.sweep` (void, see
§6). Drivers: `driver.sh` (3D + panel + persistence), `driver2.sh` (2D +
tuning), `driver3.sh` (clean interleaved trims).

## 3. Measured cost

3D, both eyes, pacer off (LOW/MED = median of 3; HIGH = mean of the 5
interleaved samples in §4):

| tier | bundle | 3D ms | 3D fps | vs 30 Hz (33.33) |
| --- | --- | --- | --- | --- |
| LOW | rays 8 @ 80 %, FX 1/2 | **9.20** (9.17–9.22) | **108.7** | 60 Hz with 7.5 ms spare |
| MED | + AO 8 half, shadow 4 taps | **14.18** (14.16–14.33) | **70.5** | 60 Hz with 2.5 ms spare |
| HIGH | + AO 12, DOF 12 half, rays 16, shadow 9, FX full, ACES | **36.6** (35.8–36.8) | **27.3** | **2.9 ms over** |

2D (one eye):

| tier | 2D ms | 2D fps |
| --- | --- | --- |
| LOW | **4.41** (4.40–5.38) | 227 |
| MED | **6.47** (6.12–7.89) | 155 |
| HIGH | **13.4** (10.85–16.16) | 62–92 |

Run-to-run drift dominates the 2D table: the batch ran straight after the 3D
batch and the same three rows drifted up 22 %, 22 % and 49 % over ~5 minutes
(hot tablet). The HIGH 3D rows, by contrast, repeated to ±0.5 % across two
batches 20 minutes apart, so the 3D numbers are trustworthy and the 2D HIGH
should be read as "10.9 ms cool, 16.2 hot".

The 3D HIGH number is also 23 % above the 28.57 ms the same key list measured
in `fx-pass-half-res-11-26-26.md` (that row had shadow 4 and no grade, which
this run prices at −1.8 and −2.1 here, so ~32.7 ms is the same-config
comparison): the tablet was hotter for every preset batch. Plan on
**~27–35 fps for HIGH** depending on thermal state.

## 4. Where HIGH's 36.6 ms goes

Interleaved A/B on one fresh app start, 5 base samples bracketing the trims
(`clean3d/`), so drift cannot be mistaken for an effect delta:

| row | 3D ms | fps | Δ vs base |
| --- | --- | --- | --- |
| HIGH as shipped (×5) | 36.76 / 36.23 / 36.63 / 36.76 / 36.76 | 27.2–27.6 | — |
| HIGH without ACES (`280 0`) | 34.48 | 29.0 | **−2.1** |
| HIGH, shadow 4 (`57 4`) | 34.84 | 28.7 | **−1.8** |
| HIGH, DOF 8 taps (`15 8`) | 35.21 | 28.4 | **−1.3** |

From the full-stack sweep (same scene, same batch as the tier rows):

| row | 3D ms | fps | reading |
| --- | --- | --- | --- |
| HIGH at FX 1/2 (`420 2`) | 23.31 | 42.9 | full-res FX costs **12.7 ms** in 3D; that is what the stereo A/B's "visibly coarser soft gradients" buys back |
| HIGH without DOF (`11 0`) | 29.24 | 34.2 | the half-res DOF chain owns **6.8 ms** |
| HIGH + 3 shadowed lights (`103 3 104 1`) | 48.31 | 20.7 | **+12.3 ms** — lights stay a manual dial |
| MED + 2 lights (`103 2`) | 14.62 | 68.4 | +0.4 ms (the default light rig is a PTC showcase, not gameplay) |
| LOW + 2 lights | 9.61 | 104.1 | +0.4 ms |

The three trims are additive: HIGH without ACES + 4-tap shadow would land at
~32.4 ms (30.9 fps), all three at ~31.3 ms (32 fps). Nothing else in the tier
is close to that cost.

## 5. Quality: what each tier loses

Stills, same camera: `3d/<row>.png` (SBS), `2d/<row>.png`, and the panel rows
`panel_quality.png` / `panel_quality_high.png`.

- **LOW** — the raw game with light shafts. No contact AO, no DOF, no shadow
  map. FX at half res: silhouettes and thin geometry ~1 px softer and the
  per-pixel dither grid becomes 2×2 blocks (the measured `420 2` trade). The
  only tier that clears 60 Hz in 3D with room to spare.
- **MED** — adds half-res AO and the 4-tap shadow map. AO keeps the amount but
  softens contact by ~2 px and perturbs flat walls 4× more than the full-res
  gather (from the MXAO findings); the shadow penumbra is the chunkiest of the
  three.
- **HIGH** — 12-tap AO, half-res C.DOF, 16-sample rays, 9-tap PCF, full-res FX
  pass and the ACES tonemap. No dynamic lights and no bloom: +12.3 ms and
  (bloom) its own pass, both well outside the 30 Hz class.

## 6. The bundles

`fx 430` applies the base first, then the tier delta. Every entry is an
existing key from the same table `cmd fx` uses.

```
base (all tiers)  1=0 11=0 50=0 103=0 21=0 70=0 100=0 261=0 280=0
                  282=0 283=100 284=0 285=100 286=100 287=0 288=0
                  420=1 421=0
                  390=1 391=2 393=0 394=1 2=100 4=8
                  380=1 381=2 14=12 15=12
                  250=0 255=8 251=80
                  51=1024 57=4 104=0
LOW   (430=0)     420=2  250=1 255=8 251=80
MED   (430=1)     420=2  1=1 4=8  50=1 57=4  250=1 255=8 251=80
HIGH  (430=2)     420=1  1=1 4=12  11=1 15=12 14=12
                  250=1 255=16 251=80  50=1 57=9
                  280=1 281=2 284=0
```

## 7. Two measurement traps found on the way

1. **`fx-sweep.sh --extract <field>` reads the PTC status line only.** `preset=`
   lives on the DUMP line, so `--extract preset` makes row 1 abort the whole
   sweep: the assignment inherits grep's exit status and `set -e` kills the
   script. The drivers verify the tier from logcat instead.
2. **`381 3` / `381 4` do not recover in-process.** After a runtime change of
   the C.DOF divisor the same `430=2` re-application measured 26.3 / 34.6 ms,
   and `381=3` / `381=4` measured 71.9 / 66.7 / 19.5 ms for the same scene —
   i.e. the reduced-res DOF targets are re-created wrongly and the pass
   flip-flops between "skipped" and "legacy full-res". Treat `381` as a
   boot-time dial; a fresh app start restored 36.6 ms. That voided the
   `fix3d` batch (rows 2–8) and is filed in BACKLOG.

## 8. Rerun

```sh
AGENT_TABLET_MUTE=1 scripts/agent-lock.sh device --timeout 1500 --reason "presets" -- \
    /media/share/tmp/artifacts/sm64/presets/driver.sh
AGENT_TABLET_MUTE=1 scripts/agent-lock.sh device --timeout 900 --reason "presets 2d" -- \
    /media/share/tmp/artifacts/sm64/presets/driver2.sh
AGENT_TABLET_MUTE=1 scripts/agent-lock.sh device --timeout 600 --reason "presets clean" -- \
    /media/share/tmp/artifacts/sm64/presets/driver3.sh
```

Driver 1 installs the worktree APK, runs the 3D plan, then the panel and
persistence checks. Driver 2 runs the 2D plan and the (now void) tuning batch
without a level reload. Driver 3 restarts the app for clean DOF targets and runs
the interleaved trims.

## 9. What to change next

- **If HIGH must hit 30 Hz on this tablet, drop the ACES tonemap** (`280 1 281 2
  284 0` → `280 0`): 34.48 ms / 29.0 fps, −2.1 ms, the brief's own "optional
  grade". Adding the 4-tap shadow (`57 9`→`57 4`, −1.8) reaches 32.4 ms /
  30.9 fps. Either is a one-line change in `s_fx_preset_high` plus a re-measure.
- **MED could take the AO quarter-res mode** (`391 4`, −28 % on the AO chain):
  it is at 14.2 ms with 2.5 ms of 60 Hz headroom, so the trade is only worth it
  if real levels push it over.
- **LOW has 7.5 ms of headroom**: adding AO 8 (the MED delta is +5.0 ms) would
  still fit 60 Hz and would make "smooth" a much better-looking tier. Worth a
  look on a real level before shipping.
- Fix the `381` re-allocation bug and then re-price DOF div 3/4; both were
  intended as cheap quality/performance levers and neither is usable today.
- Dynamic lights remain the biggest visual add-on not in any tier: 3 shadowed
  lights cost +12.3 ms in 3D, so they only fit a "CINEMATIC" tier above HIGH
  (which at 48.3 ms would be a 20 fps showcase).
