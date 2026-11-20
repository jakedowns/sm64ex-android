# Splat rendering — first slice

Prototype of the Dreams/BubbleBath-style splat mode from `plan-cool-additions.md` §5.6.
Branch: `feat/splat-prototype` (worktree `worktrees/splat-prototype`).

## What this slice does

- Tags selected **objects** while the scene graph is built (Mario, trees, or all
  objects). Object display lists are deferred into the master list, so the tag
  is stored on each `DisplayListNode` and emitted as a renderer-local marker
  immediately around that list. It therefore survives the deferred build and
  both per-eye Leia passes.
- Draws one camera-facing point sprite per tagged triangle instead of the
  textured triangle. Colour comes from the draw's existing vertex/material
  colour, so it already picks up textures/lights. The point shader applies a
  soft elliptical falloff (`gl_PointCoord`) and alpha blending.
- Level geometry is untouched, so the effect is an A/B on movable objects rather
  than a full-scene conversion.

This is deliberately the cheap end of the plan: it validates the look and the
live controls without a G-buffer bake, point-cloud cache, cluster culling, or
sorting.

## Controls

| Key | Meaning | Default |
|---|---|---|
| `60` | target: `0` off, `1` Mario, `2` trees, `3` all objects | `0` |
| `61` | splat footprint in eye-buffer pixels | `14` |
| `62` | opacity % | `100` |
| `63` | softness % (`0` crisp disc → `100` soft Gaussian) | `70` |

`SPLAT` adb toggle cycles target `0 → 1 → 2 → 3 → 0`.

```bash
B="am broadcast -n com.vdavid003.sm64port/.DebugReceiver -a com.vdavid003.sm64port.CMD"
adb shell $B --es cmd toggle --es name SPLAT
adb shell $B --es cmd set --ei k 60 --ei v 3
adb shell $B --es cmd set --ei k 61 --ei v 14
adb shell $B --es cmd set --ei k 62 --ei v 100
adb shell $B --es cmd set --ei k 63 --ei v 50
```

Suggested test: Bob-omb Battlefield (`target=2`, trees), or the castle lobby
(`target=1`, Mario). Compare with `target=0` at the same camera.

## Files

- `src/pc/gfx/gfx_pc.h` — config externs + marker opcode.
- `src/pc/gfx/gfx_pc.c` — marker emitter/handler, config globals.
- `src/game/rendering_graph_node.c` — object selector + marker around the
  object's `sharedChild` subtree.
- `src/pc/gfx/gfx_opengl.c` — point-sprite draw path and soft-falloff shader.
- `src/pc/gfx/gfx_sdl2.c` — JNI knobs `60..63`.
- `sm64portActivity.java` — `SPLAT` toggle.

## Known limits / next steps

- Fixed pixel size: no distance attenuation or per-splat anisotropy yet.
- One splat per triangle, round-robin corner: density follows the source
  tessellation; sparse meshes look sparse.
- `target=1` (Mario) is verified on device. `target=2` (trees) currently removes
  the trees without visible flecks at distance — their master-list blend/depth
  state is not reproduced by the point pass. Next step: capture per-list blend
  mode and force depth-write for splats.
- No G-buffer albedo sampling or build-time bake (plan §5.6.2 options 1–2), so
  motion/temporal stability is not addressed.
- Alpha blending relies on the object draw's blend state; splat alpha is forced
  from the footprint so cutout/leaf textures still show, but their RGB is the
  centre-sample colour.
- Level geometry, HUD, skybox and particles are not splatted.
- Knobs are session-only; no config persistence or debug-panel widgets yet.
- `SM64SPLAT` logcat tag reports marker emission/execution and draw counts for
  debugging.
