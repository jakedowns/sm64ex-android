#!/usr/bin/env bash
# Grapple targeting debug view: stills, status fields, and the "does the shown
# point match where the hook actually attaches" comparison.
#
#   AGENT_TABLET_MUTE=1 AGENT_NO_SLEEP=1 scripts/agent-lock.sh device \
#       --timeout 900 --reason "grapple debug" -- \
#       scripts/grapple-debug-test.sh
#
# Loads the PTC (level 38) and drives `ptc gdbg1..4`, a fixed-camera rig in
# ptc.c that puts the aim ray on a known wall / at the sky / at the floor from
# far away.  Every still is a single 2D frame unless the step says otherwise.
#
# Stills (in $OUT):
#   a_wall_valid       aim at the annex corridor wall      -> green ATTACH
#   b_out_of_range     floor at ~750 units, fx 314 400      -> amber RANGE
#   c_sky_no_surface   camera pitched up, open chamber      -> red NO SURFACE
#   d_floor_valid      floor ~750 units below the camera    -> green ATTACH
#   e_before_fire      rig 4, view on, trigger up
#   f_after_fire       same frame after `ptc gdbgfire`: the rope's anchor must
#                      sit on the marker drawn in e_before_fire
#   g_3d_oblique       oblique camera, fan + marker read as one 3D object
#   h_stereo_panel     the same oblique view on the 3D panel (2D duplicates off)
#   i_eyeL / j_eyeR    per-eye buffers: the marker must shift between eyes
# plus one `cmd dump` per step in results.tsv (grp=, fps=).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG=com.vdavid003.sm64port
B="am broadcast -n $PKG/.DebugReceiver -a $PKG.CMD"
OUT="${ARTIFACT_DIR:-/media/share/tmp/artifacts/sm64/grapple-debug}"
mkdir -p "$OUT"

if [ -d "$HOME/.local/lib/android-tools" ]; then
    export LD_LIBRARY_PATH="$HOME/.local/lib/android-tools${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

say()  { printf '[gdbg] %s\n' "$*"; }
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

grp_field() {
    sed -n 's/.*[[:space:]]grp=\([^[:space:]]*\).*/\1/p' <<<"$1" | head -1
}

# name | rig | settle
step() {
    local name="$1" rig="$2" settle="${3:-1.6}"
    say "$name (rig $rig)"
    ptc "gdbg$rig"
    sleep "$settle"
    shot "$name"
    local st; st=$(dump_state)
    echo "$st" >"$OUT/$name.state.txt"
    local fps grp
    fps=$(sed -n 's/.*[[:space:]]fps=\([0-9.]*\).*/\1/p' <<<"$st" | head -1)
    grp=$(grp_field "$st")
    printf '%s\tfps=%s\tgrp=%s\n' "$name" "${fps:--}" "${grp:--}" | tee -a "$OUT/results.tsv"
}

say "artifacts: $OUT"
: >"$OUT/results.tsv"

if [ "${SKIP_LAUNCH:-0}" != "1" ]; then
    # install-if-changed + restart into the PTC with QUICK DEV on
    "$ROOT/scripts/quick-dev.sh" --level 38 --quickdev 1 --restart --settle 1.5 \
        --out "$OUT/launch" --quiet || { say "ERROR: launch failed"; exit 1; }
fi
adb shell input keyevent 224 >/dev/null 2>&1 || true
/media/share/lp2-test/scripts/unlock.sh >/dev/null 2>&1 || true
sleep 1

# 2D: one eye duplicated into both halves, so each PNG is one clean image.
setk 21 1
setk 16 0
fx 200 0                 # uncap the rate limiter so fps= is real

# View on, defaults, HUD line on (it carries the angles + point in the still).
fx 310 1
fx 311 80                # 8.0 deg half-angle
fx 312 8
fx 313 1
fx 314 3400
sleep 0.5

step a_wall_valid 1

# Same pose as d_floor_valid but with the range cut below the floor distance
# (~750 units): the view must turn amber and the marker must stay on the
# surface while the fan stops at the range limit.
fx 314 400
step b_out_of_range 3
fx 314 3400

step c_sky_no_surface 2
step d_floor_valid 3

# Fire comparison: long oblique floor rig, capture, then fire the real grapple.
ptc gdbg4
sleep 1.6
shot e_before_fire
before=$(dump_state); echo "$before" >"$OUT/e_before_fire.state.txt"
ptc gdbgfire
sleep 0.20               # ~6 ticks: the hook has flown, rope is live
shot f_after_fire
after=$(dump_state); echo "$after" >"$OUT/f_after_fire.state.txt"
printf 'e_before_fire\tgrp=%s\nf_after_fire\tgrp=%s\n' \
    "$(grp_field "$before")" "$(grp_field "$after")" >>"$OUT/results.tsv"
ptc gdbgstop
sleep 0.6

step g_3d_oblique 4

# Stereo: same oblique rig, panel screenshot then the per-eye buffers.
setk 21 0
sleep 1.2
ptc gdbg4
sleep 1.6
shot h_stereo_panel
ptc eyeL
sleep 1.0
shot i_eyeL
ptc eyeR
sleep 1.0
shot j_eyeR
ptc eyeOff
sleep 0.6

# Cost of the view: same pose, off then on.
ptc gdbg4
sleep 1.4
fx 310 0
off=$(dump_state)
fx 310 1
sleep 1.0
on=$(dump_state)
printf 'cost_view_off\tfps=%s\ncost_view_on\tfps=%s\n' \
    "$(sed -n 's/.*[[:space:]]fps=\([0-9.]*\).*/\1/p' <<<"$off" | head -1)" \
    "$(sed -n 's/.*[[:space:]]fps=\([0-9.]*\).*/\1/p' <<<"$on" | head -1)" \
    | tee -a "$OUT/results.tsv"

ptc gdbgoff
setk 21 1
fx 310 0
fx 313 0
say "done: $OUT"
column -t -s $'\t' "$OUT/results.tsv" 2>/dev/null || cat "$OUT/results.tsv"
