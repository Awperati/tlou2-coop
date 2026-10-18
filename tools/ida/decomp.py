# Decompile (or disassemble) functions. env FN = comma list of hex EAs, XR_OUT = output file.
import os, ida_auto, ida_funcs, ida_pro, idautils, ida_lines
ida_auto.auto_wait()
out = []
try:
    import ida_hexrays
    hx = ida_hexrays.init_hexrays_plugin()
except Exception:
    hx = False
for s in os.environ["FN"].split(","):
    ea = int(s, 16)
    f = ida_funcs.get_func(ea)
    out.append("==== 0x%X %s" % (ea, ida_funcs.get_func_name(ea)))
    done = False
    if hx and f:
        try:
            out.append(str(ida_hexrays.decompile(f.start_ea))); done = True
        except Exception as e:
            out.append("decompile failed: %s" % e)
    if not done and f:
        for h in idautils.Heads(f.start_ea, f.end_ea):
            out.append("0x%X %s" % (h, ida_lines.tag_remove(ida_lines.generate_disasm_line(h, 0) or '')))
open(os.environ["XR_OUT"], "w").write("\n".join(out)); ida_pro.qexit(0)
