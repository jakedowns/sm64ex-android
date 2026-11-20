#!/usr/bin/env bash
# Two-stage portal relay test.
#   scripts/portal-relay-test.sh color   # stage 1: solid green relay (UV/geometry)
#   scripts/portal-relay-test.sh scene   # stage 2: destination-scene relay
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="${1:-color}"
OUT="/tmp/opencode/portal-${STAGE}.png"

RUN=$(cat <<INNER
export LD_LIBRARY_PATH=/home/ubuntu/.local/lib/android-tools:\$LD_LIBRARY_PATH
/media/share/lp2-test/scripts/unlock.sh >/dev/null 2>&1
adb shell am force-stop com.vdavid003.sm64port >/dev/null 2>&1
adb shell am start -n com.vdavid003.sm64port/.sm64portActivity >/dev/null 2>&1
sleep 2
B="am broadcast -n com.vdavid003.sm64port/.DebugReceiver -a com.vdavid003.sm64port.CMD"
if [ "${STAGE}" = "empty" ]; then
  adb shell \$B --es cmd level --ei n 37 >/dev/null 2>&1
  sleep 4
  adb shell \$B --es cmd ptc --es action emptytest >/dev/null 2>&1
else
  adb shell \$B --es cmd level --ei n 38 >/dev/null 2>&1
  sleep 4
  adb shell \$B --es cmd ptc --es action portaltest >/dev/null 2>&1
fi
sleep 2
if [ "${STAGE}" = "scene" ]; then
  adb shell \$B --es cmd ptc --es action portalcolor >/dev/null 2>&1
  sleep 1
fi
adb exec-out screencap -p > "${OUT}"
INNER
)

"$ROOT/scripts/agent-lock.sh" device --reason "portal test ${STAGE}" -- bash -lc "$RUN"

if [ "$STAGE" = "scene" ]; then
  python3 "$ROOT/scripts/portal-relay-test.py" "$OUT"
else
  python3 "$ROOT/scripts/portal-relay-test.py" "$OUT" --solid-green
fi
