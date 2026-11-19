#!/usr/bin/env bash
# Record the momentum-stacking portal smoke test at 60 FPS (30 Hz sim).
#
#   scripts/agent-lock.sh device --reason "record momentum demo" -- \
#       scripts/record-portal-momentum.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG=com.vdavid003.sm64port
ACT=$PKG/.sm64portActivity
B="am broadcast -n $PKG/.DebugReceiver -a $PKG.CMD"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="${ARTIFACT_DIR:-$ROOT/test-artifacts/portal-chamber}"
mkdir -p "$OUT"
VIDEO="$OUT/portal-momentum-stacking-60fps-$STAMP.mp4"
REMOTE=/sdcard/ptc-momentum-$STAMP.mp4

if [ -d "$HOME/.local/lib/android-tools" ]; then
    export LD_LIBRARY_PATH="$HOME/.local/lib/android-tools${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
say() { printf '[ptc-mom-video] %s\n' "$*"; }
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

adb shell $B --es cmd fps --ei v 1 >/dev/null
adb shell $B --es cmd hz --ei v 60 >/dev/null
sleep 2
adb shell $B --es cmd uncapped --ei v 0 >/dev/null
adb shell $B --es cmd level --ei n 38 >/dev/null
deadline=$((SECONDS + 25)); STATE=""
while [ $SECONDS -lt $deadline ]; do
    STATE=$(state)
    [ "$(kv "$STATE" level)" = "38" ] && break
    sleep 0.5
done
[ "$(kv "$STATE" level)" = "38" ] || { echo "Portal Test Chamber did not load" >&2; exit 1; }

adb shell $B --es cmd ptc --es action reset >/dev/null
sleep 1
adb shell screenrecord --size 1280x720 --bit-rate 12000000 --time-limit 10 "$REMOTE" &
REC_PID=$!
sleep 1
adb shell $B --es cmd ptc --es action momentum >/dev/null
say "recording momentum launch"

deadline=$((SECONDS + 12)); STATE=""
while [ $SECONDS -lt $deadline ]; do
    STATE=$(state)
    [ "$(kv "$STATE" phase)" = "5" ] && break
    [ "$(kv "$STATE" fail)" = "1" ] && break
    sleep 0.25
done
[ "$(kv "$STATE" phase)" = "5" ] || { echo "momentum run did not finish: $STATE" >&2; }
sleep 1
wait "$REC_PID" || true

adb pull "$REMOTE" "$VIDEO" >/dev/null
adb shell rm -f "$REMOTE" >/dev/null 2>&1 || true
adb shell $B --es cmd hz --ei v 120 >/dev/null
say "saved $VIDEO"

INFO=$(python3 "$ROOT/scripts/mp4-fps.py" "$VIDEO")
echo "$INFO" >"$OUT/last-momentum-video-info.txt"
say "$INFO"
FPS=$(sed -n 's/.*avg_fps=\([0-9.]*\).*/\1/p' <<<"$INFO")
if [ -n "$FPS" ] && awk "BEGIN { exit !($FPS >= 59.0 && $FPS <= 61.0) }"; then
    say "verified ~60 FPS video"
else
    say "ERROR: expected a 60 FPS recording, got '$FPS'"
    exit 1
fi
