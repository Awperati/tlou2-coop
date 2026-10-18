#!/bin/bash
# Checks 1-3 plus a prone hold: record PC A while PC B walks, crouches, goes prone and stands.
SP="$(cd "$(dirname "$0")" && pwd)"
FG="$(cygpath -w "$SP/fg.ps1")"; G="/e/Games/The Last of Us - Part II Remastered"
C='E:\Games\The Last of Us - Part II Remastered\tlou2-coop\cmd.txt'
pcb() { ssh -i ~/.ssh/tlou2_pcb -o BatchMode=yes sharn@192.168.0.238 "echo $1>> \"$C\"" 2>/dev/null; }
until powershell -NoProfile -ExecutionPolicy Bypass -File "$FG" | grep -q "fg: tlou-ii"; do sleep 5; done
echo "focused at $(date +%T)"; n0=$(wc -l < "$G/tlou2-coop.log"); p0=$(wc -l < "$G/tlou2-coop.partner.log")
pos() { printf 'players\n' >> "$G/tlou2-coop/cmd.txt"; sleep 1.5; P=$(grep "players:" "$G/tlou2-coop.log" | tail -1 | sed 's/.*\[1\] node [0-9A-F]* id [0-9]* proc 0*\([0-9A-F]*\).*/\1/'); printf "proc $P\n" >> "$G/tlou2-coop/cmd.txt"; sleep 1.5; grep "proc 000.*'Player'" "$G/tlou2-coop.log" | tail -1 | sed 's/.*\(pos .*\)/\1/'; }
echo "before: $(pos)"
powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$SP/rec.ps1")" -Out "$(cygpath -w "$SP/clonetest2.mp4")" -Sec 24 & REC=$!
sleep 1; pcb "stick l 0 32767 3000"; sleep 3.5; echo "after walk: $(pos)"
pcb "press b 300"; sleep 3; echo "after crouch: $(pos)"
pcb "press b 1300"; sleep 4; echo "after prone hold: $(pos)"
pcb "press b 300"; sleep 3; pcb "press b 300"; sleep 3
wait $REC
echo "-- PC A:"; tail -n +$n0 "$G/tlou2-coop.log" | grep "clonestance\|drift\|campick" | cut -c1-160
echo "-- PC B:"; tail -n +$p0 "$G/tlou2-coop.partner.log" | grep "clonestance\|command: press" | cut -c1-160
ffmpeg -v error -y -i "$SP/clonetest2.mp4" -vf "fps=1,scale=480:-2,tile=6x4" -frames:v 1 "$SP/clonetest2.png"
tasklist | grep -c tlou-ii; echo done
