#!/usr/bin/env bash
# Record one continuous 60 FPS demonstration of the Portal Test Chamber run.
#
# The simulation stays at its normal fixed 30 Hz (rendering is 60 FPS, fps mode
# 1). The uncapped test mode is explicitly disabled so recording uses normal
# wall-clock gameplay timing.
#
#   scripts/agent-lock.sh device --reason "record portal demo" -- \
#       scripts/record-portal-chamber.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG=com.vdavid003.sm64port
ACT=$PKG/.sm64portActivity
B="am broadcast -n $PKG/.DebugReceiver -a $PKG.CMD"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="${ARTIFACT_DIR:-$ROOT/test-artifacts/portal-chamber}"
mkdir -p "$OUT"
VIDEO="$OUT/portal-test-chamber-traversal-60fps-$STAMP.mp4"
REMOTE=/sdcard/ptc-demo-$STAMP.mp4

if [ -d "$HOME/.local/lib/android-tools" ]; then
    export LD_LIBRARY_PATH="$HOME/.local/lib/android-tools${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

say() { printf '[ptc-video] %s\n' "$*"; }
kv() { sed -n "s/.*[[:space:]]$2=\\([^[:space:]]*\\).*/\\1/p" <<<"$1" | head -1; }

state() {
    adb logcat -c >/dev/null 2>&1 || true
    adb shell $B --es cmd dump >/dev/null 2>&1 || true
    sleep 0.35
    adb logcat -d -s SM64Leia:V 2>/dev/null | grep 'PTC level=' | tail -1 || true
}

say "video: $VIDEO"
adb shell am force-stop "$PKG" >/dev/null 2>&1 || true
adb shell input keyevent KEYCODE_HOME >/dev/null 2>&1 || true
sleep 1
adb shell am start -n "$ACT" >/dev/null
sleep 10
adb shell input keyevent KEYCODE_HOME >/dev/null 2>&1 || true
sleep 1
adb shell am start -n "$ACT" >/dev/null
sleep 4

adb shell $B --es cmd fps --ei v 1 >/dev/null       # 60 FPS render, 30 Hz sim
adb shell $B --es cmd hz --ei v 60 >/dev/null        # 60 Hz physical panel mode
sleep 2                                                # let the mode settle
adb shell $B --es cmd uncapped --ei v 0 >/dev/null   # normal gameplay timing
adb shell $B --es cmd level --ei n 38 >/dev/null

deadline=$((SECONDS + 25))
STATE=""
while [ $SECONDS -lt $deadline ]; do
    STATE=$(state)
    [ "$(kv "$STATE" level)" = "38" ] && break
    sleep 0.5
done
[ "$(kv "$STATE" level)" = "38" ] || { echo "Portal Test Chamber did not load" >&2; exit 1; }

adb shell $B --es cmd ptc --es action reset >/dev/null
sleep 1

# Record to the device first so adb pull gets a finalized MP4. 8 s is ample:
# the traversal is ~2 s, plus the visible setup pause.
adb shell screenrecord --size 1280x720 --bit-rate 12000000 --time-limit 8 "$REMOTE" &
REC_PID=$!
sleep 1
adb shell $B --es cmd ptc --es action run >/dev/null
say "recording traversal"

# Wait for PASS (or failure) while the recorder runs.
deadline=$((SECONDS + 10))
while [ $SECONDS -lt $deadline ]; do
    STATE=$(state)
    [ "$(kv "$STATE" phase)" = "5" ] && break
    [ "$(kv "$STATE" fail)" = "1" ] && break
    sleep 0.3
done
[ "$(kv "$STATE" phase)" = "5" ] || { echo "traversal did not finish: $STATE" >&2; }
sleep 1
wait "$REC_PID" || true

adb pull "$REMOTE" "$VIDEO" >/dev/null
adb shell rm -f "$REMOTE" >/dev/null 2>&1 || true
adb shell $B --es cmd hz --ei v 120 >/dev/null      # restore panel mode
say "saved $VIDEO"

INFO=$(python3 "$ROOT/scripts/mp4-fps.py" "$VIDEO")
echo "$INFO" >"$OUT/last-video-info.txt"
say "$INFO"
FPS=$(sed -n 's/.*avg_fps=\([0-9.]*\).*/\1/p' <<<"$INFO")
if [ -n "$FPS" ] && awk "BEGIN { exit !($FPS >= 59.0 && $FPS <= 61.0) }"; then
    say "verified ~60 FPS video"
else
    say "ERROR: expected a 60 FPS recording, got '$FPS'"
    exit 1
fi
