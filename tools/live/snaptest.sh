#!/bin/bash
# Check 5: PC B walks, stops; compare the clone on PC A with PC B's real spot after the snap.
D="$(cd "$(dirname "$0")" && pwd)"; G="/e/Games/The Last of Us - Part II Remastered"
C='E:\Games\The Last of Us - Part II Remastered\tlou2-coop\cmd.txt'
pcb() { ssh -i ~/.ssh/tlou2_pcb -o BatchMode=yes sharn@192.168.0.238 "echo $1>> \"$C\"" 2>/dev/null; }
until powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$D/fg.ps1")" | grep -q "fg: tlou-ii"; do sleep 5; done
echo "focused at $(date +%T)"; n0=$(wc -l < "$G/tlou2-coop.log")
clone() { printf 'players\n' >> "$G/tlou2-coop/cmd.txt"; sleep 1.5; P=$(grep "players:" "$G/tlou2-coop.log" | tail -1 | sed 's/.*\[1\] node [0-9A-F]* id [0-9]* proc 0*\([0-9A-F]*\).*/\1/'); printf "proc $P\n" >> "$G/tlou2-coop/cmd.txt"; sleep 1.5; grep "proc 000.*'Player'" "$G/tlou2-coop.log" | tail -1 | sed 's/.*pos \([-0-9. ]*\),.*/\1/'; }
real() { ssh -i ~/.ssh/tlou2_pcb -o BatchMode=yes sharn@192.168.0.238 "powershell -NoProfile -Command \"(Get-Content 'E:\Games\The Last of Us - Part II Remastered\tlou2-coop.log' -Tail 300 | Select-String 'player pos' | Select-Object -Last 1).Line\"" 2>/dev/null | sed 's/.*pos \([-0-9. ]*\) yaw.*/\1/'; }
echo "start  clone: $(clone)  PC B: $(real)"
pcb "stick l 32767 32767 4000"; sleep 6
echo "walked clone: $(clone)  PC B: $(real)"
sleep 4
echo "settled clone: $(clone)  PC B: $(real)"
tail -n +$n0 "$G/tlou2-coop.log" | grep "drift" | cut -c1-120
