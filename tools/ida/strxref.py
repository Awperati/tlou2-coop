# Find functions referencing strings that contain any of env KEYS (|-separated). Out: XR_OUT.
import os, ida_auto, idc, idautils, ida_funcs, ida_pro
ida_auto.auto_wait()
keys=os.environ["KEYS"].split("|"); out=[]
for s in idautils.Strings():
    t=str(s)
    if any(k in t for k in keys):
        fns=set()
        for x in idautils.XrefsTo(s.ea):
            f=ida_funcs.get_func(x.frm)
            if f: fns.add("0x%X"%f.start_ea)
        out.append("%s @0x%X -> %s"%(t[:110], s.ea, ",".join(sorted(fns))))
open(os.environ["XR_OUT"],"w").write("\n".join(out)); ida_pro.qexit(0)
