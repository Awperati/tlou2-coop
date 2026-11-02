"""Metrics for tools/live/auto.sh from a slice of PC B's tlou2-coop.log (stdin or a file).
  python tools/live/auto_metrics.py <B-log-slice.txt>
Prints the clone's facing error (cloneface lines), weapon-state flips, drift, and the lines that name a known bug.
"""
import re, sys

lines = open(sys.argv[1], encoding='utf-8-sig', errors='replace').read().splitlines() if len(sys.argv) > 1 else sys.stdin.read().splitlines()

errs = [abs(float(m.group(1))) for l in lines if 'cloneface: clone yaw' in l and (m := re.search(r'error (-?[0-9.]+)\)', l))]
print(f"cloneface lines {len(errs)}", end='')
if errs:
    print(f"  max |err| {max(errs):.2f} rad  mean {sum(errs) / len(errs):.2f}  over 0.5: {sum(e > 0.5 for e in errs)}")
else:
    print('  (none: clone not present or no data)')

states = [m.group(1) for l in lines if 'combat: clone weapon' in l and 'weapon 0000000000000000' not in l and (m := re.search(r'state (\w+)', l))]
flips = sum(1 for a, b in zip(states, states[1:]) if a != b)
print(f"clone weapon-state lines {len(states)}, flips {flips}" + ('  <-- flicker' if len(states) > 10 and flips > len(states) * 0.3 else ''))

drift = [float(m.group(1)) for l in lines if 'clonedrift:' in l and (m := re.search(r'drift: ([0-9.]+) m', l))]
if drift:
    print(f"clonedrift lines {len(drift)}, max {max(drift):.2f} m")
for key in ('broken spot', 'spawn deferred', 'partner strafes', 'aiming, body', 'cloneaim: error grew', 'stood still', 'target y'):
    n = sum(key in l for l in lines)
    if n:
        print(f"{key}: {n}")
for l in lines:
    if re.search(r'crash|exception|access violation', l, re.I):
        print('ALERT', l[:140])
