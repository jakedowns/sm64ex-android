# HFR: held objects pulse in scale (Bowser tail spin)

Investigated 2026-11-25. Worktree `bowser-scale`.

**Report:** in the intro/attract tail-spin, Bowser visibly grows and shrinks
between frames at 120 Hz. Reported for anything Mario grips; suspected that HFR
matrix interpolation is the cause.

**Result:** the report is right that HFR is responsible, but the mechanism is
not the matrix *blend*. There were **two** defects stacked:

1. **Primary (the visible 4× pulse): the held object's previous matrix was
   built from a different parent chain than its current matrix.**
   `geo_process_held_object()` builds the current transform from the holder's
   **root** matrix (`gCurGraphNodeObject->throwMatrix`) and the previous one
   from the local matrix stack at the held node. Those are different
   transforms — in Mario's holding pose the local chain resolves to a uniform
   **0.25** scale. So at `alpha = 0` Bowser was drawn at 25% size, growing to
   100% across the tick and snapping back: a 4× pulse at 30 Hz. This is
   mechanism (b) in the report, not the element-wise lerp, and it is invisible
   with interpolation off because `alpha = 1` always shows the correct matrix.
2. **Secondary (the residual 2.6% shimmer): the element-wise 4×4 blend shrinks
   any fast rotation by `cos(delta/2)`.** After fix 1, the remaining per-tick
   spin of 22.5° still squashes the rotating plane of the model by up to 2.6%.
   The camera view is baked into every modelview, so a fast camera turn does
   the same thing to the whole scene. This is mechanism (a).

The `gfx.scale` values, the previous/current timestamps and the holder gap were
all correct during the spin; nothing was double-scaled and nothing was
interpolated from a stale tick.

## Why the primary pulse happened

Matrices are row-vector (`v' = v * M`): rows 0–2 are the transformed basis
vectors (rotation × scale) and row 3 is the translation.

```c
// current: holder root, with the hand's world translation spliced in
mtxf_copy(gMatStack[i + 1], gCurGraphNodeObject->throwMatrix);
gMatStack[i + 1][3] = gMatStack[i][3];               // hand position
mtxf_mul(gMatStack[i + 1], mat, gMatStack[i + 1]);   // hand offset
mtxf_scale_vec3f(gMatStack[i + 1], ..., objScale);

// previous: local hand stack, NOT the holder root — the bug
mtxf_mul(gMatStackPrev[i + 1], mat, gMatStackPrev[i]);
mtxf_scale_vec3f(gMatStackPrev[i + 1], ..., objScale);
```

At `alpha = 0` the displayed object is exactly the "previous" matrix, so the
held object jumped to 25% scale at the start of every tick. The measured
per-row scale of the previous root was `0.25, 0.25, 0.25` on every sampled
frame; the current root was `1.0, 1.0, 1.0`.

The fix mirrors the current construction exactly: the previous root is the
holder's previous root matrix (`gMatStackPrev` at the holder's object node,
captured as `gCurObjectRootPrev`), with the hand's previous world translation
spliced in, then the hand offset and object scale.

After that, the previous and current matrices differ only by the holder's
per-tick motion (yaw), which is what HFR is supposed to interpolate.

## Why the residual pulse happens

Two ticks of a spinning object differ by a rotation of `θ` about (mostly) the
spin axis. The old blend is

```
out = prev * (1 - alpha) + cur * alpha          (16 floats, element-wise)
```

For a pure rotation about one axis and unit scale, the rows in the rotation
plane have length `sqrt((1-t + t*cos θ)^2 + (t*sin θ)^2)`: `1` at both ends and
`cos(θ/2)` at `t = 0.5`. The row along the spin axis stays `1`, so the model is
not even uniformly shrunk — it squashes along the spin plane and springs back
every tick. With Mario's maximum spin of `0x1000` per tick (22.5°):

| alpha | row 0 (in plane) | row 1 (spin axis) | row 2 (in plane) |
|-------|------------------|-------------------|------------------|
| 0.00  | 1.0000           | 1.0000            | 1.0000           |
| 0.25  | 0.9856           | 1.0000            | 0.9856           |
| 0.50  | 0.9808           | 1.0000            | 0.9808           |
| 0.75  | 0.9856           | 1.0000            | 0.9856           |
| 1.00  | 1.0000           | 1.0000            | 1.0000           |

Measured worst case on the drawn body matrices is 2.6% because a body part adds
its own animated rotation on top of the holder's yaw.

## Telemetry

`rendering_graph_node.c` gains a debug-only per-present log (off by default):

- `gfx_hfr_scale_debug` — one `SM64HFR` line per presented frame.
- `gfx_hfr_slerp` — runtime A/B switch for fix 2.
- `gfx_hfr_force_alpha` — force every present to a fixed alpha for stills.

`ptc` actions (all runtime, no rebuild):

| action | effect |
|--------|--------|
| `hfron` / `hfroff` | start/stop the per-frame `SM64HFR` log (and reset the window) |
| `hfrdump` | print the current window summary to logcat |
| `hfrslerpon` / `hfrslerpoff` | fix 2 (decompose + slerp) vs the legacy element-wise blend |
| `hfralphaNNN` | force alpha `NNN/1000` on every present; `hfralpha` clears |
| `bowser` | spawn Bowser on the PTC floor and hold him in a full-speed tail spin |
| `bowseroff` | drop and delete the rig |

Each line carries `frame`, `tick` (`gGlobalTimer`), `alpha`, `fps`, `interp`,
slow-mo divisor, held behaviour/action/held-state/animation frame,
`gfx.scale[0..2]`, the per-row scale of `prev`, `cur` and the presented `out`,
the mean scale, `dev = max_row |out_i / lerp(prev_i, cur_i) - 1|` (blend error),
`hdev = max |out_row / cur_row - 1|` over every drawn held body matrix (the
number the eye sees), `pvmode` (0 = element-wise, 1 = slerp), the holder
timestamp `gap`, `peq` (`prev == cur`), the stack index and the previous/current
root translations.

`ptc_format_status()` appends a compact window summary:

```
hfr=1,f=66,s=0.988..1.000,dev=0.0186,body=0.0260,dmax=0.0095,jump=49
```

## Reproduction (one adb sequence)

```bash
B="am broadcast -n com.vdavid003.sm64port/.DebugReceiver -a com.vdavid003.sm64port.CMD"
adb shell $B --es cmd level --ei n 38          # PTC, open floor
adb shell $B --es cmd fps --ei v 2             # 120 Hz
adb shell $B --es cmd ptc --es action bowser   # spawn Bowser, force the spin
adb shell $B --es cmd ptc --es action hfron
adb logcat -c && sleep 4
adb logcat -d -v time -s SM64HFR:I > legacy.log
adb shell $B --es cmd ptc --es action hfrslerpon
adb logcat -c && sleep 4
adb logcat -d -v time -s SM64HFR:I > fixed.log
adb shell $B --es cmd interp --ei v 0          # HFR off (baseline)
```

`scripts/hfr-bowser-capture.sh` runs the whole matrix (legacy 120, fixed 120,
legacy 60, HFR off, logged alpha=0.5 stills) inside one device-lock hold and
writes to `/media/share/tmp/artifacts/sm64/bowser-scale/<stamp>/`.
`scripts/hfr-scale-check.py` turns the logs into per-frame CSV and a verdict.

`ptc bowser` needs Bowser's assets, so the PTC level script now loads
`script_func_global_13` (segment 6 + group 12 geo). That costs the level a few
hundred KB of pool; the PTC level still loads.

## Fix

Two changes in `rendering_graph_node.c`:

1. **Held previous matrix from the holder root** (`gCurObjectRootPrev`, set in
   `geo_process_object()` for the object currently being drawn). This removes
   the 4× pulse; it is exact, not a heuristic, because it mirrors the current
   path one tick earlier.
2. **Rotation-aware blend** in `gfx_patch_frame()`. A **model** matrix whose
   rotation changes enough to matter is decomposed into translation + rotation
   + per-row scale; the rotation is slerped, scale and translation are lerped,
   and the matrix is recomposed. Trigger: `dot(qPrev, qCur) < 0.9999`
   (≈1.6° per tick, under 0.01% element-wise error). Projection matrices,
   reflections (`det < 0.5`) and singular rows keep the old element-wise path.
   Cost: two quaternion conversions at record time per fast-rotating matrix,
   one slerp per present; static/slow matrices pay nothing.

Fix 2 alone would not have helped: with the broken previous matrix the slerp
still interpolated the wrong scale (0.25 → 1.0). The slerp phase of the first
capture still covered mean scale 0.250–0.890 and the alpha=0.5 still showed a
half-size Bowser.

## Measurements

Two captures agree: `20261125-150257` (final build) and `20261125-145221`,
under `/media/share/tmp/artifacts/sm64/bowser-scale/` (per-frame CSVs next to
the logs, `analysis.txt`, `hfr-bowser-strip.png`). 2560×1600, PTC floor, Mario
at full spin (`angleVel[1] = 0x1000`), `fps=2` (120 Hz) unless noted. Numbers
below are the final run; the earlier run differs by <0.2 percentage points.

**Before fix 1** (`20261125-144351`, legacy element-wise blend, 120 Hz):
the held root's previous rows were `0.25, 0.25, 0.25`, current `1,1,1`.
Presented mean scale covered **0.250 → 0.859** across the sampled alphas — a
**60.9% swing** (the true intra-tick extremes are 0.25 and 1.0, a 4× pulse),
blend error up to 42%, 402/536 frames over 0.5%. At 60 Hz the same run showed a
29.6% swing. The `alpha=0.5` still shows Bowser at roughly two-thirds size.

**After fix 1** (150257, same rig):

| phase | mean scale range | body distortion `hdev` | blend dev | frames >0.5% |
|-------|------------------|------------------------|-----------|--------------|
| 120 Hz, legacy blend, `hfrslerpoff` | 0.9876 – 1.0000 (1.24%) | **2.60%** | 1.86% | 404 / 539 |
| 120 Hz, slerp blend, `hfrslerpon` | **1.0000 – 1.0000 (0.00%)** | **0.00%** | 0.00% | 0 / 527 |
| 60 Hz, legacy blend | 0.9908 – 1.0000 (0.92%) | 1.92% | 1.38% | 96 / 192 |
| HFR off (`interp 0`) | 1.0000 – 1.0000 | 0.00% | 0.00% | 0 / 383 |
| forced alpha 0.5, legacy | 0.9876 – 0.9876 | 2.60% | 1.86% | 258 / 258 |
| forced alpha 0.5, slerp | 1.0000 – 1.0000 | 0.00% | 0.00% | 0 / 266 |

Representative legacy frame (fixed previous matrix, `alpha=0.5`):

```
prev=(1.000,1.000,1.000) cur=(1.000,1.000,1.000)
out=(0.9814,0.9974,0.9840) mean=0.9876 hdev=0.0241 pvmode=0
```

With the slerp blend the same frame is `out=(1.0000,1.0000,1.0000)`, `hdev=0`.

Frame strip (`hfr-bowser-strip.png` in the artifacts dir): old prev-chain blend
at alpha 0.5 (Bowser visibly small), fixed prev + legacy blend, fixed prev +
slerp, HFR off.

## Risk to other HFR content

- Fix 1 only changes held-object rendering; nothing else reads
  `gCurObjectRootPrev`.
- Fix 2 leaves slow/static matrices on the old element-wise path (byte-identical
  output), leaves projection matrices alone, and falls back for reflections and
  singular scales. Fast spinners (spinning platforms, shells, fast camera turns)
  get a rigid rotation instead of a squashed one — also a fix.
- Non-uniform scale under rotation keeps its per-row interpolation, so Bowser's
  death squash is unaffected.

## Follow-ups

- Held object scale is still taken from the **current** tick for both `prev` and
  `cur` in `geo_process_held_object()`. A held object whose `gfx.scale` changes
  per tick would step instead of interpolating. No Mario-held object animates
  scale today, but it is a one-line follow-up if one appears.
- Other held objects (shells, poles, trees) exercise the same path; the
  telemetry already logs them (`behavior`, `act`, `hs`). Worth a sweep with
  `ptc hfron` on a shell and on the tree/pole grabs if a residual pulse is ever
  reported.
- The attract-mode demo itself was not driven; the repro uses the same
  `ACT_HOLDING_BOWSER` code path. `cmd logo` plus 800 idle frames on PRESS
  START starts the recorded demo, but it is not deterministic.
