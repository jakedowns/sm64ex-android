#!/usr/bin/env bash
# Upload a recorded demo video to free, no-account hosts and print the links.
#
#   scripts/upload-video.sh test-artifacts/portal-chamber/<video>.mp4
#
# Prints:
#   direct=<url>   (uguu.se, verified by re-downloading and comparing size)
#   mirror=<url>   (gofile.io download page)
#
# catbox.moe is deliberately skipped: in this environment it accepts the POST
# but stores only the first 256 KB, producing a corrupt file.
set -euo pipefail

if [ $# -lt 1 ]; then
    echo "usage: $0 <video.mp4>" >&2
    exit 2
fi
FILE="$1"
[ -f "$FILE" ] || { echo "no such file: $FILE" >&2; exit 1; }

say() { printf '[upload] %s\n' "$*"; }
first_url() { grep -o 'https\?://[^[:space:]]*' | head -1; }
LOCAL_SIZE=$(stat -c %s "$FILE")
say "uploading $FILE ($(du -h "$FILE" | cut -f1))"

TMP=$(mktemp /tmp/opencode/upload-check.XXXXXX.mp4)
trap 'rm -f "$TMP"' EXIT
RC=1

# 1) uguu.se — free, no account, direct file URL.
if OUT=$(curl -sS --max-time 300 -F "files[]=@$FILE" \
        "https://uguu.se/upload?output=text" 2>/dev/null); then
    URL=$(first_url <<<"$OUT")
    if [ -n "$URL" ] && curl -sS --max-time 300 -o "$TMP" "$URL" 2>/dev/null \
        && [ "$(stat -c %s "$TMP")" = "$LOCAL_SIZE" ]; then
        echo "direct=$URL"
        RC=0
    else
        say "uguu.se link failed size verification"
    fi
fi

# 2) gofile.io — free, no account, longer-lived download page.
SRV=$(curl -sS --max-time 60 https://api.gofile.io/servers 2>/dev/null \
    | python3 -c 'import json,sys; print(json.load(sys.stdin)["data"]["servers"][0]["name"])' 2>/dev/null || true)
if [ -n "${SRV:-}" ]; then
    OUT=$(curl -sS --max-time 300 -F "file=@$FILE" \
        "https://$SRV.gofile.io/contents/uploadfile" 2>/dev/null || true)
    PAGE=$(python3 -c 'import json,sys; print(json.load(sys.stdin)["data"]["downloadPage"])' <<<"$OUT" 2>/dev/null || true)
    if [ -n "$PAGE" ]; then
        echo "mirror=$PAGE"
        RC=0
    fi
fi

if [ "$RC" -ne 0 ]; then
    say "upload failed"
fi
exit "$RC"
