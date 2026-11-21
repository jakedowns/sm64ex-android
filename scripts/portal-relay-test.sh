#!/usr/bin/env bash
#   scripts/portal-relay-test.sh pattern  # empty room + checker chart, L/R
#   scripts/portal-relay-test.sh scene    # PTC destination scene
#   scripts/portal-relay-test.sh color    # old solid-green geometry check
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="${1:-pattern}"
OUT="/tmp/opencode/portal-${STAGE}"
RUN=$(cat <<INNER
export LD_LIBRARY_PATH=/home/ubuntu/.local/lib/android-tools:\$LD_LIBRARY_PATH
B="am broadcast -n com.vdavid003.sm64port/.DebugReceiver -a com.vdavid003.sm64port.CMD"
/media/share/lp2-test/scripts/unlock.sh >/dev/null 2>&1
adb shell am force-stop com.vdavid003.sm64port >/dev/null 2>&1
adb shell am start -n com.vdavid003.sm64port/.sm64portActivity >/dev/null 2>&1
sleep 2
if [ "${STAGE}" = "pattern" ]; then
  adb shell \$B --es cmd level --ei n 37 >/dev/null 2>&1
  sleep 4
  adb shell \$B --es cmd ptc --es action emptytest >/dev/null 2>&1
elif [ "${STAGE}" = "color" ]; then
  adb shell \$B --es cmd level --ei n 38 >/dev/null 2>&1
  sleep 4
  adb shell \$B --es cmd ptc --es action portaltest >/dev/null 2>&1
else
  adb shell \$B --es cmd level --ei n 38 >/dev/null 2>&1
  sleep 4
  adb shell \$B --es cmd ptc --es action portaltest >/dev/null 2>&1
  adb shell \$B --es cmd ptc --es action portalcolor >/dev/null 2>&1
fi
sleep 2
adb exec-out screencap -p > "${OUT}-L.png"
if [ "${STAGE}" = "pattern" ]; then
  adb shell \$B --es cmd ptc --es action eyeL >/dev/null 2>&1
  sleep 0.5
  adb exec-out screencap -p > "${OUT}-L.png"
  adb shell \$B --es cmd ptc --es action eyeR >/dev/null 2>&1
  sleep 0.5
  adb exec-out screencap -p > "${OUT}-R.png"
  adb shell \$B --es cmd ptc --es action eyeOff >/dev/null 2>&1
fi
INNER
)
"$ROOT/scripts/agent-lock.sh" device --reason "portal test ${STAGE}" -- bash -lc "$RUN"
if [ "$STAGE" = "pattern" ]; then
  python3 "$ROOT/scripts/portal-relay-test.py" "${OUT}-L.png" --pattern --pair "${OUT}-R.png"
elif [ "$STAGE" = "color" ]; then
  python3 "$ROOT/scripts/portal-relay-test.py" "${OUT}-L.png" --solid-green
else
  python3 "$ROOT/scripts/portal-relay-test.py" "${OUT}-L.png"
fi
