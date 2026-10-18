import os, ida_auto, idautils, ida_funcs, ida_pro, ida_lines
ida_auto.auto_wait(); out=[]
lo=int(os.environ["LO"],16); hi=int(os.environ["HI"],16)
for f in idautils.Functions(lo, hi):
    hits=[]; n=0
    fn=ida_funcs.get_func(f)
    for h in idautils.Heads(fn.start_ea, fn.end_ea):
        d=ida_lines.tag_remove(ida_lines.generate_disasm_line(h, 0) or ''); n+=1
        if "+1Ch]" in d or "+0D0h]" in d or "+0D8h]" in d: hits.append("0x%X %s"%(h,d))
    if hits: out.append("== 0x%X %s (%d ins)\n  "%(f, ida_funcs.get_func_name(f), n)+"\n  ".join(hits[:8]))
open(os.environ["XR_OUT"],"w").write("\n".join(out)); ida_pro.qexit(0)
