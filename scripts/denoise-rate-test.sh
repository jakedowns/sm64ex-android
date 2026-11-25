#!/usr/bin/env bash
# Rate-limit + denoiser A/B smoke test.
#
#   AGENT_TABLET_MUTE=1 scripts/agent-lock.sh device --reason "denoise test" -- \
#       scripts/denoise-rate-test.sh
#
# Uses the PTC light test scene so the result is deterministic, labels every
# segment with the on-screen captions (ptc 217), records the whole run and
# saves dumps + stills next to the video.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG=com.vdavid003.sm64port
ACT=$PKG/.sm64portActivity
B="am broadcast -n $PKG/.DebugReceiver -a $PKG.CMD"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="${ARTIFACT_DIR:-$ROOT/test-artifacts/denoise-$STAMP}"
mkdir -p "$OUT"

if [ -d "$HOME/.local/lib/android-tools" ]; then
    export LD_LIBRARY_PATH="$HOME/.local/lib/android-tools${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

say() { printf '[denoise] %s\n' "$*"; }
kv() { sed -n "s/.*[[:space:]]$2=\\([^[:space:]]*\\).*/\\1/p" <<<"$1" | head -1; }
shot() { adb exec-out screencap -p >"$OUT/$1.png"; say "still $1.png"; }
dump_state() {
    adb logcat -c >/dev/null 2>&1 || true
    adb shell $B --es cmd dump >/dev/null 2>&1 || true
    sleep 0.3
    adb logcat -d -s SM64Leia:V 2>/dev/null | grep 'PTC level=' | tail -1 || true
}
setfx() { adb shell $B --es cmd fx --ei k "$1" --ei v "$2" >/dev/null; }
segment() {
    local name="$1"; shift
    local kv
    say "segment $name"
    for kv in "$@"; do
        setfx "${kv%%=*}" "${kv##*=}"
    done
    sleep 4
    local st; st=$(dump_state)
    echo "$st" >"$OUT/$name.state.txt"
    adb shell $B --es cmd dump >/dev/null 2>&1 || true
    sleep 0.2
    shot "$name"
    printf '%s\tdn=%s\trate=%s\tfps=%s\n' "$name" "$(kv "$st" dn)" "$(kv "$st" rate)" "$(kv "$st" fps)" | tee -a "$OUT/results.txt"
}

say "artifacts: $OUT"
adb shell am force-stop "$PKG" >/dev/null 2>&1 || true
adb shell am start -n "$ACT" >/dev/null 2>&1
sleep 10
adb shell $B --es cmd level --ei n 38 >/dev/null
sleep 9
adb shell $B --es cmd pause --ei v 0 >/dev/null
adb shell $B --es cmd ptc --es action lighttest >/dev/null
sleep 1

# Deterministic default state.
setfx 210 0; setfx 211 0; setfx 201 60; setfx 200 1; setfx 202 0
setfx 217 1
sleep 0.5

adb shell rm -f /sdcard/denoise.mp4
adb shell screenrecord --time-limit 60 --size 1600x1000 --bit-rate 16000000 /sdcard/denoise.mp4 &
SR=$!
sleep 0.8

segment 01_main_off            210=0 211=0 201=60
segment 02_alt_off             210=1 211=0
segment 03_alt_i30_p1_f1_b1    210=1 211=1 212=30 213=1 215=1 216=1
segment 04_alt_i70_p2_f2_b2    212=70 213=2 215=2 216=2
segment 05_alt_i100_p3_f4_b4   212=100 213=3 215=4 216=4
segment 06_alt_rate30          201=30 200=1
# Reduced-resolution default: same intensity/passes at 1/1, 1/2 and 1/4 res.
segment 07_alt_r1_p2           200=1 201=60 212=70 213=2 215=2 216=2 220=1
segment 08_alt_r2_p2           220=2
segment 09_alt_r4_p2           220=4
segment 10_alt_r4_p3           213=3 216=1
segment 11_alt_r4_denoise_off  211=0

wait $SR || true
adb pull /sdcard/denoise.mp4 "$OUT/denoise.mp4" >/dev/null 2>&1 || true
adb shell rm -f /sdcard/denoise.mp4
# Restore the main path for normal use.
setfx 210 0; setfx 211 0; setfx 201 60
say "video $OUT/denoise.mp4"
say "results $OUT/results.txt"
