#!/usr/bin/env bash
# Serialize access to shared Android resources between agents.
#
#   scripts/agent-lock.sh build        [--timeout SECONDS] [--reason TEXT] -- <command...>
#   scripts/agent-lock.sh device       [--timeout SECONDS] [--reason TEXT] -- <command...>
#   scripts/agent-lock.sh device-quick [--timeout SECONDS] [--reason TEXT] -- <command...>
#   scripts/agent-lock.sh status
#
# Lock scope:
#   build        - one lock for this repository, shared by ALL git worktrees/copies
#   device       - one lock per host, shared by ALL worktrees and projects using adb
#   device-quick - a SHARED lock for short read-only probes (dumpsys, logcat,
#                  screencap, pidof). It waits while a device holder has the
#                  tablet (exclusive), but several quick probes may run at once,
#                  so a 2 s status check never queues behind a 3 min recording.
#                  It does not mute, mark BUSY or sleep the tablet, and it will
#                  not wake a sleeping device: use it on an awake tablet
#
# Run this from the checkout/worktree you are working in. Metadata (branch,
# commit, worktree) is taken from the current working directory, not from where
# this script happens to live, so the main checkout's copy may be used from a
# feature-branch worktree.
#
# The lock is held for the lifetime of <command...>. If the command forks
# background processes they do NOT inherit the lock (fd 9 is closed in the
# child), so a stray logcat cannot block other agents after this script exits.
#
# Exit codes:
#   0   command ran (its own exit code is propagated)
#   2   usage error
#   75  timed out waiting for the lock (EX_TEMPFAIL: retry later)

set -euo pipefail

# Resolve the checkout being acted on from CWD, so invoking the main
# checkout's helper from a worktree still records and checks that worktree.
TARGET_ROOT="${AGENT_REPO_ROOT:-$(git -C "$PWD" rev-parse --show-toplevel 2>/dev/null || pwd)}"
if [[ -z "$TARGET_ROOT" ]]; then
    TARGET_ROOT="$PWD"
fi

# Locks must outlive any single worktree and be shared by every checkout of
# this repo on the host. They are therefore stored in host state, not in the
# repository or worktree.
LOCK_ROOT="${AGENT_LOCK_ROOT:-${AGENT_LOCK_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}/opencode}}"
PROJECT_KEY="${AGENT_PROJECT_KEY:-android-sm64}"
BUILD_LOCK="${AGENT_BUILD_LOCK:-$LOCK_ROOT/$PROJECT_KEY/build.lock}"
DEVICE_LOCK="${AGENT_DEVICE_LOCK:-$LOCK_ROOT/devices/android.lock}"
DEVICE_QUICK_LOCK="${AGENT_DEVICE_QUICK_LOCK:-$LOCK_ROOT/devices/android.quick.lock}"
# Resolve adb once: some non-interactive shells do not have ~/.local/bin on
# PATH, and every device helper here (and the power guard) needs it.
if [[ -z "${ADB:-}" ]]; then
    if command -v adb >/dev/null 2>&1; then
        ADB="$(command -v adb)"
    elif [[ -x /home/ubuntu/.local/bin/adb ]]; then
        ADB=/home/ubuntu/.local/bin/adb
    else
        ADB=adb
    fi
fi
export ADB

# Start the adb server BEFORE any lock is taken. The fork-server is long-lived,
# and a server spawned by a helper that runs while we hold the lock inherits the
# lock fd — the device lock then looks permanently held by `adb fork-server`
# (seen in practice: one orphaned server blocked the tablet for an hour).
if [[ -x "$ADB" || -n "$(command -v "$ADB" 2>/dev/null)" ]]; then
    "$ADB" start-server >/dev/null 2>&1 || true
fi

LEGACY_PROJECT_KEY="sm64ex-android"
LEGACY_BUILD_LOCK="$LOCK_ROOT/$LEGACY_PROJECT_KEY/build.lock"

usage() {
    cat <<'EOF'
Usage:
  scripts/agent-lock.sh build        [--timeout SECONDS] [--reason TEXT] -- <command...>
  scripts/agent-lock.sh device       [--timeout SECONDS] [--reason TEXT] -- <command...>
  scripts/agent-lock.sh device-quick [--timeout SECONDS] [--reason TEXT] -- <command...>
  scripts/agent-lock.sh status

The build lock is shared by all worktrees of this repository. The device lock
is shared by all worktrees and projects on this host. Run from the worktree you
are working in; metadata records that branch/worktree.

The lock is held for the lifetime of <command...>. If the command forks
background processes they do NOT inherit the lock (fd 9 is closed in the
child), so a stray logcat cannot block other agents after this script exits.

Environment:
  AGENT_LOCK_ROOT    base directory for lock files
                     (default: ${XDG_STATE_HOME:-$HOME/.local/state}/opencode)
  AGENT_BUILD_LOCK   full path override for the build lock
  AGENT_DEVICE_LOCK  full path override for the device lock
  AGENT_LOCK_EVENT_LOG
                     append-only JSONL event log; defaults to lock-events.log
                     next to the build/device lock files
  AGENT_PROJECT_KEY  per-repository directory name (default: android-sm64)
  AGENT_ID           name recorded in lock metadata
  AGENT_REPO_ROOT    override the checkout used for metadata/preflight

Acquire, release, and timeout events are appended to the event log. Logging is
best-effort and never affects whether the lock is acquired or released.

Exit codes:
  0   command ran (its own exit code is propagated)
  2   usage error
  75  timed out waiting for the lock (EX_TEMPFAIL: retry later)
EOF
}

die() {
    echo "agent-lock: $*" >&2
    exit 2
}

agent_name() {
    printf '%s' "${AGENT_ID:-${OPENCODE_SESSION_ID:-$(id -un)@$(hostname)}}"
}

# Project-specific device housekeeping: mark the app BUSY while the device lock
# is held, then clear it and sleep the tablet on release.
device_app_busy() {
    local value="$1"
    local adb_bin="${ADB:-adb}"
    export LD_LIBRARY_PATH="/home/ubuntu/.local/lib/android-tools${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    command -v "$adb_bin" 9>&- >/dev/null 2>&1 || return 0
    "$adb_bin" 9>&- shell pidof com.vdavid003.sm64port >/dev/null 2>&1 || return 0
    "$adb_bin" 9>&- shell am broadcast -n com.vdavid003.sm64port/.DebugReceiver \
        -a com.vdavid003.sm64port.CMD --es cmd busy --ei v "$value" >/dev/null 2>&1 || true
}

# --- power audit ----------------------------------------------------------
# The tablet is adb-over-Wi-Fi: anything that blanks, reboots, powers off or
# changes the screen timeout takes the link away from every other agent, and
# afterwards nobody can tell who did it. Every device hold therefore records
# the power state before and after, and the lock logs its own sleep action.
DEVICE_POWER_STATE() {
    printf '%s/devices/power-state.txt' "$LOCK_ROOT"
}

# "boot=<id> reason=<r> wake=<w> batt=<n>"
device_probe_power() {
    local adb_bin="${ADB:-adb}" boot reason wake batt
    command -v "$adb_bin" 9>&- >/dev/null 2>&1 || return 0
    boot=$("$adb_bin" 9>&- shell cat /proc/sys/kernel/random/boot_id 2>/dev/null | tr -d '\r')
    reason=$("$adb_bin" 9>&- shell getprop sys.boot.reason 2>/dev/null | tr -d '\r')
    wake=$("$adb_bin" 9>&- shell dumpsys power 2>/dev/null | sed -n 's/.*mWakefulness=\([A-Za-z]*\).*/\1/p' | head -1)
    batt=$("$adb_bin" 9>&- shell dumpsys battery 2>/dev/null | sed -n 's/^ *level: \([0-9]*\).*/\1/p' | head -1)
    printf 'boot=%s reason=%s wake=%s batt=%s' "${boot:-?}" "${reason:-?}" "${wake:-?}" "${batt:-?}"
}

# Screen timeout the hold leaves behind, and the value it was raised to.
device_timeout_raise() {
    local adb_bin="${ADB:-adb}" cur
    command -v "$adb_bin" 9>&- >/dev/null 2>&1 || return 0
    cur=$("$adb_bin" 9>&- shell settings get system screen_off_timeout 2>/dev/null | tr -d '\r')
    case "$cur" in ''|null) cur=30000 ;; esac
    printf '%s' "$cur" > "$(DEVICE_POWER_STATE).timeout"
    "$adb_bin" 9>&- shell settings put system screen_off_timeout 600000 >/dev/null 2>&1 || true
}

device_timeout_restore() {
    local adb_bin="${ADB:-adb}" cur
    command -v "$adb_bin" 9>&- >/dev/null 2>&1 || return 0
    cur=$(cat "$(DEVICE_POWER_STATE).timeout" 2>/dev/null || true)
    [[ -n "$cur" ]] || return 0
    "$adb_bin" 9>&- shell settings put system screen_off_timeout "$cur" >/dev/null 2>&1 || true
    rm -f "$(DEVICE_POWER_STATE).timeout"
}

device_sleep() {
    if [[ "${AGENT_NO_SLEEP:-0}" == "1" ]]; then
        log_event sleep_skipped device "$DEVICE_LOCK" "AGENT_NO_SLEEP=1" 0 0 0 null
        return 0
    fi
    # Do not sleep the panel while another agent is queued for it: the sleep
    # drops the Wi-Fi adb link and the next waiter cannot wake it.
    # Count other agents queued for the device lock. Exclude this script and its
    # parent shell, otherwise the pattern matches ourselves and never sleeps.
    if [[ "$(pgrep -f 'agent-lock\.sh (device|device-quick)' 2>/dev/null \
              | grep -vx -e "$$" -e "${PPID:-0}" | wc -l)" -gt 0 ]]; then
        log_event sleep_skipped device "$DEVICE_LOCK" "another agent is waiting" 0 0 0 null
        return 0
    fi
    local adb_bin="${ADB:-adb}"
    export LD_LIBRARY_PATH="/home/ubuntu/.local/lib/android-tools${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    command -v "$adb_bin" 9>&- >/dev/null 2>&1 || return 0
    if "$adb_bin" 9>&- shell dumpsys power 2>/dev/null | grep -q "mWakefulness=Awake"; then
        "$adb_bin" 9>&- shell input keyevent 26 >/dev/null 2>&1 || true
        log_event sleep_sent device "$DEVICE_LOCK" "screen off (keyevent 26)" 0 0 0 null
    fi
}

# Device housekeeping: the tablet must be silent for every adb session.  The
# caller gates this with AGENT_TABLET_MUTE=1 (the project rule); we mute every
# stream and verify, then print the result.  `media` is absent on this tablet,
# so use `cmd media_session`.  Alarm has a minimum index of 1 on this device.
device_mute() {
    local adb_bin="${ADB:-adb}"
    export LD_LIBRARY_PATH="/home/ubuntu/.local/lib/android-tools${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    command -v "$adb_bin" 9>&- >/dev/null 2>&1 || return 0
    if [[ "${AGENT_TABLET_MUTE:-0}" != "1" ]]; then
        echo "agent-lock: WARNING: AGENT_TABLET_MUTE is not 1; tablet audio not verified muted" >&2
        return 0
    fi
    "$adb_bin" 9>&- get-state >/dev/null 2>&1 || return 0
    for st in 0 1 2 3 5 6 7 8 9 10 11; do
        "$adb_bin" 9>&- shell cmd media_session volume --stream "$st" --set 0 >/dev/null 2>&1 || true
    done
    "$adb_bin" 9>&- shell cmd media_session volume --stream 4 --set 1 >/dev/null 2>&1 || true
    "$adb_bin" 9>&- shell settings put system volume_alarm 0 >/dev/null 2>&1 || true
    local music ring alarm
    music=$("$adb_bin" 9>&- shell cmd media_session volume --stream 3 --get 2>/dev/null | tr -d '\r' | sed -n 's/.*volume is \([0-9][0-9]*\).*/\1/p' | tail -1)
    ring=$("$adb_bin" 9>&- shell cmd media_session volume --stream 2 --get 2>/dev/null | tr -d '\r' | sed -n 's/.*volume is \([0-9][0-9]*\).*/\1/p' | tail -1)
    alarm=$("$adb_bin" 9>&- shell cmd media_session volume --stream 4 --get 2>/dev/null | tr -d '\r' | sed -n 's/.*volume is \([0-9][0-9]*\).*/\1/p' | tail -1)
    echo "agent-lock: mute music=$music ring=$ring alarm=$alarm" >&2
    if [[ "$music" != "0" || "$ring" != "0" ]]; then
        echo "agent-lock: ERROR: tablet is not muted (music=$music ring=$ring)" >&2
        return 1
    fi
    return 0
}

lock_file_for() {
    case "$1" in
        build)        printf '%s' "$BUILD_LOCK" ;;
        device)       printf '%s' "$DEVICE_LOCK" ;;
        device-quick) printf '%s' "$DEVICE_QUICK_LOCK" ;;
        *)      die "unknown lock kind: $1 (expected 'build', 'device' or 'device-quick')" ;;
    esac
}

default_timeout_for() {
    case "$1" in
        build)        printf '%s' "${AGENT_BUILD_LOCK_TIMEOUT:-3600}" ;;
        device)       printf '%s' "${AGENT_DEVICE_LOCK_TIMEOUT:-300}" ;;
        device-quick) printf '%s' "${AGENT_DEVICE_QUICK_LOCK_TIMEOUT:-60}" ;;
    esac
}

event_log_for() {
    if [[ -n "${AGENT_LOCK_EVENT_LOG:-}" ]]; then
        printf '%s' "$AGENT_LOCK_EVENT_LOG"
        return
    fi
    case "$1" in
        build)  printf '%s' "$(dirname -- "$BUILD_LOCK")/lock-events.log" ;;
        device) printf '%s' "$(dirname -- "$DEVICE_LOCK")/lock-events.log" ;;
    esac
}

utc_now() {
    date -u +%Y-%m-%dT%H:%M:%SZ
}

now_ms() {
    local t
    t="$(date +%s%3N 2>/dev/null || true)"
    if [[ "$t" =~ ^[0-9]+$ ]]; then
        printf '%s' "$t"
    else
        printf '%s000' "$(date +%s)"
    fi
}

json_escape() {
    local s="${1-}"
    s="${s//\\/\\\\}"
    s="${s//\"/\\\"}"
    s="${s//$'\n'/\\n}"
    s="${s//$'\r'/\\r}"
    s="${s//$'\t'/\\t}"
    printf '%s' "$s"
}

# Best-effort append-only JSONL audit record. Never fails the lock operation.
log_event() {
    local event="$1" kind="$2" lock_path="$3" event_reason="$4"
    local wait_timeout="$5" waited_ms="$6" hold_ms="$7" exit_code="$8"
    local log_file exit_json="null"
    log_file="$(event_log_for "$kind")"
    [[ "$exit_code" == "null" ]] || exit_json="$exit_code"
    mkdir -p -- "$(dirname -- "$log_file")" 2>/dev/null || true
    {
        printf '{"v":1,"ts":"%s","event":"%s","lock":"%s","lock_file":"%s","agent":"%s","pid":%s,"host":"%s","worktree":"%s","branch":"%s","head":"%s","reason":"%s","wait_timeout_s":%s,"waited_ms":%s,"hold_ms":%s,"exit":%s}\n' \
            "$(utc_now)" \
            "$(json_escape "$event")" \
            "$(json_escape "$kind")" \
            "$(json_escape "$lock_path")" \
            "$(json_escape "$(agent_name)")" \
            "$$" \
            "$(json_escape "$(hostname)")" \
            "$(json_escape "$TARGET_ROOT")" \
            "$(json_escape "$(git_branch)")" \
            "$(json_escape "$(git_head)")" \
            "$(json_escape "$event_reason")" \
            "$wait_timeout" "$waited_ms" "$hold_ms" "$exit_json" \
            >>"$log_file"
    } 2>/dev/null || true
}

git_branch() {
    git -C "$TARGET_ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || printf 'unknown'
}

git_head() {
    git -C "$TARGET_ROOT" rev-parse --short HEAD 2>/dev/null || printf 'unknown'
}

legacy_build_lock_is_held() {
    [[ "$PROJECT_KEY" != "$LEGACY_PROJECT_KEY" ]] || return 1
    [[ -e "$LEGACY_BUILD_LOCK" ]] || return 1
    ! flock -n "$LEGACY_BUILD_LOCK" true 2>/dev/null
}

warn_if_locks_are_checkout_local() {
    local resolved
    resolved="$(realpath -m -- "$1" 2>/dev/null || printf '%s' "$1")"
    case "$resolved" in
        "$TARGET_ROOT"/*)
            echo "agent-lock: warning: lock file is inside the current checkout:" >&2
            echo "  $resolved" >&2
            echo "  Worktrees/copies will not share this lock. Set AGENT_LOCK_ROOT" >&2
            echo "  to a host-level directory instead." >&2
            ;;
    esac
}

check_build_environment() {
    [[ "$1" == "build" ]] || return 0
    if [[ ! -f "$TARGET_ROOT/local.properties" ]] \
        && [[ -z "${ANDROID_HOME:-}" ]] \
        && [[ -z "${ANDROID_SDK_ROOT:-}" ]]; then
        echo "agent-lock: warning: no local.properties and no ANDROID_HOME/" >&2
        echo "  ANDROID_SDK_ROOT for $TARGET_ROOT; Gradle may not find the SDK." >&2
    fi
}

print_status() {
    local kind file
    mkdir -p "$(dirname -- "$BUILD_LOCK")" "$(dirname -- "$DEVICE_LOCK")"
    if legacy_build_lock_is_held; then
        echo "warning: legacy build lock is HELD by a pre-rename agent:" >&2
        echo "  $LEGACY_BUILD_LOCK" >&2
        echo "  Wait for it to finish; new builds gate on it first." >&2
    fi
    echo "lock root: $LOCK_ROOT"
    echo "worktree:  $TARGET_ROOT ($(git_branch) @ $(git_head))"
    for kind in build device device-quick; do
        file="$(lock_file_for "$kind")"
        if [[ ! -e "$file" ]]; then
            echo "$kind: free (never acquired)"
            echo "  $file"
        elif flock -n "$file" true 2>/dev/null; then
            echo "$kind: free (last used $(stat -c '%y' "$file" 2>/dev/null || echo unknown))"
            echo "  $file"
            sed 's/^/  /' "$file" 2>/dev/null || true
        else
            echo "$kind: HELD"
            echo "  $file"
            sed 's/^/  /' "$file" 2>/dev/null || true
        fi
        echo "  event log: $(event_log_for "$kind")"
    done
}

# ---- status is a standalone subcommand -------------------------------------
if [[ "${1:-}" == "status" ]]; then
    warn_if_locks_are_checkout_local "$BUILD_LOCK"
    warn_if_locks_are_checkout_local "$DEVICE_LOCK"
    print_status
    exit 0
fi

# ---- parse arguments -------------------------------------------------------
kind=""
timeout=""
reason=""
while (( $# )); do
    case "$1" in
        build|device|device-quick)
            [[ -z "$kind" ]] || die "lock kind specified twice"
            kind="$1"
            shift
            ;;
        --timeout)
            [[ $# -ge 2 ]] || die "--timeout needs a value"
            timeout="$2"
            shift 2
            ;;
        --reason)
            [[ $# -ge 2 ]] || die "--reason needs a value"
            reason="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        *)
            die "unexpected argument: $1"
            ;;
    esac
done

[[ -n "$kind" ]] || { usage >&2; exit 2; }
[[ $# -ge 1 ]] || die "no command given (use: ... -- <command...>)"
[[ -n "$timeout" ]] || timeout="$(default_timeout_for "$kind")"
[[ "$timeout" =~ ^[0-9]+$ ]] || die "--timeout must be a whole number of seconds"

lock_file="$(lock_file_for "$kind")"
mkdir -p "$(dirname -- "$lock_file")"

warn_if_locks_are_checkout_local "$lock_file"
check_build_environment "$kind"

# Open with >> so that waiting never truncates the current holder's metadata.
wait_start_ms="$(now_ms)"
waited_ms=0
hold_start_ms=0

# Migration gate: the project key was renamed from sm64ex-android to
# android-sm64. While the deprecated legacy lock file exists, acquire it for
# the whole build as well, so old-path and new-path agents cannot build
# concurrently. Once the legacy directory is removed, this gate is skipped.
if [[ "$kind" == "build" ]] \
    && [[ "$PROJECT_KEY" != "$LEGACY_PROJECT_KEY" ]] \
    && [[ -e "$LEGACY_BUILD_LOCK" ]]; then
    echo "agent-lock: using legacy build lock for compatibility:" >&2
    echo "  $LEGACY_BUILD_LOCK" >&2
    exec 8>>"$LEGACY_BUILD_LOCK"
    if ! flock -w "$timeout" 8; then
        waited_ms=$(( $(now_ms) - wait_start_ms ))
        log_event timeout "$kind" "$LEGACY_BUILD_LOCK" "${reason:-unspecified}" \
            "$timeout" "$waited_ms" 0 75
        echo "agent-lock: timed out waiting for the legacy build lock." >&2
        echo "Do not bypass the lock. Retry later or report the wait." >&2
        exit 75
    fi
fi

exec 9>>"$lock_file"
# device-quick takes a SHARED lock: several read-only probes may run together,
# but none may run while a `device` holder has the tablet.
flock_mode="-w"
[[ "$kind" == "device-quick" ]] && flock_mode="-s -w"
if ! flock $flock_mode "$timeout" 9; then
    waited_ms=$(( $(now_ms) - wait_start_ms ))
    log_event timeout "$kind" "$lock_file" "${reason:-unspecified}" \
        "$timeout" "$waited_ms" 0 75
    {
        echo "agent-lock: timed out after ${timeout}s waiting for the $kind lock"
        echo "lock file: $lock_file"
    } >&2
    if [[ -s "$lock_file" ]]; then
        echo "current holder:" >&2
        sed 's/^/  /' "$lock_file" >&2
    fi
    echo "Do not bypass the lock. Retry later or report the wait." >&2
    exit 75
fi

waited_ms=$(( $(now_ms) - wait_start_ms ))
hold_start_ms="$(now_ms)"

# We own the lock now; rewrite metadata (informational only).
{
    echo "kind=$kind"
    echo "agent=$(agent_name)"
    echo "pid=$$"
    echo "host=$(hostname)"
    echo "worktree=$TARGET_ROOT"
    echo "branch=$(git_branch)"
    echo "head=$(git_head)"
    echo "started=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "waited_for=${timeout}s_max"
    echo "reason=${reason:-unspecified}"
} >"$lock_file"

log_event acquire "$kind" "$lock_file" "${reason:-unspecified}" \
    "$timeout" "$waited_ms" 0 null

# Marker for child scripts so they can prove they are inside the lock (for
# example fx-sweep.sh refuses to drive the tablet unlocked).
if [[ "$kind" == "device" ]]; then
    export AGENT_DEVICE_LOCK_HELD=1
else
    export AGENT_BUILD_LOCK_HELD=1
fi
export AGENT_LOCK_KIND="$kind"

if [[ "$kind" == "device" ]]; then
    if ! device_mute; then
        echo "agent-lock: refusing to run with tablet audio unmuted" >&2
        exit 3
    fi
    device_app_busy 1

    # Power audit: attribute any reboot between holds to the previous holder,
    # warn on a low battery, and keep the panel awake for this hold.  It must
    # never fail the lock, so errexit is off for the whole block.
    set +e
    {
        probe=$(device_probe_power)
        if [[ -n "$probe" ]]; then
            prev=$(cat "$(DEVICE_POWER_STATE)" 2>/dev/null || true)
            prev_boot=$(sed -n 's/.*boot=\([^ ]*\).*/\1/p' <<<"$prev")
            cur_boot=$(sed -n 's/.*boot=\([^ ]*\).*/\1/p' <<<"$probe")
            if [[ -n "$prev_boot" && -n "$cur_boot" && "$prev_boot" != "$cur_boot" ]]; then
                prev_holder=$(sed -n 's/^last_agent=//p' "$LOCK_ROOT/devices/android.lock" 2>/dev/null)
                log_event rebooted device "$DEVICE_LOCK" \
                    "boot id changed; previous holder ${prev_holder:-unknown}; now $probe" \
                    0 0 0 null
            fi
            case "$probe" in
                *"batt="[0-9]*)
                    b=${probe##*batt=}
                    if [[ "$b" =~ ^[0-9]+$ ]] && (( b < 15 )); then
                        log_event low_battery device "$DEVICE_LOCK" "$probe" 0 0 0 null
                    fi
                    ;;
            esac
            printf '%s' "$probe" > "$(DEVICE_POWER_STATE)"
        fi
        device_timeout_raise
    }
    set -e
fi

release_note() {
    local rc=$?
    local hold_ms=$(( $(now_ms) - hold_start_ms ))
    if [[ "$kind" == "device" ]]; then
        set +e
        device_timeout_restore
        device_app_busy 0
        device_sleep
        printf '%s' "$(device_probe_power)" > "$(DEVICE_POWER_STATE)" 2>/dev/null
        set -e
    fi
    # device-quick deliberately leaves the tablet alone: no BUSY, no sleep.
    # fd 9 is still open here, so we still own the lock.
    {
        echo "released_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "last_kind=$kind"
        echo "last_agent=$(agent_name)"
        echo "last_worktree=$TARGET_ROOT"
        echo "last_branch=$(git_branch)"
        echo "last_head=$(git_head)"
        echo "last_exit=$rc"
        echo "last_hold_ms=$hold_ms"
        echo "last_waited_ms=$waited_ms"
    } >"$lock_file" 2>/dev/null || true
    log_event release "$kind" "$lock_file" "${reason:-unspecified}" \
        "$timeout" "$waited_ms" "$hold_ms" "$rc"
}
trap release_note EXIT

# Run the command with fd 9 closed in the child so forked/background
# processes cannot inherit and outlive the lock.
# Power guard: put a shim `adb` first in PATH for device holds so a reboot,
# poweroff, shutdown or screen-blank keyevent is logged and refused instead of
# silently taking the tablet away from every other agent. Whitelist a deliberate
# power action with AGENT_ALLOW_POWER=1.
guard_install() {
    local dir="$LOCK_ROOT/guard" real
    real="${ADB:-$(command -v adb 2>/dev/null)}"
    [[ -x "$real" || -n "$(command -v "$real" 2>/dev/null)" ]] || return 0
    mkdir -p "$dir" || return 0
    cat >"$dir/adb" <<'GUARD'
#!/usr/bin/env bash
# generated by agent-lock.sh: blocks power verbs unless AGENT_ALLOW_POWER=1
real=__REAL_ADB__
evlog=__EVENT_LOG__
args="$*"
case "$args" in
  *" reboot"*|*" poweroff"*|*" shutdown"*|*" svc power"*|*" keyevent 26"*|*" keyevent 223"*|*" keyevent 224"*)
    if [[ "${AGENT_ALLOW_POWER:-0}" == "1" ]]; then
      echo "adb-guard: ALLOWED power command (AGENT_ALLOW_POWER=1): adb $args" >&2
    else
      echo "adb-guard: REFUSED power command: adb $args" >&2
      printf '{"v":1,"ts":"%s","event":"power_blocked","lock":"device","agent":"%s","worktree":"%s","args":"%s"}\n' \
        "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "${AGENT_ID:-unknown}" "$PWD" "$args" >>"$evlog" 2>/dev/null
      exit 97
    fi
    ;;
esac
exec "$real" "$@"
GUARD
    python3 - "$dir/adb" "$real" "$(event_log_for device)" <<'PYSUB'
import sys
path, real, evlog = sys.argv[1], sys.argv[2], sys.argv[3]
text = open(path).read().replace("__REAL_ADB__", real).replace("__EVENT_LOG__", evlog)
open(path, "w").write(text)
PYSUB
    chmod +x "$dir/adb" 2>/dev/null || return 0
    PATH="$dir:$PATH"
    export PATH
}

if [[ "$kind" == "device" && "${AGENT_GUARD_POWER:-1}" == "1" ]]; then
    guard_install
fi

if "$@" 8>&- 9>&-; then
    rc=0
else
    rc=$?
fi

exit "$rc"
