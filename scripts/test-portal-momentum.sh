#!/usr/bin/env bash
# Momentum-stacking portal smoke test.
#
# Mario drops from the high P7 platform into the floor portal, is launched out
# of the 45-degree ramp portal, and must reach the second debug star on P9.
#
#   scripts/agent-lock.sh device --reason "momentum smoke test" -- \
#       scripts/test-portal-momentum.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG=com.vdavid003.sm64port
ACT=$PKG/.sm64portActivity
B="am broadcast -n $PKG/.DebugReceiver -a $PKG.CMD"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="${ARTIFACT_DIR:-$ROOT/test-artifacts/portal-chamber/momentum-$STAMP}"
mkdir -p "$OUT"

if [ -d "$HOME/.local/lib/android-tools" ]; then
    export LD_LIBRARY_PATH="$HOME/.local/lib/android-tools${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

say() { printf '[ptc-mom] %s\n' "$*"; }
fail() {
    say "FAIL: $*"
    {
        echo "FAIL: $*"
        echo "--- last PTC line ---"
        echo "${STATE:-<none>}"
        adb shell $B --es cmd dump >/dev/null 2>&1 || true
        adb logcat -d -s SM64Leia:V | tail -100 || true
    } >"$OUT/failure.txt" 2>&1 || true
    adb shell screencap -p /sdcard/mom-failure.png >/dev/null 2>&1 || true
    adb pull /sdcard/mom-failure.png "$OUT/99_failure.png" >/dev/null 2>&1 || true
    exit 1
}
shot() {
    adb shell screencap -p "/sdcard/$1.png" >/dev/null 2>&1 || return 0
    adb pull "/sdcard/$1.png" "$OUT/$1.png" >/dev/null 2>&1 || true
    say "screenshot $1.png"
}
kv() { sed -n "s/.*[[:space:]]$2=\\([^[:space:]]*\\).*/\\1/p" <<<"$1" | head -1; }
reason() { sed -n 's/.*reason="\([^"]*\)".*/\1/p' <<<"$1" | head -1; }
dump_state() {
    adb logcat -c >/dev/null 2>&1 || true
    adb shell $B --es cmd dump >/dev/null 2>&1 || true
    sleep 0.35
    adb logcat -d -s SM64Leia:V 2>/dev/null | grep 'PTC level=' | tail -1 || true
}

say "artifacts: $OUT"
adb shell am force-stop "$PKG" >/dev/null 2>&1 || true
adb shell input keyevent KEYCODE_HOME >/dev/null 2>&1 || true
sleep 1
adb shell am start -n "$ACT" >/dev/null
sleep 10
adb shell input keyevent KEYCODE_HOME >/dev/null 2>&1 || true
sleep 1
adb shell am start -n "$ACT" >/dev/null
sleep 4
adb logcat -c >/dev/null 2>&1 || true

adb shell $B --es cmd fps --ei v 1 >/dev/null
adb shell $B --es cmd uncapped --ei v 0 >/dev/null
adb shell $B --es cmd level --ei n 38 >/dev/null
deadline=$((SECONDS + 25)); STATE=""
while [ $SECONDS -lt $deadline ]; do
    STATE=$(dump_state)
    [ "$(kv "$STATE" level)" = "38" ] && break
    sleep 0.5
done
[ "$(kv "$STATE" level)" = "38" ] || fail "Portal Test Chamber did not load"
shot 01_level_loaded

adb shell $B --es cmd ptc --es action reset >/dev/null
sleep 0.5
STATE=$(dump_state)
BEFORE_STARS=$(kv "$STATE" stars)
BEFORE_SAVE=$(kv "$STATE" save)
[ "$(kv "$STATE" star2)" = "0" ] || fail "second star did not reset"
shot 02_reset

adb shell $B --es cmd ptc --es action momentum >/dev/null
say "momentum drop started"
deadline=$((SECONDS + 12)); STATE=""
while [ $SECONDS -lt $deadline ]; do
    STATE=$(dump_state)
    [ "$(kv "$STATE" phase)" = "5" ] && break
    [ "$(kv "$STATE" fail)" = "1" ] && break
    sleep 0.2
done

for s in 03_drop 04_launch 05_target 06_star; do
    shot "$s"
    sleep 0.3
done

STATE=$(dump_state)
echo "$STATE" >"$OUT/final-state.txt"
[ -n "$STATE" ] || fail "no PTC state line"
[ "$(kv "$STATE" mode)" = "1" ] || fail "not in momentum mode"
[ "$(kv "$STATE" fail)" = "0" ] || fail "momentum test failed: $(reason "$STATE")"
[ "$(kv "$STATE" phase)" = "5" ] || fail "momentum phase did not finish"
[ "$(kv "$STATE" portals)" = "2" ] || fail "not both portals placed"
[ "$(kv "$STATE" shotA)" = "1" ] || fail "floor portal shot failed"
[ "$(kv "$STATE" shotB)" = "1" ] || fail "ramp portal shot failed"
[ "$(kv "$STATE" tele)" -ge 1 ] || fail "no portal teleport observed"
[ "$(kv "$STATE" src)" = "0" ] || fail "Mario did not enter the floor portal"
[ "$(kv "$STATE" dst)" = "1" ] || fail "Mario did not exit the ramp portal"
[ "$(kv "$STATE" onplat)" = "1" ] || fail "Mario did not reach the target platform"
[ "$(kv "$STATE" star2)" = "1" ] || fail "second debug star was not collected"

ENTRY=$(kv "$STATE" mom_enter)
EXIT=$(kv "$STATE" mom_exit)
RATIO=$(kv "$STATE" mom_ratio)
TURN=$(kv "$STATE" mom_turn)
HOLD=$(kv "$STATE" mom_hold)
HCOS=$(kv "$STATE" mom_hcos)
[ -n "$ENTRY" ] && awk "BEGIN { exit !($ENTRY >= 55.0) }" \
    || fail "entrance velocity below momentum threshold ($ENTRY < 55)"
[ -n "$EXIT" ] && awk "BEGIN { exit !($EXIT >= 55.0) }" \
    || fail "exit velocity below momentum threshold ($EXIT < 55)"
# A portal is an orthonormal transform: the mapped exit must keep the entry
# speed exactly and turn the vector by the pair's fixed angle.
[ -n "$RATIO" ] && awk "BEGIN { exit !($RATIO >= 0.999 && $RATIO <= 1.001) }" \
    || fail "portal changed the crossing speed (ratio $RATIO)"
[ -n "$TURN" ] && awk "BEGIN { exit !($TURN >= -0.72 && $TURN <= -0.70) }" \
    || fail "portal turn did not match the floor/45-degree pair (dot $TURN)"
# One physics tick later the airborne state must still be flying along the
# mapped exit, not have been rewritten to a standing/walking velocity.
[ -n "$HCOS" ] && awk "BEGIN { exit !($HCOS >= 0.99) }" \
    || fail "air state lost the exit direction (cos $HCOS)"
[ -n "$HOLD" ] && awk "BEGIN { exit !($HOLD >= 0.9 * $ENTRY) }" \
    || fail "exit speed lost on the next tick ($HOLD vs $ENTRY)"
[ "$(kv "$STATE" stars)" = "$BEFORE_STARS" ] || fail "real star count changed"
[ "$(kv "$STATE" save)" = "$BEFORE_SAVE" ] || fail "save star count changed"

say "PASS (artifacts: $OUT)"
say "$STATE"
exit 0
