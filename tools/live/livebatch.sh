#!/bin/bash
# One "game in front" window for the waiting live checks (read-only wait, no focus tricks):
#   1. clone weapon/aim (aimtest.sh body)   2. M8 relay run with the real games (relay on, walk 20 s, relay off)
# The relay run never edits coop.ini: it uses the mod's `relay` command and always ends with `relay off`.
D="$(cd "$(dirname "$0")" && pwd)"; G="/e/Games/The Last of Us - Part II Remastered"; B="$D/../../build"
C='E:\Games\The Last of Us - Part II Remastered\tlou2-coop\cmd.txt'; L='E:\Games\The Last of Us - Part II Remastered\tlou2-coop.log'
SECRET="${RELAY_SECRET:-coop-live-$(date +%s)}"
ssh_() { ssh -i ~/.ssh/tlou2_pcb -o BatchMode=yes sharn@192.168.0.238 "$1" 2>/dev/null; }
a_cmd() { printf '%s\n' "$1" >> "$G/tlou2-coop/cmd.txt"; }
b_cmd() { ssh_ "echo $1>> \"$C\""; }
cleanup() { [ -n "$STARTED" ] || return 0; b_cmd "relay off"; a_cmd "relay off"; a_cmd "clonebuttons 1F0"; taskkill //f //im coop_relay.exe >/dev/null 2>&1; echo "cleanup: relay off on both, relay stopped"; }
# One batch at a time: stopping a background run does not always stop its waiting loop, and on
# 2026-10-19 five stale batches fired together when the game came to the front.
LOCK="$TEMP/tlou2-livebatch.lock"
if [ -f "$LOCK" ] && kill -0 "$(cat "$LOCK")" 2>/dev/null; then echo "another livebatch is running (pid $(cat "$LOCK"))"; exit 1; fi
echo $$ > "$LOCK"
trap 'cleanup; rm -f "$LOCK"' EXIT
until powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$D/fg.ps1")" | grep -q "fg: tlou-ii"; do sleep 3; done
STARTED=1; echo "focused at $(date +%T)"
# After a relaunch the game sits at the title: continue the story with the pad, then wait for the clone.
if ! tail -n 30 "$G/tlou2-coop.log" | grep -aq "player pos"; then
  echo "at the menu: continuing the story"; a_cmd "press a"; sleep 4; a_cmd "press a"; sleep 3; a_cmd "press a"
fi
for i in $(seq 1 90); do grep -aq "clone: player 1 ready" "$G/tlou2-coop.log" && break; sleep 2; done
echo "clone: $(grep -a "clone: player 1 ready" "$G/tlou2-coop.log" | tail -1 | cut -c1-80)"; sleep 3
# --- 1. weapon and aim
n=$(wc -l < "$G/tlou2-coop.log")
a_cmd "clonebuttons 23F0"; a_cmd "giveweapon 1 - 30"; sleep 2
powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$D/rec.ps1")" -Out "$(cygpath -w "$TEMP")\aim3.mp4" -Sec 18 >/dev/null & REC=$!
sleep 1; b_cmd "trigger l 255 6000"; sleep 2.5
b_cmd "stick r 0 20000 1000"; sleep 2
b_cmd "stick r 20000 0 1000"; sleep 2.5
b_cmd "trigger lr 255 1000"; sleep 2.5; b_cmd "trigger lr 255 1000"
wait $REC
a_cmd "clonebuttons 1F0"
echo "-- aim log"; tail -n +$n "$G/tlou2-coop.log" | grep -a "cloneaim\|weapon\|shots" | cut -c1-150
echo "games running: $(tasklist | grep -c tlou-ii) (PC A)"
m=$(wc -l < "$G/tlou2-coop.log"); a_cmd "clonebody 1"; sleep 2; echo "-- body: $(tail -n +$m "$G/tlou2-coop.log" | grep -a "clonebody:" | tail -1 | cut -c16-120)"
m=$(wc -l < "$G/tlou2-coop.log"); a_cmd "props 1"; sleep 3; echo "-- props: $(tail -n +$m "$G/tlou2-coop.log" | grep -a "props: player 1 has" | tail -1 | cut -c16-220)"
tail -n +$m "$G/tlou2-coop.log" | grep -a "props: player 1 has" | grep -q "(0/" && echo "!! REGRESSION: a cloth prop of the clone is hidden"
ffmpeg -v error -y -i "$TEMP/aim3.mp4" -vf "fps=1.5,crop=iw/2:ih/2:iw/4:ih/6,scale=480:-2,tile=6x4" -frames:v 1 "$TEMP/aim3.png"
# In-game reload audit: player count and level packages must stay flat across a hot reload.
leaks_now() { local m=$(wc -l < "$G/tlou2-coop.log"); a_cmd "leaks"; sleep 2; tail -n +$m "$G/tlou2-coop.log" | grep -a "leaks:" | tail -1 | cut -c16-; }
echo "-- in-game reload audit"; echo "before: $(leaks_now)"
before=$(head -1 "$G/tlou2-coop.log"); cp "$G/tlou2-coop/coop.dll" "$G/tlou2-coop/coop.tmp" && mv -f "$G/tlou2-coop/coop.tmp" "$G/tlou2-coop/coop.dll"
for k in $(seq 1 60); do [ "$(head -1 "$G/tlou2-coop.log")" != "$before" ] && grep -aq "clone: player 1 ready" "$G/tlou2-coop.log" && break; sleep 1; done; sleep 3
echo "after:  $(leaks_now)"; grep -a "still there from before the reload" "$G/tlou2-coop.log" | tail -1 | cut -c1-110
# Before/after for the clone's walking error: 10 s with catch-up off, 10 s with it on (PC A drives the clone).
for mode in off on; do
  a_cmd "catchup $mode"; m=$(wc -l < "$G/tlou2-coop.log")
  b_cmd "stick l 0 32767 5000"; sleep 5.2; b_cmd "stick l 32767 0 5000"; sleep 6
  echo "-- catch-up $mode: clone error while walking (m)"
  tail -n +$m "$G/tlou2-coop.log" | grep -a "clonedrift:" | sed 's/.*clonedrift: \([0-9.]*\) m.*/\1/' | sort -n | awk '{a[NR]=$1; s+=$1} END {if (NR) { k=int(NR*0.95+0.999); if (k<1) k=1; printf "%d samples, mean %.2f, p95 %.2f, max %.2f\n", NR, s/NR, a[k], a[NR] } else print "no samples"}'
done
a_cmd "catchup on"
# --- 2. relay run
n=$(wc -l < "$G/tlou2-coop.log")
("$B/coop_relay.exe" "$SECRET" 47830 > "$TEMP/relay_live.out" 2>&1 &); sleep 1
a_cmd "relay 127.0.0.1 47830 $SECRET"; sleep 1; b_cmd "relay 192.168.0.237 47830 $SECRET"
for i in $(seq 1 30); do tail -n +$n "$G/tlou2-coop.log" | grep -aq "player 1 joined" && break; sleep 1; done
echo "-- relay join"; tail -n +$n "$G/tlou2-coop.log" | grep -a "relay\|joined\|left" | cut -c1-140
b_cmd "stick l 0 32767 10000"; sleep 10.5; b_cmd "stick l 32767 0 10000"; sleep 11
echo "-- relay walk"; tail -n +$n "$G/tlou2-coop.log" | grep -a "clonedrive: (want|drift)" -E | tail -4 | cut -c1-140
echo "-- clone error while the partner walks (m): count, mean, p95, max"
tail -n +$n "$G/tlou2-coop.log" | grep -a "clonedrift:" | sed 's/.*clonedrift: \([0-9.]*\) m.*/\1/' | sort -n | awk '{a[NR]=$1; s+=$1} END {if (NR) { k=int(NR*0.95+0.999); if (k<1) k=1; printf "%d samples, mean %.2f, p95 %.2f, max %.2f\n", NR, s/NR, a[k], a[NR] } else print "no samples"}'
tail -2 "$TEMP/relay_live.out"
echo done
