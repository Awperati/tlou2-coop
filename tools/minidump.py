# Minimal minidump reader: exception, faulting module+offset, and game return addresses
# on the stack. Usage: python -I tools/minidump.py <file.dmp>
import sys, struct
d = open(sys.argv[1], 'rb').read()
assert d[:4] == b'MDMP'
n, dirrva = struct.unpack_from('<II', d, 8)
streams = {}
for i in range(n):
    t, size, rva = struct.unpack_from('<III', d, dirrva + 12 * i)
    streams[t] = (size, rva)
BS = chr(92)
mods = []
size, rva = streams[4]
cnt = struct.unpack_from('<I', d, rva)[0]
for i in range(cnt):
    o = rva + 4 + 108 * i
    base, sz = struct.unpack_from('<QI', d, o)
    namerva = struct.unpack_from('<I', d, o + 20)[0]
    ln = struct.unpack_from('<I', d, namerva)[0]
    name = d[namerva + 4:namerva + 4 + ln].decode('utf-16le')
    mods.append((base, sz, name.split(BS)[-1]))
def where(a):
    for b, s, nm in mods:
        if b <= a < b + s: return f"{nm}+{a - b:#x}"
    return hex(a)
size, rva = streams[6]
tid = struct.unpack_from('<I', d, rva)[0]
code, flags, rec, addr, np_ = struct.unpack_from('<IIQQI', d, rva + 8)
params = struct.unpack_from('<15Q', d, rva + 8 + 32)
print("thread", tid, "code", hex(code), "at", where(addr), "params", [hex(p) for p in params[:np_]])
ctxsize, ctxrva = struct.unpack_from("<II", d, rva + 8 + 152)
rsp = struct.unpack_from('<Q', d, ctxrva + 0x98)[0]
rip = struct.unpack_from('<Q', d, ctxrva + 0xF8)[0]
print("rip", where(rip), "rsp", hex(rsp))
def readmem(addr, ln):
    if 9 in streams:
        size, rva = streams[9]
        cnt, base = struct.unpack_from('<QQ', d, rva)
        cur = base
        for i in range(cnt):
            st, sz = struct.unpack_from('<QQ', d, rva + 16 + 16 * i)
            if st <= addr < st + sz: return d[cur + (addr - st):cur + (addr - st) + ln]
            cur += sz
    if 5 in streams:
        size, rva = streams[5]
        cnt = struct.unpack_from('<I', d, rva)[0]
        for i in range(cnt):
            st, sz, r = struct.unpack_from('<QII', d, rva + 4 + 16 * i)
            if st <= addr < st + sz: return d[r + (addr - st):r + (addr - st) + ln]
    return b''
stk = readmem(rsp, 0x2000)
hits = []
for i in range(0, len(stk) - 8, 8):
    v = struct.unpack_from('<Q', stk, i)[0]
    w = where(v)
    if ('tlou-ii' in w or 'coop' in w) and len(hits) < 30: hits.append(w)
print("stack:", hits)
