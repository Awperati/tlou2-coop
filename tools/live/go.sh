#!/bin/bash
# Drive PC A's own player with the virtual pad, then save a frame: go.sh "<pad command>" <wait s> [name]
D="$(cd "$(dirname "$0")" && pwd)"; G="/e/Games/The Last of Us - Part II Remastered"
powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$D/fg.ps1")" | grep -q "fg: tlou-ii" || { echo "game not in front"; exit 1; }
printf '%s\n' "$1" >> "$G/tlou2-coop/cmd.txt"; sleep "${2:-3}"
powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$D/rec.ps1")" -Out "$(cygpath -w "$TEMP")\go.mp4" -Sec 1 >/dev/null
ffmpeg -v error -y -i "$TEMP/go.mp4" -frames:v 1 -vf scale=960:-2 "$TEMP/${3:-go}.png"
grep -a "player pos" "$G/tlou2-coop.log" | tail -1 | cut -c1-80
