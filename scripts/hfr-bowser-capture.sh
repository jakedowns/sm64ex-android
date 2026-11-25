#!/usr/bin/env bash
# Frame-by-frame HFR scale capture for the held-Bowser tail spin.
#
#   scripts/agent-lock.sh device --reason "hfr bowser" -- \
#       scripts/hfr-bowser-capture.sh [--no-install]
#
# Assumes the shared device lock is already held (the caller wraps this in
# scripts/agent-lock.sh device). One boot, one spin rig, all phases, ~40 s.
# Writes under /media/share/tmp/artifacts/sm64/bowser-scale/<stamp>/:
#   phase-<n>-<name>.log   raw SM64HFR lines (one per presented frame)
#   phase-<n>-<name>.status PTC status line for that phase
#   still-*.png            deterministic alpha=0.5 / HFR-off screenshots
#   meta.txt               device, apk stamp, commands
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="/media/share/tmp/artifacts/sm64/bowser-scale/$STAMP"
APK="$ROOT/app/build/outputs/apk/debug/app-debug.apk"
PKG=com.vdavid003.sm64port
ACT="$PKG/.sm64portActivity"
B="am broadcast -n $PKG/.DebugReceiver -a $PKG.CMD"
NO_INSTALL="${1:-}"

export LD_LIBRARY_PATH="/home/ubuntu/.local/lib/android-tools${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
mkdir -p "$OUT"

log() { printf '[hfr-capture] %s\n' "$*"; }
bc() { adb shell $B "$@" >/dev/null 2>&1 || true; }

status() {
    adb shell $B --es cmd dump >/dev/null 2>&1 || true
    sleep 0.4
    adb logcat -d -v brief -s SM64Leia:I 2>/dev/null | grep 'PTC level=' | tail -1
}

wait_level() {
    local want="$1" i
    for i in $(seq 1 40); do
        if status | grep -q "PTC level=$want "; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

# Capture N seconds of SM64HFR into a phase log.
phase() {
    local name="$1" secs="$2"
    adb logcat -c >/dev/null 2>&1
    sleep "$secs"
    adb logcat -d -v time -s SM64HFR:I > "$OUT/phase-$name.log" 2>/dev/null || true
    adb shell $B --es cmd ptc --es action hfrdump >/dev/null 2>&1 || true
    sleep 0.3
    status > "$OUT/phase-$name.status" || true
    log "$name: $(grep -c 'SM64HFR' "$OUT/phase-$name.log" || true) lines"
}

shot() {
    local name="$1"
    adb exec-out screencap -p > "$OUT/still-$name.png"
    log "still $name"
}

/media/share/lp2-test/scripts/unlock.sh >/dev/null 2>&1 || true
sleep 0.5

if [[ "$NO_INSTALL" != "--no-install" ]]; then
    log "installing $APK"
    adb install -r "$APK" >/dev/null
fi

adb shell am force-stop "$PKG" >/dev/null 2>&1 || true
adb shell am start -n "$ACT" >/dev/null 2>&1
log "waiting for app"
for i in $(seq 1 40); do
    pidof_out=$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r' || true)
    [[ -n "$pidof_out" ]] && break
    sleep 0.5
done
sleep 3
adb logcat -c >/dev/null 2>&1

bc --es cmd level --ei n 38
wait_level 38 || { log "level 38 did not load"; exit 1; }
log "PTC level 38 up"

# 120 Hz, held Bowser, full-speed spin.
bc --es cmd fps --ei v 2
bc --es cmd ptc --es action bowser
sleep 1.5
bc --es cmd ptc --es action hfron
sleep 0.5

# 1) Legacy element-wise 4x4 lerp (the bug), 120 Hz.
bc --es cmd interp --ei v 1
bc --es cmd ptc --es action hfrslerpoff
sleep 0.3
phase "1-legacy-120" 4

# 2) Slerp/recompose fix, 120 Hz.
bc --es cmd ptc --es action hfrslerpon
sleep 0.3
phase "2-fixed-120" 4

# 3) Same bug at 60 Hz (only one interpolated frame per tick).
bc --es cmd fps --ei v 1
bc --es cmd ptc --es action hfrslerpoff
sleep 0.3
phase "3-legacy-60" 3

# 4) HFR off: both prev and cur are the same tick -> the scale is rock steady.
bc --es cmd fps --ei v 2
bc --es cmd interp --ei v 0
sleep 0.3
phase "4-hfroff" 3

# 5) Deterministic stills at alpha=0.5 (worst case of a tick), logged so the
# still can be tied to the measured body distortion.
bc --es cmd ptc --es action hfralpha500
bc --es cmd ptc --es action hfrslerpoff
sleep 0.6
shot "alpha500-legacy"
phase "5-alpha500-legacy" 2
bc --es cmd ptc --es action hfrslerpon
sleep 0.3
shot "alpha500-fixed"
phase "6-alpha500-fixed" 2
bc --es cmd ptc --es action hfralpha
bc --es cmd interp --ei v 0
sleep 0.3
shot "hfroff"
phase "7-hfroff" 1

# Tidy up the rig and leave the game normal.
bc --es cmd ptc --es action hfroff
bc --es cmd interp --ei v 1
bc --es cmd ptc --es action bowseroff
bc --es cmd ptc --es action hfrslerpon
bc --es cmd fps --ei v 2

{
    echo "stamp=$STAMP"
    echo "apk=$APK"
    echo "apk_mtime=$(stat -c %y "$APK" 2>/dev/null || echo unknown)"
    echo "device=$(adb shell getprop ro.product.model 2>/dev/null | tr -d '\r')"
    echo "build=$(adb shell getprop ro.build.display.id 2>/dev/null | tr -d '\r')"
    echo "phases=1-legacy-120 2-fixed-120 3-legacy-60 4-hfroff 5-alpha500-legacy 6-alpha500-fixed 7-hfroff"
} > "$OUT/meta.txt"

log "artifacts: $OUT"
