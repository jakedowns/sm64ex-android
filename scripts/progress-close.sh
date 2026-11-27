#!/usr/bin/env bash
# Close progress entries from the parent session, e.g. a relay `*-build` entry
# whose command was taken over by a central build.
#
#   scripts/progress-close.sh ID [ID...] [--detail "text"] [--status done]
#
# Relay entries are owned by the command they wrap: when that command is killed
# or superseded, the entry lingers as "running" forever because nothing writes
# to it again. Closing it here keeps the tree honest.
set -euo pipefail

MCP="/media/share/progress-tool-linux-vm/mcp-server/index.js"
SESSION="${PROGRESS_SESSION_ID:-${OPENCODE_SESSION_ID:-}}"
DETAIL="closed by parent: superseded / already handled centrally"
STATUS="done"
IDS=()

while (($#)); do
    case "$1" in
        --detail) DETAIL="$2"; shift 2 ;;
        --status) STATUS="$2"; shift 2 ;;
        --session) SESSION="$2"; shift 2 ;;
        -h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --) shift; IDS+=("$@"); break ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *) IDS+=("$1"); shift ;;
    esac
done

[[ ${#IDS[@]} -gt 0 ]] || { echo "usage: $0 ID [ID...] [--detail TEXT]" >&2; exit 2; }
[[ -f "$MCP" ]] || { echo "progress server not found at $MCP" >&2; exit 2; }
command -v node >/dev/null 2>&1 || { echo "node not found" >&2; exit 2; }

for id in "${IDS[@]}"; do
    args=(emit --id "$id" --status "$STATUS" --detail "$DETAIL")
    [[ -n "$SESSION" ]] && args+=(--session "$SESSION")
    if node "$MCP" "${args[@]}" >/dev/null 2>&1; then
        printf 'closed %s (%s)\n' "$id" "$STATUS"
    else
        printf 'could not close %s\n' "$id" >&2
    fi
done
