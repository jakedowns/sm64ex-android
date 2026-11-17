# Multi-agent rules

Rules for several coding agents working in this repository at the same time.
They exist because two shared resources cannot be used concurrently: the
connected Android device (via `adb`) and the Android build itself. Two agents
touching either one at the same time produce corrupted outputs, flaky installs,
and misleading test results rather than a clean error.

These rules are mandatory for every agent, including subagents. "Agent" below
means any process acting on this checkout or one of its worktrees: an
interactive session, a subagent, a CI job, or a human running an automated
script.

## 1. Shared resources and their locks

Locks protect **host-level resources**, not directories. A git worktree is a
separate source tree, not a separate build machine, so it does **not** get a
second build slot.

| Resource | Lock | Scope | Default path | Max wait |
| --- | --- | --- | --- | --- |
| Gradle/NDK/asset build | `build.lock` | This repository, shared by **all worktrees and copies** | `$AGENT_LOCK_ROOT/android-sm64/build.lock` | 3600 s |
| Connected Android device / `adb` server | `android.lock` | **This host**, shared by all worktrees and projects using adb | `$AGENT_LOCK_ROOT/devices/android.lock` | 300 s |

`AGENT_LOCK_ROOT` defaults to `${XDG_STATE_HOME:-$HOME/.local/state}/opencode`.
`AGENT_BUILD_LOCK` and `AGENT_DEVICE_LOCK` override the full paths, and
`AGENT_PROJECT_KEY` changes the per-repository directory name (default:
`android-sm64`). Locks therefore live outside every checkout and survive
worktree creation and removal. If agents use a different root, they must agree
on it: set `AGENT_LOCK_ROOT` explicitly in their environment.

The project key was renamed from `sm64ex-android` to `android-sm64`. A build
started under the old key is still detected: the helper waits for the legacy
`$AGENT_LOCK_ROOT/sm64ex-android/build.lock` and holds it for the duration of
the new build, so the old and new paths cannot build concurrently. Once
`status` no longer reports a legacy holder and no pre-rename agent is running,
the deprecated `$AGENT_LOCK_ROOT/sm64ex-android/` directory can be archived or
removed.

Do **not** create or use `<worktree>/.locks/`. A repo-local lock directory
fragments the mutex: `android-sm64-ssfx/.locks` and
`sm64-camera-enhancements/.locks` are different files, so two branches could
build or install at the same time. `.locks/` remains gitignored only as a
defence against misconfiguration.

Use the helper, not raw `flock`:

```bash
scripts/agent-lock.sh build  [--timeout S] [--reason TEXT] -- <command...>
scripts/agent-lock.sh device [--timeout S] [--reason TEXT] -- <command...>
scripts/agent-lock.sh status
```

The helper holds the lock for exactly the lifetime of `<command...>`, records
the agent, branch, commit, and worktree in the lock file, and exits `75` if the
wait times out. It resolves the checkout from the current directory, so the
main checkout's copy can be used from a feature-branch worktree and still
records that branch. Because the device lock is host-wide, `status` may report
a holder from another worktree or even another project; that is expected.

### 1.1 Lock event log

Every acquire, release, and timeout is appended as one JSON object per line
(JSONL), kept separate from the `flock` files:

- build events: `$AGENT_LOCK_ROOT/android-sm64/lock-events.log`
- device events: `$AGENT_LOCK_ROOT/devices/lock-events.log`
- `AGENT_LOCK_EVENT_LOG` overrides both to a single append-only file

Each record contains `v`, `ts`, `event` (`acquire`, `release`, or `timeout`),
`lock`, `lock_file`, `agent`, `pid`, `host`, `worktree`, `branch`, `head`,
`reason`, `wait_timeout_s`, `waited_ms`, `hold_ms`, and `exit` (`null` on
acquire, `75` on timeout, otherwise the command's exit code). `status` prints
the active log paths.

Logging is **best-effort**: an unwritable path or full disk never prevents a
lock from being acquired or released. The log is append-only with no automatic
rotation; rotate or delete it manually when it grows (deleting the log is safe —
it is not the lock file). Because `reason` is persisted, never put secrets in it.

```bash
# Recent build holders, with hold time and exit status
jq -r 'select(.event=="release") |
  [.ts, .branch, .reason, .waited_ms, .hold_ms, .exit] | @tsv' \
  ~/.local/state/opencode/android-sm64/lock-events.log | tail -20

# Who timed out waiting for the device?
jq -r 'select(.event=="timeout") | [.ts, .worktree, .reason, .waited_ms] | @tsv' \
  ~/.local/state/opencode/devices/lock-events.log
```

### 1.2 Build lock — what it covers

Hold the build lock for anything that writes build outputs or shared Gradle
state, in **any** worktree:

- `./gradlew ...` in any form: `assembleDebug`, `assembleRelease`, `clean`,
  `lint`, `test`, `:app:externalNativeBuildDebug`, `--refresh-dependencies`
- `ndk-build` / the Gradle-invoked NDK build
- `make` in `app/jni/src/` and `bash Setup-MINGW64.sh`
- anything that writes `app/build/`, `app/obj/`, `app/.cxx/`, `app/libs/`,
  `build/`, or `.gradle/`
- worktree provisioning that copies generated native assets
- repository-wide format/asset tools that regenerate sources used by a build
- `sdkmanager`, SDK/NDK installs, or Gradle cache maintenance

Per-worktree output directories reduce file collisions, but they do not make
parallel builds safe. Worktrees share:

- the Gradle user home (`~/.gradle` caches, wrapper downloads, daemons)
- the Android SDK/NDK, build-tools, and
  `android.aapt2FromMavenOverride=/media/share/lp2-test/toolchain/x86bin/aapt2`
- the debug keystore (`~/.android/debug.keystore`)
- CPU, RAM, and disk; `org.gradle.jvmargs=-Xmx1536m` plus four ABI filters in
  `app/build.gradle` means two concurrent NDK builds can exhaust memory

Never run two of the above concurrently, even in different worktrees, and never
"just quickly" to see whether a worktree builds.

### 1.3 Device lock — what it covers

Hold the device lock for **every** invocation that talks to the device or the
`adb` server, including:

- `adb devices`, `adb start-server`, `adb kill-server`, `adb reconnect`
- `adb install`, `uninstall`, `shell`, `push`, `pull`, `reverse`, `forward`
- `adb logcat`, `adb bugreport`, `adb exec-out screencap`
- `adb shell input ...`, `am start`, `am force-stop`, `monkey`
- `./gradlew installDebug`, `connectedAndroidTest`, `connectedCheck`
- `scrcpy`, emulator console commands, or any other tool that attaches to the
  same device

The device is a single physical resource shared by every branch under test. A
second agent installing an APK or starting a second `logcat` invalidates the
first agent's observations.

## 2. Mandatory protocol

1. **Acquire before touching.** Never run `adb` or Gradle "just quickly" to
   check something without the matching lock. The health check is inside the
   lock too.
2. **Keep the hold short.** Do not open interactive shells, editors, or
   long-lived `logcat` streams under a lock. Wrap streams in `timeout`.
3. **Release by exiting.** The helper releases when the wrapped command exits.
   Never delete a lock file to "free" it (see §7).
4. **Hold one lock at a time.** If a workflow needs both resources, split it
   into a build phase and a device phase (§3).
5. **A worktree is not a second build slot.** Different branch, different
   directory, same lock.
6. **Never bypass on timeout.** Exit code `75` means "busy, retry later", not
   "the lock is broken". Retry with backoff or report the wait.
7. **Always go through the helper.** Raw `flock`, `mkdir`-based mutexes, or
   "check if adb is in use" heuristics are not accepted substitutes.

## 3. Canonical workflows

### 3.1 Build, then install (two phases)

```bash
# Phase 1: build only, no device access.
scripts/agent-lock.sh build --reason "assembleDebug" -- \
    ./gradlew assembleDebug --no-daemon

# Phase 2: device only, no build.
scripts/agent-lock.sh device --reason "install debug apk" -- \
    adb install -r app/build/outputs/apk/debug/app-debug.apk
```

This is deliberately split. It keeps the device lock held for seconds, not for
the minutes an NDK build takes, and it avoids holding both locks.

### 3.2 Observe a run

```bash
scripts/agent-lock.sh device --reason "launch + bounded logcat" -- bash -c '
    adb shell am force-stop com.vdavid003.sm64port
    adb shell am start -n com.vdavid003.sm64port/.MainActivity
    timeout 20 adb logcat -d -s SDL:V DEBUG:V > /tmp/opencode/agent-logcat.txt
'
```

Bounded capture only. Never leave `adb logcat` running after the command
returns.

### 3.3 Instrumented tests / `installDebug` (the one coupling exception)

Tasks that inherently build and then act on the device need both locks. Use the
canonical order **build first, then device**, and hold both only for that task:

```bash
scripts/agent-lock.sh build --reason "connectedAndroidTest" -- \
    scripts/agent-lock.sh device --reason "connectedAndroidTest" -- \
        ./gradlew connectedAndroidTest --no-daemon
```

Nested acquisition in the opposite order is forbidden. Even better, restructure
the workflow so a prebuilt APK is installed with `adb install` in the device
phase instead.

### 3.4 Native-only iteration

`make` in `app/jni/src/` is still a build: it writes `app/libs` and object
files. Use the build lock even when Gradle is not involved.

### 3.5 Building a feature-branch worktree

Run the helper from inside the worktree. The lock is still the shared build
lock, but metadata records the branch and worktree:

```bash
wt="$(git rev-parse --show-toplevel)"
branch="$(git rev-parse --abbrev-ref HEAD)"
sha="$(git rev-parse --short HEAD)"

"$wt/scripts/agent-lock.sh" build --reason "assembleDebug $branch @ $(basename "$wt")" -- \
    ./gradlew assembleDebug --no-daemon
```

If `scripts/` is not present on that branch yet, invoke the main checkout's
copy (`/home/ubuntu/opencode/android-sm64/scripts/agent-lock.sh`) while your
shell is inside the worktree. The helper uses the current git toplevel for
metadata and preflight checks, so it still operates on the worktree you are
standing in.

## 4. Worktrees and feature branches

### 4.1 Locks stay outside the worktree

Never copy `.locks/` into a worktree, never `AGENT_LOCK_ROOT` a path inside
one, and never remove a lock as part of worktree cleanup. Removing a worktree
with `git worktree remove` must not affect any lock; locks live under host
state for exactly this reason.

A plain `cp -a` copy (not a git worktree) has its own `.git` and would otherwise
get its own locks. If agents use plain copies, they must all set
`AGENT_LOCK_ROOT` to the same host-level directory.

### 4.2 Provision each worktree, under the build lock

A new worktree does not contain gitignored/generated data. In particular:

- `app/jni/src/build/` (generated native assets, about 134 MB)
- `local.properties` (SDK path)
- `app/libs/`, `app/.cxx/`, `app/obj/`

Copy or regenerate these **inside the build lock**, and give every worktree its
own copy. Do not symlink mutable generated directories to the main checkout:
branch A's build can overwrite data branch B depends on, and stale object files
or assets produce a binary that does not match the checked-out source.

A worktree without `local.properties` needs `ANDROID_HOME` or
`ANDROID_SDK_ROOT` in the environment; the helper warns during the build phase
if neither is available.

### 4.3 Use the worktree's own artifacts

Always derive the APK path from the current worktree:

```bash
apk="$(git rev-parse --show-toplevel)/app/build/outputs/apk/debug/app-debug.apk"
```

Never install the main checkout's `app/build/.../app-debug.apk` while testing a
branch. Record provenance before installing:

```bash
sha256sum "$apk"
cp "$apk" "/tmp/opencode/artifacts/$(basename "$wt")-$(git rev-parse --short HEAD).apk"
```

Artifacts for different branches must not share a filename; otherwise the next
agent installs the wrong build.

### 4.4 Device state crosses branches

All worktrees build the same `applicationId`
(`com.vdavid003.sm64port`), so installing branch B replaces branch A in place
and keeps its app data. Saves created on one branch may not be compatible with
another. The device lock serializes access but does not isolate state.

At the start of every device phase:

```bash
adb shell am force-stop com.vdavid003.sm64port
adb logcat -c
adb install -r "$apk"
adb shell dumpsys package com.vdavid003.sm64port | grep -E 'lastUpdateTime|versionName'
```

State the branch/commit being installed in `--reason`, and leave the device
clean (app force-stopped, no streaming `logcat`) before releasing the lock.
`pm clear` destroys saves; require explicit user consent rather than clearing
automatically.

### 4.5 Worktree administration

Creating and removing worktrees mutates the shared `.git/worktrees` metadata.
Git protects its own refs, but agents should still:

- use unique worktree names (branch/session based) to avoid collisions
- never run `git worktree prune` while another agent has an active worktree
- avoid `git gc`/repository maintenance while builds or other agents are active
- remove only their own worktree, and never touch the main checkout's files

## 5. Lock ordering and deadlock avoidance

The safe pattern is **no agent ever holds more than one lock**. Long build holds
and short device holds then overlap freely between agents and worktrees.

For the rare task that needs both, the only allowed order is:

```
build lock  ->  device lock  ->  run  ->  release device  ->  release build
```

Never acquire the device lock and then wait for the build lock. With two locks
and a fixed order there is no cycle, so deadlocks cannot occur. Do not introduce
a third lock or a lock hierarchy without updating this document.

If queueing from a single build lock becomes painful, the replacement is a
global semaphore of N build slots, each slot still exclusive and host-wide.
Per-worktree locks are not the answer; the default remains one build at a time.

## 6. Timeouts, retries, and reporting

- Build lock default wait: 3600 s. Raise it with `--timeout` only for a
  genuinely long build, and only after checking `status`.
- Device lock default wait: 300 s. A device phase should normally finish in
  well under a minute; a long wait means someone else is stuck.
- On exit `75`: run `scripts/agent-lock.sh status`, wait 15–30 s, retry. Do not
  retry in a tight loop, and do not run diagnostic `adb` commands while waiting.
- If a wait exceeds roughly twice the default, stop and tell the user which lock
  is held, by whom, on which branch/worktree, and for what reason. Do not
  force-release on your own.

## 7. Stale locks and crashes

The kernel releases a `flock` automatically when the holding process exits, so
a crashed agent or a removed worktree cannot leave the resource permanently
locked. The metadata in the lock file may look stale; that is normal and
harmless.

- **Never `rm` a lock file.** Removing the path lets another process create a
  new inode and acquire a "second" lock while the first holder still runs.
- **Never kill a holder based on lock-file metadata alone.** PID reuse exists.
  Confirm ownership first with
  `fuser ~/.local/state/opencode/android-sm64/build.lock` (adjust for your
  `AGENT_LOCK_ROOT`) or `ls -l /proc/<pid>/fd` before escalating.
- The helper writes a `released_at` record on exit. Treat all metadata as
  advisory; `flock` membership is the source of truth.
- The event log is the durable history of lock usage. A release line is written
  on normal exit; a `SIGKILL` can leave only the acquire line, which is expected
  and does not mean the lock is still held.
- If a device is genuinely wedged, that is a human escalation, not a license to
  skip the lock. `adb kill-server` is disruptive and runs under the device lock.

## 8. Host and filesystem assumptions

The build lock assumes all worktrees/copies of this repository on one host see
the same lock file. The device lock assumes the same for the whole host.
Agents running as different OS users do not share the default
`$HOME/.local/state/...` root; they must set a common writable
`AGENT_LOCK_ROOT` instead.

`flock` over NFS is not reliable. If work moves across machines, designate one
"device host" and have other agents remote into it, or replace the helper with a
real lock service. Also never start a second `adb` server against the same USB
device; that is itself an unsynchronized access.

If several devices/emulators are attached, the single device lock is still the
safe default. Splitting it per serial is a future change that requires a
consistent enumeration protocol; do not improvise one.

## 9. Hygiene

- Locks live under host state, not in the repository; `.locks/` stays untracked
  if someone creates it by mistake.
- Put logs, screenshots, and artifacts in `/tmp/opencode/`, ideally under a
  branch/worktree-specific subdirectory, not in the repository.
- Do not store secrets or long transcripts in lock metadata or the event log.
  `--reason` is persisted in both; keep it short, for example
  `"assembleDebug feat/hfr @ hfr-worktree"` or `"install + logcat"`.
- The event log is append-only and not rotated automatically. Rotate or delete
  it manually if it grows; this is safe because it is not the lock file.
- When you are done, leave no background `adb`, `gradle`, or emulator process
  running. The helper prevents lock inheritance, but it does not clean up your
  work for you.
- Removing a worktree does not clean generated assets or artifacts; delete those
  yourself, under the build lock if a build might be running.

## 10. Lume Pad 2 target ABI

The only supported target is the Lume Pad 2, which is `arm64-v8a`. Do not build
`armeabi-v7a`, `x86`, or `x86_64` unless the user explicitly asks for another
target.

- `app/build.gradle` defaults `SM64_ABIS` to `arm64-v8a`.
- Other ABIs are an explicit opt-in, for example:
  `./gradlew assembleDebug -PSM64_ABIS=armeabi-v7a,arm64-v8a,x86,x86_64`
- Do not use `-PEXCLUDE_NATIVE_LIBS=1` as a way to avoid cross-ABI builds. Use
  the ABI filter; `EXCLUDE_NATIVE_LIBS` only means “reuse prebuilt native
  libraries” and is valid only when those libraries are the intended arm64
  build.
- `app/jni/Application.mk` must stay `APP_ABI := arm64-v8a`. Raw `ndk-build`
  invocations bypass Gradle and rely on that file.
- Worktrees are separate checkouts. If a worktree does not yet contain the ABI
  default, propagate or cherry-pick the `app/build.gradle` change before
  building; do not fall back to a four-ABI build.
- Verify the APK before installing:

  ```bash
  unzip -l app/build/outputs/apk/debug/app-debug.apk | grep 'lib/'
  # or
  aapt dump badging app/build/outputs/apk/debug/app-debug.apk | grep native-code
  ```

  The only expected native-code line is `arm64-v8a`. If other ABIs are present,
  the APK was built with the wrong filter; rebuild before using the device.
- The device reports its ABI set; check it under the device lock:

  ```bash
  scripts/agent-lock.sh device --reason "check target ABI" -- \
      adb shell getprop ro.product.cpu.abilist
  ```

  `arm64-v8a` must be first.

## 11. Pre-flight / post-flight checklist

Before starting:

- [ ] Run `scripts/agent-lock.sh status` and note the lock root and holder.
- [ ] Confirm you are in the intended checkout/worktree
      (`git rev-parse --show-toplevel`, `git branch --show-current`).
- [ ] Decide whether this is a build phase, a device phase, or an explicitly
      coupled task.
- [ ] Confirm the build targets `arm64-v8a` only (Lume Pad 2); no extra ABIs.
- [ ] For a fresh worktree, provision generated assets and `local.properties`
      under the build lock.
- [ ] Use the helper with a clear `--reason`; include the branch for worktrees.

After finishing:

- [ ] Confirm the wrapped command exited and the lock was released (`status`).
- [ ] Verify the APK contains only `lib/arm64-v8a/`.
- [ ] Record/verify the APK path and SHA-256 for the branch you installed.
- [ ] Force-stop the app; kill any process you started outside the helper.
- [ ] Report any timeout or suspected stale holder to the user.
