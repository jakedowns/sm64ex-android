# Denoiser research — alternative rendering path (dr01)

Date: 2026-11-23
Scope: pick **one** open-source denoiser that can sit on the *alternative*
rendering path (cheap/noisy frame in, denoised frame out) on the Leia Lume Pad 2,
without touching the default path.

Everything below that is not a quoted spec is an **estimate** and is marked as
such. Nothing here has been timed on the tablet yet — the numbers are derived
from the device's peak capability plus the cost model of each filter, and the
doc says where the measurement is the only way to settle the question.

---

## 1. Bottom line

**Recommended: a GLES2, temporally-reprojected edge-aware à-trous ("SVGF-lite")
filter, written in this repo's own shader style, running on the noisy *effect*
buffer at reduced resolution (quarter to half of the per-eye size), 3 à-trous
iterations plus one temporal-accumulation pass.**

- Estimated cost: **6–14 ms/frame for both eyes** at quarter/half res
  (estimate, ±2×; section 5.3 gives the arithmetic and the bandwidth check).
- It is pure fragment shading: no compute, no Vulkan, no ML runtime, no extra
  APK weight, no readback stall. It fits the existing per-eye post pass that
  already reads `GL_OES_depth_texture` and reconstructs normals (`SSFX.md`).
- License: written by us, so no upstream obligation beyond the paper citation
  (algorithm, not code).

**Fallback (cheapest possible):** temporal accumulation only — jittered
half-res effect buffer, reproject + clamp against history, no spatial passes.
Roughly **2–4 ms/frame** (estimate) at visible ghosting on moving geometry.

**Explicitly not viable at 30–60 fps on this device:** OIDN, OpenCV
non-local-means, and full-resolution CNN inference (DnCNN/FFDNet). Details and
numbers in section 5. They are not bad denoisers — they are 1–2 orders of
magnitude outside the frame budget, and OIDN additionally needs a GPU→CPU
readback that alone costs a frame.

---

## 2. Target hardware

The brief guessed "Snapdragon 8 Gen 1 / Adreno 730". **That is wrong by one
generation.** Three independent checks agree on Snapdragon 888:

| Item | Value | How it was established |
| --- | --- | --- |
| SoC | Qualcomm **SM8350**, Snapdragon 888 5G (5 nm) | on-device `getprop ro.soc.model` → `SM8350`, `ro.board.platform` → `lahaina` |
| GPU | **Adreno 660** | on-device `dumpsys SurfaceFlinger` → `GLES: Qualcomm, Adreno (TM) 660` |
| GL driver | `OpenGL ES 3.2 V@0530.0 (GIT@f3aa497c33, I7c31881d50)`, build date **2021-12-16** | same dump |
| RAM | **8 GB** (8192 MB) | UL 3DMark device page; Schenker retail listing |
| Storage | 128 GB + microSD | UL / Schenker |
| OS | **Android 12** (API 31) | on-device `ro.build.version.release` / `sdk`; UL listing agrees |
| Display | 12.4", **2560×1600**, **120 Hz**, lenticular 3D | Schenker listing (120 Hz); on-device SurfaceFlinger `refresh-rate: 120.00fps` |
| Model | `LPD-20W` (Lume Pad 2) | on-device `ro.product.model` |
| GPU sanity score | 3DMark Wild Life **5681** (GT1 ≈ 34 fps), Wild Life Extreme **1516** (≈ 9 fps) | UL benchmark median for this exact device |

Sources: on-device dumps taken 2026-11-23 under the device lock;
<https://benchmarks.ul.com/hardware/tablet/Leia+Lume+Pad+2+review>;
<https://www.schenker-tech.de/en/leia-lume-pad-2-3d-tablet/>.

Consequences for a denoiser:

- **Adreno 660 is a tile-based (HSR) GPU** with roughly 1.2 TFLOP/s fp32 peak
  and ~2–4 Gtexel/s bilinear texture throughput (estimate, from Adreno 6xx
  class parts). It is a 2021 flagship: strong for fragment shading, weak for
  anything that needs a separate compute API.
- **The driver is ES 3.2 capable even though the renderer targets ES 2.0.** So
  `textureLod`, MRT and integer ops are physically available; the *path* is
  ES2-only by policy. If a later variant needs `textureLod` for a mip-pyramid
  variance estimate, that is a policy question, not a hardware one.
- **LPDDR5, ~50 GB/s class, shared with the CPU** (estimate for this platform).
  On a mobile SoC the filter that survives is the one that touches least memory,
  not the one with fewest ALU ops. This is the single most important constraint
  in this document.
- **8 GB RAM** is generous, but the app ships `arm64-v8a` only and
  the per-eye FBOs are large: one RGBA16F eye target at 1280×1534 is **15.7 MB**,
  so a 3-deep ping-pong of full-res history buffers costs ~94 MB for both eyes.
  Half res is 3.9 MB per eye buffer. Budget accordingly.

### 2.1 What "no compute / no Vulkan" costs us

| ES 2.0 limitation | Effect on denoiser design |
| --- | --- |
| No compute shaders, no SSBO/images | All filter work is fragment passes over FBOs; no shared-memory tiles, no workgroup reductions |
| No `textureLod` in fragment shaders (needs `EXT_shader_texture_lod`) | Mip-pyramid variance estimate must be built with explicit downsample passes, or skipped |
| MRT only via `EXT_draw_buffers` | Write one target per pass; avoid designs that want to emit 3 AOVs at once |
| Loops must have constant bounds | Fixed 3×3/5×5 taps, unrolled; no adaptive "walk until converged" loops |
| `mediump` is fp16 | Weight math must stay in `highp` or be renormalised; fp16 is fine for the accumulation, marginal for variance |
| Float render targets need `OES_texture_float`/`OES_texture_half_float` + `EXT_color_buffer_half_float` | Adreno supports both, but verify at runtime and keep an RGBA8 fallback (encode luminance/variance into channels) |

---

## 3. Comparison

`ms/frame @1600×1000` means the whole frame the app would denoise (either the
1.6 MP capture or both 1.96 MP eye buffers — the difference is small next to the
error bars). **Every number in this column is an estimate** unless marked
*measured*.

| # | Algorithm | License | Runtime | Model / data size | Est. ms/frame @1600×1000 | GLES2 feasible? |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | **Intel OIDN** 2.5.1 (RT filter) | Apache-2.0 | CPU only (ARM64 supported; Android **not** an official OS) | weights **1.84 MB** (rt_hdr_alb_nrm.tza, verified); "small" variant **641 KB**; run-time workspace est. 150–500 MB | **250–900 ms** (est., 6–8 threads, NEON) + 5–15 ms readback | No — CPU library, needs `glReadPixels` round-trip per frame |
| 2 | **OpenCV** `fastNlMeansDenoising{,Colored}` | Apache-2.0 | CPU | ~441 neighbour comparisons/px; lib adds ~5–20 MB per ABI | **300–1500 ms** (est.) | No — CPU; also spatially incoherent (flicker) |
| 3a | **SVGF** (Schied et al., HPG 2017) | paper; NVIDIA reference **BSD-3** but DXR/HLSL only | fragment/GLSL after port | no model | **8–25 ms** at half res, 3–4 passes (est.) | **Yes, this is the family to use** |
| 3b | **À-trous wavelet** (Dammertz et al. 2010) | MIT C++ reference (`alexdalat/atrous`, CPU) | fragment/GLSL after port | no model | same as 3a (SVGF = à-trous + temporal + variance) | **Yes** |
| 3c | NVIDIA **NRD** | NVIDIA RTX SDK license (proprietary, vendor-scoped) | D3D12/Vulkan compute | n/a | n/a | **No** — license effectively excludes Adreno; compute-only |
| 3d | AMD **FidelityFX Denoiser** | MIT (FidelityFX SDK) | HLSL, D3D12/Vulkan compute | n/a | n/a | No — compute/wave ops; usable as algorithm reference only |
| 4a | **DnCNN** (17×64ch) via ncnn/TFLite | KAIR impl **MIT**; original repo **no license** (avoid) | CPU (NEON) / Vulkan / NNAPI | weights **~2.2 MB** fp32, ~1.1 MB fp16 | **2–10 s** CPU; **50–200 ms** via Vulkan GPU; ~**130+ ms** int8 NPU | No at full res; only at ≤1/8 res as a guided-upsample prior |
| 4b | **FFDNet** via KAIR | MIT (KAIR); original MATLAB repo unlicensed | same as 4a | ~2.5 MB fp32 | ~1/4 the FLOPs of DnCNN → **500 ms–2.5 s** CPU (est.) | No at 30–60 fps |
| 4c | MobileNet-style joint-lateral denoiser (own training) | Apache-2.0 runtime (TFLite) + our weights | TFLite CPU/GPU delegate | 0.3–2 MB int8 | 30–120 ms (est.) — **and needs a training pipeline + our noise model** | Borderline; not implementable inside this task |
| 5 | Qualcomm AI Hub / QNN NPU demos | model-dependent; **QNN SDK is proprietary** with redistribution terms | Hexagon NPU via QNN/SNPE | varies | unknown; NPU path doubles the build system and adds a vendor SDK | No for this codebase |
| — | **Temporal accumulation only** (fallback) | ours | GLES2 fragment | none | **2–4 ms** (est.) | Yes |
| — | **TAA/FXAA/SMAA** (context) | MIT/BSD-ish | GLES2 fragment | none | 1–3 ms | Yes — anti-aliasing, not denoising |

Notes on individual rows:

1. **OIDN** — verified from the v2.5.1 tree: Apache-2.0, "ARM64 (AArch64)
   architecture CPUs (e.g. Apple silicon CPUs)" listed as supported, and the
   system requirements say *"a 64-bit Windows, Linux, or macOS operating
   system"*. Android is not supported upstream; you would build it with the
   NDK yourself. The top-level `CMakeLists.txt` exposes only device switches
   (`OIDN_DEVICE_CPU/SYCL/CUDA/HIP/Metal`) — no TBB option — so it uses its own
   tasking pool, which is good for Android. The model is **small**: the
   albedo+normal RT weights blob is 1,841,160 bytes and the "small" variant
   641,480 bytes (read from the `oidn-weights` LFS store), not the ~18 MB often
   assumed. The killer is not model size but throughput: the UNet is a
   compute-heavy multi-level network, and the SD888's eight cores
   (1×Cortex-X1 + 3×A78 + 4×A55, thermally throttled in a tablet) sustain
   maybe 25–60 GFLOPS of convolution (estimate). Add the `glReadPixels` of a
   1.6 MP RGBA frame (~6.4 MB, plus a full pipeline flush) and this is a
   0.3–1 s filter **per frame**. It is an excellent *offline* denoiser (photo
   mode, "render one hero frame" button) and a non-starter for 30–60 fps.
2. **Non-local means** — Apache-2.0 in OpenCV 4/5 (verified LICENSE = Apache
   2.0). NLM's quality comes from comparing 7×7 patches over a 21×21 search
   window; OpenCV's implementation uses integral images to make each comparison
   cheap, but it is still ~400+ operations per pixel and it is **spatial only**.
   On stochastic, temporally-varying noise it produces *crawling* artefacts
   because each frame is filtered independently — exactly the failure mode we
   cannot hide in a stereo view. It also has no depth/normal concept, so it
   smears across silhouettes.
3. **SVGF / à-trous** — the only family that lands inside the budget. SVGF
   (Schied, Peters, Dachsbacher, HPG 2017) is à-trous (Dammertz et al., 2010)
   plus three additions we actually need: temporal reprojection with an
   exponential history, per-pixel **variance** to steer the filter, and
   depth/normal edge-stopping weights. Community ports exist but are mostly
   **unlicensed** (e.g. `loudonclear/SVGF`, `jacquespillet/SVGF`,
   `TheVaffel/spatiotemporal-variance-guided-filtering`, `lumixem/SVGF-HLSL`
   — none declare a license), so the safe move is to implement from the paper
   and cite it, rather than copy an unlicensed shader. NVIDIA's own reference
   lives in `NVIDIAGameWorks/DxrTutorials` (**BSD-3-Clause**, verified) but is
   DXR/HLSL and cannot be dropped in. `alexdalat/atrous` is **MIT** (verified)
   and a clean, readable C++ implementation of the à-trous kernel and the
   B3-spline weights — good to read, not to ship (it is CPU code).
4. **CNNs** — the arithmetic is brutal and worth stating plainly. DnCNN is 17
   convolutions of 64 channels at 3×3, i.e. ≈1.25 MFLOP **per pixel**; at 1.6 MP
   that is **~2 TFLOP** for one frame, one eye, one pass. An à-trous pass at
   the same resolution is ~1 kFLOP/px. That is a **~1000×** difference. No
   runtime (ncnn BSD-3, TFLite Apache-2.0, NNAPI/Hexagon) closes a 1000× gap by
   being well engineered; only running at 1/8–1/16 resolution does, and then the
   network is being asked to hallucinate detail it never saw. The weights are
   also not drop-in for our noise: DnCNN/FFDNet are trained on AWGN and camera
   sensor noise, not on 2–4-spp screen-space GI noise with depth disocclusion.
   TFLite's GPU delegate additionally needs **OpenCL** (a second API and context
   next to EGL), and QNN/NPU needs Qualcomm's proprietary SDK.
5. **Mobile NPU demos** — Qualcomm AI Hub hosts models under mixed licenses and
   the runtime that reaches the Hexagon NPU is the proprietary QNN SDK; shipping
   it means new redistribution terms and a second build system. Not worth it for
   a filter that a fragment shader does in single-digit milliseconds.

---

## 4. Why the shape of the problem favours a GPU edge-aware filter

The alternative path's job is to make a **cheap, temporally unstable** image
acceptable. That noise has three properties that a generic image denoiser does
not exploit and a screen-space denoiser does:

1. **We have the geometry.** Depth is already attached (`GL_OES_depth_texture`)
   and normals are already reconstructed from it (`SSFX.md`). Edge-stopping on
   depth/normal is free information; NLM and a blind CNN have to guess.
2. **We have the previous frame.** Camera motion is known (view matrix), so
   depth-based reprojection is cheap and effective for static geometry.
3. **The noise is in an effect buffer, not the beauty image.** AO/GI/SSR are
   low-frequency, low-channel signals. Filtering a 1–4 channel buffer at
   reduced resolution costs a fraction of filtering RGB at full res, and the
   upsampled result is composited into the beauty image where the eye is far
   less sensitive to smoothness than to edges.

That third point is the practical unlock: **denoise the effect, not the frame.**

### 4.1 Recommended implementation (fits this codebase)

Reuse the existing per-eye post pass structure from `SSFX.md`; the new path is a
superset that can be toggled at runtime:

1. **Noisy buffer** — render AO/GI/SSR at `S` = 1/2 or 1/4 of the per-eye size
   (suggest starting at **1/4**: 320×384 per eye) into one RGBA16F target per
   eye. Pack up to four scalar effects into RGBA instead of four textures.
2. **Temporal pass** — reproject history with `prevViewProj`, reject on depth
   delta and normal divergence, clamp the history to the current 3×3
   neighbourhood's mean ± k·σ (k≈2–3) to kill ghosting, blend with α≈0.1–0.2.
   Output variance = spatial variance of the 3×3 (or a cheap 2-tap temporal
   variance) into the alpha channel.
3. **3 à-trous iterations** — 5×5 B3-spline taps, step widths 1, 2, 4, weights
   `w = w_z · w_n · w_v` with `w_v` from the variance channel (SVGF's key
   trick: variance steers the filter so flat regions smooth hard and noisy
   edges are left alone). Constant loop bounds → unrolls cleanly in ES2.
4. **Depth-aware upsample** — one full-res (or half-res) joint bilateral pass
   that samples the low-res result with depth weighting, so the upsample does
   not blur silhouettes.
5. **Composite** — feed the result into the existing FX combine
   (`colour * ao + gi`, `SSFX.md` line 13) so UIPOST masking keeps working.

Fallback if the budget is tight: drop steps 3–4 to one 3×3 bilateral pass
(≈2 ms) and accept softer edges; or keep temporal accumulation only.

### 4.2 Cost model (the numbers behind "6–14 ms")

Per eye at 1/4 res: 320×384 ≈ 0.123 MPx; both eyes ≈ 0.25 MPx; RGBA16F = 2 MB.

| Config (both eyes) | Taps/px | Texture taps per frame | Est. cost |
| --- | --- | --- | --- |
| Temporal only (@1/2 res, 0.98 MPx) | 5 | 4.9 M | 2–4 ms |
| 3 à-trous + temporal (@1/4 res, 0.25 MPx) | 25×3 + 5 = 80 | 20 M | **6–10 ms** |
| 3 à-trous + temporal (@1/2 res, 0.98 MPx) | 80 | 78 M | 15–30 ms |
| 5 à-trous @ full per-eye (3.9 MPx) | 125 | 490 M | 80–150 ms — don't |

At 2–4 Gtexel/s (Adreno 660 class, estimate) 20 M taps is 5–10 ms of pure
texture throughput, and the bandwidth check agrees: 3 à-trous passes over a
2 MB target is ~12 MB written + reads with good cache locality at 1/4 res, i.e.
well under 1 ms of the ~50 GB/s budget — so at quarter res we are **texture-ALU
bound, not bandwidth bound**, which is the regime a mobile GPU likes.

Caveat specific to tilers: every fullscreen pass has fixed per-pass overhead
(render-pass begin/end, resolve). 3 à-trous + temporal + upsample × 2 eyes ≈ 12
passes per frame. That is fine, but do not add `glReadPixels`, `glFinish`, or
`glCopyTexImage2D` into this loop — those serialise the tiler and cost more than
the filter.

**These are estimates.** The only honest way to close them is to build a
one-pass à-trous shader behind a debug toggle and read the existing frame-time
telemetry (dr02) on the device. Cost scales almost exactly linearly in
`pixels × taps`, so one measurement at 1/4 res extrapolates reliably.

---

## 5. Licensing notes

| Component | License | Obligation if shipped |
| --- | --- | --- |
| OIDN (`RenderKit/oidn`, weights repo) | Apache-2.0 | Keep `LICENSE` + `NOTICE`, state modifications. Includes a patent grant. Only matters if the offline mode ships. |
| OpenCV | Apache-2.0 | Same. Larger APK (`libopencv_java4.so` per ABI) unless you build a trimmed static lib. |
| ncnn | BSD-3-Clause (+ zlib-licensed third-party files listed in `LICENSE.txt`) | Reproduce copyright notice + disclaimer; no endorsement claims. |
| TFLite | Apache-2.0 | Same as OpenCV. |
| KAIR (DnCNN/FFDNet *code and training*) | MIT | Attribution. **Weights** have no explicit license → treat as unclear; retrain or ask. |
| `cszn/FFDNet`, `cszn/DnCNN` originals | **no license file** | Do not copy code from these; use KAIR's MIT reimplementation. |
| `NVIDIAGameWorks/DxrTutorials` | BSD-3-Clause | Attribution + disclaimer. DXR only, so reference use. |
| NVIDIA NRD | NVIDIA RTX SDK license (proprietary) | Vendor-scoped; not usable on Adreno. Do not link. |
| AMD FidelityFX SDK | MIT (per AMD's SDK license) | Attribution, if any code is adapted. |
| `alexdalat/atrous` | MIT | Attribution if code is adapted. |
| SVGF / à-trous **papers** | algorithms, not code | Cite in the doc/credits. Algorithms as such are not copyrightable; **no known patent assertions** on à-trous edge-avoiding wavelets or SVGF that I could find — this is *not legal advice*, and it is a reason to implement from the paper rather than vendor a binary. |
| This repo | (upstream sm64ex-android license) | Permissive deps above are GPL-compatible, so adding any of them creates no conflict. |

Practical rule: the recommended path adds **zero** third-party code, so the only
new obligation is a citation line in the README credits next to the existing
"SSRTX is inspired by ReShade" note.

---

## 6. Rate limiting / frame pacing (feeds dr02)

The current limiter is already a hybrid sleep+spin, in
`app/jni/src/src/pc/gfx/gfx_sdl2.c:1029-1040`:

```c
double next = now + target;
double t;
while ((t = gfx_now_secs()) < next) {
    double remain = next - t;
    if (remain > 0.002) SDL_Delay((Uint32)((remain - 0.001) * 1000.0));
}
```

and the 30 Hz path (`configWindow.vsync <= 1`) does the simpler
`if (t < TICK) SDL_Delay((Uint32)((TICK - t) * 1000.0));` at line 1039.
Note the 30 Hz branch is a floor, not a deadline: it sleeps the *remaining*
time after the work, so a long frame pushes the next tick late instead of
reporting a dropped frame. dr02 should measure, not just sleep.

| Mechanism | Granularity / behaviour | Accuracy to expect | Use for |
| --- | --- | --- | --- |
| `nanosleep()` (POSIX, hrtimer) | ns request, but the kernel rounds up to the next timer tick and applies **timer slack** (default 50 µs for normal threads — `prctl(PR_SET_TIMERSLACK)`) | typically **+0.05–0.3 ms**, 1–5 ms tails when a core is idle/ramping or the system is busy | the sleep half of a hybrid loop |
| `SDL_Delay()` | millisecond integer API, implemented as a `nanosleep` loop; truncates the request | **+0.5–2 ms** per call; the `(Uint32)` cast in the current code floors sub-ms remainders | coarse waits only |
| Busy-spin on `gfx_now_secs()` | `clock_gettime`/perf-counter precision | **±20–50 µs** | last ~0.5–1 ms before the deadline |
| `Choreographer` (Java) | callback ~1–2 ms before the display's vsync; **quantised to the panel refresh** — 120 Hz here, so 8.33 ms | exact vsync multiples, **±8.33 ms** quantisation at this panel | presentation-aligned pacing, not sub-frame accuracy |
| `eglSwapInterval(1)` | blocks until the next vsync if the queue is full | ±1 refresh; drivers may clamp interval to 1 | not a rate limiter — combine with explicit pacing |

Consequences for dr03/dr02:

- 30 fps = **33.333 ms**, 60 fps = **16.667 ms**, and this panel's vsync is
  **8.333 ms** — so 30/60 fps are exact integer multiples (4 and 2 vsyncs).
  That is a lucky, useful property: a Choreographer- or vsync-locked limiter can
  be exact, and a sleep-based limiter has no beat-frequency problem.
- Keep the hybrid shape (sleep to within ~1 ms, spin the remainder). Pure
  `SDL_Delay` pacing will show 1–2 ms of frame-to-frame jitter — visible as
  micro-stutter in stereo, and it corrupts any "is the denoiser fast enough"
  measurement. Shrink the spin window from 1 ms to ~0.3–0.5 ms and/or
  `sched_yield()` inside it: a 1 ms spin on a Cortex-X1 burns power for nothing
  and heats the tablet, which then throttles and changes the numbers you are
  trying to measure.
- Report **p50/p95/p99 frame time and count of missed deadlines**, not average
  FPS. With a denoiser toggle, the metric people actually see is the tail: a
  14 ms filter that occasionally spikes to 40 ms ruins a 30 fps path.
- Vsync-lock the *presentation*, deadline-lock the *work*: measure the filter
  cost between `glFinish`-free markers (query objects are not available in ES2
  without `EXT_disjoint_timer_query`; use CPU timestamps around the pass
  submission and accept that they measure submission, or use
  `EXT_disjoint_timer_query` if the Adreno driver exposes it — worth checking
  once, it is the difference between a real number and a guess).

---

## 7. Uncertainty register

| Claim | Confidence | Why |
| --- | --- | --- |
| SoC is SM8350 / Adreno 660 / 8 GB / Android 12 | **Confirmed** | three sources, one of them the device itself |
| OIDN not viable at 30–60 fps | **High** | 0.25–0.9 s est. CPU cost + readback; even a 4× error leaves it outside 33 ms |
| OIDN model size 1.84 MB / 641 KB | **Confirmed** | byte sizes read from the weights LFS objects |
| NLM 0.3–1.5 s | Medium | OpenCV's integral-image optimisation makes the constant hard to predict; order of magnitude is safe |
| DnCNN ≈2 TFLOP @1.6 MP | **High** (arithmetic) | 17×64×64×9×2 FLOP/px × 1.6 Mpx |
| à-trous 6–10 ms @1/4 res | Medium | derived from tap count and 2–4 Gtexel/s; **±2×**; needs one on-device measurement |
| Adreno 660 ≈1.2 TFLOP fp32, 2–4 Gtexel/s | Medium | class estimate; no per-SKU public spec sheet was reachable (TechPowerUp/Wikipedia rate-limited this session) |
| Android 12 will never be updated on this unit | Low | not investigated; if it is, the driver version cited here may change |
| No patent risk in à-trous/SVGF | Low–Medium | no patent found, no search of patent databases performed |

---

## 8. Sources

Device / platform
- UL 3DMark device page (SoC, RAM, OS, scores): <https://benchmarks.ul.com/hardware/tablet/Leia+Lume+Pad+2+review>
- Schenker retail spec listing (Snapdragon 888, 8 GB, 128 GB, Android 12, 120 Hz 2560×1600): <https://www.schenker-tech.de/en/leia-lume-pad-2-3d-tablet/>
- On-device: `getprop ro.soc.model` → SM8350; `dumpsys SurfaceFlinger` → Adreno 660, `OpenGL ES 3.2 V@0530.0`, `refresh-rate 120.00fps` (2026-11-23)
- Snapdragon 888 overview: <https://en.wikipedia.org/wiki/Snapdragon_888> (fetched, rate-limited; core arrangement treated as background, not as a cited spec)

Denoisers
- Intel OIDN (Apache-2.0, ARM64, supported OS list, CMake options): <https://github.com/RenderKit/oidn> — README v2.5.1, `CMakeLists.txt`
- OIDN trained weights and their byte sizes: <https://github.com/RenderKit/oidn-weights>
- OpenCV license + NLM docs: <https://github.com/opencv/opencv> (`LICENSE` = Apache-2.0), <https://docs.opencv.org/4.x/d1/d79/group__photo__denoise.html>
- SVGF paper: Schied, Peters, Dachsbacher, *Spatiotemporal Variance-Guided Filtering*, HPG 2017 — <https://research.nvidia.com/publication/2017-07_Spatiotemporal-Variance-Guided-Filtering%3A>
- À-trous paper: Dammertz, Sewtz, Hanika, Lensch, *Edge-Avoiding À-Trous Wavelet Transform for fast Global Illumination Filtering*, HPG 2010 — <https://jo.dreggn.org/home/2010_atrous.pdf>
- MIT C++ à-trous reference: <https://github.com/alexdalat/atrous>
- NVIDIA DXR samples (BSD-3-Clause, SVGF reference, DXR only): <https://github.com/NVIDIAGameWorks/DxrTutorials>
- NVIDIA NRD (proprietary RTX SDK license): <https://github.com/NVIDIA-RTX/NRD>
- AMD FidelityFX SDK (MIT): <https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK>
- ncnn (BSD-3-Clause + third-party notices): <https://github.com/Tencent/ncnn>
- KAIR (MIT; DnCNN/FFDNet implementations): <https://github.com/cszn/KAIR>
- DnCNN/FFDNet originals, **no license file**: <https://github.com/cszn/FFDNet>

Frame pacing
- Current limiter: `app/jni/src/src/pc/gfx/gfx_sdl2.c:1029-1040`, `:1037-1040`
- `nanosleep(2)` / timer slack: <https://man7.org/linux/man-pages/man2/nanosleep.2.html>, <https://man7.org/linux/man-pages/man2/prctl.2.html>
- `Choreographer`: <https://developer.android.com/reference/android/view/Choreographer>
- `SDL_Delay` (ms granularity): <https://wiki.libsdl.org/SDL2/SDL_Delay>
