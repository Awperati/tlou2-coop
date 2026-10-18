import os, ida_auto, idautils, ida_funcs, ida_pro, ida_nalt
ida_auto.auto_wait(); out=[]; want=os.environ["IMP"].split(",")
def cb(ea, name, ordn):
    if name and any(w in name for w in want):
        for x in idautils.XrefsTo(ea):
            f=ida_funcs.get_func(x.frm)
            out.append("%s from 0x%X in 0x%X %s"%(name, x.frm, f.start_ea if f else 0, ida_funcs.get_func_name(x.frm)))
    return True
for i in range(ida_nalt.get_import_module_qty()):
    ida_nalt.enum_import_names(i, cb)
open(os.environ["XR_OUT"],"w").write("\n".join(out)); ida_pro.qexit(0)
