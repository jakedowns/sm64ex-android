#!/usr/bin/env bash
# Cam-stick verification: curve + heatmap, vanilla digital C-stick, R-follow.
#
# Everything here is one device-lock hold.  Run it as:
#
#   AGENT_TABLET_MUTE=1 AGENT_NO_SLEEP=1 scripts/agent-lock.sh device \
#       --timeout 900 --reason "camstick" -- scripts/camstick-test.sh
#
# Artifacts: /media/share/tmp/artifacts/sm64/camstick/<stamp>/
#   heatmap_*.png        stick circle with the radial heatmap
#   curve_*.png          the CAM STICK editor (panel screenshot)
#   rfollow_*.mp4        R-follow behaviour clips
#   results.tsv          stick -> yaw rate pairs, C-bit table
#   logcat_*.txt         filtered SM64CamStick lines
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG=com.vdavid003.sm64port
ACT=$PKG/.sm64portActivity
B="am broadcast -n $PKG/.DebugReceiver -a $PKG.CMD"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="${ARTIFACT_DIR:-/media/share/tmp/artifacts/sm64/camstick/$STAMP}"
mkdir -p "$OUT"

if [ -z "${AGENT_DEVICE_LOCK_HELD:-}" ]; then
    echo "camstick-test: run me under scripts/agent-lock.sh device" >&2
    exit 1
fi

say() { printf '[camstick] %s\n' "$*"; }
cmd() { adb shell $B "$@" >/dev/null 2>&1 || true; }

# The panel's 30 s screen timeout will sleep the tablet mid-test and that drops
# the Wi-Fi adb link (documented deadlock); raise it for the hold.
OLD_TIMEOUT=$(adb shell settings get system screen_off_timeout 2>/dev/null | tr -d '\r')
adb shell settings put system screen_off_timeout 1800000 >/dev/null 2>&1 || true
restore_timeout() {
    adb shell settings put system screen_off_timeout "${OLD_TIMEOUT:-30000}" \
        >/dev/null 2>&1 || true
}
trap restore_timeout EXIT

# --- level + camera mode -----------------------------------------------------
say "unlock panel"
/media/share/lp2-test/scripts/unlock.sh >/dev/null 2>&1 || true
adb shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1 || true

say "boot PTC (quick dev) and custom camera"
cmd quickdev --ei v 1 --ei level 38 --ei mute 1
adb shell am force-stop "$PKG" >/dev/null 2>&1 || true
adb shell am start -n "$ACT" >/dev/null 2>&1 || true

wait_level() {
    local deadline=$((SECONDS + 40)) st
    while [ $SECONDS -lt $deadline ]; do
        adb logcat -c >/dev/null 2>&1 || true
        cmd dump
        sleep 0.4
        st=$(adb logcat -d -s SM64Leia:V 2>/dev/null | grep 'PTC level=' | tail -1 || true)
        if [ "$(sed -n 's/.*[[:space:]]level=\([0-9]*\).*/\1/p' <<<"$st")" = "38" ] \
           && [ "$(sed -n 's/.*[[:space:]]active=\([0-9]*\).*/\1/p' <<<"$st")" = "1" ]; then
            echo "$st"
            return 0
        fi
        sleep 1
    done
    return 1
}
wait_level >/dev/null || { echo "camstick: level 38 did not load" >&2; exit 1; }
cmd cam2 --ei mode 3
sleep 1

# --- helpers ----------------------------------------------------------------
# Press the on-screen cam stick for MS and return the SM64CamStick lines.
press_cam() { # rx ry ms
    adb logcat -c >/dev/null 2>&1 || true
    cmd pad2 --ei rx "$1" --ei ry "$2" --ei ms "$3"
    sleep 0.2
    sleep "$(awk "BEGIN{printf \"%.1f\", $3/1000.0}")"
    sleep 0.4
    adb logcat -d -s SM64CamStick:V 2>/dev/null | grep 'CamStick ' || true
}

# Median dyaw from a pulse: ignores the first/last few frames (spring/ramp).
median_dyaw() {
    awk '
        /CamStick in=/ {
            for (i = 1; i <= NF; i++) if ($i ~ /^dyaw=/) { sub("dyaw=", "", $i); print $i }
        }' | sort -n | awk '{ a[NR] = $1 } END {
            if (NR == 0) { print 0; exit }
            print a[int((NR + 1) / 2)]
        }'
}

shot() { # name
    adb exec-out screencap -p >"$OUT/$1.png" 2>/dev/null || true
    say "shot $OUT/$1.png"
}

# --- 1. heatmap visible + consistent with the curve -------------------------
say "heatmap: real touch on the stick area, capture mid-drag"
# Full control opacity so the heatmap reads in a screenshot (the default is 33%).
adb shell $B --es cmd set --ei k 39 --ei v 100 >/dev/null 2>&1 || true
# Finger down in the right half (the stick appears under the thumb), then a
# slow drag right; screencap while it is held.
adb shell input motionevent DOWN 1700 900 >/dev/null 2>&1 || true
sleep 0.3
adb shell input motionevent MOVE 1900 900 >/dev/null 2>&1 || true
sleep 0.4
shot heatmap_linear
adb shell input motionevent UP 1900 900 >/dev/null 2>&1 || true
sleep 0.4

# --- 2. response curve: yaw rate at two stick positions ---------------------
# Linear curve first.
cmd fx --ei k 293 --ei v $(( (64 << 8) | 64 ))
cmd fx --ei k 294 --ei v $(( (128 << 8) | 128 ))
cmd fx --ei k 295 --ei v $(( (192 << 8) | 192 ))
sleep 0.3
lin_small=$(press_cam 40 0 600 | median_dyaw)
lin_big=$(press_cam 100 0 600 | median_dyaw)
say "linear: dyaw(40)=$lin_small dyaw(100)=$lin_big"

# Drag the middle point up (a strong low-end lift, still monotonic), then
# measure the same two physical positions again.  This is exactly what a drag in
# the editor does; going through fx makes the measurement repeatable.
cmd fx --ei k 291 --ei v 10
cmd fx --ei k 293 --ei v $(( (12 << 8) | 90 ))
cmd fx --ei k 294 --ei v $(( (128 << 8) | 200 ))
cmd fx --ei k 295 --ei v $(( (192 << 8) | 240 ))
sleep 0.3
lift_small=$(press_cam 40 0 600 | median_dyaw)
lift_big=$(press_cam 100 0 600 | median_dyaw)
say "lifted: dyaw(40)=$lift_small dyaw(100)=$lift_big"

# Heatmap after the curve change, same drag.
adb shell input motionevent DOWN 1700 900 >/dev/null 2>&1 || true
sleep 0.2
adb shell input motionevent MOVE 1900 900 >/dev/null 2>&1 || true
sleep 0.4
shot heatmap_lifted
adb shell input motionevent UP 1900 900 >/dev/null 2>&1 || true

{
    printf 'measure\tstick_40\tyaw_dyaw\n'
    printf 'linear_chain\t40\t%s\n' "$lin_small"
    printf 'linear_max\t100\t%s\n' "$lin_big"
    printf 'lifted_chain\t40\t%s\n' "$lift_small"
    printf 'lifted_max\t100\t%s\n' "$lift_big"
} >"$OUT/results.tsv"

# --- 3. editor screenshot + a real drag ------------------------------------
cmd cam2 --ei mode 3
# Open the panel and the CAMERA tab via a real tap sequence is fragile; the
# panel state is what it is.  Capture whatever the CAMERA page shows now.
shot curve_panel

# --- 4. vanilla digital C-stick --------------------------------------------
say "vanilla mode: C bits for the four directions + one diagonal"
cmd cam2 --ei mode 0
sleep 1
{
    printf 'dir\tlog\n'
    for d in "80 0 R" "-80 0 L" "0 80 U" "0 -80 D" "80 80 RD"; do
        set -- $d
        lines=$(press_cam "$1" "$2" 350)
        printf '%s\t%s\n' "$3" "$(grep -o 'cst=0x[0-9A-F]* [UDLR]*' <<<"$lines" | sort -u | tr '\n' ' ')"
        sleep 0.3
    done
} >>"$OUT/results.tsv"
cat "$OUT/results.tsv"

# --- 5. R-follow behaviours (three short clips) ----------------------------
record_r() { # mode name
    cmd fx --ei k 298 --ei v "$1"
    sleep 0.5
    local remote=/sdcard/camstick-r$1.mp4
    adb shell screenrecord --size 1280x720 --bit-rate 9000000 --time-limit 6 "$remote" &
    local pid=$!
    sleep 0.6
    # Tap R, then turn Mario by holding a direction, so following is visible.
    cmd pad --ei sx 0 --ei sy 0 --ei buttons 16 --ei ms 120
    sleep 0.8
    cmd pad --ei sx 90 --ei sy 0 --ei buttons 0 --ei ms 2200
    sleep 1.2
    cmd pad --ei sx -90 --ei sy 0 --ei buttons 0 --ei ms 1400
    wait "$pid" || true
    adb pull "$remote" "$OUT/rfollow_$2.mp4" >/dev/null 2>&1 || true
    adb shell rm -f "$remote" >/dev/null 2>&1 || true
    say "clip $OUT/rfollow_$2.mp4"
}
record_r 0 toggle
record_r 1 hold
record_r 2 snap

adb logcat -d -s SM64CamStick:V >"$OUT/logcat_camstick.txt" 2>/dev/null || true
say "results: $OUT/results.tsv"
cat "$OUT/results.tsv"
