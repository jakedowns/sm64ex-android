#!/usr/bin/env bash
# Goomba-through-the-portal demo: a deterministic Goomba walks into portal A
# while a mirrored copy walks out of portal B, so the crossing is seamless
# (no mid-air snap).  Captures a filmstrip at key moments.
#
#   scripts/portal-goomba-test.sh            # default filmstrip
#   scripts/portal-goomba-test.sh 0.4 1.6 2.6 3.4
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT=/tmp/opencode/goomba
TIMES=("$@")
if [ "${#TIMES[@]}" -eq 0 ]; then
  TIMES=(0.4 1.4 2.4 3.0 3.4 3.8)
fi
RUN=$(cat <<INNER
export LD_LIBRARY_PATH=/home/ubuntu/.local/lib/android-tools:\$LD_LIBRARY_PATH
B="am broadcast -n com.vdavid003.sm64port/.DebugReceiver -a com.vdavid003.sm64port.CMD"
/media/share/lp2-test/scripts/unlock.sh >/dev/null 2>&1
adb shell am force-stop com.vdavid003.sm64port >/dev/null 2>&1
adb shell am start -n com.vdavid003.sm64port/.sm64portActivity >/dev/null 2>&1
sleep 6
adb shell "\$B --es cmd level --ei n 37" >/dev/null
sleep 7
adb shell "\$B --es cmd ptc --es action goomba" >/dev/null
PREV=0
i=0
for t in ${TIMES[*]}; do
  sleep \$(awk "BEGIN{print \$t - \$PREV}")
  PREV=\$t
  adb exec-out screencap -p > "${OUT}-\$i.png"
  i=\$((i+1))
done
adb shell "\$B --es cmd dump" >/dev/null
sleep 1
adb logcat -d -s SM64Leia:I | grep -o "PTC level=.*" | tail -1 | cut -c1-220
INNER
)
AGENT_NO_SLEEP=1 "$ROOT/scripts/agent-lock.sh" device --reason "goomba portal demo" -- bash -lc "$RUN"
ls -1 "${OUT}"-*.png
