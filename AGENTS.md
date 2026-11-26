# AGENTS.md

Notes for automated agents working in this repository.

## Mandatory shared locks

All builds and all device/adb work must go through `scripts/agent-lock.sh`:

```bash
# Build (shared by every worktree of this repo)
scripts/agent-lock.sh build --reason "what you are building" -- ./gradlew assembleDebug --no-daemon

# Device / adb (shared by every project on this host)
scripts/agent-lock.sh device --reason "what you are testing" -- bash -lc '...adb commands...'

# Short read-only probe: dumpsys, logcat, screencap, pidof (~1 s, shared)
scripts/agent-lock.sh device-quick --reason "status check" -- bash -lc 'adb shell ...'
```

`device-quick` takes a **shared** lock: several probes may run at once, but none
may run while a `device` holder has the tablet. Use it for anything read-only
and short so a status check never queues behind a three-minute screen recording.
It does not mute, mark BUSY or sleep the tablet, and it will not wake a sleeping
device — use it on an awake tablet, and never run installs, recordings or
state-changing commands under it.

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
- **`screencap` of a locked or sleeping tablet returns an all-black image**, not
  an error. Wake and unlock first (`/media/share/lp2-test/scripts/unlock.sh`),
  then capture; a black screenshot is almost always power state, not a
  rendering bug. Check with `dumpsys power | grep mWakefulness` before blaming
  the app. The panel's `screen_off_timeout` is **30 s**, and a sleeping surface
  also freezes the level script mid-load; `scripts/quick-dev.sh` raises it for
  the hold and restores it on exit.
- **adb is over Wi-Fi (`192.168.1.207:5555`), so sleeping the tablet drops the
  connection.** During a multi-agent session pass `AGENT_NO_SLEEP=1` on
  intermediate device calls and only let the final call sleep it. If `adb
  devices` is empty, that is usually why.
- **Power audit is automatic now.** Each device hold records `boot_id`,
  `sys.boot.reason`, wakefulness and battery level, and logs a `rebooted` event
  naming the previous holder if the boot id changed, `low_battery` below 15 %,
  and `sleep_sent` / `sleep_skipped` with the reason. A `shutdown,battery` boot
  reason means the tablet simply ran out of charge — no agent did it. Read the
  device events with:
  `grep -E 'rebooted|low_battery|sleep_|power_blocked' ~/.local/state/opencode/devices/lock-events.log`
- **A power guard wraps `adb` inside every device hold.** `reboot`, `poweroff`,
  `shutdown`, `svc power` and the screen keyevents (`26`, `223`, `224`) are
  refused with exit 97 and logged as `power_blocked`. A deliberate power action
  needs `AGENT_ALLOW_POWER=1` in the environment of that command, and then it is
  logged as allowed. The lock's own sleep is unaffected (it runs outside the
  child) and is skipped when another agent is queued.
- **Never `adb reboot` the tablet.** `getprop sys.boot.reason` records
  `reboot,shell` when an agent does this, and it destroys everyone's session
  state. If the tablet stops answering, say so and let a human wake it; a
  reboot is not a retry.
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

**Fresh worktrees need two extra things** (cost two failed builds otherwise):

```bash
export NDK_HOST_PYTHON=/usr/bin/python3          # the NDK's bundled host python is an
                                                 # unwrapped x86-64 binary here
cp -r /home/ubuntu/opencode/android-sm64/app/jni/src/build/us_pc \
      <worktree>/app/jni/src/build/              # generated assets, git-ignored
```

The main checkout has neither problem, which is why a central incremental build
is still the cheapest way to compile an agent's patch.

APK: `app/build/outputs/apk/debug/app-debug.apk`.

The NDK toolchain runs under qemu here and occasionally crashes with a spurious
`python3: Syntax error` or header corruption. Re-run the build; it is not
usually a code error.

## Efficient debugging (read before any tuning session)

Runtime dials beat recompiles, and one batched device sweep beats many
rebuild-install-look loops. A dial change is ~0.1 s, a sweep step ~3 s, and a
rebuild + install + level load 2-4 minutes.

- Prefer `cmd fx --ei k <key> --ei v <value>` over editing a constant.
- Every new effect ships with an `fx` key, a panel control, a status field and a
  README row, so the next session can tune it without a compiler.
- Write the whole measurement matrix into a plan and run it in ONE device-lock
  hold with `scripts/fx-sweep.sh <plan>` (it refuses to run unlocked).
- Check `BACKLOG.md`, `MOTION-VECTORS.md`, `DENOISER-RATE-LIMIT.md` and the
  README tables before re-deriving a number.

Full rationale, costs, probing examples and anti-patterns:
[`EFFICIENT-DEBUGGING.md`](EFFICIENT-DEBUGGING.md).

## Quick device loop

One command runs the whole loop in one lock hold and prints a summary you can act
on without opening anything else:

```bash
# install-if-changed, start, wait for the level, fx/ptc, capture, summary
AGENT_TABLET_MUTE=1 scripts/agent-lock.sh device --reason "quick dev" -- \
    scripts/quick-dev.sh --level 38 --ptc annex --fx "100=1 101=200"

# read-only capture of the current screen/state (shared lock, never queues;
# only on an awake tablet — it does not wake the screen, so a dozing panel
# gives a black PNG)
scripts/agent-lock.sh device-quick --reason "quick capture" -- \
    scripts/quick-dev.sh --capture-only
```

It installs only when the APK stamp (mtime+size) or the installed package's
`lastUpdateTime` changed, waits for `PTC level=<n>` with `lvl>=1` (the level
script has finished `init_level()`) and `surfaceValid=true` instead of sleeping,
and writes `status.txt`, `shot.png`, filtered `logcat.txt` and `meta.txt` under
`/media/share/tmp/artifacts/sm64/quickdev/<stamp>/`. The summary is level/area/
phase, `fps=`, `slowmo=`, `rays=` (plus `glow=`/`occl=` when present), any
`FATAL`/`ANR` lines, artifact paths and the measured wall time.

- Changing a **number**: use `--fx` or `scripts/fx-sweep.sh`, never a rebuild.
- Changing **behaviour**: rebuild once; then `--fx`/`--ptc`/`--level` for every
  data point after that.
- `--dry-run` prints the plan without touching the tablet; `--help` lists every
  flag. Measured here: ~3 s when the app is already at the level, ~7 s for a
  restart into the level with QUICK DEV on (~12 s with it off), plus ~10 s when
  the APK changed and has to be installed.

The script also wakes the panel and raises its 30 s screen timeout for the hold
(a sleeping tablet renders black and freezes the level script), restoring it on
exit.

The in-app **DEBUG → SYS → QUICK DEV** toggle (off by default) makes launches
themselves cheap: it boots the configured level directly (default L38) by
starting at the title-screen entry with the debug level-select request already
armed, skipping the logo, attract demo and press-start wait (the title entry
advances by itself, no input), and caps the boot presentation sleeps at 2
frames until `init_level()` clears the cap. It adds `qd=<on>,<level>,<mute>` to
`cmd dump`.
`quick-dev.sh --quickdev 1 --level 38 --restart` sets it from the shell. Leave it
off when reproducing a normal launch.

## Todo / backlog

`TODO.md` is the short checked-in "what is left" (blocked-on-hardware list, open
bugs, next perf levers, not-started items). `BACKLOG.md` holds the long-form
notes and the *why*. Read `TODO.md` first, update it in the same commit as the
work that closes a line.

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
adb shell $B --es cmd captions --ei v 1     # caption text: 1 JAVA (default), 0 GAME FONT
adb shell $B --es cmd layout --es action toggle  # touch layout editor on/off (HUD chip)
adb shell $B --es cmd feedback              # capture a feedback item (see below)
adb shell $B --es cmd feedback --es text "Mario pops at the pyramid"
```

Useful PTC actions: `annex`, `cubetop`, `slam`, `groundslam`, `balltest`,
`ballreset`, `ballequip`, `resetpad`, `portaltest`, `portalforce`,
`portalauto`, `portalalign`, `portalblit`, `eyeL`, `eyeR`, `eyeOff`,
`bowser` (held tail-spin HFR rig), `hfron`/`hfroff`, `hfrdump`,
`hfrslerpon`/`hfrslerpoff`, `hfralphaNNN`.

## Feedback outbox

The debug HUD has a **FEEDBACK** button directly under the FPS meter. A short
tap captures immediately with no text; long press opens an optional note
dialog. The label doubles as the pending-item badge (`FEEDBACK (n)`), and
**CLEAR** removes everything for the user. The bar ends with the touch-layout
chip (**EDIT LAYOUT** / **● DONE**), which enters and leaves the layout editor
through the same `TouchOverlay.setEditMode()` path as DEBUG > SYS > TOUCH
LAYOUT and `cmd layout on|off|toggle`; it stays tappable while edit mode has
game input disabled. Each tap writes one directory:

```
/sdcard/Android/data/com.vdavid003.sm64port/files/feedback/<yyyyMMdd-HHmmss>/
    feedback.txt   user text (empty for a one-tap capture)
    shot.png       game frame presented at the tap (SBS, <=1920 px wide)
    state.txt      debug-report header: versions, FPS mode, rate, FX flags,
                   stereo, full PTC status line (qd=/rays=/glow=/occl=), timings
    log.txt        last 256 KB of the rolling app log (DebugLog)
```

Items accumulate until an agent deletes them. The outbox is in the app's
external files dir (pullable without root), and is **not** inside
`files/debug-logs`, so it can never rotate a crash log away.

```sh
D=/sdcard/Android/data/com.vdavid003.sm64port/files/feedback
adb shell ls -l $D                    # pending items
adb pull $D/<timestamp>               # one item
adb pull $D .                         # the whole outbox
adb shell rm -rf $D/<timestamp>       # delete after pulling
adb shell rm -rf $D/*                 # clear the outbox
```

`cmd feedback` triggers the same capture remotely (useful for tests):
`adb shell $B --es cmd feedback --es text "..."`. `cmd dump` reports `fb=<n>`,
the pending count, so a script can wait for a capture without screen-scraping.
Do not clear items the user created unless you have pulled them.

## Portal relay testing

`scripts/portal-relay-test.sh` loads the PTC, runs a fixed-camera
`portaltest`, captures left/right eyes with `exec-out`, and runs
`scripts/portal-relay-test.py` for checkerboard edge/period metrics.
See `PORTAL-RELAY-TEST.md`.

`ptc portalalign` draws a magenta source-portal fan behind the relay decal and
logs the decal GL viewport/NDC bounds — use it to distinguish a geometry
mismatch from a missing texture.
