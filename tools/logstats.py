"""Summarise a co-op mod log (tlou2-coop.log and its .prevN rotations) for a live run, read-only.
  python -I tools/logstats.py <log> [<log> ...] [--from HH:MM[:SS]] [--to HH:MM[:SS]]
Reports, for the lines in the time window:
  - focus: share of clonedrive lines with the game in front (her walk only works then)
  - drift: clonedrift samples (count, mean, p95, max) and drift snaps by reason (moving / standing)
  - facing: cloneface lines while standing with |error| > 0.35 rad, and turns (standing turn / faceforce)
  - stance presses, jumps sent / clone jump taps, traversal presses, theta changes, crashes or faults
Lines from several files are merged by time; times wrap at midnight, so keep a window within one day.
"""
import re, statistics, sys
from collections import Counter

TS = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\] (.*)$")


def secs(h, m, s, frac="0"):
    return int(h) * 3600 + int(m) * 60 + int(s) + float("0." + frac)


def parse_hm(s):
    p = [int(x) for x in s.split(":")] + [0, 0]
    return p[0] * 3600 + p[1] * 60 + p[2]


def main(argv):
    files, lo, hi = [], None, None
    i = 0
    while i < len(argv):
        if argv[i] == "--from": lo = parse_hm(argv[i + 1]); i += 2
        elif argv[i] == "--to": hi = parse_hm(argv[i + 1]); i += 2
        else: files.append(argv[i]); i += 1
    lines = []
    for f in files:
        for raw in open(f, encoding="utf-8", errors="replace"):
            m = TS.match(raw.rstrip("\n"))
            if not m: continue
            t = secs(*m.groups()[:4])
            if (lo is None or t >= lo) and (hi is None or t <= hi): lines.append((t, m.group(5)))
    lines.sort(key=lambda x: x[0])
    if not lines:
        print("no timestamped lines in the window"); return
    fmt = lambda t: "%02d:%02d:%02d" % (t // 3600, t // 60 % 60, t % 60)
    print("window %s - %s, %d lines" % (fmt(int(lines[0][0])), fmt(int(lines[-1][0])), len(lines)))

    front = Counter(); drift = []; snaps = Counter(); face_bad = face_stand = 0; turns = Counter()
    stance = jumps_sent = jump_taps = 0; trav = Counter(); theta = []; faults = []
    for t, s in lines:
        if s.startswith("clonedrive: want"):
            m = re.search(r"game in front (\d)", s)
            if m: front[m.group(1)] += 1
        elif s.startswith("clonedrift:"):
            m = re.match(r"clonedrift: ([\d.]+) m", s)
            if m: drift.append(float(m.group(1)))
        elif s.startswith("clonedrive: drift"):
            snaps["standing" if "(standing)" in s else "moving"] += 1
        elif s.startswith("cloneface: clone yaw"):
            m = re.search(r"\(error (-?[\d.]+)\), (standing|moving)", s)
            if m and m.group(2) == "standing":
                face_stand += 1
                if abs(float(m.group(1))) > 0.35: face_bad += 1
        elif s.startswith("cloneface: standing turn"):
            turns["teleport turn"] += 1
        elif s.startswith("clonestance:"):
            stance += 1
        elif s.startswith("jump: own player jumped"):
            jumps_sent += 1
        elif s.startswith("clonejump:"):
            jump_taps += 1
        elif s.startswith("clonetraversal:"):
            m = re.match(r"clonetraversal: (\S+)", s)
            trav[m.group(1) if m else "?"] += 1
        elif s.startswith("clonetheta: asked"):
            m = re.search(r"theta (-?[\d.]+) -> (-?[\d.]+)", s)
            if m: theta.append((t, float(m.group(1)), float(m.group(2))))
        if re.search(r"\bcrash|exception|\bFAULT\b|FAILED|\bfault\b", s) and not s.startswith("command:"):
            faults.append((t, s[:140]))

    n = sum(front.values())
    if n: print("focus: game in front %d of %d drive lines (%.0f%%)" % (front["1"], n, 100.0 * front["1"] / n))
    if drift:
        d = sorted(drift); p95 = d[min(len(d) - 1, int(len(d) * 0.95 + 0.999) - 1)]
        print("drift: %d samples, mean %.2f m, p95 %.2f m, max %.2f m" % (len(d), statistics.mean(d), p95, d[-1]))
    print("snaps: %d (moving %d, standing %d)" % (sum(snaps.values()), snaps["moving"], snaps["standing"]))
    if face_stand: print("facing while standing: %d of %d lines more than 0.35 rad off" % (face_bad, face_stand))
    if turns: print("turns: " + ", ".join("%s %d" % kv for kv in turns.items()))
    print("stance presses %d; jumps sent %d; clone jump taps %d" % (stance, jumps_sent, jump_taps))
    if trav: print("traversal: " + ", ".join("%s %d" % kv for kv in trav.most_common()))
    if theta: print("theta: %d changes, %.2f -> %.2f" % (len(theta), theta[0][1], theta[-1][2]))
    print("faults: %d" % len(faults))
    for t, s in faults[:10]: print("  %s %s" % (fmt(int(t)), s))


if __name__ == "__main__":
    main(sys.argv[1:])
