# Every thread in a minidump: rip, and the game / coop return addresses on its stack.
# Usage: python -I tools/dumpthreads.py <file.dmp> [max threads]
import sys, struct
d = open(sys.argv[1], 'rb').read(); lim = int(sys.argv[2]) if len(sys.argv) > 2 else 400
n, dirrva = struct.unpack_from('<II', d, 8); streams = {}
for i in range(n):
    t, size, rva = struct.unpack_from('<III', d, dirrva + 12 * i); streams[t] = (size, rva)
mods = []
size, rva = streams[4]
for i in range(struct.unpack_from('<I', d, rva)[0]):
    o = rva + 4 + 108 * i; base, sz = struct.unpack_from('<QI', d, o)
    nr = struct.unpack_from('<I', d, o + 20)[0]; ln = struct.unpack_from('<I', d, nr)[0]
    mods.append((base, sz, d[nr + 4:nr + 4 + ln].decode('utf-16le').split(chr(92))[-1]))
def where(a):
    for b, s, nm in mods:
        if b <= a < b + s: return f"{nm}+{a - b:#x}"
    return None
size, rva = streams[3]
cnt = struct.unpack_from('<I', d, rva)[0]
for i in range(min(cnt, lim)):
    o = rva + 4 + 48 * i
    tid, = struct.unpack_from('<I', d, o)
    stst, = struct.unpack_from('<Q', d, o + 24); ssz, srva = struct.unpack_from('<II', d, o + 32)
    csz, crva = struct.unpack_from('<II', d, o + 40)
    rip = struct.unpack_from('<Q', d, crva + 0xF8)[0]; rsp = struct.unpack_from('<Q', d, crva + 0x98)[0]
    stk = d[srva + max(0, rsp - stst): srva + ssz]
    hits = []
    for j in range(0, len(stk) - 8, 8):
        w = where(struct.unpack_from('<Q', stk, j)[0])
        if w and ('tlou-ii' in w or 'coop' in w) and len(hits) < 14: hits.append(w)
    print(tid, where(rip) or hex(rip), '|', ' '.join(hits))
