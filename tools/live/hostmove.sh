#!/bin/bash
# Check 8: PC A's own player walks; PC B's clone of the host (player 1 on PC B) should follow.
D="$(cd "$(dirname "$0")" && pwd)"; G="/e/Games/The Last of Us - Part II Remastered"
C='E:\Games\The Last of Us - Part II Remastered\tlou2-coop\cmd.txt'; L='E:\Games\The Last of Us - Part II Remastered\tlou2-coop.log'
ssh_() { ssh -i ~/.ssh/tlou2_pcb -o BatchMode=yes sharn@192.168.0.238 "$1" 2>/dev/null; }
powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$D/fg.ps1")" | grep -q "fg: tlou-ii" || { echo "PC A game not in front"; exit 1; }
bclone() { ssh_ "echo players>> \"$C\""; sleep 1.5; P=$(ssh_ "powershell -NoProfile -Command \"(Get-Content '$L' -Tail 200 | Select-String 'players:' | Select-Object -Last 1).Line\"" | sed 's/.*\[1\] node [0-9A-F]* id [0-9]* proc 0*\([0-9A-F]*\).*/\1/'); ssh_ "echo proc $P>> \"$C\""; sleep 1.5; ssh_ "powershell -NoProfile -Command \"(Get-Content '$L' -Tail 200 | Select-String \\\"'Player'\\\" | Select-Object -Last 1).Line\"" | sed 's/.*pos \([-0-9. ]*\),.*/\1/'; }
apos() { grep -a "player pos" "$G/tlou2-coop.log" | tail -1 | sed 's/.*pos \([-0-9. ]*\) yaw.*/\1/'; }
echo "before: host $(apos) | PC B clone $(bclone)"
printf 'stick l 0 32767 3000\n' >> "$G/tlou2-coop/cmd.txt"; sleep 8
echo "after:  host $(apos) | PC B clone $(bclone)"
ssh_ "powershell -NoProfile -Command \"Get-Content '$L' -Tail 300 | Select-String 'clonedrive: (want|drift)' | Select-Object -Last 3 | % Line\"" | cut -c1-140
