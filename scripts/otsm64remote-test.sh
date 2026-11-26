#!/usr/bin/env bash
# otsm64remote evidence run: config in, level up, state out, one remote jump.
#
# Everything happens inside ONE device-lock hold. The script writes the bridge
# config, (re)starts the app into a level, waits for pushes to accumulate,
# prints the REMOTE status line, optionally fires the local jump test hook and
# captures a log + screenshot pair per step.
#
#   AGENT_TABLET_MUTE=1 scripts/agent-lock.sh device --reason "otsm64remote" -- \
#       scripts/otsm64remote-test.sh --url https://omitest.jakedowns.com/otsm64remote/ \
#           --token "$SM64_REMOTE_TOKEN" --level 38 --jump
#
#   # disabled run (proves nothing is sent):
#   AGENT_TABLET_MUTE=1 scripts/agent-lock.sh device --reason "otsm64remote off" -- \
#       scripts/otsm64remote-test.sh --disable --level 38
#
# Options:
#   --url URL          relay base (directory or .../state.php)
#   --token TOKEN      shared secret (sent as X-Sm64-Token; never logged)
#   --interval-ms N    push period (default 80)
#   --level N          level to boot (default 38, PTC)
#   --enable/--disable write enabled = true/false
#   --jump             fire `cmd rinput jump` after the push check
#   --push-cfg-only    only write the config file, do not restart/dump
#   --out DIR          artifact dir (default /media/share/tmp/artifacts/sm64/otsm64remote/<stamp>)
#   --dry-run          print the plan without touching the device
#
# Output: <out>/status.txt, <out>/shot.png, <out>/logcat.txt, one row per step
# in <out>/results.tsv. Never writes the token to any artifact.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG=com.vdavid003.sm64port
B="am broadcast -n $PKG/.DebugReceiver -a $PKG.CMD"
CFG_DIR="/sdcard/Android/data/$PKG/files"
STAMP=$(date +%Y%m%d-%H%M%S)
OUT="/media/share/tmp/artifacts/sm64/otsm64remote/$STAMP"
URL=""
TOKEN=""
INTERVAL=80
LEVEL=38
ENABLED=0
JUMP=0
PUSH_ONLY=0
DRY=0

while [ $# -gt 0 ]; do
    case "$1" in
        --url) URL="${2:-}"; shift 2 ;;
        --token) TOKEN="${2:-}"; shift 2 ;;
        --interval-ms) INTERVAL="${2:-80}"; shift 2 ;;
        --level) LEVEL="${2:-38}"; shift 2 ;;
        --enable) ENABLED=1; shift ;;
        --disable) ENABLED=0; shift ;;
        --jump) JUMP=1; shift ;;
        --push-cfg-only) PUSH_ONLY=1; shift ;;
        --out) OUT="${2:-$OUT}"; shift 2 ;;
        --dry-run) DRY=1; shift ;;
        -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

ENABLED_TXT=false
[ "$ENABLED" = 1 ] && ENABLED_TXT=true

PLAN=$(cat <<EOF
enabled = $ENABLED_TXT
url = $URL
token = $TOKEN
interval_ms = $INTERVAL
EOF
)

if [ "$DRY" = 1 ]; then
    echo "config -> $CFG_DIR/sm64remote.cfg"
    echo "$PLAN" | sed 's/^token = .*/token = <redacted>/'
    echo "level=$LEVEL jump=$JUMP out=$OUT"
    exit 0
fi

mkdir -p "$OUT"
adb shell mkdir -p "$CFG_DIR"
# The config is pushed through stdin so the token never lands in a host file
# or in the shell history of the device.
printf '%s\n' "$PLAN" | adb shell "cat > $CFG_DIR/sm64remote.cfg"
echo "config written: $CFG_DIR/sm64remote.cfg (enabled=$ENABLED_TXT interval=${INTERVAL}ms)"

if [ "$PUSH_ONLY" = 1 ]; then
    echo "push-cfg-only: done"
    exit 0
fi

adb logcat -c
adb shell am force-stop "$PKG"
adb shell "$B" --es cmd level --ei n "$LEVEL" >/dev/null
adb shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1 || \
    adb shell am start -n "$PKG/.sm64portActivity" >/dev/null

# Wait until the bridge is doing what the config asked: pushes accumulating
# when enabled, a plain "remote=off" when disabled.
ok=0
for _ in $(seq 1 60); do
    sleep 1
    adb shell "$B" --es cmd dump >/dev/null 2>&1 || true
    line=$(adb logcat -d -s SM64Leia:V | grep "REMOTE remote=" | tail -1 || true)
    if [ "$ENABLED" = 1 ]; then
        if echo "$line" | grep -qE "pushed=[1-9]"; then ok=1; break; fi
    else
        if echo "$line" | grep -q "remote=off"; then ok=1; break; fi
    fi
done
[ "$ok" = 1 ] || echo "WARN: bridge state not reached within 60 s" >&2
sleep 3

adb logcat -d -s SM64Remote:V SM64Leia:V SM64Cmd:V > "$OUT/logcat.txt" || true
grep -E "REMOTE |enabled|disabled|cmd jump" "$OUT/logcat.txt" \
    | sed 's/token=[^ ]*/token=<redacted>/' > "$OUT/status.txt" || true
adb exec-out screencap -p > "$OUT/shot.png"

printf 'step\tt\n' > "$OUT/results.tsv"
grep -E "REMOTE remote=" "$OUT/logcat.txt" | tail -1 \
    | awk -F'REMOTE ' '{print "push\t" $2}' >> "$OUT/results.tsv" || true

if [ "$JUMP" = 1 ]; then
    adb logcat -c
    adb shell "$B" --es cmd rinput --es input jump >/dev/null
    sleep 1
    adb logcat -d -s SM64Remote:V SM64Cmd:V > "$OUT/jump-logcat.txt" || true
    adb shell "$B" --es cmd remote >/dev/null
    sleep 1
    adb logcat -d -s SM64Leia:V >> "$OUT/jump-logcat.txt" || true
    adb exec-out screencap -p > "$OUT/shot-jump.png"
    grep -E "cmd jump|REMOTE " "$OUT/jump-logcat.txt" \
        | sed 's/token=[^ ]*/token=<redacted>/' >> "$OUT/status.txt" || true
    printf 'jump\t%s\n' "$(grep -c 'cmd jump' "$OUT/jump-logcat.txt" || true)" >> "$OUT/results.tsv"
fi

echo "--- status ---"
cat "$OUT/status.txt"
echo "--- artifacts: $OUT ---"
