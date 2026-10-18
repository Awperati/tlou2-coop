# Callers tree. env FN = comma hex EAs, DEPTH (default 2), XR_OUT.
import os, ida_auto, idautils, ida_funcs, ida_pro
ida_auto.auto_wait()
out = []
depth = int(os.environ.get("DEPTH", "2"))
def callers(ea):
    fs = []
    for x in idautils.CodeRefsTo(ea, 0):
        f = ida_funcs.get_func(x)
        if f and f.start_ea not in fs: fs.append(f.start_ea)
    for x in idautils.DataRefsTo(ea):
        f = ida_funcs.get_func(x)
        tag = ("data:0x%X" % x) if not f else None
        if f and f.start_ea not in fs: fs.append(f.start_ea)
        elif tag: out.append("    (data ref %s)" % tag)
    return fs
def walk(ea, d, seen):
    for c in callers(ea):
        out.append("  " * d + "<- 0x%X %s" % (c, ida_funcs.get_func_name(c)))
        if d < depth and c not in seen:
            seen.add(c); walk(c, d + 1, seen)
for s in os.environ["FN"].split(","):
    ea = int(s, 16)
    out.append("== 0x%X %s" % (ea, ida_funcs.get_func_name(ea)))
    walk(ea, 1, set())
open(os.environ["XR_OUT"], "w").write("\n".join(out)); ida_pro.qexit(0)
