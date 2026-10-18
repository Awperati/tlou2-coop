#!/bin/bash
# Leak audit: force N hot reloads of coop.dll (touch the deployed copy) and print the `leaks` counts after each.
G="/e/Games/The Last of Us - Part II Remastered"; N=${1:-5}
for i in $(seq 0 $N); do
  if [ $i -gt 0 ]; then
    before=$(head -1 "$G/tlou2-coop.log")
    cp "$G/tlou2-coop/coop.dll" "$G/tlou2-coop/coop.tmp" && mv -f "$G/tlou2-coop/coop.tmp" "$G/tlou2-coop/coop.dll"
    for k in $(seq 1 60); do [ "$(head -1 "$G/tlou2-coop.log")" != "$before" ] && grep -aq "hosting\|joining" "$G/tlou2-coop.log" && break; sleep 1; done
    sleep 8
  fi
  n=$(wc -l < "$G/tlou2-coop.log"); printf 'leaks\n' >> "$G/tlou2-coop/cmd.txt"
  for k in $(seq 1 20); do tail -n +$n "$G/tlou2-coop.log" | grep -aq "leaks:" && break; sleep 1; done
  echo "reload $i: $(tail -n +$n "$G/tlou2-coop.log" | grep -a "leaks:" | tail -1 | cut -c16-)"
done
