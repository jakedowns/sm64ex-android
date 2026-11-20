#!/usr/bin/env bash
# Capture the deterministic PTC portal-relay view (left eye, then right eye)
# and analyse it for a structured relay texture / distortion.
#
# Usage: scripts/portal-relay-test.sh [/tmp/opencode/portal]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-/tmp/opencode/portal}"
mkdir -p "$(dirname "$OUT")"

RUN=$(cat <<EOF
export LD_LIBRARY_PATH=/home/ubuntu/.local/lib/android-tools:\$LD_LIBRARY_PATH
B="am broadcast -n com.vdavid003.sm64port/.DebugReceiver -a com.vdavid003.sm64port.CMD"
adb shell \$B --es cmd level --ei n 38 >/dev/null 2>&1
sleep 4
adb shell \$B --es cmd ptc --es action portaltest >/dev/null 2>&1
sleep 2
adb exec-out screencap -p > "${OUT}-L.png"
adb shell \$B --es cmd ptc --es action eyeR >/dev/null 2>&1
sleep 1
adb exec-out screencap -p > "${OUT}-R.png"
adb shell \$B --es cmd ptc --es action eyeOff >/dev/null 2>&1
EOF
)

"$ROOT/scripts/agent-lock.sh" device --reason "portal relay capture" -- bash -lc "$RUN"

python3 "$ROOT/scripts/portal-relay-test.py" "${OUT}-L.png" --pair "${OUT}-R.png"
