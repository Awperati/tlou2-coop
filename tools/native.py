"""Find a script native's function by name.
  python -I tools/native.py <exe> name [name...]
Natives register as: lea rax,[obj]; mov r8,<fnv1a64(name)>; ... call Register.
The obj is initialised elsewhere with: lea rdx,[func]; lea rcx,[obj]; jmp ctor.
"""
import re, struct, sys

d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
n = struct.unpack_from('<H', d, pe + 6)[0]
opt = struct.unpack_from('<H', d, pe + 20)[0]
secs = []
for i in range(n):
    o = pe + 24 + opt + 40 * i
    name = d[o:o + 8].rstrip(b'\0')
    vs, va, rs, ra = struct.unpack_from('<IIII', d, o + 8)
    secs.append((name, va, rs, ra))
text = [s for s in secs if s[0] == b'.text'][0]
_, tva, trs, tra = text
t = d[tra:tra + trs]
base = 0x140000000

def off2va(off): return base + tva + (off - tra)

def fnv(s):
    h = 0xcbf29ce484222325
    for b in s.encode(): h ^= b; h = (h * 0x100000001b3) & (2**64 - 1)
    return h

# Map obj -> func from "lea rdx,[func]; lea rcx,[obj]; jmp" sequences.
init = {}
for m in re.finditer(rb'\x48\x8D\x15(....)\x48\x8D\x0D(....)\xE9', t, re.S):
    k = m.start()
    f = base + tva + k + 7 + struct.unpack('<i', m.group(1))[0]
    obj = base + tva + k + 14 + struct.unpack('<i', m.group(2))[0]
    init[obj] = f

for nm in sys.argv[2:]:
    sid = struct.pack('<Q', fnv(nm))
    i = t.find(b'\x49\xB8' + sid)
    if i < 0:
        print(f"{nm}: not registered"); continue
    # lea rax,[obj] just before (48 8D 05 disp32)
    j = t.rfind(b'\x48\x8D\x05', max(0, i - 16), i)
    obj = base + tva + j + 7 + struct.unpack_from('<i', t, j + 3)[0] if j >= 0 else 0
    print(f"{nm}: sid {fnv(nm):#x} obj {obj:#x} func {init.get(obj, 0):#x}")
