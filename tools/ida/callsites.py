import ida_bytes, os, ida_auto, idautils, ida_pro, ida_lines
ida_auto.auto_wait(); out=[]
for s in os.environ["FN"].split(","):
    t=int(s,16); out.append("== callers of 0x%X"%t)
    for x in idautils.CodeRefsTo(t,0):
        e=x
        lines=[]
        for i in range(10): e=ida_bytes.prev_head(e, 0); lines.append("   0x%X %s"%(e, ida_lines.tag_remove(ida_lines.generate_disasm_line(e, 0) or '')))
        out.append("call at 0x%X"%x); out.extend(reversed(lines))
open(os.environ["XR_OUT"],"w").write("\n".join(out)); ida_pro.qexit(0)
