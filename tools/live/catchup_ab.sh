#!/bin/bash
# Longer catch-up A/B (>= 30 samples each) and a full-frame aim recording. Stops if the owner becomes
# active or PC A's game leaves the front (read-only checks, nothing is brought forward).
D="$(cd "$(dirname "$0")" && pwd)"; G="/e/Games/The Last of Us - Part II Remastered"
C='E:\Games\The Last of Us - Part II Remastered\tlou2-coop\cmd.txt'
b_cmd() { ssh -i ~/.ssh/tlou2_pcb -o BatchMode=yes sharn@192.168.0.238 "($1)>> \"$C\"" 2>/dev/null; }
a_cmd() { printf '%s\n' "$1" >> "$G/tlou2-coop/cmd.txt"; }
ok() { local s; s=$(powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$D/fg.ps1")"); echo "$s" | grep -q "fg: tlou-ii" && [ "$(echo "$s" | awk '{print $3}')" -gt 60000 ]; }
stats() { grep -a "clonedrift:" | sed 's/.*clonedrift: \([0-9.]*\) m.*/\1/' | sort -n | awk '{a[NR]=$1; s+=$1} END {if (NR) { k=int(NR*0.95+0.999); printf "%d samples, mean %.2f, p95 %.2f, max %.2f\n", NR, s/NR, a[k], a[NR] } else print "no samples"}'; }
ok || { echo "not running: game not in front or owner active"; exit 1; }
trap 'a_cmd "catchup on"; a_cmd "clonebuttons 1F0"' EXIT
for mode in off on; do
  a_cmd "catchup $mode"; m=$(wc -l < "$G/tlou2-coop.log")
  for seg in "0 32767" "32767 0" "0 -32767" "-32767 0" "23000 23000" "-23000 -23000"; do
    ok || { echo "stopped: owner active or game left the front"; exit 2; }
    b_cmd "echo stick l $seg 4000"; sleep 4.3
  done
  sleep 2; echo "catch-up $mode: $(tail -n +$m "$G/tlou2-coop.log" | stats)"
  tail -n +$m "$G/tlou2-coop.log" | grep -ac "drift.*snapped" | sed 's/^/  snaps: /'
done
ok || { echo "stopped before the aim recording"; exit 2; }
a_cmd "clonebuttons 23F0"; sleep 1
powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$D/rec.ps1")" -Out "$(cygpath -w "$TEMP")\aimwide.mp4" -Sec 12 >/dev/null & REC=$!
sleep 1; b_cmd "echo trigger l 255 8000"; sleep 2; b_cmd "echo stick r 20000 0 1500"; sleep 2.5; b_cmd "echo trigger lr 255 800"; sleep 2; b_cmd "echo trigger lr 255 800"
wait $REC
ffmpeg -v error -y -i "$TEMP/aimwide.mp4" -vf "fps=1,scale=640:-2,tile=4x3" -frames:v 1 "$TEMP/aimwide.png"; echo done
