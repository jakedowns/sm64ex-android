#!/usr/bin/env bash
# Motion-blur performance profile smoke test.
#
#   AGENT_TABLET_MUTE=1 scripts/agent-lock.sh device --reason "mb perf" -- \
#       scripts/motionblur-perf-test.sh
#
# Drives the PTC light loop (counter-rotating camera -> fast vectors) and walks
# through motion-blur profiles from off to quality, plus camera-only vs
# per-vertex vectors and the vector heatmap. Each segment saves a still + dump;
# results.txt holds the frame time so the profiles can be compared.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG=com.vdavid003.sm64port
ACT=$PKG/.sm64portActivity
B="am broadcast -n $PKG/.DebugReceiver -a $PKG.CMD"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="${ARTIFACT_DIR:-$ROOT/test-artifacts/motionblur-$STAMP}"
mkdir -p "$OUT"

if [ -d "$HOME/.local/lib/android-tools" ]; then
    export LD_LIBRARY_PATH="$HOME/.local/lib/android-tools${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

say() { printf '[mb] %s\n' "$*"; }
kv() { sed -n "s/.*[[:space:]]$2=\([^[:space:]]*\).*/\1/p" <<<"$1" | head -1; }
shot() { adb exec-out screencap -p >"$OUT/$1.png"; }
setfx() { adb shell $B --es cmd fx --ei k "$1" --ei v "$2" >/dev/null; }
dump_state() {
    adb logcat -c >/dev/null 2>&1 || true
    adb shell $B --es cmd dump >/dev/null 2>&1 || true
    sleep 0.3
    adb logcat -d -s SM64Leia:V 2>/dev/null | grep 'PTC level=' | tail -1 || true
}

# name | key=value pairs...
segment() {
    local name="$1"; shift
    say "segment $name"
    for kvp in "$@"; do
        setfx "${kvp%%=*}" "${kvp##*=}"
    done
    sleep 2.0
    local st; st=$(dump_state)
    echo "$st" >"$OUT/$name.state.txt"
    shot "$name"
    local rate dn
    rate=$(kv "$st" rate)
    dn=$(kv "$st" dn)
    local ema_us fps ms
    ema_us=$(cut -d, -f3 <<<"$rate")
    if [ -n "$ema_us" ] && [ "$ema_us" -gt 0 ] 2>/dev/null; then
        ms=$(awk -v u="$ema_us" 'BEGIN{printf "%.1f", u/1000.0}')
        fps=$(awk -v u="$ema_us" 'BEGIN{printf "%.0f", 1000000.0/u}')
    else
        ms="-"; fps="-"
    fi
    printf '%s\tms=%s\tfps=%s\tdn=%s\trate=%s\targs=%s\n' \
        "$name" "$ms" "$fps" "$dn" "$rate" "${*:-none}" | tee -a "$OUT/results.txt"
}

say "artifacts: $OUT"
adb shell am force-stop "$PKG" >/dev/null 2>&1 || true
adb shell am start -n "$ACT" >/dev/null 2>&1
sleep 10
adb shell $B --es cmd level --ei n 38 >/dev/null
sleep 9
adb shell $B --es cmd ptc --es action lightshow >/dev/null
sleep 2

# Captions carry PATH/DN/rate + MB strength/x/res and a VIEW VECTORS marker.
setfx 217 1
setfx 140 1
setfx 149 1
sleep 0.5

adb shell rm -f /sdcard/mb.mp4
adb shell screenrecord --time-limit 45 --size 1600x1000 --bit-rate 16000000 /sdcard/mb.mp4 &
SR=$!
sleep 1

segment 01_off            100=0 31=0
segment 02_cheap          100=1 101=60  142=1 144=8 145=2 146=0   143=12
segment 03_balanced       100=1 101=110 142=4 144=4 145=1 146=100 143=24
segment 04_quality        100=1 101=160 142=8 144=2 145=1 146=100 143=48 141=320
segment 05_camera_vectors 140=0
segment 06_vertex_vectors 140=1
segment 07_heatmap        100=0 31=9
segment 08_off_again      31=0 100=0

wait $SR || true
adb pull /sdcard/mb.mp4 "$OUT/motionblur.mp4" >/dev/null 2>&1 || true
adb shell rm -f /sdcard/mb.mp4
# Leave the game in a clean state for normal use.
setfx 100 0; setfx 31 0; setfx 140 1
adb shell $B --es cmd ptc --es action lightshowstop >/dev/null 2>&1 || true

say "video  $OUT/motionblur.mp4"
say "table  $OUT/results.txt"
column -t -s $'\t' "$OUT/results.txt" | sed -n '1,20p'
