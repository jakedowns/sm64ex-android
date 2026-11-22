#!/bin/bash
# Frame-by-frame portal-crossing capture (see TESTING.md, "Portal seamless
# crossing").
#
#   wall  : west-wall portal -> south-wall portal (Mario walks west)
#   back  : south-wall portal -> west-wall portal (walks south)
#   ceil  : N7 tower wall portal -> N6 ceiling portal (the ninja-chain pair)
#
# Run it through the device lock (do NOT let it take the lock itself):
#   AGENT_NO_SLEEP=1 scripts/agent-lock.sh device --reason "portal seam $m" -- \
#       scripts/portal-seam-capture.sh <mode> <tag>
#
# It installs the current APK, loads level 38, arms the deterministic walk,
# hard-pauses, then advances exactly one simulation tick per capture with
# `cmd step`, dumping Mario/camera state every tick and saving PNGs for the
# frames around the crossing into /tmp/opencode/pseam/.
set -u

MODE="${1:-wall}"
TAG="${2:-$MODE}"
APK=app/build/outputs/apk/debug/app-debug.apk
OUT=/tmp/opencode/pseam
FRAMES="${FRAMES:-140}"
SHOT_FROM="${SHOT_FROM:-70}"
SHOT_TO="${SHOT_TO:-130}"

case "$MODE" in
  wall) ACTION=portalwalk ;;
  back) ACTION=portalwalkback ;;
  ceil) ACTION=portalwalkceil ;;
  *) echo "unknown mode $MODE (wall|back|ceil)"; exit 2 ;;
esac

mkdir -p "$OUT/frames-$TAG"
rm -f "$OUT"/frames-"$TAG"/*.png
: > "$OUT/dump-$TAG.txt"

B="am broadcast -n com.vdavid003.sm64port/.DebugReceiver -a com.vdavid003.sm64port.CMD"

echo "== install + launch"
adb install -r "$APK" | tail -1
/media/share/lp2-test/scripts/unlock.sh >/dev/null 2>&1
adb shell am force-stop com.vdavid003.sm64port
adb shell am start -n com.vdavid003.sm64port/.sm64portActivity
sleep 10
echo "== level 38"
adb shell "$B --es cmd level --ei n 38"
sleep 8
adb shell "$B --es cmd interp --ei v 0"
if [ -n "${CAM_MODE:-}" ]; then
  # 0 = legacy 24-frame camera swoop, 1 = seamless yaw-rig handoff (default)
  adb shell "$B --es cmd set --ei k 66 --ei v $CAM_MODE"
fi

echo "== arm $ACTION and pause right after the command applies"
# Both broadcasts in one device shell: the ptc command needs one simulation
# tick to be applied; the walk itself waits 90 ticks, so the pause lands while
# Mario is still standing.
adb shell "$B --es cmd ptc --es action $ACTION; sleep 0.25; $B --es cmd pause --ei v 1"
sleep 0.5
adb shell logcat -c

echo "== stepping $FRAMES ticks (shots $SHOT_FROM..$SHOT_TO)"
for ((i = 1; i <= FRAMES; i++)); do
  adb shell "$B --es cmd step --ei n 1" >/dev/null
  sleep 0.12
  adb shell "$B --es cmd dump" >/dev/null
  sleep 0.14
  LINE=$(adb logcat -d -t 600 -s SM64Leia:I | grep "PTC level=" | tail -1)
  if [ -z "$LINE" ]; then LINE="MISSING"; fi
  echo "f=$i $LINE" >> "$OUT/dump-$TAG.txt"
  if [ "$i" -ge "$SHOT_FROM" ] && [ "$i" -le "$SHOT_TO" ]; then
    adb exec-out screencap -p > "$OUT/frames-$TAG/f$(printf %03d "$i").png"
  fi
done

echo "captured $TAG ($MODE) ${FRAMES} ticks -> $OUT/dump-$TAG.txt"
