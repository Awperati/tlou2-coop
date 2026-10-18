# For each ActionPack class (env CLASSES, comma list): find its vtable via MSVC RTTI and decompile
# vtable slot env SLOT (default 1, i.e. +8), which npc-get-entered-ap-type calls for the type id.
# Out: XR_OUT. Uses ida_* modules only.
import os, ida_auto, ida_bytes, ida_pro, ida_segment, ida_funcs, ida_hexrays, ida_idaapi, ida_nalt
ida_auto.auto_wait()
out = []
base = ida_nalt.get_imagebase()
slot = int(os.environ.get("SLOT", "1"))
rdata = ida_segment.get_segm_by_name(".rdata")
data = ida_bytes.get_bytes(rdata.start_ea, rdata.end_ea - rdata.start_ea)
dsegs = [s for s in (ida_segment.get_segm_by_name(".data"),) if s]

def find_all(blob, pat, start_ea):
    res, i = [], blob.find(pat)
    while i >= 0:
        res.append(start_ea + i)
        i = blob.find(pat, i + 1)
    return res

for cls in os.environ["CLASSES"].split(","):
    name = (".?AV%s@@" % cls).encode()
    hits = []
    for seg in [rdata] + dsegs:
        blob = ida_bytes.get_bytes(seg.start_ea, seg.end_ea - seg.start_ea)
        hits += find_all(blob, name, seg.start_ea)
    if not hits:
        out.append("%s: no type descriptor" % cls); continue
    td = hits[0] - 16
    td_rva = (td - base).to_bytes(4, "little")
    vts = []
    for col_field in find_all(data, td_rva, rdata.start_ea):
        col = col_field - 12
        if ida_bytes.get_dword(col) != 1:   # COL signature for x64
            continue
        for ref in find_all(data, col.to_bytes(8, "little"), rdata.start_ea):
            vts.append(ref + 8)
    for vt in vts[:2]:
        fn = ida_bytes.get_qword(vt + 8 * slot)
        f = ida_funcs.get_func(fn)
        text = "?"
        if f:
            try:
                text = str(ida_hexrays.decompile(f.start_ea))
            except Exception as e:
                text = "decompile failed: %s" % e
        out.append("== %s vtable 0x%X slot %d -> 0x%X\n%s" % (cls, vt, slot, fn, text))
    if not vts:
        out.append("%s: no vtable" % cls)
open(os.environ["XR_OUT"], "w").write("\n".join(out))
ida_pro.qexit(0)
