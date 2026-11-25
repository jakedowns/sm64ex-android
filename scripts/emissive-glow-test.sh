#!/usr/bin/env bash
# Emissive materials + bloom smoke test and cost measurement.
#
#   AGENT_TABLET_MUTE=1 AGENT_NO_SLEEP=1 scripts/agent-lock.sh device \
#       --reason "emissive glow" -- bash -c 'scripts/emissive-glow-test.sh'
#
# Loads the PTC (level 38), parks Mario on the open floor west of the cube
# pyramid (`ptc annex`, chase camera settles clear of the stack), equips the
# jetpack and holds L so the nozzle flame is on screen for every capture.
#
# Stills:
#   a_baseline        emissive tags off, bloom off (reference frame)
#   b_tags            emissive tags on,  bloom off (cost of the emissive buffer)
#   c_bloom           bloom on at defaults
#   d_emissive_view   bloom off, debug view 268=1 (raw emissive buffer)
#   e_light_off       jetpack light off
#   f_light_on        jetpack light on
# and one `cmd dump` per segment (fps=) appended to results.txt.
#
# FLAT (2D) mode is used so a still is a single image, while the stereo/FX path
# (and therefore the bloom chain) still runs.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG=com.vdavid003.sm64port
ACT=$PKG/.sm64portActivity
B="am broadcast -n $PKG/.DebugReceiver -a $PKG.CMD"
OUT="${ARTIFACT_DIR:-/media/share/tmp/artifacts/sm64/emissive}"
mkdir -p "$OUT"

if [ -d "$HOME/.local/lib/android-tools" ]; then
    export LD_LIBRARY_PATH="$HOME/.local/lib/android-tools${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

say()  { printf '[glow] %s\n' "$*"; }
shot() { adb exec-out screencap -p >"$OUT/$1.png"; }
fx()   { adb shell $B --es cmd fx --ei k "$1" --ei v "$2" >/dev/null; }
setk() { adb shell $B --es cmd set --ei k "$1" --ei v "$2" >/dev/null 2>&1 || true; }
ptc()  { adb shell $B --es cmd ptc --es action "$1" >/dev/null 2>&1 || true; }

dump_state() {
    adb logcat -c >/dev/null 2>&1 || true
    adb shell $B --es cmd dump >/dev/null 2>&1 || true
    sleep 0.4
    adb logcat -d -s SM64Leia:V 2>/dev/null | grep 'PTC level=' | tail -1 || true
}

# Put Mario on the open floor with a full tank and the trigger held down.
# `jetshot` is a capture-only PTC action (ptc.c): it equips + refills the pack,
# places Mario west of the pyramid facing the chase camera, and holds L.
park() {
    ptc jetshot
    sleep 1.8            # camera settles, Mario already climbing
}
release() {
    ptc jetshotstop
    sleep 1.0
}

# name | dials are applied by the caller
segment() {
    local name="$1"
    say "segment $name"
    fx 268 0             # never inherit a debug view from the last segment
    park
    sleep 0.2
    shot "$name"
    local st; st=$(dump_state)
    echo "$st" >"$OUT/$name.state.txt"
    local fps glow
    fps=$(sed -n 's/.*[[:space:]]fps=\([0-9.]*\).*/\1/p' <<<"$st" | head -1)
    glow=$(sed -n 's/.*[[:space:]]glow=\([^[:space:]]*\).*/\1/p' <<<"$st" | head -1)
    local ms="-"
    [ -n "$fps" ] && ms=$(awk -v f="$fps" 'BEGIN{ if (f > 0) printf "%.2f", 1000.0 / f; else printf "-" }')
    printf '%s\tms=%s\tfps=%s\tglow=%s\n' "$name" "$ms" "$fps" "$glow" | tee -a "$OUT/results.txt"
    release
    sleep 1.4
}

say "artifacts: $OUT"
: >"$OUT/results.txt"
if [ "${SKIP_LAUNCH:-0}" != "1" ]; then
adb shell am force-stop "$PKG" >/dev/null 2>&1 || true
sleep 1
adb shell am start -n "$ACT" >/dev/null 2>&1 || true
for _ in $(seq 1 30); do
    [ -n "$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r')" ] && break
    sleep 1
done
sleep 20
adb shell input keyevent 224 >/dev/null 2>&1 || true
/media/share/lp2-test/scripts/unlock.sh >/dev/null 2>&1 || true
sleep 2
# Title/file-select needs a START press before a level command sticks.
adb shell $B --es cmd pad --ei buttons 4096 --ei ms 150 >/dev/null 2>&1 || true
sleep 4
# Load the PTC level and make sure it actually took.
in_level=0
for _ in $(seq 1 6); do
    adb shell $B --es cmd level --ei n 38 >/dev/null 2>&1 || true
    sleep 6
    if dump_state | grep -q 'level=38'; then in_level=1; break; fi
done
if [ "$in_level" != 1 ]; then
    say "ERROR: level 38 never loaded (still in menu?)"
    exit 1
fi
fi
# FLAT: eye 0 duplicated into both halves, stereo/FX path still active.
setk 21 1
setk 16 0
# Turn the frame-rate limiter off so fps= reports real frame time.
fx 200 0
sleep 1

# A: reference frame, nothing emissive, no bloom.
fx 269 0; fx 261 0; fx 268 0
segment a_baseline

# B: emissive tags on, bloom off (the cost of the emissive buffer alone).
fx 269 1; fx 261 0
segment b_tags

# C: bloom on at defaults (threshold 48, intensity 120, radius 3, 2 passes).
fx 261 1
segment c_bloom

# D: debug view of the raw emissive buffer, bloom off.
fx 261 0; fx 268 1
segment d_emissive_view
fx 268 0

# E/F: jetpack light off vs on, bloom off, everything else identical.
fx 271 0
segment e_light_off
fx 271 1
segment f_light_on

# G/H: isolate the bloom chain's own cost from "the FX pass had to run":
# MXAO on in both, so the +delta is threshold+blur+upsample only.
fx 1 1; fx 261 0
segment g_ao_no_bloom
fx 261 1
segment h_ao_bloom
fx 1 0; fx 261 0

say "done; results in $OUT/results.txt"
