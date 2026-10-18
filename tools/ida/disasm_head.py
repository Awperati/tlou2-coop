import os, ida_auto, ida_pro, ida_lines, ida_bytes, ida_idaapi
ida_auto.auto_wait()
out=[]
for s in os.environ["FN"].split(","):
    ea=int(s,16); out.append("== 0x%X"%ea)
    for i in range(int(os.environ.get("N","8"))):
        out.append("0x%X %s"%(ea, ida_lines.tag_remove(ida_lines.generate_disasm_line(ea, 0) or ''))); ea=ida_bytes.next_head(ea, ida_idaapi.BADADDR)
open(os.environ["XR_OUT"],"w").write("\n".join(out)); ida_pro.qexit(0)
