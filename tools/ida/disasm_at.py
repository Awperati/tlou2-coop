import os, ida_auto, ida_pro, ida_lines, ida_bytes, ida_idaapi
ida_auto.auto_wait(); out=[]
for s in os.environ["AT"].split(","):
    ea=int(s,16); start=ea
    for i in range(int(os.environ.get("BACK","6"))): start=ida_bytes.prev_head(start, 0)
    out.append("== 0x%X"%ea); e=start
    for i in range(int(os.environ.get("N","14"))):
        out.append("0x%X %s"%(e, ida_lines.tag_remove(ida_lines.generate_disasm_line(e, 0) or ''))); e=ida_bytes.next_head(e, ida_idaapi.BADADDR)
open(os.environ["XR_OUT"],"w").write("\n".join(out)); ida_pro.qexit(0)
