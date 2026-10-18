"""Name script natives from their NativeObject address (the reverse of native.py).
  python -I tools/native_rev.py <exe> <dict files...> -- <obj rva hex> [<obj rva hex> ...]
Registration code is `lea rax,[obj]; ... mov r8,<fnv1a64(name)>; ... call Register`, so the obj's
lea is followed closely by the name hash. Names come from a dictionary: every printable word
(a-z0-9-?!*>) found in the given files (the exe itself, DC script binaries, text dumps) is hashed.
"""
import re, struct, sys

args = sys.argv[1:]
cut = args.index('--')
exe, dicts, objs = args[0], args[1:cut], [int(a, 16) for a in args[cut + 1:]]
d = open(exe, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
n = struct.unpack_from('<H', d, pe + 6)[0]
opt = struct.unpack_from('<H', d, pe + 20)[0]
text = None
for i in range(n):
    o = pe + 24 + opt + 40 * i
    if d[o:o + 8].rstrip(b'\0') == b'.text':
        text = struct.unpack_from('<IIII', d, o + 8)
_, tva, trs, tra = text
t = d[tra:tra + trs]

def fnv(s):
    h = 0xcbf29ce484222325
    for b in s: h ^= b; h = (h * 0x100000001b3) & (2**64 - 1)
    return h

names = {}
word = re.compile(rb'[a-z][a-z0-9\-?!*>]{2,80}')
for f in [exe] + dicts:
    for m in word.finditer(open(f, 'rb').read()):
        w = m.group().rstrip(b'-')
        names.setdefault(fnv(w), w.decode())

want = set(objs)
found = {}
# lea rax,[rip+disp32] = 48 8D 05 disp32
for m in re.finditer(rb'\x48\x8D\x05(....)', t, re.S):
    k = m.start()
    target = tva + k + 7 + struct.unpack('<i', m.group(1))[0]
    if target not in want or target in found: continue
    window = t[k:k + 48]
    j = window.find(b'\x49\xB8')
    if j >= 0:
        found[target] = struct.unpack_from('<Q', window, j + 2)[0]
for o in objs:
    sid = found.get(o)
    print('obj+%x  sid %s  %s' % (o, ('%016x' % sid) if sid else '?', names.get(sid, '?') if sid else ''))
