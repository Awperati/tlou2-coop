import os, ida_auto, ida_funcs, ida_pro, idautils, ida_bytes, ida_nalt
ida_auto.auto_wait(); out=[]
for s in os.environ["AT"].split(","):
    ea=0x140000000+int(s,16); f=ida_funcs.get_func(ea)
    strs=[]
    if f:
        for h in idautils.Heads(f.start_ea, min(f.end_ea, f.start_ea+0x3000)):
            for x in idautils.DataRefsFrom(h):
                t=ida_bytes.get_strlit_contents(x, -1, ida_nalt.STRTYPE_C)
                if t and len(t)>6: strs.append(t.decode(errors='ignore')[:90])
    out.append("rva %s in 0x%X %s | %s"%(s, f.start_ea if f else 0, ida_funcs.get_func_name(ea), " || ".join(strs[:3])))
open(os.environ["XR_OUT"],"w").write("\n".join(out)); ida_pro.qexit(0)
