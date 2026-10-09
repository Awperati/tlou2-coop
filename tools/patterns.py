"""Pattern helper for tlou-ii.exe.
  python tools/patterns.py <exe> bytes <va> <len>     dump bytes at a VA
  python tools/patterns.py <exe> find "<pattern>"     list VAs matching an IDA pattern
"""
import re, struct, sys

def load(path):
    d = open(path, 'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    n = struct.unpack_from('<H', d, pe + 6)[0]
    opt = struct.unpack_from('<H', d, pe + 20)[0]
    secs = []
    for i in range(n):
        o = pe + 24 + opt + 40 * i
        vs, va, rs, ra = struct.unpack_from('<IIII', d, o + 8)
        secs.append((va, rs, ra))
    return d, secs

def off_to_va(secs, off):
    for va, rs, ra in secs:
        if ra <= off < ra + rs:
            return 0x140000000 + va + off - ra
def va_to_off(secs, v):
    r = v - 0x140000000
    for va, rs, ra in secs:
        if va <= r < va + rs:
            return ra + r - va

d, secs = load(sys.argv[1])
if sys.argv[2] == 'bytes':
    o = va_to_off(secs, int(sys.argv[3], 16))
    print(' '.join('%02X' % b for b in d[o:o + int(sys.argv[4])]))
else:
    rx = b''.join(b'.' if t in ('?', '??') else re.escape(bytes([int(t, 16)])) for t in sys.argv[3].split())
    hits = [off_to_va(secs, m.start()) for m in re.finditer(rx, d, re.S)]
    print(len(hits), ' '.join(hex(h) for h in hits[:10]))
