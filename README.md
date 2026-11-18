# sm64ex-android
 Super Mario 64, sm64ex, for Android

It features support for touch controls, rumble, controllers, and keyboards.  
The touch controls will automatically hide when using a controller or keyboard.  
Vibration or rumble can disabled in pause -> options -> controls -> rumble strength -> 0  


## How to compile on windows for android:

Install MSYS2 and android studio.

https://www.msys2.org/
https://developer.android.com/studio

Run MINGW64

```
pacman -S git
git clone https://github.com/MegaBitmap/sm64ex-android
```

Rename the original sm64 rom to baserom.us.z64  
Move baserom.us.z64 into the folder:
`C:\msys64\home\%username%\sm64ex-android\app\jni\src\`  

```
cd sm64ex-android
bash Setup-MINGW64.sh
```

It will check for updates then start extracting assets from the rom.  

This may take a while.  

After that is finished, run Android Studio and open the project folder:
`C:\msys64\home\%username%\sm64ex-android\`

Wait for gradle to sync.

Optional: Replace the app icons in `sm64ex-android\app\src\main\res\mipmap*` folders  
https://developer.android.com/studio/write/create-app-icons#access

After gradle finishes, click the hammer icon to build the app.  

This may take a while.  

Upon a successful build, the output apk file will be located here:  
`C:\msys64\home\%username%\sm64ex-android\app\build\outputs\apk\debug\app-debug.apk`  

Rename the apk to `sm64ex-android.apk`, install, and play!

## Build tip: stream long builds through the progress relay

When an agent drives the build, pipe it through the progress-tool relay so the
user's progress indicator streams live (and at zero token cost) instead of the
agent waiting on a wall of compiler output:

```sh
progress-relay --id build-1 -- /path/to/build-worktree.sh /path/to/worktree
# or: /path/to/build-worktree.sh /path/to/worktree | progress-relay --id build-1
```

The relay parses OSC 9;4 progress sequences and maps the exit code to done/error.

## Working with multiple agents

If more than one agent works in this checkout at the same time, follow
`multi-agent-rules.md`. All Gradle/NDK builds must run under the build lock and
all `adb`/device access under the device lock:

```
scripts/agent-lock.sh build  --reason "assembleDebug" -- ./gradlew assembleDebug
scripts/agent-lock.sh device --reason "install apk" -- adb install -r app/build/outputs/apk/debug/app-debug.apk
scripts/agent-lock.sh status
```

The locks live under host state, not in the repository, so they are shared by
every git worktree of this project. Run the helper from the worktree you are
building; branch and commit are recorded automatically. Acquire/release/timeout
events are appended as JSONL next to the locks, and `status` prints the exact
log paths.

## Lume Pad 2 fork additions

This fork turns the port into a glasses-free 3D, high-framerate build for the
**Leia Lume Pad 2** and adds a lot of debug tooling on top of upstream:

- **Leia interlaced 3D** output: per-eye framebuffers, adjustable IPD,
  zero-parallax/convergence, depth, toe (auto-toe to the DOF focus), UI depth,
  plus a screen-locked UI layer.
- **Interpolated 60/120 Hz rendering** while the simulation stays at a fixed
  30 Hz (camera, objects, animation, shadows).
- **Screen-space effects**: MXAO, cinematic DOF (auto focus on Mario) and
  SSGI-lite, with live sliders and debug views.
- **UI after post**: the HUD, dialogs, menus and transitions are composited
  after the FX pass so they stay sharp and never bleed into the scene.
- **Touch overlay**: floating left stick, A/B/Z/R/L, C-buttons or an analog
  camera stick, START, portal buttons, adjustable opacity.
- **Camera**: free camera (X enables, Y recentres), head-tracked parallax and
  gyro camera canting with independent strength sliders and a tilt-pivot height.
- **Stay in level after a star** (default on).
- **Portal gun prototype**: pick the gun up in the castle grounds, aim with the
  centre reticle, place a blue (P1) and an orange (P2) portal on solid
  geometry, and walk into one to teleport with momentum preserved.
- **Debug channel** over adb (`dump`, `set`, `toggle`, `fx`, `fps`, `portal`,
  `givegun`, `busy`, ...) plus a red/green hands-off indicator.

See [README-lumepad-2-fork.md](README-lumepad-2-fork.md) for the full feature
list, the knob/key tables and the known gaps.

## Sources

https://github.com/libsdl-org/SDL/tree/release-2.30.3

https://github.com/VDavid003/sm64-port-android/tree/ex/master

https://github.com/VDavid003/sm64-port-android-base/tree/sm64ex

https://github.com/VDavid003/sm64-port-android/commit/f54aa0aa1bcf4ec7626e42fc960840d65ef324d3

