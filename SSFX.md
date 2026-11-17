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
| `SSGI` | enable, intensity %, radius, steps, rays, `SHOW GI` debug view |

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
- Debug views: `31` (0 off, 1 AO, 2 CoC, 3 GI), `32` invert normals

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
`35` UIPOST, `36` UIPARALLAX, `37` UIBUF. Dump fields: `stay=`, `uipost=`,
`uipar=`, `uibuf=`. UI FBO creation is logged as `SM64FBO ui <eye> fbo=...`.

## Stay in level after a star (STAY)

`configStayInLevel` (`stay_in_level` in `sm64config.txt`, off by default) makes
`interact_star_or_key()` route normal stars through the vanilla no-exit path
(star dance → save prompt → idle) instead of the star-exit warp, so the course
keeps running; 100-coin stars already behaved that way. Toggle with the `STAY`
panel button or `cmd toggle --es name STAY`.
