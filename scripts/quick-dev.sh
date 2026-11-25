#!/usr/bin/env bash
# One command for the whole device loop, in ONE device-lock hold.
#
# What it saves: the old loop was install (20-60 s), start, blind `sleep 8`,
# `cmd level`, blind `sleep 8`, dump, screenshot. This script skips the install
# when the APK is unchanged, waits for the *state* instead of sleeping, and
# writes a full artifact set plus a one-screen summary.
#
# Full cycle (install/launch/state changes) - EXCLUSIVE device lock:
#
#   AGENT_TABLET_MUTE=1 scripts/agent-lock.sh device --reason "quick dev" -- \
#       scripts/quick-dev.sh --level 38 [--ptc annex] [--fx "100=1 101=200"]
#
# Read-only capture of whatever is on screen right now - SHARED device-quick
# lock (no install, no launch, no state changes). device-quick does not wake
# the tablet, so use this while it is already awake, or the PNG will be black:
#
#   scripts/agent-lock.sh device-quick --reason "quick capture" -- \
#       scripts/quick-dev.sh --capture-only
#
# Output (never /tmp):
#   /media/share/tmp/artifacts/sm64/quickdev/<stamp>/{status.txt,shot.png,
#       logcat.txt,meta.txt}
#
# The install stamp lives in /media/share/tmp/cache/sm64/quickdev/ and records
# both the host APK (mtime+size) and the device package's lastUpdateTime, so a
# skip is only taken when this exact APK is the one installed.
#
# Options:
#   --level N         level to load (-1 = do not warp; default 38 = PTC)
#   --stereo 0|1      stereo mode to set (default: leave untouched)
#   --ptc ACTION      PTC action after the level settles
#   --fx "K=V K=V"    apply cmd fx keys (repeatable, applied in order)
#   --settle S        seconds to wait before capture (default 1.0)
#   --timeout S       level/app wait timeout (default 30)
#   --install         force install even when the stamp matches
#   --no-install      never install
#   --restart         force-stop and start the app
#   --quickdev 0|1    set the in-app QUICK DEV toggle (implies a restart)
#   --capture-only    read-only: no install/launch/state changes
#   --out DIR         artifact directory (default under .../quickdev/<stamp>)
#   --no-shot         skip the screenshot
#   --tag NAME        extra logcat tag to keep (repeatable)
#   --grep PATTERN    extra logcat filter / summary pattern
#   --lines N         keep at most N logcat lines (default 400)
#   --quiet           suppress progress lines, keep the summary
#   --dry-run         print the plan and exit without touching anything
#   --allow-unlocked  bypass the lock guards (idle tablet only)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG=com.vdavid003.sm64port
ACT=$PKG/.sm64portActivity
B="am broadcast -n $PKG/.DebugReceiver -a $PKG.CMD"
APK="$ROOT/app/build/outputs/apk/debug/app-debug.apk"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="/media/share/tmp/artifacts/sm64/quickdev/$STAMP"
CACHE=/media/share/tmp/cache/sm64/quickdev
STAMP_FILE="$CACHE/installed.stamp"

LEVEL=38
STEREO=-1
PTC_ACTION=""
FX_KEYS=()
SETTLE=1.0
TIMEOUT=30
INSTALL=auto
RESTART=0
QUICKDEV=-1
CAPTURE_ONLY=0
NO_SHOT=0
QUIET=0
DRY=0
ALLOW_UNLOCKED=0
EXTRA_TAGS=()
EXTRA_GREP=""
LOG_LINES=400

usage() { sed -n '2,55p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --level) LEVEL="$2"; shift 2 ;;
        --stereo) STEREO="$2"; shift 2 ;;
        --ptc) PTC_ACTION="$2"; shift 2 ;;
        --fx) FX_KEYS+=("$2"); shift 2 ;;
        --settle) SETTLE="$2"; shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --install) INSTALL=force; shift ;;
        --no-install) INSTALL=never; shift ;;
        --restart) RESTART=1; shift ;;
        --quickdev) QUICKDEV="$2"; shift 2 ;;
        --capture-only) CAPTURE_ONLY=1; shift ;;
        --out) OUT="$2"; shift 2 ;;
        --no-shot) NO_SHOT=1; shift ;;
        --tag) EXTRA_TAGS+=("$2"); shift 2 ;;
        --grep) EXTRA_GREP="$2"; shift 2 ;;
        --lines) LOG_LINES="$2"; shift 2 ;;
        --quiet) QUIET=1; shift ;;
        --dry-run) DRY=1; shift ;;
        --allow-unlocked) ALLOW_UNLOCKED=1; shift ;;
        -h|--help) usage 0 ;;
        *) echo "quick-dev: unknown option: $1" >&2; usage 2 ;;
    esac
done

say() { [[ "$QUIET" == 1 ]] || printf '[quickdev] %s\n' "$*"; }

if [[ -d "$HOME/.local/lib/android-tools" ]]; then
    export LD_LIBRARY_PATH="$HOME/.local/lib/android-tools${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

if [[ "$CAPTURE_ONLY" == 1 ]]; then
    BAD=0
    [[ "$INSTALL" != auto ]] && BAD=1
    [[ "$RESTART" == 1 ]] && BAD=1
    [[ "$QUICKDEV" != -1 ]] && BAD=1
    [[ -n "$PTC_ACTION" ]] && BAD=1
    [[ ${#FX_KEYS[@]} -gt 0 ]] && BAD=1
    [[ "$STEREO" != -1 ]] && BAD=1
    if [[ "$BAD" == 1 ]]; then
        echo "quick-dev: --capture-only is read-only; drop install/restart/quickdev/ptc/fx/stereo." >&2
        exit 2
    fi
    LEVEL=-1
    # Keep the shared-lock probe short unless the caller asked for a timeout.
    [[ "$TIMEOUT" == 30 ]] && TIMEOUT=5
fi

# ------------------------------------------------------------- install stamp
apk_stamp() { [[ -f "$APK" ]] && stat -c '%s:%Y' "$APK" || echo missing; }
device_pkg_marker() {
    adb shell dumpsys package "$PKG" 2>/dev/null \
        | grep -m1 'lastUpdateTime=' | tr -d '\r' || true
}

need_install() {
    local cur_apk="$1" cur_dev="$2" cached_apk cached_dev
    [[ "$INSTALL" == force ]] && { echo install; return; }
    [[ "$INSTALL" == never ]] && { echo skip-forced; return; }
    [[ -f "$STAMP_FILE" ]] || { echo install; return; }
    IFS=$'\t' read -r cached_apk cached_dev <"$STAMP_FILE" || true
    if [[ "$cached_apk" != "$cur_apk" ]]; then echo install; return; fi
    if [[ -n "$cur_dev" && "$cached_dev" != "$cur_dev" ]]; then echo install; return; fi
    # No device marker means the package is not installed (or dumpsys failed).
    [[ -z "$cur_dev" ]] && { echo install; return; }
    echo skip
}

# ------------------------------------------------------------------- dry run
if [[ "$DRY" == 1 ]]; then
    printf 'quick-dev plan\n'
    printf '  mode:      %s\n' "$([[ $CAPTURE_ONLY == 1 ]] && echo 'capture-only (device-quick)' || echo 'full cycle (device)')"
    printf '  apk:       %s (%s)\n' "$APK" "$(apk_stamp)"
    printf '  install:   %s' "$([[ $CAPTURE_ONLY == 1 ]] && echo 'never (capture-only)' || echo "$INSTALL")"
    if [[ -f "$STAMP_FILE" ]]; then
        printf '  stamp: %s' "$(tr '\t' ' ' <"$STAMP_FILE")"
    else
        printf '  stamp: none'
    fi
    printf '\n  start:     %s\n' "$([[ $CAPTURE_ONLY == 1 ]] && echo '(none: capture the live app)' || { [[ $RESTART == 1 ]] && echo 'force-stop + am start' || echo 'only if not running'; })"
    printf '  level:     %s\n' "$LEVEL"
    printf '  stereo:    %s\n' "$([[ $STEREO == -1 ]] && echo '(untouched)' || echo "$STEREO")"
    printf '  fx:        %s\n' "$([[ ${#FX_KEYS[@]} -eq 0 ]] && echo '(none)' || echo "${FX_KEYS[*]}")"
    printf '  ptc:       %s\n' "${PTC_ACTION:-none}"
    printf '  settle:    %ss   timeout: %ss\n' "$SETTLE" "$TIMEOUT"
    printf '  out:       %s\n' "$OUT"
    printf '  shot:      %s\n' "$([[ $NO_SHOT == 1 ]] && echo no || echo yes)"
    printf '  logcat:    SM64 tags%s, last %s lines\n' \
        "${EXTRA_TAGS[*]:+ + ${EXTRA_TAGS[*]}}" "$LOG_LINES"
    [[ -n "$EXTRA_GREP" ]] && printf '  grep:      %s\n' "$EXTRA_GREP"
    printf '  (dry run touched nothing: no adb, no files)\n'
    exit 0
fi

# ---------------------------------------------------------------- lock guard
LOCK_KIND="${AGENT_LOCK_KIND:-}"
LOCK_HELD="${AGENT_DEVICE_LOCK_HELD:-}"
# device-quick exports AGENT_LOCK_KIND but not AGENT_DEVICE_LOCK_HELD.
if [[ "$LOCK_KIND" == "device-quick" ]]; then
    LOCK_HELD=1
fi
if [[ "$LOCK_HELD" != "1" && "$ALLOW_UNLOCKED" != 1 ]]; then
    cat >&2 <<'LOCK'
quick-dev: refusing to touch the device without a device lock.

  full cycle (install/launch/state):
    AGENT_TABLET_MUTE=1 scripts/agent-lock.sh device --reason "quick dev" -- \
        scripts/quick-dev.sh [options]

  read-only capture only:
    scripts/agent-lock.sh device-quick --reason "quick capture" -- \
        scripts/quick-dev.sh --capture-only

Override with --allow-unlocked only for a throwaway check on an idle tablet.
LOCK
    exit 3
fi
if [[ "$ALLOW_UNLOCKED" != 1 && -n "$LOCK_KIND" ]]; then
    if [[ "$CAPTURE_ONLY" == 1 && "$LOCK_KIND" != "device-quick" ]]; then
        echo "quick-dev: --capture-only must run under device-quick (shared, read-only)." >&2
        exit 3
    fi
    if [[ "$CAPTURE_ONLY" != 1 && "$LOCK_KIND" == "device-quick" ]]; then
        echo "quick-dev: install/launch needs the exclusive device lock, not device-quick." >&2
        exit 3
    fi
fi

# ---------------------------------------------------------------- adb helpers
# $B is intentionally unquoted: it is a command prefix, like fx-sweep.sh.
bcast() { adb shell $B "$@" >/dev/null 2>&1 || true; }

# The panel sleeps after 30 s here; that blacks the surface and freezes the
# level script mid-wait, which used to look like "quick dev does not boot".
# Raise the timeout for the hold and put it back on exit.
ORIG_SCREEN_TIMEOUT=""
screen_timeout_bump() {
    [[ "$CAPTURE_ONLY" == 1 ]] && return 0
    ORIG_SCREEN_TIMEOUT=$(adb shell settings get system screen_off_timeout 2>/dev/null | tr -d '\r') || true
    [[ "$ORIG_SCREEN_TIMEOUT" =~ ^[0-9]+$ ]] || ORIG_SCREEN_TIMEOUT=30000
    adb shell settings put system screen_off_timeout 180000 >/dev/null 2>&1 || true
    trap 'adb shell settings put system screen_off_timeout "${ORIG_SCREEN_TIMEOUT:-30000}" >/dev/null 2>&1 || true' EXIT
}

wake_screen() {
    [[ "$CAPTURE_ONLY" == 1 ]] && return 0
    local awake
    awake=$(adb shell dumpsys power 2>/dev/null \
        | grep -m1 -o 'mWakefulness=[A-Za-z]*' | cut -d= -f2)
    if [[ "$awake" != "Awake" ]]; then
        say "wake tablet (${awake:-unknown})"
        "${UNLOCK_SH:-/media/share/lp2-test/scripts/unlock.sh}" >/dev/null 2>&1 || true
        adb shell input keyevent 82 >/dev/null 2>&1 || true
        sleep 1
    fi
}

last_status() {
    adb logcat -d -s SM64Leia:V -t 400 2>/dev/null \
        | grep 'PTC level=' | tail -1 | sed 's/^.*PTC /PTC /' || true
}

# The game renders only when the panel surface is valid; a black 20 KB
# screenshot means the tablet dozed off (screen_off_timeout is 30 s here).
# NB: `logcat -t N` is not a per-tag tail here (the SM64Leia stream is busy);
# 80 lines can miss the dump that was just printed. 600 is reliably recent.
java_surface_ready() {
    adb logcat -d -s SM64Leia:V -t 600 2>/dev/null \
        | grep 'DUMP drawable=' | tail -1 | grep -q 'surfaceValid=true'
}

java_drawable() {
    adb logcat -d -s SM64Leia:V -t 600 2>/dev/null \
        | grep -o 'drawable=[0-9]*x[0-9]*' | tail -1 || true
}

dump_status() {
    # One fresh dump. The Java side logs the DUMP line, then the PTC line.
    bcast --es cmd dump
    local i line
    for i in 1 2 3 4 5 6; do
        sleep 0.12
        line="$(last_status)"
        [[ -n "$line" ]] && { printf '%s' "$line"; return 0; }
    done
    return 1
}

dump_java() {
    adb logcat -d -s SM64Leia:V -t 600 2>/dev/null | grep 'DUMP drawable=' | tail -1 || true
}

field() { grep -o "$1=[^ ]*" <<<"$2" | head -1 | cut -d= -f2- || true; }

proc_alive() { [[ -n "$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r')" ]]; }

wait_proc() {
    local deadline=$(( $(date +%s) + 20 ))
    while (( $(date +%s) < deadline )); do
        proc_alive && return 0
        sleep 0.3
    done
    return 1
}

# Wait until the PTC status reports the requested level (or any level when
# LEVEL < 0). Returns 0 on success, 1 on timeout. Prints the status line.
LAST_LINE=""
LAST_SURFACE=""
wait_level() {
    local want="$1" timeout="$2" deadline line cur
    deadline=$(( $(date +%s) + timeout ))
    while (( $(date +%s) <= deadline )); do
        line="$(dump_status || true)"
        if [[ -n "$line" ]]; then
            LAST_LINE="$line"
            cur="$(field level "$line")"
            if [[ "$want" == "-1" || "$cur" == "$want" ]]; then
                # `lvl=` is the number of completed init_level() calls: it is 0
                # while the intro/level-select transition is still running, so
                # it rejects gCurrLevelNum-before-the-area-is-live.
                local lvl ready=1
                lvl="$(field lvl "$line")"
                if [[ -n "$lvl" && "$want" != "-1" && "$lvl" -lt 1 ]]; then
                    ready=0
                fi
                # A level can be selected before the panel is rendering; keep
                # waiting so the captured PNG is not a black asleep-screen.
                if [[ "$CAPTURE_ONLY" != 1 ]] && ! java_surface_ready; then
                    ready=0
                fi
                if [[ "$ready" == 1 ]]; then
                    LAST_SURFACE="$(java_drawable)"
                    printf '%s' "$line"
                    return 0
                fi
            fi
        fi
        sleep 0.3
    done
    LAST_SURFACE="$(java_drawable)"
    return 1
}

# ------------------------------------------------------------------- install
screen_timeout_bump
START_MS=$(date +%s%3N)
INSTALL_MS=0
mkdir -p "$CACHE"
if [[ "$CAPTURE_ONLY" == 1 || "$INSTALL" == never ]]; then
    DECISION=skip
else
    [[ -f "$APK" ]] || { echo "quick-dev: no APK at $APK; build first" >&2; exit 2; }
    HOST_STAMP="$(apk_stamp)"
    DEV_MARKER="$(device_pkg_marker)"
    DECISION="$(need_install "$HOST_STAMP" "$DEV_MARKER")"
fi

if [[ "$DECISION" == install ]]; then
    [[ "$INSTALL" == never ]] && { echo "quick-dev: APK changed but --no-install given" >&2; exit 2; }
    say "install ($(basename "$APK"))"
    t0=$(date +%s%3N)
    if ! adb install -r "$APK" >/dev/null 2>&1; then
        echo "quick-dev: adb install failed" >&2
        exit 2
    fi
    INSTALL_MS=$(( $(date +%s%3N) - t0 ))
    HOST_STAMP="$(apk_stamp)"
    DEV_MARKER="$(device_pkg_marker)"
    printf '%s\t%s\n' "$HOST_STAMP" "$DEV_MARKER" >"$STAMP_FILE"
    say "installed in $(( INSTALL_MS / 1000 )).$(( (INSTALL_MS % 1000) / 100 ))s"
elif [[ "$CAPTURE_ONLY" != 1 ]]; then
    say "install skipped (APK stamp matches the installed package)"
fi

wake_screen

# --------------------------------------------------------------- QUICK DEV
if [[ "$QUICKDEV" != -1 ]]; then
    if ! proc_alive; then
        say "start once to set the QUICK DEV toggle"
        adb shell am start -n "$ACT" >/dev/null 2>&1 || true
        wait_proc || true
    fi
    # The debug channel needs a live activity, not just a live process.
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        dump_status >/dev/null 2>&1 && break
        sleep 0.5
    done
    bcast --es cmd quickdev --ei v "$QUICKDEV" --ei level "$LEVEL"
    # Give the synchronous prefs write a beat before force-stop.
    sleep 0.5
    say "QUICK DEV=$QUICKDEV level=$LEVEL (takes effect on restart)"
    RESTART=1
fi

# ---------------------------------------------------------------- start app
BOOT_MS=0
t_boot=$(date +%s%3N)
if [[ "$CAPTURE_ONLY" == 1 ]]; then
    proc_alive || { echo "quick-dev: app not running; --capture-only needs a live app" >&2; exit 5; }
elif [[ "$RESTART" == 1 || ! proc_alive ]]; then
    if [[ "$RESTART" == 1 ]]; then
        say "restart app"
        adb shell am force-stop "$PKG" >/dev/null 2>&1 || true
        sleep 0.4
    else
        say "start app"
    fi
    # Fresh buffer so the poll cannot match a status line from the old process,
    # and so the captured logcat is this launch (boot logs included).
    adb logcat -c >/dev/null 2>&1 || true
    adb shell am start -n "$ACT" >/dev/null 2>&1 || true
    wait_proc || { echo "quick-dev: app process did not start within 20 s" >&2; exit 4; }
else
    # Already running: drop old lines before polling for a fresh dump.
    adb logcat -c >/dev/null 2>&1 || true
fi

# ------------------------------------------------------------ level / state
WANT="$LEVEL"
if [[ "$WANT" != "-1" ]]; then
    if [[ "$CAPTURE_ONLY" != 1 ]]; then
        # Only warp when the app is not already in the requested level.
        cur="$(dump_status || true)"
        cur="${cur:+$(field level "$cur")}"
        if [[ "$cur" != "$WANT" ]]; then
            say "warp to level $WANT"
            bcast --es cmd level --ei n "$WANT"
        fi
    fi
fi
say "wait for level${WANT:+ $WANT}"
STATUS="$(wait_level "$WANT" "$TIMEOUT" || true)"
BOOT_MS=$(( $(date +%s%3N) - t_boot ))
[[ -n "$STATUS" ]] || STATUS="$LAST_LINE"
if [[ -z "$STATUS" ]]; then
    say "WARNING: level/state not reached within ${TIMEOUT}s; capturing anyway"
fi

if [[ "$CAPTURE_ONLY" != 1 ]]; then
    if [[ "$STEREO" != -1 ]]; then
        say "stereo=$STEREO"
        bcast --es cmd stereo --ei v "$STEREO"
    fi
    for group in "${FX_KEYS[@]}"; do
        for kv in $group; do
            k="${kv%%=*}"; v="${kv##*=}"
            [[ "$k" =~ ^[0-9]+$ && "$v" =~ ^-?[0-9]+$ ]] || {
                echo "quick-dev: bad --fx token: $kv" >&2; exit 2; }
            bcast --es cmd fx --ei k "$k" --ei v "$v"
        done
    done
    if [[ -n "$PTC_ACTION" ]]; then
        say "ptc $PTC_ACTION"
        bcast --es cmd ptc --es action "$PTC_ACTION"
    fi
fi

# ------------------------------------------------------------------ capture
[[ -n "$SETTLE" ]] && sleep "$SETTLE" || true
mkdir -p "$OUT"

# Final status: a fresh dump right before the screenshot.
STATUS="$(dump_status || printf '%s' "$STATUS")"
JAVA_DUMP="$(dump_java)"
SURFACE="$(field surfaceValid "$JAVA_DUMP")"
OBSERVED_LEVEL="$(field level "$STATUS")"
OBSERVED_AREA="$(field area "$STATUS")"
{
    printf '%s\n' "$JAVA_DUMP" | sed '/^$/d'
    printf '%s\n' "$STATUS"
} >"$OUT/status.txt"

if [[ "$NO_SHOT" != 1 ]]; then
    adb exec-out screencap -p >"$OUT/shot.png" 2>/dev/null || rm -f "$OUT/shot.png"
fi

# Filtered logcat: all SM64 tags, the requested extras and user patterns.
# Tags, not the package name: SurfaceFlinger logs every frame with
# "com.vdavid003.sm64port" in it, which used to drown the useful lines.
FILTER='SM64|AndroidRuntime|FATAL'
for t in "${EXTRA_TAGS[@]}"; do FILTER="$FILTER|$t"; done
[[ -n "$EXTRA_GREP" ]] && FILTER="$FILTER|$EXTRA_GREP"
adb logcat -d -v time -t 4000 2>/dev/null \
    | grep -E "$FILTER" | tail -n "$LOG_LINES" >"$OUT/logcat.txt" || true
# An empty capture is valid (quiet launch); keep the file, not an error.

END_MS=$(date +%s%3N)
TOTAL_MS=$(( END_MS - START_MS ))

{
    echo "git_head=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "git_branch=$(git -C "$ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
    echo "apk=$APK"
    echo "apk_stamp=$(apk_stamp)"
    echo "apk_mtime=$(stat -c '%y' "$APK" 2>/dev/null || echo missing)"
    echo "install=$DECISION install_ms=$INSTALL_MS"
    echo "level_requested=$LEVEL"
    echo "level_observed=$OBSERVED_LEVEL area=$OBSERVED_AREA lvl=$(field lvl "$STATUS") surfaceValid=$SURFACE"
    echo "stereo=$STEREO ptc=${PTC_ACTION:-none}"
    echo "fx=${FX_KEYS[*]:-none}"
    echo "quickdev_toggle=$QUICKDEV"
    echo "settle=$SETTLE timeout=$TIMEOUT"
    echo "lock_kind=${LOCK_KIND:-unlocked} mode=$([[ $CAPTURE_ONLY == 1 ]] && echo capture-only || echo full)"
    echo "boot_ms=$BOOT_MS total_ms=$TOTAL_MS"
    echo "started=$STAMP"
} >"$OUT/meta.txt"

# ---------------------------------------------------------------- summary
ms_fmt() { awk -v ms="$1" 'BEGIN { printf "%.1fs", ms/1000 }'; }
FPS="$(field fps "$STATUS")"
SLOWMO="$(field slowmo "$STATUS")"
RAYS="$(field rays "$STATUS")"
GLOW="$(field glow "$STATUS")"
OCCL="$(field occl "$STATUS")"
QD="$(field qd "$STATUS")"
ERR="$(grep -nE '\b(FATAL|ANR|AndroidRuntime|SIGSEGV|SIGABRT)\b' "$OUT/logcat.txt" 2>/dev/null | head -5 || true)"

echo
printf '== quick-dev %s (%s, install=%s %s) ==\n' \
    "$STAMP" "$(ms_fmt "$TOTAL_MS")" "$DECISION" \
    "$([[ $INSTALL_MS -gt 0 ]] && ms_fmt "$INSTALL_MS" || echo '0s')"
printf 'level=%s area=%s phase=%s mode=%s lvl=%s surfaceValid=%s\n' \
    "${OBSERVED_LEVEL:-?}" "${OBSERVED_AREA:-?}" \
    "$(field phase "$STATUS")" "$(field mode "$STATUS")" \
    "$(field lvl "$STATUS")" "${SURFACE:-?}"
printf 'fps=%s slowmo=%s rays=%s\n' "${FPS:-?}" "${SLOWMO:-?}" "${RAYS:-?}"
[[ -n "$GLOW" ]] && printf 'glow=%s\n' "$GLOW"
[[ -n "$OCCL" ]] && printf 'occl=%s\n' "$OCCL"
[[ -n "$QD" ]] && printf 'quickdev(qd)=%s (on,level,mute)\n' "$QD"
if [[ -n "$ERR" ]]; then
    echo 'errors:'
    sed 's/^/  /' <<<"$ERR"
else
    echo 'errors: none'
fi
[[ -n "$EXTRA_GREP" ]] && {
    echo "grep '$EXTRA_GREP':"
    grep -nE "$EXTRA_GREP" "$OUT/logcat.txt" | tail -8 | sed 's/^/  /' || true
}
printf 'artifacts: %s\n' "$OUT"
[[ "$NO_SHOT" != 1 && -s "$OUT/shot.png" ]] && printf '  shot.png  %s\n' "$(du -h "$OUT/shot.png" | cut -f1)"

if [[ "$WANT" != "-1" && "$OBSERVED_LEVEL" != "$WANT" ]]; then
    echo "quick-dev: FAILED to reach level $WANT (timeout ${TIMEOUT}s)" >&2
    exit 4
fi
if [[ "$CAPTURE_ONLY" != 1 && "$SURFACE" != "true" ]]; then
    echo "quick-dev: WARNING: surface not rendering (screen asleep?); screenshot may be black" >&2
fi
