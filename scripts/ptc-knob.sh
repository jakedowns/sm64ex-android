#!/usr/bin/env bash
# Set Portal Test Chamber knobs without rebuilding.
#
#   scripts/ptc-knob.sh                          # list current knobs
#   scripts/ptc-knob.sh scene.chart_z 1200       # one knob
#   scripts/ptc-knob.sh scene.chart_cols 24 scene.chart_rows 24
#   scripts/ptc-knob.sh --reload                 # re-read ptc.cfg
#
# Values are read back from `cmd dump`, so this never needs an APK rebuild.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export LD_LIBRARY_PATH=/home/ubuntu/.local/lib/android-tools:${LD_LIBRARY_PATH:-}
PKG=com.vdavid003.sm64port
ACTION=$PKG.CMD

# adb shell joins its arguments with spaces, so one quoted argument keeps the
# action string (which contains spaces) intact through the device shell.
send() {
  "$ROOT/scripts/agent-lock.sh" device --reason "ptc $1" -- \
    adb shell "am broadcast -n $PKG/.DebugReceiver -a $ACTION --es cmd ptc --es action '$1'" >/dev/null
}

dump_knobs() {
  "$ROOT/scripts/agent-lock.sh" device --reason "ptc knob dump" -- bash -c '
    export LD_LIBRARY_PATH=/home/ubuntu/.local/lib/android-tools:$LD_LIBRARY_PATH
    adb logcat -c
    adb shell "am broadcast -n com.vdavid003.sm64port/.DebugReceiver -a com.vdavid003.sm64port.CMD --es cmd dump" >/dev/null
    sleep 1
    adb logcat -d -s SM64Leia:I | grep -o "knobs .*" | tail -1
  '
}

case "${1:-}" in
  "")
    dump_knobs
    ;;
  --reload)
    send "reload"
    ;;
  *)
    if [ "$#" -lt 2 ] || [ $(( $# % 2 )) -ne 0 ]; then
      echo "usage: $0 [--reload | name value ...]" >&2
      exit 2
    fi
    while [ "$#" -ge 2 ]; do
      send "knob $1 $2"
      shift 2
    done
    dump_knobs
    ;;
esac
