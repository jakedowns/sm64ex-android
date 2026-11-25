#!/usr/bin/env bash
# Motion-blur parameter sweep, recorded on the blur showcase.
#
#   AGENT_TABLET_MUTE=1 scripts/agent-lock.sh device --reason "blur sweep" -- \
#       scripts/blur-sweep-test.sh
#
# 2D capture, 10 s camera orbit around Mario, slow-motion backflips, lights off.
# Each segment sets one dial to an extreme while the on-screen caption shows
# the live values and the debug note names the sweep.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG=com.vdavid003.sm64port
ACT=$PKG/.sm64portActivity
B="am broadcast -n $PKG/.DebugReceiver -a $PKG.CMD"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="${ARTIFACT_DIR:-$ROOT/test-artifacts/blursweep-$STAMP}"
mkdir -p "$OUT"

if [ -d "$HOME/.local/lib/android-tools" ]; then
    export LD_LIBRARY_PATH="$HOME/.local/lib/android-tools${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

say() { printf '[sweep] %s\n' "$*"; }
setfx() { adb shell $B --es cmd fx --ei k "$1" --ei v "$2" >/dev/null; }
note() { adb shell "$B --es cmd ptc --es action 'note $1'" >/dev/null 2>&1 || true; }
shot() { adb exec-out screencap -p >"$OUT/$1.png"; }
kv() { sed -n "s/.*[[:space:]]$2=\([^[:space:]]*\).*/\1/p" <<<"$1" | head -1; }
dump_state() {
    adb logcat -c >/dev/null 2>&1 || true
    adb shell $B --es cmd dump >/dev/null 2>&1 || true
    sleep 0.3
    adb logcat -d -s SM64Leia:V 2>/dev/null | grep 'PTC level=' | tail -1 || true
}

segment() {
    local name="$1"; local label="$2"; shift 2
    say "$label"
    note "$label"
    for kvp in "$@"; do
        setfx "${kvp%%=*}" "${kvp##*=}"
    done
    sleep 1.6
    # Capture the still at the fastest point of the orbit within this step.
    # Camera speed comes from two status dumps: the scripted orbit is the
    # dominant screen motion, so its speed is the blur magnitude.
    local best="0" v st st2 c1 c2
    for _ in 1 2 3; do
        sleep 0.3
        st=$(dump_state)
        st2=$(dump_state)
        c1=$(kv "$st" cam)
        c2=$(kv "$st2" cam)
        v=$(awk -v a="$c1" -v b="$c2" 'BEGIN{
            if (a == "" || b == "") { print "0"; exit }
            split(a, p, ","); split(b, q, ",")
            dx = p[1] - q[1]; dz = p[3] - q[3]
            printf "%.1f", sqrt(dx * dx + dz * dz)
        }')
        if awk -v a="$v" -v b="$best" 'BEGIN{exit !(a > b)}'; then
            best=$v
            shot "$name"
        fi
    done
    [ -f "$OUT/$name.png" ] || shot "$name"
    st=$(dump_state)
    echo "$st" >"$OUT/$name.state.txt"
    local fps ms
    fps=$(grep -o 'fps=[0-9.]*' <<<"$st" | cut -d= -f2)
    [ -z "$fps" ] && fps=$(kv "$st" rate | cut -d, -f3)
    ms=$(awk -v f="${fps:-0}" 'BEGIN{ if (f+0 > 0) printf "%.1f", 1000.0/(f+0); else printf "-" }')
    printf '%s\t%s\tms=%s\tfps=%s\tmotion=%s\targs=%s\n' \
        "$name" "$label" "$ms" "$fps" "$best" "${*:-none}" \
        | tee -a "$OUT/results.txt"
}

say "artifacts: $OUT"
adb shell am force-stop "$PKG" >/dev/null 2>&1 || true
sleep 1
adb shell am start -n "$ACT" >/dev/null 2>&1 || true
for _ in $(seq 1 25); do
    [ -n "$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r')" ] && break
    sleep 1
done
sleep 10
adb shell $B --es cmd level --ei n 38 >/dev/null || true
sleep 9
adb shell $B --es cmd stereo --ei v 0 >/dev/null 2>&1 || true
sleep 1
adb shell $B --es cmd ptc --es action blurshow >/dev/null 2>&1 || true
sleep 3
setfx 217 1
setfx 140 1
setfx 149 2

adb shell rm -f /sdcard/sweep.mp4
adb shell screenrecord --time-limit 150 --size 1600x1000 --bit-rate 16000000 /sdcard/sweep.mp4 &
SR=$!
trap 'kill $SR 2>/dev/null || true' EXIT
sleep 1

segment 01_baseline "BASELINE: BLUR OFF" 100=0
segment 02_s40 "SWEEP: STRENGTH 40"  100=1 101=40  142=8 141=360 143=128
segment 03_s100 "SWEEP: STRENGTH 100" 101=100
segment 04_s160 "SWEEP: STRENGTH 160" 101=160
segment 05_s220 "SWEEP: STRENGTH 220" 101=220
segment 06_s300 "SWEEP: STRENGTH 300" 101=300
segment 07_x1 "SWEEP: SUB-STEPS 1"  142=1  101=220 141=360
segment 08_x2 "SWEEP: SUB-STEPS 2"  142=2
segment 09_x4 "SWEEP: SUB-STEPS 4"  142=4
segment 10_x8 "SWEEP: SUB-STEPS 8"  142=8
segment 11_x16 "SWEEP: SUB-STEPS 16" 142=16
segment 12_sh90 "SWEEP: SHUTTER 90"  142=8 141=90
segment 13_sh180 "SWEEP: SHUTTER 180" 141=180
segment 14_sh270 "SWEEP: SHUTTER 270" 141=270
segment 15_sh360 "SWEEP: SHUTTER 360" 141=360
segment 16_px8 "SWEEP: MAX BLUR 8"  143=8
segment 17_px24 "SWEEP: MAX BLUR 24" 143=24
segment 18_px64 "SWEEP: MAX BLUR 64" 143=64
segment 19_px128 "SWEEP: MAX BLUR 128" 143=128
segment 20_heatmap "HEATMAP: VECTORS" 100=1 31=9 149=2

wait $SR || true
trap - EXIT
adb pull /sdcard/sweep.mp4 "$OUT/blursweep.mp4" >/dev/null 2>&1 || true
adb shell rm -f /sdcard/sweep.mp4
adb shell $B --es cmd ptc --es action blurshowstop >/dev/null 2>&1 || true
adb shell $B --es cmd stereo --ei v 1 >/dev/null 2>&1 || true
setfx 100 0; setfx 31 0

if command -v python3 >/dev/null 2>&1 && python3 -c 'import PIL' 2>/dev/null; then
    python3 "$ROOT/scripts/contact-sheet.py" "$OUT" "$OUT/contact_sheet.png" 5 || true
    say "sheet  $OUT/contact_sheet.png"
fi
say "video $OUT/blursweep.mp4"
say "table $OUT/results.txt"
column -t -s $'\t' "$OUT/results.txt" | sed -n '1,24p'
