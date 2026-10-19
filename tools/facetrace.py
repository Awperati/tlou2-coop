"""Standing-turn delay from a co-op log (2026-10-20, owner: "she turns smoothly but it is delayed").
  python -I tools/facetrace.py <tlou2-coop.log> [from HH:MM:SS] [to HH:MM:SS]
Reads the `facetrace:` lines (`facetrace on`, one every 100 ms while the clone is off the target):
    facetrace: target T clone C error E, target last moved N ms ago, standing|moving
A turn starts when the target moves more than 0.1 rad from where it rested. For each turn it prints
  start      when the target first moved
  settled    when the target stopped moving (last change of more than 0.03 rad)
  reached    when the clone first came within 0.1 rad of the final target
  lag        reached - settled: how long she trails once the partner holds still
  total      reached - start
Without facetrace lines it falls back to the 1 s `cloneface: clone yaw` lines (coarse).
"""
import math
import re
import sys

TRACE = re.compile(r'\[(\d\d):(\d\d):(\d\d)\.(\d+)\] facetrace: target (\S+) clone (\S+) error (\S+), target last moved (\d+) ms ago, (\w+)')
FACE = re.compile(r'\[(\d\d):(\d\d):(\d\d)\.(\d+)\] cloneface: clone yaw (\S+), other body \S+, other camera (\S+) .*?, (\w+),')


def ms(h, m, s, frac):
    return ((int(h) * 60 + int(m)) * 60 + int(s)) * 1000 + int(frac[:3].ljust(3, '0'))


def ang(a, b):
    return abs(math.remainder(a - b, 2 * math.pi))


def hhmmss(t):
    t //= 1000
    return '%02d:%02d:%02d' % (t // 3600, t // 60 % 60, t % 60)


def samples(path, lo, hi):
    out, coarse = [], []
    for line in open(path, encoding='utf-8', errors='replace'):
        m = TRACE.search(line)
        if m:
            t = ms(*m.group(1, 2, 3, 4))
            if lo <= t <= hi:
                out.append((t, float(m[5]), float(m[6]), m[9]))
            continue
        m = FACE.search(line)
        if m:
            t = ms(*m.group(1, 2, 3, 4))
            if lo <= t <= hi:
                coarse.append((t, float(m[6]), float(m[5]), m[7]))
    return (out, 100) if out else (coarse, 1000)


def turns(rows, start_rad=0.1, still_rad=0.03, reach_rad=0.1):
    """rows: (t ms, target, clone, state). Returns dicts per turn (see the module doc)."""
    res = []
    rest = None
    i = 0
    while i < len(rows):
        t, tgt, cl, st = rows[i]
        if rest is None:
            rest = tgt
        if ang(tgt, rest) <= start_rad:
            i += 1
            continue
        start = t
        settled, last = t, tgt
        j = i + 1
        reached = None
        while j < len(rows):
            tj, tg, cj, _ = rows[j]
            if ang(tg, last) > still_rad:
                settled, last = tj, tg
                reached = None
            if reached is None and ang(cj, last) <= reach_rad:
                reached = tj
            if reached is not None and tj - settled > 1500:
                break
            j += 1
        res.append({'start': start, 'settled': settled, 'reached': reached, 'target': last,
                    'lag': None if reached is None else max(0, reached - settled),
                    'total': None if reached is None else reached - start})
        rest = last
        i = j
    return res


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    lo = ms(*sys.argv[2].split(':'), '0') if len(sys.argv) > 2 else 0
    hi = ms(*sys.argv[3].split(':'), '999') if len(sys.argv) > 3 else 10 ** 9
    rows, step = samples(sys.argv[1], lo, hi)
    if not rows:
        print('no facetrace or cloneface lines in that window')
        return 1
    res = turns(rows)
    print('%d samples (%s, %d ms apart), %d turns' % (len(rows), 'facetrace' if step == 100 else 'cloneface only', step, len(res)))
    for r in res:
        print('%s  to %+.2f  settled +%5d ms  reached %s  lag %s  total %s' % (
            hhmmss(r['start']), r['target'], r['settled'] - r['start'],
            '-' if r['reached'] is None else '+%d ms' % (r['reached'] - r['start']),
            '-' if r['lag'] is None else '%d ms' % r['lag'],
            '-' if r['total'] is None else '%d ms' % r['total']))
    lags = sorted(r['lag'] for r in res if r['lag'] is not None)
    if lags:
        print('lag median %d ms, max %d ms, never reached %d (resolution %d ms)' % (
            lags[len(lags) // 2], lags[-1], sum(r['lag'] is None for r in res), step))
    return 0


if __name__ == '__main__':
    sys.exit(main())
