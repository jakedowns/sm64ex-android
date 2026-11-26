#!/usr/bin/env bash
# Capture the rippling-painting effect at 30 / 60 / 120 Hz and A/B the
# presentation-time interpolation (`fx 350`).
#
# The ripple mesh is regenerated on 30 Hz sim ticks and baked into the display
# list; with `fx 350 1` (default) gfx_patch_frame() rewrites the baked height and
# normal for every presented frame, with `fx 350 0` it steps at the sim rate.
#
# Run inside the device lock (the script refuses to run unlocked):
#
#   AGENT_TABLET_MUTE=1 AGENT_NO_SLEEP=1 scripts/agent-lock.sh device \
#       --timeout 900 --reason "paint ripple" -- scripts/paint-ripple-test.sh
#
# Options:
#   --matrix 0|1     0 = only the smooth takes, 1 = also the fx 350 0 A/B takes
#   --seconds N      clip length (default 4)
#   --skip-install   do not install the APK even if it is newer
#   --warp           also run the real-warp duration takes (ptc paintwarp)
#
# Output: $ARTIFACT_DIR (default /media/share/tmp/artifacts/sm64/paint-ripple/
# <stamp>/) with one MP4 per take, its frame/diff analysis, the raw SM64Paint
# telemetry and a summary.txt.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG=com.vdavid003.sm64port
ACT=$PKG/.sm64portActivity
B="am broadcast -n $PKG/.DebugReceiver -a $PKG.CMD"
APK=$ROOT/app/build/outputs/apk/debug/app-debug.apk
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="${ARTIFACT_DIR:-/media/share/tmp/artifacts/sm64/paint-ripple}/$STAMP"
MATRIX=1
SECONDS_CLIP=4
SKIP_INSTALL=0
DO_WARP=0

while [ $# -gt 0 ]; do
    case "$1" in
        --matrix) MATRIX="$2"; shift 2 ;;
        --seconds) SECONDS_CLIP="$2"; shift 2 ;;
        --skip-install) SKIP_INSTALL=1; shift ;;
        --warp) DO_WARP=1; shift ;;
        -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done

if [ "${AGENT_DEVICE_LOCK_HELD:-0}" != "1" ]; then
    echo "refusing to touch the tablet without the device lock:" >&2
    echo "  scripts/agent-lock.sh device --reason ... -- $0" >&2
    exit 2
fi

if [ -d "$HOME/.local/lib/android-tools" ]; then
    export LD_LIBRARY_PATH="$HOME/.local/lib/android-tools${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

say() { printf '[paint] %s\n' "$*"; }
kv() { sed -n "s/.*[[:space:]]$2=\\([^[:space:]]*\\).*/\\1/p" <<<"$1" | head -1; }

dump_state() {
    adb logcat -c >/dev/null 2>&1 || true
    adb shell $B --es cmd dump >/dev/null 2>&1 || true
    sleep 0.4
    adb logcat -d -s SM64Leia:V 2>/dev/null | grep 'PTC level=' | tail -1 || true
}

mkdir -p "$OUT"
say "output: $OUT"

# ---------------------------------------------------------------- install
if [ "$SKIP_INSTALL" != "1" ] && [ -f "$APK" ]; then
    WANT=$(stat -c '%Y:%s' "$APK")
    HAVE=$(adb shell dumpsys package "$PKG" 2>/dev/null | sed -n 's/.*lastUpdateTime=//p' | head -1)
    INSTALLED=$(adb shell "stat -c '%Y' /data/app/$PKG-*/base.apk 2>/dev/null" 2>/dev/null | head -1)
    if [ -z "$INSTALLED" ] || [ "$INSTALLED" -lt "$(stat -c '%Y' "$APK")" ]; then
        say "installing $APK"
        adb install -r -d "$APK" >/dev/null || adb install -r "$APK" >/dev/null
    else
        say "APK already installed ($WANT, package $HAVE)"
    fi
fi

# ------------------------------------------------------------------ start
adb shell input keyevent KEYCODE_WAKEUP >/dev/null 2>&1 || true
adb shell wm dismiss-keyguard >/dev/null 2>&1 || true
adb shell am force-stop "$PKG" >/dev/null 2>&1 || true
sleep 1
adb shell am start -n "$ACT" >/dev/null
say "waiting for boot"
deadline=$((SECONDS + 60))
STATE=""
while [ $SECONDS -lt $deadline ]; do
    STATE=$(dump_state)
    [ -n "$STATE" ] && break
    sleep 1
done
[ -n "$STATE" ] || { echo "app never reported a PTC status line" >&2; exit 1; }

# Castle Inside, area 1: the Bob painting lives there.
adb shell $B --es cmd level --ei n 16 >/dev/null
deadline=$((SECONDS + 45))
while [ $SECONDS -lt $deadline ]; do
    STATE=$(dump_state)
    [ "$(kv "$STATE" level)" = "16" ] && break
    sleep 0.5
done
[ "$(kv "$STATE" level)" = "16" ] || { echo "Castle Inside did not load: $STATE" >&2; exit 1; }
say "Castle Inside loaded"

adb shell $B --es cmd ptc --es action paintdump >/dev/null   # per-present telemetry on
adb shell $B --es cmd ptc --es action paint >/dev/null        # fixed camera + ripples
sleep 2

# ---------------------------------------------------------------- takes
take() { # take <tag> <fpsmode> <panel_hz> <interp>
    local tag=$1 mode=$2 hz=$3 interp=$4
    local remote=/sdcard/paint-$STAMP-$tag.mp4
    local dir="$OUT/$tag"
    mkdir -p "$dir"
    adb shell $B --es cmd ptc --es action paintoff >/dev/null
    adb shell $B --es cmd fx --ei k 350 --ei v "$interp" >/dev/null
    adb shell $B --es cmd fps --ei v "$mode" >/dev/null
    adb shell $B --es cmd hz --ei v "$hz" >/dev/null
    sleep 1.5
    # Start the recorder first, then arm the rig: the ripple is triggered on the
    # next tick, so the clip opens on a full-amplitude entry ripple instead of
    # wherever the previous decay happened to be.
    adb logcat -c >/dev/null 2>&1 || true
    adb shell screenrecord --time-limit "$SECONDS_CLIP" --bit-rate 16000000 "$remote" &
    local rec=$!
    sleep 1
    adb shell $B --es cmd ptc --es action paint >/dev/null
    wait "$rec" || true
    adb pull "$remote" "$dir/clip.mp4" >/dev/null
    adb shell rm -f "$remote" >/dev/null 2>&1 || true
    adb logcat -d -s SM64Paint >"$dir/telemetry.txt" 2>/dev/null || true
    adb logcat -d -s SM64Leia:V 2>/dev/null | grep 'PTC level=' | tail -1 >"$dir/status.txt" || true
    python3 "$ROOT/scripts/paint-ripple-analyze.py" "$dir/clip.mp4" --out "$dir" \
        --strip 10 >"$dir/analysis.txt" 2>&1 || echo "analysis failed for $tag" >>"$dir/analysis.txt"
    say "$(printf '%-12s %s' "$tag" "$(cat "$dir/analysis.txt")")"
}

take smooth-30   0 30  1
take smooth-60   1 60  1
take smooth-120  2 120 1
if [ "$MATRIX" = "1" ]; then
    take step-60    1 60  0
    take step-120   2 120 0
fi

# -------------------------------------------------------- warp durations
if [ "$DO_WARP" = "1" ]; then
    for interp in 1 0; do
        adb shell $B --es cmd ptc --es action paintoff >/dev/null
        adb shell $B --es cmd fx --ei k 350 --ei v "$interp" >/dev/null
        adb shell $B --es cmd fps --ei v 1 >/dev/null
        adb shell $B --es cmd level --ei n 16 >/dev/null
        deadline=$((SECONDS + 30))
        while [ $SECONDS -lt $deadline ]; do
            STATE=$(dump_state)
            [ "$(kv "$STATE" level)" = "16" ] && break
            sleep 0.5
        done
        sleep 1
        adb logcat -c >/dev/null 2>&1 || true
        adb shell $B --es cmd ptc --es action paintwarp >/dev/null
        sleep 4
        adb logcat -d -s SM64Paint >"$OUT/warp-interp$interp.txt" 2>/dev/null || true
        say "warp interp=$interp: $(grep -c 'RIG' "$OUT/warp-interp$interp.txt" || true) rig lines"
    done
fi

adb shell $B --es cmd ptc --es action paintoff >/dev/null
adb shell $B --es cmd ptc --es action paintdump >/dev/null   # telemetry back off
adb shell $B --es cmd fx --ei k 350 --ei v 1 >/dev/null
adb shell $B --es cmd fps --ei v 2 >/dev/null
adb shell $B --es cmd level --ei n 38 >/dev/null

{
    echo "paint ripple capture $STAMP"
    echo "apk: $(stat -c '%y %s' "$APK" 2>/dev/null || echo none)"
    for d in "$OUT"/*/; do
        [ -f "$d/analysis.txt" ] || continue
        printf '%-12s %s\n' "$(basename "$d")" "$(cat "$d/analysis.txt")"
    done
    for f in "$OUT"/warp-interp*.txt; do
        [ -f "$f" ] || continue
        echo "== $(basename "$f")"
        grep 'RIG' "$f" || true
    done
} | tee "$OUT/summary.txt"

say "done: $OUT"
