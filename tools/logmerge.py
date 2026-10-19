"""Merge two PCs' co-op logs onto one clock (2026-10-20: PC B's clock ran ~6.45 s behind PC A's).
  python -I tools/logmerge.py <this PC's log> <other PC's log> [grep regex] [--offset ms]
The offset (other PC's clock minus this one's) comes from this log's latest
    net: peer N clock +X ms vs ours ...
line (session ping/pong, clocksync.h), or from --offset. Other-PC lines are shifted onto this PC's
clock and tagged B; this PC's lines are tagged A. Prints the merged lines in time order.
"""
import re
import sys

STAMP = re.compile(r'^\[(\d\d):(\d\d):(\d\d)\.(\d{3})\] ')
CLOCK = re.compile(r'net: peer \d+ clock ([+-]?\d+) ms vs ours')


def ms(m):
    return ((int(m[1]) * 60 + int(m[2])) * 60 + int(m[3])) * 1000 + int(m[4])


def fmt(t):
    t %= 24 * 3600 * 1000
    return '%02d:%02d:%02d.%03d' % (t // 3600000, t // 60000 % 60, t // 1000 % 60, t % 1000)


def read(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return f.read().splitlines()


def main(argv):
    args = [a for a in argv[1:] if not a.startswith('--')]
    if len(args) < 2:
        print(__doc__)
        return 2
    a, b = read(args[0]), read(args[1])
    pat = re.compile(args[2]) if len(args) > 2 else None
    off = None
    if '--offset' in argv:
        off = int(argv[argv.index('--offset') + 1])
    else:
        for line in a:
            m = CLOCK.search(line)
            if m:
                off = int(m[1])
    if off is None:
        print('no "net: peer ... clock" line in %s; pass --offset <ms>' % args[0])
        return 1
    rows = []
    for tag, lines, shift in (('A', a, 0), ('B', b, -off)):
        for line in lines:
            m = STAMP.match(line)
            if not m or (pat and not pat.search(line)):
                continue
            rows.append((ms(m) + shift, tag, line[m.end():]))
    rows.sort(key=lambda r: r[0])
    print('other clock %+d ms vs this one; B lines shifted by %+d ms' % (off, -off))
    for t, tag, text in rows:
        print('[%s] %s %s' % (fmt(t), tag, text))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
