#!/usr/bin/env bash
# Tone mapping + colour grading rig (GRADE tab, fx 280-289).
#
# One device-lock hold does the whole matrix at a fixed camera:
#   1. install + boot level 38 with QUICK DEV, PTC annex shot, HUD off
#   2. sweep plan A with NO LUT file present (operators, analytic looks and the
#      missing-LUT fallback)
#   3. push a valid 256x16 stripe LUT, re-select look 6, capture it
#   4. push a wrong-size LUT, verify the rejection log line
#   5. average fps/ms for grading off vs each operator (repeated dumps)
#   6. remove the test LUTs so the next agent's captures are unaffected
#
#   AGENT_TABLET_MUTE=1 AGENT_NO_SLEEP=1 scripts/agent-lock.sh device \
#       --timeout 900 --reason "grading" -- scripts/grading-test.sh
#
# Artifacts: /media/share/tmp/artifacts/sm64/grading/<stamp>/
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG=com.vdavid003.sm64port
ACT=$PKG/.sm64portActivity
B="am broadcast -n $PKG/.DebugReceiver -a $PKG.CMD"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="/media/share/tmp/artifacts/sm64/grading/$STAMP"
LUT_SRC="/media/share/tmp/artifacts/sm64/grading/grade.png"
LUT_BAD="/media/share/tmp/artifacts/sm64/grading/grade_bad_dims.png"
LUT_DIR=/sdcard/Android/data/$PKG/files/luts
UNLOCK=/media/share/lp2-test/scripts/unlock.sh

[[ "${AGENT_DEVICE_LOCK_HELD:-}" == "1" ]] || {
    echo "grading-test: run me under scripts/agent-lock.sh device" >&2; exit 3; }
[[ -f "$LUT_SRC" && -f "$LUT_BAD" ]] || {
    echo "grading-test: missing $LUT_SRC or $LUT_BAD (run scripts/grade-lut.py)" >&2; exit 2; }

mkdir -p "$OUT"
say() { printf '[grading] %s\n' "$*"; }
setfx() { adb shell $B --es cmd fx --ei k "$1" --ei v "$2" >/dev/null; }
dump_state() {
    adb logcat -c >/dev/null 2>&1 || true
    adb shell $B --es cmd dump >/dev/null 2>&1 || true
    sleep 0.4
    adb logcat -d -s SM64Leia:V 2>/dev/null | grep 'PTC level=' | tail -1 || true
}
field() { grep -o "$1=[^ ]*" <<<"$2" | head -1 | cut -d= -f2-; }

# ---- device prep ---------------------------------------------------------
"$UNLOCK" >/dev/null 2>&1 || true
adb devices | grep -qw device || {
    echo "grading-test: no adb device (Wi-Fi adb drops when the tablet sleeps)" >&2
    exit 4
}
say "install"
adb install -r "$ROOT/app/build/outputs/apk/debug/app-debug.apk" >/dev/null 2>&1 || true
adb shell $B --es cmd quickdev --ei v 1 --ei level 38 --ei mute 1 >/dev/null 2>&1 || true
adb shell am force-stop "$PKG" >/dev/null 2>&1 || true
sleep 1
adb shell am start -n "$ACT" >/dev/null 2>&1 || true
for _ in $(seq 1 40); do
    [[ -n "$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r')" ]] && break
    sleep 1
done
say "wait for level"
for _ in $(seq 1 40); do
    st=$(dump_state)
    [[ -n "$(field lvl "$st")" ]] && [[ "$(field lvl "$st")" -ge 1 ]] && break
    sleep 1
done
adb shell $B --es cmd ptc --es action annex >/dev/null || true
adb shell $B --es cmd hud --ei v 0 >/dev/null || true
sleep 3

# ---- 1. sweep without any LUT -------------------------------------------
adb shell mkdir -p "$LUT_DIR" >/dev/null 2>&1 || true
adb shell rm -f "$LUT_DIR/grade.png" >/dev/null 2>&1 || true
say "sweep A (no LUT)"
adb logcat -c >/dev/null 2>&1 || true
"$ROOT/scripts/fx-sweep.sh" "$ROOT/scripts/plans/grading.sweep" \
    --out "$OUT/sweepA" --level "" --stereo 0 --extract grade \
    --extract rays >"$OUT/sweepA.log" 2>&1 || true
adb logcat -d -s SM64FX:V >"$OUT/lut_missing_logcat.txt" 2>&1 || true

# ---- 2. valid LUT --------------------------------------------------------
say "push valid LUT"
adb push "$LUT_SRC" "$LUT_DIR/grade.png" >/dev/null
adb logcat -c >/dev/null 2>&1 || true
setfx 280 1; setfx 281 2; setfx 284 6; sleep 3
adb exec-out screencap -p >"$OUT/lut.png"
adb logcat -d -s SM64FX:V >"$OUT/lut_load_logcat.txt" 2>&1 || true

# ---- 3. invalid LUT ------------------------------------------------------
say "push invalid LUT (rejection path)"
adb push "$LUT_BAD" "$LUT_DIR/grade.png" >/dev/null
adb logcat -c >/dev/null 2>&1 || true
setfx 284 6; sleep 3
adb exec-out screencap -p >"$OUT/lut_rejected.png"
adb logcat -d -s SM64FX:V >"$OUT/lut_reject_logcat.txt" 2>&1 || true

# ---- 4. cost: repeated fps dumps, grading off vs operators ---------------
say "measure cost"
{
    printf 'case\tfps_runs\tfps_avg\tms_avg\n'
    for spec in "off|280=0" \
                "linear|280=1 281=0 284=0" \
                "reinhard|280=1 281=1 284=0" \
                "aces|280=1 281=2 284=0" \
                "filmic|280=1 281=3 284=0" \
                "aces_cel|280=1 281=2 284=5" \
                "off2|280=0"; do
        name=${spec%%|*}; sets=${spec#*|}
        for kv in $sets; do setfx "${kv%%=*}" "${kv##*=}"; done
        sleep 2
        runs=""; sum=0; n=0
        for _ in $(seq 1 5); do
            st=$(dump_state)
            f=$(field fps "$st"); f=${f:-0}
            runs="${runs:+$runs,}$f"
            sum=$(awk -v a="$sum" -v b="$f" 'BEGIN{print a+b}')
            n=$((n+1))
            sleep 0.6
        done
        avg=$(awk -v s="$sum" -v n="$n" 'BEGIN{printf "%.1f", s/n}')
        ms=$(awk -v f="$avg" 'BEGIN{ if (f+0>0) printf "%.2f", 1000.0/f; else printf "-" }')
        printf '%s\t%s\t%s\t%s\n' "$name" "$runs" "$avg" "$ms"
    done
} | tee "$OUT/cost.tsv"

# ---- 5. leave the device as found ---------------------------------------
adb shell rm -f "$LUT_DIR/grade.png" >/dev/null 2>&1 || true
adb shell $B --es cmd hud --ei v 1 >/dev/null 2>&1 || true
setfx 280 0
say "done: $OUT"
ls -1 "$OUT"
