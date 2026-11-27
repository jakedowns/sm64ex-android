# Black frame after sleep / resume

**Status:** mechanisms identified; probe + recovery shipped with the
`w7-blackframe` patch; hardware verification owed (5 × sleep→wake in 2D and 3D).

Two distinct bugs produce the same symptom:

| # | where | trigger | evidence |
|---|---|---|---|
| A | `initLeia()` aborted by a panel exception | any launch with no QUALITY tier stored | `WEAVER ... Leia init FAILED`, `ArrayIndexOutOfBoundsException`, `texs=0`, `flat=1`, `drawable=2560x1534` |
| B | GL context left on the previous `EGLSurface` | sleep→wake / resume | `glstale`/`glfix` rising, `swapfail` 0 |

Class A was observed live on 2026-11-27 (see *Live evidence* below) and is the
simpler of the two: the weaver was never bound at all.

## Symptom

After the panel sleeps (30 s timeout, or the device lock's `sleep_sent`) or
after a relaunch/resume, the app can come back **black**: the touch overlay, the
captions and the debug panel all keep drawing, the game layer does not.

- `cmd dump` looks healthy: `surfaceValid=true`, `view=1920x1200`, `qd=`,
  fps/counters ticking, `cam=`/`focus=` sane.
- `cmd reinit` (rebind-only by design) does **not** help.
- `HOME` + reopen helps sometimes and not always.
- A `screencap` of a *sleeping/locked* panel is also black (mean ≈ 1.5), so
  check `dumpsys power | grep mWakefulness` first. This document is about the
  black frame on an **awake, unlocked** panel.

## Live evidence (2026-11-27, `master` build, tablet awake)

Captured with the bug live, before any recovery attempt:

```
screencap 2560x1600: whole-frame mean 4.8, left third 0.4, right third 10.6
  (the calibration "black" of a sleeping panel is ~1.5; here the game area is
  0.4 and the right third is the debug panel - overlay drawn, game not)
cmd dump: drawable=2560x1534 eyeW=1280 fps=60 view=2560x1534 surfaceValid=false
      stereoOn=true ... (PTC/"cam=" fields all sane, fb=1)
```

`sm64-debug.log` for that launch:

```
WEAVER +90ms TouchOverlay attached
WEAVER +119ms Leia init FAILED, staying flat (possible black screen cause)
java.lang.ArrayIndexOutOfBoundsException: length=3; index=-1
    at updateQualityPills(sm64portActivity.java:3708)
    at addQualityRow(...) / buildTuningPanel(...) / initLeia(...:808)
```

`initLeia()` threw while building the tuning panel (`fxPreset == -1`, no QUALITY
tier stored yet, `QUALITY_DETAIL[fxPreset]`), i.e. **before**
`leiaView.setViewAsset(asset)`. So the asset was never bound, the requested
`SurfaceTexture` was never created, `onCreate` fell into
`SDLActivity.failOverride()`, and SDL rendered into its own (hidden)
`SurfaceView` at the display size `2560x1534` - visible fps, invisible game.
`cmd reinit` requests a surface that is still never delivered (`asset` is not
bound to the view), which is why it could not help.

Both halves are fixed: the array guard (`updateQualityPills` now shows "no tier
chosen"), and the panel build is wrapped so a UI bug can never abort the weaver
setup again (`panel=FAILED` in `cmd dump` makes that visible).

## Mechanism

The game never draws to the panel directly. SDL renders a 2×1 SBS frame into a
`SurfaceTexture` owned by Leia's `InputViewsAsset`, and the interlacer weaves it:

```
SDL GL thread ──eglSwapBuffers──▶ ANativeWindow(from Leia SurfaceTexture)
                                        │  (buffer queue)
                                        ▼
Leia GL thread ──updateTexImage──▶ InterlacedRenderer ──▶ panel
```

On resume the interlacer tears down and rebuilds its side of that pipe
(`InterlacedSurfaceView.onResume()` → `InputViewsAsset.scheduleRecreatingAssets()`
→ new `SurfaceTexture` + `onSurfaceTextureReady` on the main thread). The app
then re-binds SDL with `SDLActivity.setOverrideSurface(new Surface(texture))`,
which calls `onNativeSurfaceCreated()` → **destroys and recreates SDL's
`EGLSurface` on the Java thread**.

Nothing re-binds the GL context to that new `EGLSurface` on the render thread.
SDL's own restore path (`android_egl_context_restore`) only runs at the pause →
resume boundary and binds whatever `data->egl_surface` is at that instant, so a
recreation that lands *after* it leaves the context bound to the old (destroyed)
surface. From then on every `eglSwapBuffers(window_surface)` targets a surface
that is not current: the driver drops it, the render loop keeps counting frames
and `cmd dump` stays green, and the panel keeps showing the weaver's last
(black) clear.

That also explains the two observations that made this look unfixable:

- **`cmd reinit` cannot help.** It re-binds the *same* producer surface, so
  `ANativeWindow_fromSurface()` returns the same pointer, `onNativeSurfaceCreated`
  short-circuits, and the dead `EGLSurface` is kept.
- **`HOME` + reopen only sometimes helps.** It depends on whether a new
  `SurfaceTexture` (and therefore a new `ANativeWindow` and `EGLSurface`) is
  created in that particular resume.

Three distinguishable failure classes, all of which present as "black":

| class | evidence in `cmd dump` | repaired by |
|---|---|---|
| GL context not current to the window surface | `glstale` rising, `glfix` rising, `swapfail` may stay 0 | per-frame make-current repair (shipped) |
| producer surface abandoned (new `SurfaceTexture` never re-bound, or the weaver dropped the queue) | `swapfail` rising, `glfix=0` | fresh `SurfaceTexture` (`cmd blackfix`, watchdog) |
| SDL replaced the **GL context** (`SDL_RENDER_DEVICE_RESET`, "context no longer valid") | `ctxreset>0` | restart only (every GL name is stale) |

## The fix

1. **Per-frame binding check** (`gfx_sdl2.c`, `gfx_sdl_swap_buffers_begin`):
   before each swap, compare `eglGetCurrentSurface(EGL_DRAW)` with the window's
   `EGLSurface` (`SDL_GetWindowWMInfo` → `info.android.surface`) and
   `eglMakeCurrent()` it back when they differ. One log line per repair. This is
   the actual root-cause fix for class 1, and it costs two EGL lookups per frame.
2. **Deterministic recovery** for classes 1–2: ask the asset for a fresh
   `SurfaceTexture` (`InputViewsAsset.scheduleRecreatingAssets()`), which
   re-runs the whole re-bind chain through `leiaSurfaceReady()`. **No view
   hide/show and no interlacer pause/resume** — both detach the surface on newer
   firmware (1.0.48) and black the display for good.
3. **Last resort** for class 3 (and anything else): restart the activity once
   into the level the session is in. `QUICK DEV` is borrowed for that one boot
   (`nativeGetCurrLevel()` → `PREF_QUICK_DEV_LEVEL`), and `onCreate()` restores
   the user's toggle from `PREF_BLACKFIX_RESTORE` so a recovery never changes
   their settings. Rate limited to once per 10 minutes.

The interlacer "kick" (`onPause`/`onResume` + `requestRender`) now runs only on
the **first** `SurfaceTexture` delivery. It is safe exactly once, at startup;
repeating it on a resume destroys the surface SDL is drawing into.

## Probe

```
adb shell am broadcast -n com.vdavid003.sm64port/.DebugReceiver \
    -a com.vdavid003.sm64port.CMD --es cmd blackcheck
```

Logs one line (`SM64Leia`):

```
BLACKCHECK mean=58 max=214 frames=1 presents=10432 swapfail=0 glfix=0 glstale=0 ctxreset=0 texs=2 status=ok
```

- `mean`/`max`: luminance (0..255) of the frame the renderer is about to
  present, sampled from a full `glReadPixels` of the back buffer — the same
  readback the feedback outbox uses.
- `frames`: swaps serviced while the probe was pending (≥ 1 proves the loop is
  presenting; `status=stalled` means the loop never came back to swap).
- `status`: `ok` (mean > 4 or max > 24), `black` (mean ≤ 4 **and** max ≤ 24),
  `stalled` (probe never serviced), `error` (native probe failed), `flat`
  (no SDL override surface: the game is drawing into SDL's own hidden view, so
  luminance is irrelevant - the binding is the bug).

`cmd dump` carries the same telemetry permanently:

```
... surfaceValid=true texs=2 blk=ok/58/214/1 panel=ok flat=0 presents=10432 swapfail=0 glfix=0 glstale=0 ctxreset=0 ...
```

`texs=0` + `flat=1` is class A (weaver never bound); `blk=black` with `swapfail`
or `glfix` rising is class B.

## Recovery commands

```
# the safe repair (fresh SurfaceTexture, no view hide/show)
adb shell $B --es cmd blackfix

# force the last-resort restart into the current level
adb shell $B --es cmd blackfix --es action restart
```

Automatic: the watchdog arms itself on every resume that is not the boot
(`presents >= 240`), probes 2.5 s later, requires two consecutive black probes
1.2 s apart (a level fade is never that long), then repairs, re-probes, repairs
again, and only then restarts. Each step is one `BLACKWATCH`/`BLACKFIX` log
line; `cmd reinit` also arms it (with the restart rung disabled).

## Verification plan

Device work must run inside `scripts/agent-lock.sh`. One cycle:

```bash
B="am broadcast -n com.vdavid003.sm64port/.DebugReceiver -a com.vdavid003.sm64port.CMD"
adb shell $B --es cmd blackcheck                       # baseline: status=ok
adb shell input keyevent 26                            # sleep
sleep 6
/media/share/lp2-test/scripts/unlock.sh
sleep 4
adb shell $B --es cmd blackcheck                       # must be status=ok
adb exec-out screencap -p > /tmp/opencode/blackframe/cycle-N.png
```

Matrix (all on one awake tablet, one device lock hold per batch):

1. 5 × sleep → wake → resume in **3D**, each reaching `status=ok` and a
   non-black screenshot.
2. 5 × sleep → wake → resume in **2D** (`--es cmd toggle --es name 3D`).
3. One `am force-stop` + `am start` cycle in each mode.
4. Deliberate fault injection to prove the repair path, not just the happy
   path: `cmd reinit` on a healthy frame must log `status=ok`; if a cycle comes
   back black, the expected log order is

   ```
   BLACKWATCH arm #n (resume)
   BLACKWATCH #n mean=.. max=.. frames=1 ... status=black
   BLACKWATCH #n mean=.. max=.. frames=1 ... status=black      (confirmation)
   BLACKFIX watchdog 1/2: fresh Leia SurfaceTexture ...
   SurfaceTextureReady #3 (resize 1920x1200) tex=... oldReleased=false
   BLACKWATCH recovered after 1 repair(s): ... status=ok
   ```

Record, per cycle: `mean`, `max`, `frames`, `swapfail`, `glfix`, `glstale`,
`ctxreset`, and the screenshot. `swapfail>0` with `glfix=0` on a *recovered*
cycle is the class-2 signature and means the recovery path was the one that
worked; `glfix>0` is class 1 (the make-current repair was the fix).
