# AGENTS.md

Notes for automated agents working in this repository.

## Mandatory shared locks

All builds and all device/adb work must go through `scripts/agent-lock.sh`:

```bash
# Build (shared by every worktree of this repo)
scripts/agent-lock.sh build --reason "what you are building" -- ./gradlew assembleDebug --no-daemon

# Device / adb (shared by every project on this host)
scripts/agent-lock.sh device --reason "what you are testing" -- bash -lc '...adb commands...'
```

The lock is held for the lifetime of the command. Do **not** bypass it. If the
lock times out (exit code `75`), wait/retry later or report the wait — never
delete or bypass the lock file.

The helper records metadata and JSONL events next to the lock files; see
`scripts/agent-lock.sh --help` for environment variables (`AGENT_LOCK_ROOT`,
`AGENT_DEVICE_LOCK`, `AGENT_ID`, `AGENT_LOCK_EVENT_LOG`, ...).

The device lock also:

- marks the app **BUSY** through the debug channel, so the bottom debug text
  turns red and humans know not to touch the tablet;
- clears BUSY and **puts the tablet to sleep** on release.

Use it for every adb interaction, even a one-line `dumpsys`.

## Device hygiene

- Unlock the tablet with `/media/share/lp2-test/scripts/unlock.sh`.
- Prefer `adb exec-out screencap -p > /tmp/opencode/shot.png` so screenshots
  are never written to `/sdcard`.
- If you do create device files, delete them when done:
  `adb shell rm -f /sdcard/your-shot.png`.
- **Always lock the tablet when you are done testing.** The device lock does
  this automatically on release; if you used `AGENT_NO_SLEEP=1` for an
  intermediate step, finish with a normal device-lock command (or
  `adb shell input keyevent 26`) so the screen is off.
- Keep the device lock command self-contained; its trap clears BUSY and sleeps
  the tablet even if the command fails.
- For multi-step testing, `AGENT_NO_SLEEP=1` keeps the tablet awake between
  device-lock calls; never leave it set for the final call.

## Build environment

Typical local build invocation:

```bash
export JAVA_HOME=/home/ubuntu/opencode/jdk
export ANDROID_HOME=/home/ubuntu/opencode/sdk
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export ANDROID_NDK_HOME="$ANDROID_HOME/ndk/27.2.12479018"
export GRADLE_USER_HOME=/home/ubuntu/opencode/gradle-home
export GNUMAKE=/usr/bin/make
export PATH="$JAVA_HOME/bin:/media/share/lp2-test/toolchain/hostbin:/media/share/lp2-test/toolchain/x86bin:$PATH"

scripts/agent-lock.sh build --reason "my change" -- ./gradlew assembleDebug --no-daemon
```

APK: `app/build/outputs/apk/debug/app-debug.apk`.

The NDK toolchain runs under qemu here and occasionally crashes with a spurious
`python3: Syntax error` or header corruption. Re-run the build; it is not
usually a code error.

## Backlog

Open threads (unfinished effects, known bugs, environment gotchas such as the
stubbed `osGetTime()` and the `rate=` metric trap) are tracked in
[`BACKLOG.md`](BACKLOG.md). Read it before starting new work and update it in the
same commit as the change that makes a line stale.

## Debug channel

```bash
B="am broadcast -n com.vdavid003.sm64port/.DebugReceiver -a com.vdavid003.sm64port.CMD"
adb shell $B --es cmd level --ei n 38       # Portal Test Chamber
adb shell $B --es cmd ptc --es action annex # warp to the cube pyramid
adb shell $B --es cmd ptc --es action launch # portal momentum launch demo
adb shell $B --es cmd ptc --es action toolswap # portal gun <-> grapple
adb shell $B --es cmd ptc --es action run   # smoke test
adb shell $B --es cmd dump                  # status line in logcat (SM64Leia: PTC)
adb shell $B --es cmd busy --ei v 1         # BUSY/red (device lock does this)
```

Useful PTC actions: `annex`, `cubetop`, `slam`, `groundslam`, `balltest`,
`ballreset`, `ballequip`, `resetpad`, `portaltest`, `portalforce`,
`portalauto`, `portalalign`, `portalblit`, `eyeL`, `eyeR`, `eyeOff`,
`bowser` (held tail-spin HFR rig), `hfron`/`hfroff`, `hfrdump`,
`hfrslerpon`/`hfrslerpoff`, `hfralphaNNN`.

## Portal relay testing

`scripts/portal-relay-test.sh` loads the PTC, runs a fixed-camera
`portaltest`, captures left/right eyes with `exec-out`, and runs
`scripts/portal-relay-test.py` for checkerboard edge/period metrics.
See `PORTAL-RELAY-TEST.md`.

`ptc portalalign` draws a magenta source-portal fan behind the relay decal and
logs the decal GL viewport/NDC bounds — use it to distinguish a geometry
mismatch from a missing texture.
