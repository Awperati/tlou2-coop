#!/bin/bash
# Clone weapon/aim test: waits (read-only) for PC A's game in front, then PC B aims, looks up, fires.
D="$(cd "$(dirname "$0")" && pwd)"; G="/e/Games/The Last of Us - Part II Remastered"
C='E:\Games\The Last of Us - Part II Remastered\tlou2-coop\cmd.txt'
ssh_() { ssh -i ~/.ssh/tlou2_pcb -o BatchMode=yes sharn@192.168.0.238 "$1" 2>/dev/null; }
until powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$D/fg.ps1")" | grep -q "fg: tlou-ii"; do sleep 3; done
echo "focused at $(date +%T)"; n=$(wc -l < "$G/tlou2-coop.log")
printf 'clonebuttons 23F0\ngiveweapon 1 - 30\n' >> "$G/tlou2-coop/cmd.txt"; sleep 2
powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$D/rec.ps1")" -Out "$(cygpath -w "$TEMP")\aim3.mp4" -Sec 18 >/dev/null & REC=$!
sleep 1; ssh_ "echo trigger l 255 6000>> \"$C\""; sleep 2.5
ssh_ "echo stick r 0 20000 1000>> \"$C\""; sleep 2
ssh_ "echo stick r 20000 0 1000>> \"$C\""; sleep 2.5
ssh_ "echo trigger lr 255 1000>> \"$C\""; sleep 2.5; ssh_ "echo trigger lr 255 1000>> \"$C\""
wait $REC
printf 'clonebuttons 1F0\n' >> "$G/tlou2-coop/cmd.txt"
tail -n +$n "$G/tlou2-coop.log" | grep -a "cloneaim\|weapon\|shots\|clonebuttons" | cut -c1-150
tasklist | grep -c tlou-ii
ffmpeg -v error -y -i "$TEMP/aim3.mp4" -vf "fps=1.5,crop=iw/2:ih/2:iw/4:ih/6,scale=480:-2,tile=6x4" -frames:v 1 "$TEMP/aim3.png"; echo done
