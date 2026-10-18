import os, ida_auto, idautils, ida_funcs, ida_pro, ida_bytes
ida_auto.auto_wait(); out=[]
ea=int(os.environ["EA"],16)
# walk back to vtable start (previous qword not a code pointer or has a data xref)
start=ea
while True:
    p=start-8
    v=ida_bytes.get_qword(p)
    if not ida_funcs.get_func(v) or list(idautils.XrefsTo(start)) and start!=ea: break
    start=p
out.append("vtable start 0x%X"%start)
for x in idautils.XrefsTo(start):
    f=ida_funcs.get_func(x.frm); out.append("  xref 0x%X in 0x%X %s"%(x.frm, f.start_ea if f else 0, ida_funcs.get_func_name(x.frm)))
for i in range(40):
    v=ida_bytes.get_qword(start+8*i)
    if not ida_funcs.get_func(v): break
    out.append("  [%d] 0x%X %s"%(i, v, ida_funcs.get_func_name(v)))
open(os.environ["XR_OUT"],"w").write("\n".join(out)); ida_pro.qexit(0)
