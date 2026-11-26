# Screen-space FX debug tabs (v32-ssfx)

Prototype post-processing for the Leia stereo path. The effects are *inspired by*
ReShade shaders (MXAO, CinematicDOF, SSGI/RTGI); they are compact GLES2
approximations intended for tuning and visual checks, not ports.

## Pipeline

`gfx_pc.c` still renders each eye into its own FBO. Before the SBS composite,
`gfx_opengl.c` runs one fullscreen post pass per eye when any FX is enabled:

```
colour_out = dof(colour) * ao(colour, depth) + gi(colour, depth)
```

- Depth comes from the existing per-eye `GL_OES_depth_texture` attachment
  (available on the Lume Pad 2 / Adreno 660).
- View-space positions are rebuilt from the game's last perspective node
  (`near`, `far`, `fov`, aspect), so radius/focus values are in world units.
- Normals are reconstructed from neighbouring depth samples (no derivatives).
- The pass writes to a pair of `pp` FBOs; the composite then samples those
  instead of the raw eye textures.
- With all FX off (the default), the v31 composite path is used unchanged.

## Debug panel tabs

The bottom panel now has four pages:

| Tab | Controls |
| --- | --- |
| `STEREO` | all existing Leia/stereo knobs |
| `MXAO` | enable, intensity %, radius (world units), samples, bias, power %, `SHOW AO` debug view |
| `C.DOF` | enable, focus (world units), aperture %, max blur px, samples, `SHOW CoC` debug view |
| `SSGI` | enable, intensity %, radius, steps, rays, resolution divisor, blur, roto jitter, sky skip, normal reject, `SHOW GI` debug view |

`RESET` restores every slider, including the FX pages.

## Headless debug channel

```bash
B="am broadcast -n com.vdavid003.sm64port/.DebugReceiver -a com.vdavid003.sm64port.CMD"

adb shell $B --es cmd toggle --es name MXAO    # also CDOF / SSGI / FXDBG
adb shell $B --es cmd fx --ei k 2 --ei v 250   # set an FX knob
adb shell $B --es cmd dump                     # mxao/cdof/ssgi/fxdbg state
```

FX knob keys (`cmd fx`):

- AO: `1` enable, `2` intensity %, `3` radius, `4` samples, `5` bias, `6` power %
- DOF: `11` enable, `12` focus, `13` aperture %, `14` max blur px, `15` samples
- SSGI: `21` enable, `22` intensity %, `23` radius, `24` steps, `25` rays
- SSGI resolution (the gather runs at canvas/div in its own chain, then a
  dedicated upsample adds it back; see README "Reduced-resolution SSGI"):
  `400` divisor (1 = old full-res loop, 2, 4 default), `401` low-res blur
  passes, `402` rotated-grid/temporal jitter, `403` low-res debug view
  (1 GI, 2 depth, 3 normals), `404` far/sky skip %, `405` normal-aware tap reject
- SSRTX: `70` enable, `71` intensity %, `72` distance, `73` steps, `74` rays,
  `75` thickness, `76` roughness %, `77` edge fade %
- Debug views: `31` (0 off, 1 AO, 2 CoC, 3 GI, 4 SSRTX), `32` invert normals

## Notes / limitations

- Single combined pass for the prototype: cost grows with the sample sliders.
  The game's 30 fps limiter was still met with the defaults in the title scene.
- GI is a screen-space colour-bleed approximation, not a ray-traced bounce.
- DOF is a single-pass spiral gather (no separate bokeh/CoC passes).
- AO/GI need the depth texture; if `GL_OES_depth_texture` is missing the pass
  logs a warning and stays disabled.

## UI after the post pass (UIPOST)

By default the frame's display list is split in `render_game()`: everything up
to the HUD scissor is the 3D scene, everything after is 2D UI (HUD, cannon
reticle, dialogs, Peach letter, pause menu, text labels, transitions, native
touch controls, borders). `gfx_run()` renders the scene range into the eye FBO
and the UI range into a second per-eye RGBA buffer (`gfx_opengl_ui_begin/end`),
then composites:

    out = scene.rgb * (1 - ui.a) + ui.rgb      (UI buffer is premultiplied)

so MXAO/DOF/SSGI never see, blur or bleed into the HUD.

- `UIPOST` (default ON) — split + composite; OFF reproduces the v32 whole-frame
  FX path.
- `UIPAR` (default OFF) — OFF renders the UI screen-locked (no IPD shear, both
  eyes identical); ON keeps the legacy per-eye sheared UI.
- `UIBUF` (default OFF) — shows the captured UI buffer over a dimmed scene, to
  verify that every element landed in the UI range.
- Fallback: if there is no area / no scene this frame (`gGfxHasScene == 0`) the
  whole-list path is used.

Stereo-int keys (`cmd set --ei k ... --ei v ...`): `30` STAY,
`35` UIPOST, `36` UIPARALLAX, `37` UIBUF, `38` UI depth (px), `40` TOEAUTO.
Dump fields: `stay=`, `uipost=`, `uipar=`, `uibuf=`, `toeauto=`. UI FBO creation
is logged as `SM64FBO ui <eye> fbo=...`.

### World-position reconstruction and the stereo shear

`viewPos()` removes the per-eye projection adjustments before rebuilding the
view-space position: the clip-space IPD shear is subtracted from `ndc.x`, the
toe cant is removed with `ndc.x -= toe * ndc.z`, and `FLIPY` is undone. Without
this, shadow/AO/DOF/GI lookups would be offset by the eye separation (tens of
world units at typical depths). Per-eye values are captured in
`gfx_apply_stereo_shear()` and uploaded for each eye by the post pass.

## SSRTX — screen-space ray-traced reflections (v34)

A ReShade SSR/SSRT-style pass inside the existing per-eye FX shader:

1. Rebuild the view-space position/normal (as above, with the stereo
   shear/toe removed).
2. Reflect the view vector about the normal.
3. March the ray in view space (`Distance` world units over `Steps` samples),
   projecting each sample back to canvas UV with the *forward* stereo
   projection (`viewToCanvas`), and read the scene depth.
4. A sample is a hit when the ray is behind the depth-buffer surface by less
   than `Thickness`; the scene colour there is accumulated.
5. Blend `colour * Fresnel * edgeFade * Intensity`. `Rays > 1` jitters the
   reflection inside a cone (`Roughness`) for soft/rough reflections.

The tab is `SSRTX`; defaults are **off** (opt-in) because the march is the most
expensive post effect. Headless knobs are keys `70`..`77` via `cmd fx`; debug
view `4` (`SHOW SSRTX`) displays the reflection buffer.

## Realtime dynamic shadow map (v33)

Before the two eye passes, `gfx_pc.c` replays the perspective scene range
(`gGfxShadowStart` .. `gGfxUiStart`) from a directional sun into a depth-only
FBO, then the per-eye post pass projects the reconstructed world position into
light space and applies PCF. See the README's SHADOW tab and `cmd shadow`
key list. The background (skybox) master list is excluded because
`gGfxShadowStart` is recorded at the first Z-buffered master list only.
Quality/perf knobs: resolution, PCF tap count, filter radius, depth/normal
bias, sun angles, ortho range, and map update interval.

### Auto toe (TOEAUTO, on by default)

With TOEAUTO on, the manual Toe in/out slider is ignored and both eyes are
canted at `theta = atan((ipd/2) / d)`, where `d` is the DOF autofocus distance
(|camera - Mario's centre| - hitbox radius, updated every tick). The pair
therefore aims at Mario's front: the cant grows as he comes closer and the eyes
are nearly parallel at long range. Capped at ~20 degrees. TOEAUTO off restores
the manual slider (positive % = toe-out).

## Stay in level after a star (STAY)

`configStayInLevel` (`stay_in_level` in `sm64config.txt`, off by default) makes
`interact_star_or_key()` route normal stars through the vanilla no-exit path
(star dance → save prompt → idle) instead of the star-exit warp, so the course
keeps running; 100-coin stars already behaved that way. Toggle with the `STAY`
panel button or `cmd toggle --es name STAY`.
