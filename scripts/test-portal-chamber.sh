#!/usr/bin/env bash
# Automated Portal Test Chamber regression test.
#
# Prerequisite: the debug APK is installed and the device is reachable. Run it
# under the shared device lock:
#
#   scripts/agent-lock.sh device --reason "portal smoke test" -- \
#       scripts/test-portal-chamber.sh
#
# The script warps to level 38 (Portal Test Chamber), resets the debug star and
# portals, runs the in-game traversal, captures milestone screenshots and
# verifies the final state. All artifacts go to test-artifacts/portal-chamber/.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG=com.vdavid003.sm64port
ACT=$PKG/.sm64portActivity
B="am broadcast -n $PKG/.DebugReceiver -a $PKG.CMD"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="${ARTIFACT_DIR:-$ROOT/test-artifacts/portal-chamber/$STAMP}"
mkdir -p "$OUT"

# The host's adb build needs its private Android libraries on some machines.
if [ -d "$HOME/.local/lib/android-tools" ]; then
    export LD_LIBRARY_PATH="$HOME/.local/lib/android-tools${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

say() { printf '[ptc] %s\n' "$*"; }
fail() {
    say "FAIL: $*"
    {
        echo "FAIL: $*"
        echo "artifact: $OUT"
        echo "--- last PTC line ---"
        echo "${STATE:-<none>}"
        echo "--- dump ---"
        adb shell $B --es cmd dump >/dev/null 2>&1 || true
        adb logcat -d -s SM64Leia:V | tail -80 || true
    } >"$OUT/failure.txt" 2>&1 || true
    # Keep a final screenshot and a bounded logcat slice for diagnosis.
    adb shell screencap -p /sdcard/ptc-failure.png >/dev/null 2>&1 || true
    adb pull /sdcard/ptc-failure.png "$OUT/99_failure.png" >/dev/null 2>&1 || true
    adb logcat -d -t 400 >"$OUT/failure-logcat.txt" 2>&1 || true
    exit 1
}

shot() {
    local name="$1"
    adb shell screencap -p "/sdcard/$name.png" >/dev/null 2>&1 || return 0
    adb pull "/sdcard/$name.png" "$OUT/$name.png" >/dev/null 2>&1 || true
    say "screenshot $name.png"
}

dump_state() {
    adb logcat -c >/dev/null 2>&1 || true
    adb shell $B --es cmd dump >/dev/null 2>&1 || true
    sleep 0.35
    adb logcat -d -s SM64Leia:V 2>/dev/null | grep 'PTC level=' | tail -1 || true
}

kv() { # $1 = state line, $2 = key
    sed -n "s/.*[[:space:]]$2=\\([^[:space:]]*\\).*/\\1/p" <<<"$1" | head -1
}

reason() { # quoted free-text failure reason
    sed -n 's/.*reason="\([^"]*\)".*/\1/p' <<<"$1" | head -1
}

wait_phase() { # $1 = wanted phase, $2 = timeout seconds
    local want="$1" deadline=$((SECONDS + $2)) state phase
    while [ $SECONDS -lt $deadline ]; do
        state=$(dump_state)
        [ -n "$state" ] || { sleep 0.2; continue; }
        STATE="$state"
        phase=$(kv "$state" phase)
        if [ "$phase" = "$want" ]; then
            return 0
        fi
        if [ "$(kv "$state" fail)" = "1" ]; then
            fail "test reported failure: $(kv "$state" reason)"
        fi
        sleep 0.15
    done
    return 1
}

say "artifacts: $OUT"
adb shell am force-stop "$PKG" >/dev/null 2>&1 || true
adb shell input keyevent KEYCODE_HOME >/dev/null 2>&1 || true
sleep 1
adb shell am start -n "$ACT" >/dev/null
sleep 10
# The Leia surface occasionally needs a real pause/resume to re-bind after a
# cold launch or after a new APK install.
adb shell input keyevent KEYCODE_HOME >/dev/null 2>&1 || true
sleep 1
adb shell am start -n "$ACT" >/dev/null
sleep 4
adb logcat -c >/dev/null 2>&1 || true

# 60 Hz rendering is the normal comparison/video mode. The test itself always
# runs at a fixed 30 Hz simulation rate.
adb shell $B --es cmd fps --ei v 1 >/dev/null
adb shell $B --es cmd uncapped --ei v 0 >/dev/null
adb shell $B --es cmd level --ei n 38 >/dev/null
say "requested Portal Test Chamber"

deadline=$((SECONDS + 25))
STATE=""
while [ $SECONDS -lt $deadline ]; do
    STATE=$(dump_state)
    if [ "$(kv "$STATE" level)" = "38" ] && [ "$(kv "$STATE" area)" = "1" ]; then
        break
    fi
    sleep 0.5
done
[ "$(kv "$STATE" level)" = "38" ] || fail "Portal Test Chamber did not load"
say "level loaded"
shot 01_level_loaded

adb shell $B --es cmd ptc --es action reset >/dev/null
sleep 0.5
STATE=$(dump_state)
[ "$(kv "$STATE" star)" = "0" ] || fail "debug star did not reset"
[ "$(kv "$STATE" portals)" = "0" ] || fail "portals were not cleared"
BEFORE_STARS=$(kv "$STATE" stars)
BEFORE_SAVE=$(kv "$STATE" save)
say "reset: stars=$BEFORE_STARS save=$BEFORE_SAVE"
shot 02_reset

adb shell $B --es cmd ptc --es action run >/dev/null
say "traversal started"

# Placement window is intentionally visible for one second.
if wait_phase 1 6; then shot 03_portals_placed; fi
if wait_phase 2 8; then shot 04_walking; fi
if wait_phase 3 8; then shot 05_teleport; fi
if wait_phase 5 10; then shot 06_star_collected; fi

STATE=$(dump_state)
echo "$STATE" >"$OUT/final-state.txt"

[ -n "$STATE" ] || fail "no PTC state line"
[ "$(kv "$STATE" level)" = "38" ] || fail "not in level 38"
[ "$(kv "$STATE" area)" = "1" ] || fail "not in area 1"
[ "$(kv "$STATE" fail)" = "0" ] || fail "test failed: $(reason "$STATE")"
[ "$(kv "$STATE" phase)" = "5" ] || fail "phase is not DONE (got $(kv "$STATE" phase))"
[ "$(kv "$STATE" portals)" = "2" ] || fail "not both portals placed"
[ "$(kv "$STATE" shotA)" = "1" ] || fail "entry portal shot failed"
[ "$(kv "$STATE" shotB)" = "1" ] || fail "exit portal shot failed"
[ "$(kv "$STATE" tele)" -ge 1 ] || fail "no portal teleport observed"
[ "$(kv "$STATE" src)" = "0" ] || fail "Mario did not enter portal 0"
[ "$(kv "$STATE" dst)" = "1" ] || fail "Mario did not exit portal 1"
[ "$(kv "$STATE" onplat)" = "1" ] || fail "Mario is not on the upper platform"
[ "$(kv "$STATE" star)" = "1" ] || fail "debug star was not collected"
[ "$(kv "$STATE" stars)" = "$BEFORE_STARS" ] || fail "real star count changed"
[ "$(kv "$STATE" save)" = "$BEFORE_SAVE" ] || fail "save star count changed"
CAPPED_STATE="$STATE"

# ---- capped vs uncapped equivalence -------------------------------------
# The uncapped option only removes display pacing; the fixed 30 Hz tick and
# all physics are identical. Run the same scenario again and compare the final
# gameplay state, not wall-clock timing.
say "uncapped equivalence check"
adb shell $B --es cmd uncapped --ei v 1 >/dev/null
sleep 0.4
adb shell $B --es cmd ptc --es action reset >/dev/null
sleep 0.4
adb shell $B --es cmd ptc --es action run >/dev/null
sleep 1.5
UNCAPPED_STATE=$(dump_state)
echo "$UNCAPPED_STATE" >"$OUT/uncapped-final-state.txt"
adb shell $B --es cmd uncapped --ei v 0 >/dev/null
[ "$(kv "$UNCAPPED_STATE" phase)" = "5" ] || fail "uncapped run did not finish"
# The final animation action can differ by one idle/landing tick between the
# paced and uncapped runs; it is not a physics outcome.  Position, velocity,
# teleport, platform, star and save state must match exactly.
for key in mario vel tele src dst star onplat portals shotA shotB fail stars save; do
    a=$(kv "$CAPPED_STATE" "$key")
    b=$(kv "$UNCAPPED_STATE" "$key")
    [ "$a" = "$b" ] || fail "uncapped mismatch for $key: capped='$a' uncapped='$b'"
done
say "capped and uncapped final states match"

say "PASS (artifacts: $OUT)"
say "$STATE"
exit 0
