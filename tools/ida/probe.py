# Find strings, list code that references them, and decompile those functions.
# Usage: set PROBE_STRINGS="a|b|c" and PROBE_OUT=path, then
#   idat -A -S"tools/ida/probe.py" re/tlou-ii.i64
import os
import ida_auto, ida_bytes, ida_funcs, ida_hexrays, ida_name, ida_pro, idautils, idc

wanted = [s for s in os.environ.get("PROBE_STRINGS", "").split("|") if s]
out_path = os.environ.get("PROBE_OUT", "probe.txt")
max_funcs = int(os.environ.get("PROBE_MAX_FUNCS", "6"))
exact = os.environ.get("PROBE_EXACT", "1") == "1"

ida_auto.auto_wait()
have_hr = ida_hexrays.init_hexrays_plugin()
base = idc.get_inf_attr(idc.INF_MIN_EA) & ~0xFFFF

lines = []
def emit(s=""):
    lines.append(s)

strings = list(idautils.Strings())
for w in wanted:
    emit("=" * 100)
    emit("STRING %r" % w)
    hits = [s for s in strings if (str(s) == w if exact else w in str(s))]
    for s in hits[:8]:
        emit("  at 0x%X: %r" % (s.ea, str(s)[:120]))
        funcs = []
        for x in idautils.XrefsTo(s.ea):
            f = ida_funcs.get_func(x.frm)
            name = ida_name.get_name(f.start_ea) if f else "?"
            emit("    xref 0x%X in %s (0x%X, rva 0x%X)" % (x.frm, name, f.start_ea if f else 0, (f.start_ea - 0x140000000) if f else 0))
            if f and f.start_ea not in funcs:
                funcs.append(f.start_ea)
        for fea in funcs[:max_funcs]:
            emit("-" * 100)
            emit("FUNC 0x%X size %d" % (fea, ida_funcs.get_func(fea).size()))
            if have_hr:
                try:
                    c = ida_hexrays.decompile(fea)
                    text = str(c)
                    if len(text) > 12000:
                        text = text[:12000] + "\n... [truncated]"
                    emit(text)
                except Exception as e:
                    emit("decompile failed: %s" % e)

# PROBE_FUNCS="0x1403F86F0,0x..." decompiles those functions directly.
for a in [x for x in os.environ.get("PROBE_FUNCS", "").split(",") if x]:
    ea = int(a, 16)
    f = ida_funcs.get_func(ea)
    emit("=" * 100)
    if not f:
        emit("NO FUNC at %s" % a)
        continue
    emit("FUNC 0x%X size %d" % (f.start_ea, f.size()))
    if have_hr:
        try:
            text = str(ida_hexrays.decompile(f.start_ea))
            emit(text[:12000] + ("\n... [truncated]" if len(text) > 12000 else ""))
        except Exception as e:
            emit("decompile failed: %s" % e)

# PROBE_IMM="FC5BA7D4" lists code that carries that 32-bit immediate.
import ida_search
for imm in [x for x in os.environ.get("PROBE_IMM", "").split(",") if x]:
    v = int(imm, 16)
    pat = " ".join("%02X" % b for b in v.to_bytes(4, "little"))
    emit("=" * 100)
    emit("IMM 0x%X" % v)
    ea = idc.get_inf_attr(idc.INF_MIN_EA)
    end = idc.get_inf_attr(idc.INF_MAX_EA)
    n = 0
    while n < 60:
        ea = ida_bytes.find_bytes(pat, ea, range_end=end)
        if ea == idc.BADADDR or ea is None:
            break
        head = idc.get_item_head(ea)
        f = ida_funcs.get_func(head)
        if f and idc.is_code(ida_bytes.get_flags(head)):
            emit("  0x%X in %s (0x%X): %s" % (head, ida_name.get_name(f.start_ea), f.start_ea, idc.generate_disasm_line(head, 0)))
            n += 1
        ea += 1

# PROBE_DISASM="0x140DA8A80:40" prints that many instructions with their bytes.
for spec in [x for x in os.environ.get("PROBE_DISASM", "").split(",") if x]:
    a, n = spec.split(":")
    ea = int(a, 16)
    emit("=" * 100)
    emit("DISASM 0x%X" % ea)
    for _ in range(int(n)):
        size = idc.get_item_size(ea)
        raw = " ".join("%02X" % b for b in ida_bytes.get_bytes(ea, size))
        emit("  0x%X  %-36s %s" % (ea, raw, idc.generate_disasm_line(ea, 0)))
        ea += size

with open(out_path, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))
ida_pro.qexit(0)
