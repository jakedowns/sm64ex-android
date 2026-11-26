#!/usr/bin/env bash
# Run a parameter sweep on the tablet in ONE device-lock session.
#
# Why: rebuilding to change a number, or taking the device lock once per
# measurement, is the slowest way to work. Every dial worth sweeping should be
# reachable at runtime (`cmd fx --ei k <key> --ei v <value>`), and a whole plan
# should run inside a single lock hold.
#
#   AGENT_TABLET_MUTE=1 scripts/agent-lock.sh device --reason "fx sweep" -- \
#       scripts/fx-sweep.sh plans/blur.sweep [--out DIR] [--level 38] [--extra "cmd=value"]
#
# Plan format (one step per line, '#' comments, fields separated by '|'):
#
#   name | fx settings            | settle | caption
#   base | 100=0                  | 2      | BASELINE
#   s100 | 100=1 101=100          | 2      | STRENGTH 100
#
# Settings are applied with the debug channel before the settle wait; every
# step then records the PTC status line (fps= and all key fields), takes a
# screenshot and appends one row to <out>/results.tsv.
#
# Output: <out>/results.tsv and <out>/<name>.png per step, defaults under
# /media/share/tmp/artifacts/sm64/sweeps/, never /tmp.
#
# Options:
#   --out DIR       artifact directory (default: /media/share/tmp/artifacts/sm64/sweeps/<stamp>)
#   --level N       PTC test level to load first (default: 38)
#   --stereo 0|1    stereo mode to set (default: 0, 2D)
#   --ptc ACTION    PTC action to run after the level loads (e.g. blurshow)
#   --extract FIELD extra status field to record (repeatable; fps and the whole line are always kept)
#   --install       install app/build/outputs/apk/debug/app-debug.apk first
#   --restart       force-stop and restart the app first
#   --no-settle     skip the per-step screenshots (measurement only)
#   --dry-run       print the resolved plan and exit without touching the device
#   --allow-unlocked run without the device lock (idle tablet only)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG=com.vdavid003.sm64port
ACT=$PKG/.sm64portActivity
B="am broadcast -n $PKG/.DebugReceiver -a $PKG.CMD"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="/media/share/tmp/artifacts/sm64/sweeps/$STAMP"
LEVEL=38
STEREO=0
PTC_ACTION=""
INSTALL=0
RESTART=0
SETTLE_SHOTS=1
DRY=0
ALLOW_UNLOCKED=0
EXTRACT=()

usage() { sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

PLAN=""
POSITIONAL=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out) OUT="$2"; shift 2 ;;
        --level) LEVEL="$2"; shift 2 ;;
        --stereo) STEREO="$2"; shift 2 ;;
        --ptc) PTC_ACTION="$2"; shift 2 ;;
        --extract) EXTRACT+=("$2"); shift 2 ;;
        --install) INSTALL=1; shift ;;
        --restart) RESTART=1; shift ;;
        --no-settle) SETTLE_SHOTS=0; shift ;;
        --dry-run) DRY=1; shift ;;
        --allow-unlocked) ALLOW_UNLOCKED=1; shift ;;
        -h|--help) usage 0 ;;
        --) shift; POSITIONAL+=("$@"); break ;;
        -*) echo "unknown option: $1" >&2; usage 2 ;;
        *) POSITIONAL+=("$1"); shift ;;
    esac
done

PLAN="${POSITIONAL[0]:-}"
[[ -n "$PLAN" ]] || { echo "missing plan file" >&2; usage 2; }
[[ -f "$PLAN" ]] || { echo "no such plan: $PLAN" >&2; exit 2; }

mapfile -t LINES < <(grep -vE '^\s*(#|$)' "$PLAN")
[[ ${#LINES[@]} -gt 0 ]] || { echo "plan has no steps" >&2; exit 2; }

if [[ "$DRY" == 1 ]]; then
    echo "plan:   $PLAN (${#LINES[@]} steps)"
    echo "out:    $OUT"
    echo "level:  $LEVEL   stereo: $STEREO   ptc: ${PTC_ACTION:-none}"
    echo "install: $INSTALL  restart: $RESTART  shots: $SETTLE_SHOTS"
    for line in "${LINES[@]}"; do
        IFS='|' read -r name settings settle caption <<<"$line"
        name="$(echo "$name" | xargs)"; settings="$(echo "${settings:-}" | xargs)"
        settle="$(echo "${settle:-2}" | xargs)"; caption="$(echo "${caption:-$name}" | xargs)"
        printf '  %-14s settle=%-4s fx=[%s] %s\n' "$name" "$settle" "$settings" "$caption"
    done
    exit 0
fi

# The caller must hold the device lock: several agents share one tablet, and a
# sweep that runs unlocked can collide with an install, a screen recording or a
# test another agent is watching. agent-lock.sh exports the marker.
if [[ "${AGENT_DEVICE_LOCK_HELD:-}" != "1" && "$ALLOW_UNLOCKED" != 1 ]]; then
    cat >&2 <<'LOCK'
fx-sweep: refusing to touch the device without the device lock.

  AGENT_TABLET_MUTE=1 scripts/agent-lock.sh device --reason "fx sweep" -- \
      scripts/fx-sweep.sh <plan> [options]

Override with --allow-unlocked only for a throwaway check on an idle tablet.
LOCK
    exit 3
fi

mkdir -p "$OUT"
say() { printf '[sweep] %s\n' "$*"; }
setfx() { adb shell $B --es cmd fx --ei k "$1" --ei v "$2" >/dev/null; }
dump_state() {
    adb logcat -c >/dev/null 2>&1 || true
    adb shell $B --es cmd dump >/dev/null 2>&1 || true
    sleep 0.35
    adb logcat -d -s SM64Leia:V 2>/dev/null | grep 'PTC level=' | tail -1 || true
}
field() { grep -o "$1=[^ ]*" <<<"$2" | head -1 | cut -d= -f2-; }

if [[ -d "$HOME/.local/lib/android-tools" ]]; then
    export LD_LIBRARY_PATH="$HOME/.local/lib/android-tools${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

say "artifacts: $OUT"
if [[ "$INSTALL" == 1 ]]; then
    [[ -f "$ROOT/app/build/outputs/apk/debug/app-debug.apk" ]] || {
        echo "no APK; build first" >&2; exit 2; }
    say "install"
    adb install -r "$ROOT/app/build/outputs/apk/debug/app-debug.apk" >/dev/null 2>&1 || true
fi
if [[ "$RESTART" == 1 ]]; then
    say "restart app"
    adb shell am force-stop "$PKG" >/dev/null 2>&1 || true
    sleep 1
    adb shell am start -n "$ACT" >/dev/null 2>&1 || true
    for _ in $(seq 1 25); do
        [[ -n "$(adb shell pidof "$PKG" 2>/dev/null | tr -d '\r')" ]] && break
        sleep 1
    done
    sleep 6
fi
if [[ -n "$LEVEL" ]]; then
    say "level $LEVEL"
    adb shell $B --es cmd level --ei n "$LEVEL" >/dev/null || true
    sleep 8
fi
adb shell $B --es cmd stereo --ei v "$STEREO" >/dev/null 2>&1 || true
if [[ -n "$PTC_ACTION" ]]; then
    say "ptc $PTC_ACTION"
    adb shell "$B --es cmd ptc --es action '$PTC_ACTION'" >/dev/null 2>&1 || true
    sleep 2
fi

printf 'step\tfps\tms\t%s\tstatus\n' "$(IFS=,; echo "${EXTRACT[*]:-extra}")" >"$OUT/results.tsv"

for line in "${LINES[@]}"; do
    IFS='|' read -r name settings settle caption <<<"$line"
    name="$(echo "$name" | xargs)"; settings="$(echo "${settings:-}" | xargs)"
    settle="$(echo "${settle:-2}" | xargs)"; caption="$(echo "${caption:-$name}" | xargs)"
    say "$name  $caption"
    adb shell "$B --es cmd ptc --es action 'note $caption'" >/dev/null 2>&1 || true
    for kv in $settings; do setfx "${kv%%=*}" "${kv##*=}"; done
    sleep "$settle"
    st=$(dump_state)
    # `field` returns 1 when the field is absent; without the guards a missing
    # field kills the whole plan under `set -e` (the assignment inherits it).
    fps=$(field fps "$st" || true); [[ -n "$fps" ]] || fps=0
    ms=$(awk -v f="$fps" 'BEGIN{ if (f+0 > 0) printf "%.2f", 1000.0/(f+0); else printf "-" }')
    extras=""
    for e in "${EXTRACT[@]}"; do extras="${extras:+$extras,}$(field "$e" "$st" || true)"; done
    [[ "$SETTLE_SHOTS" == 1 ]] && adb exec-out screencap -p >"$OUT/$name.png" || true
    printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$fps" "$ms" "${extras:--}" "${caption:-$name}" \
        | tee -a "$OUT/results.tsv"
done

say "done: $OUT/results.tsv"
column -t -s $'\t' "$OUT/results.tsv" | sed -n '1,40p'
