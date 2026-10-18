import os, ida_auto, idautils, ida_funcs, ida_pro, ida_lines
ida_auto.auto_wait(); out=[]
for s in os.environ["EA"].split(","):
    ea=int(s,16)
    for x in idautils.XrefsTo(ea):
        f=ida_funcs.get_func(x.frm)
        out.append("0x%X <- 0x%X in 0x%X %s | %s"%(ea, x.frm, f.start_ea if f else 0, ida_funcs.get_func_name(x.frm), ida_lines.tag_remove(ida_lines.generate_disasm_line(x.frm, 0) or '')))
open(os.environ["XR_OUT"],"w").write("\n".join(out)); ida_pro.qexit(0)
