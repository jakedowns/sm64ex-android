# sm64ex-android — Lume Pad 2 fork

Super Mario 64 (sm64ex) for Android, forked for the **Lume Pad 2** — a
glasses-free 3D tablet — with a rebuilt stereo renderer, a large screen-space
effects suite, and debugging tools aimed at making the port tunable without
recompiling.

Upstream project and its build instructions (Windows/Android Studio, stock
Android): **[README-original.md](README-original.md)**. This file describes what
*this* fork is and how to build and run it here.

---

## What this fork adds

- **Leia stereo** — per-eye rendering woven by the Lume Pad's display, with
  depth placement, toe-in, eye shear, focus and pop-out controls, plus a
  head-tracked/gyro parallax camera.
- **High frame rate** — the sim stays at 30 Hz while the renderer presents 60 or
  120 Hz, interpolating camera and model matrices per present, including held
  objects (the tail-spin Bowser pulse) and painting ripples.
- **Screen-space effects**, all with runtime dials and a panel page:
  MXAO, C.DOF (bokeh), SSGI, SSRTX, crepuscular rays, shadow-mapped dynamic
  lights with light probes, emissive materials + bloom, tone mapping and colour
  grading with LUTs, motion blur with real per-vertex motion vectors, and a
  reduced-resolution denoiser.
- **Quality presets** — LOW / MED / HIGH, one tap, measured on the tablet
  (3D, PTC test chamber): **LOW 9.2 ms ≈ 108 fps**, **MED 14.2 ms ≈ 70 fps**,
  **HIGH 36.6 ms ≈ 27 fps**.
- **Gameplay/dev extras** — a Portal Test Chamber with scripted test rigs
  (portal relay, ninja chain, wall-run corner timings, grapple targeting view,
  Bowser tail-spin HFR rig), wall-run + grapple + jetpack movement kit, an
  occluded-Mario punch-through, and a touch-control layout editor with profiles.
- **Debugging without a compiler** — every tunable has an `fx` key and a panel
  control, `cmd dump` reports a machine-readable status line, and there is an
  in-app feedback outbox plus a `quick-dev` loop that gets a change on screen in
  seconds.
- **Opt-in telemetry mirror** (`otsm64remote`, off by default) — numeric state
  (position, yaw, speed, animation, action label) is mirrored to a browser
  viewer over HTTPS, and a viewer `{"input":"jump"}` becomes a single A press.
  Config in `<external files>/sm64remote.cfg`; see
  [`OTSM64REMOTE.md`](OTSM64REMOTE.md). Nothing is sent unless enabled.

## Build

The toolchain lives outside `PATH`; nothing is installed system-wide.

```bash
export JAVA_HOME=/home/ubuntu/opencode/jdk                 # JDK 17
export ANDROID_HOME=/home/ubuntu/opencode/sdk              # platform 34, build-tools 34.0.0
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export ANDROID_NDK_HOME="$ANDROID_HOME/ndk/27.2.12479018"
export GRADLE_USER_HOME=/home/ubuntu/opencode/gradle-home
export GNUMAKE=/usr/bin/make
export PATH="$JAVA_HOME/bin:/home/ubuntu/opencode/toolchain-bin/hostbin:/home/ubuntu/opencode/toolchain-bin/x86bin:$PATH"

# all builds go through the shared lock (see AGENTS.md)
scripts/agent-lock.sh build --reason "my change" -- ./gradlew assembleDebug --no-daemon
# -> app/build/outputs/apk/debug/app-debug.apk
```

The NDK's host tools are x86_64 running under qemu on this arm64 VM: an
**incremental build is ~1 minute**, a **fresh worktree is a full native rebuild
at 15–25 minutes** (set `NDK_HOST_PYTHON=/usr/bin/python3` and copy the
git-ignored `app/jni/src/build/us_pc` tree first).

## Run it on the tablet

The tablet is adb-over-Wi-Fi (`192.168.1.207:5555`); device work also goes
through the lock, which marks the app BUSY and sleeps the panel on release.

```bash
export LD_LIBRARY_PATH=/home/ubuntu/.local/lib/android-tools:$LD_LIBRARY_PATH

# one-shot install + launch + level + state/report capture
AGENT_TABLET_MUTE=1 scripts/agent-lock.sh device --reason "quick dev" -- \
    scripts/quick-dev.sh --install --level 38 --fx "100=1 101=200"
```

`scripts/quick-dev.sh --help` lists the flags; `DEBUG → SYS → QUICK DEV` makes
subsequent launches skip the logo/attract sequence.

## Where to read more

| file | what it is |
| --- | --- |
| [`README-lumepad-2-fork.md`](README-lumepad-2-fork.md) | the long-form fork manual: every effect, every `fx` key, panel layout, controls, measured costs |
| [`TODO.md`](TODO.md) | what is still open (blocked device captures, bugs, next perf work) |
| [`BACKLOG.md`](BACKLOG.md) | long-form notes, decisions and gotchas for open threads |
| [`AGENTS.md`](AGENTS.md) | build/device protocol, locks, debug channel — read this before touching the tablet |
| [`EFFICIENT-DEBUGGING.md`](EFFICIENT-DEBUGGING.md) | how to tune at runtime instead of rebuilding |
| [`OTSM64REMOTE.md`](OTSM64REMOTE.md) | opt-in telemetry/remote-input bridge: config, protocol, relay, what is and is not sent |
| [`TESTING.md`](TESTING.md) | test rigs and harnesses (portal relay, chain, wall-run corner) |
| [`PORTAL-TEST-CHAMBER.md`](PORTAL-TEST-CHAMBER.md) | the PTC level and its scripted actions |
| [`MOTION-VECTORS.md`](MOTION-VECTORS.md), [`DENOISER-RATE-LIMIT.md`](DENOISER-RATE-LIMIT.md) | motion blur and denoiser internals with measurements |
| [`findings/`](findings/) | perf investigations: the full FX sweep, DOF/SSGI/MXAO rewrites, the half-res FX pass, presets |

## Controls

Touch controls with an on-screen layout editor (drag + corner resize, SAVE /
DISCARD in the editor, saved profiles), controller and keyboard support with
rumble, and a debug panel in four docks (bottom/top, horizontal/vertical) that
exposes every graphics knob.
The panel's DEBUG → MOVE / PORTAL / BALL / SYS / LOGS pages drive the test rigs
and produce bug reports.

## Credits and licence

This is a fork of [sm64ex-android](https://github.com/VDavid003/sm64ex-android)
(see [README-original.md](README-original.md)) which in turn builds on
[sm64ex](https://github.com/sm64pc/sm64ex) and the [n64decomp/sm64](https://github.com/n64decomp/sm64)
decompilation. The Leia stereo integration uses Leia's CNSDK. Licensed under the
same terms as upstream; the original README carries the attribution and the
asset-extraction instructions.
