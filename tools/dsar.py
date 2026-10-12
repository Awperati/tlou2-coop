"""Read TLOU2 PC .psarc files: an outer 'DSAR' container of LZ4 blocks around a
standard PSARC (zlib entries). Read-only; for finding names (looks, levels) offline.
  python tools/dsar.py list <archive.psarc> [filter]
  python tools/dsar.py get <archive.psarc> <inner path> <out file>
"""
import bisect, hashlib, re, struct, sys, zlib


def lz4_block(src, size):
    out = bytearray()
    i = 0
    n = len(src)
    while i < n:
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255: break
        out += src[i:i + lit]; i += lit
        if i >= n or len(out) >= size: break
        off = src[i] | (src[i + 1] << 8); i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255: break
        ml += 4
        start = len(out) - off
        for k in range(ml):
            out.append(out[start + k])
    return bytes(out)


class Dsar:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(32)
        if h[:4] != b"DSAR":
            raise ValueError("not DSAR")
        self.nblocks = struct.unpack_from("<I", h, 8)[0]
        self.blocks = []
        self.f.seek(0x20)
        tab = self.f.read(32 * self.nblocks)
        for k in range(self.nblocks):
            uo, co, us, cs, t = struct.unpack_from("<QQIIB", tab, 32 * k)
            self.blocks.append((uo, co, us, cs, t))
        self.starts = [b[0] for b in self.blocks]
        self.cache = {}

    def block(self, k):
        if k in self.cache:
            return self.cache[k]
        uo, co, us, cs, t = self.blocks[k]
        self.f.seek(co)
        raw = self.f.read(cs)
        data = raw if cs == us else lz4_block(raw, us)
        if len(self.cache) > 64:
            self.cache.clear()
        self.cache[k] = data
        return data

    def read(self, off, size):
        out = bytearray()
        while size > 0:
            k = bisect.bisect_right(self.starts, off) - 1
            b = self.block(k)
            rel = off - self.blocks[k][0]
            chunk = b[rel:rel + size]
            if not chunk:
                break
            out += chunk; off += len(chunk); size -= len(chunk)
        return bytes(out)


class Psarc:
    def __init__(self, path):
        self.d = Dsar(path)
        h = self.d.read(0, 32)
        assert h[:4] == b"PSAR", h[:4]
        self.comp = h[8:12]
        self.toc_len, self.entry_size, self.nfiles, self.block_size, self.flags = struct.unpack(">IIIII", h[12:32])
        toc = self.d.read(32, self.toc_len - 32)
        self.entries = []
        for k in range(self.nfiles):
            e = toc[k * self.entry_size:(k + 1) * self.entry_size]
            zi = struct.unpack(">I", e[16:20])[0]
            ln = int.from_bytes(e[20:25], "big")
            off = int.from_bytes(e[25:30], "big")
            self.entries.append((zi, ln, off))
        bstart = self.nfiles * self.entry_size
        width = 2 if self.block_size <= 0x10000 else 3 if self.block_size <= 0x1000000 else 4
        rest = toc[bstart:]
        self.zsizes = [int.from_bytes(rest[i:i + width], "big") for i in range(0, len(rest) - width + 1, width)]
        # Entries are matched to names by MD5 of the path (TOC order differs from the manifest).
        names = re.split(r"[\x00\n]", self.entry(0).decode("utf-8", "replace"))
        by_md5 = {toc[k * self.entry_size:k * self.entry_size + 16]: k for k in range(self.nfiles)}
        self.names = {}
        for n in names:
            k = by_md5.get(hashlib.md5(n.encode()).digest()) if n else None
            if k is not None:
                self.names[n] = k

    def entry(self, k):
        zi, ln, off = self.entries[k]
        out = bytearray()
        while len(out) < ln:
            zs = self.zsizes[zi] or self.block_size
            chunk = self.d.read(off, zs)
            off += zs; zi += 1
            want = min(self.block_size, ln - len(out))
            if zs == want or zs == self.block_size and want == self.block_size and chunk[:2] not in (b"\x78\xda", b"\x78\x9c", b"\x78\x01"):
                out += chunk
            else:
                try:
                    out += zlib.decompress(chunk)
                except zlib.error:
                    out += chunk
        return bytes(out[:ln])


if __name__ == "__main__":
    cmd, path = sys.argv[1], sys.argv[2]
    p = Psarc(path)
    if cmd == "list":
        flt = sys.argv[3] if len(sys.argv) > 3 else ""
        for n, k in sorted(p.names.items()):
            if flt in n:
                print(p.entries[k][1], n)
    elif cmd == "get":
        name = sys.argv[3]
        k = p.names.get(name) or p.names.get("/" + name.lstrip("/"))
        open(sys.argv[4], "wb").write(p.entry(k))
