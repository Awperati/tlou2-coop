#!/bin/bash
# Automated two-PC check: puppets the HOST (PC A) on its virtual pad, reads PC B's log, grabs a frame of PC B
# from its live stream (http://192.168.0.238:8766/stream.mjpg, one viewer at a time: close OBS first) and prints metrics.
#   bash tools/live/auto.sh walk|aim|all  [outdir]
# Needs: PC A game running, PC B in the same level, tools/share.py not required. Focuses PC A's game (owner allowed puppeteering).
T="$(cd "$(dirname "$0")/.." && pwd)"; G="/e/Games/The Last of Us - Part II Remastered"; C="$G/tlou2-coop/cmd.txt"
OUT="${2:-${TEMP:-/tmp}/auto-$(date +%H%M%S)}"; mkdir -p "$OUT"
BLOG='E:\Games\The Last of Us - Part II Remastered\tlou2-coop.log'
ssh_ps() { enc=$(printf '%s' "$1" | iconv -f utf-8 -t utf-16le | base64 -w0); timeout 40 ssh -i ~/.ssh/tlou2_pcb -o BatchMode=yes sharn@192.168.0.238 "powershell -NoProfile -EncodedCommand $enc" 2>/dev/null | grep -v -e CLIXML -e '<Objs' | tr -d '\r'; }
frameB() { curl -s -m 4 -o "$OUT/$1.bin" http://192.168.0.238:8766/stream.mjpg; python - "$OUT/$1.bin" "$OUT/$1.jpg" <<'EOF'
import sys
b=open(sys.argv[1],'rb').read(); i=b.find(b'\xff\xd8'); j=b.find(b'\xff\xd9',i)
if i>=0 and j>i: open(sys.argv[2],'wb').write(b[i:j+2])
EOF
rm -f "$OUT/$1.bin"; }
mark() { NB=$(ssh_ps "(Get-Content '$BLOG').Count" | grep -E '^[0-9]+$' | head -1); }
report() { ssh_ps "Get-Content '$BLOG' | Select-Object -Skip $NB" > "$OUT/$1-B.log"; echo "== $1 (PC B)"; python "$T/live/auto_metrics.py" "$OUT/$1-B.log"; }
powershell -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$T/focus.ps1")" -Seconds 1 | head -1
walk() {
  mark; frameB walk-before
  for leg in "0 32767" "32767 0" "0 -32767" "-32767 0"; do printf 'stick l %s 1400\n' "$leg" >> "$C"; sleep 1.9; printf 'stick l 0 0 300\n' >> "$C"; sleep 0.9; done
  frameB walk-after; report walk
}
aim() {
  mark; frameB aim-before
  printf 'trigger l 255 9000\n' >> "$C"; sleep 1.5
  printf 'stick r 20000 0 1200\n' >> "$C"; sleep 2; frameB aim-mid
  printf 'stick r -20000 0 1200\n' >> "$C"; sleep 2
  printf 'stick r 0 20000 1000\n' >> "$C"; sleep 5; frameB aim-after; report aim
}
aimwalk() {
  mark; frameB aimwalk-before
  printf 'trigger l 255 9000\n' >> "$C"; sleep 1.2
  for leg in "0 32767" "32767 0" "0 -32767"; do
    printf 'stick l %s 1400\n' "$leg" >> "$C"; sleep 1.9
    printf 'stick l 0 0 300\n' >> "$C"; sleep 0.6
  done
  frameB aimwalk-after; sleep 3; report aimwalk
}
case "${1:-all}" in walk) walk ;; aim) aim ;; aimwalk) aimwalk ;; *) walk; aim; aimwalk ;; esac
echo "frames and logs in $OUT"
