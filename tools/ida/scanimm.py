# Functions containing an instruction whose text matches any of env PATS (|-separated).
import os, ida_auto, idautils, ida_funcs, ida_pro, ida_segment, ida_lines, ida_bytes
ida_auto.auto_wait(); out=[]; pats=os.environ["PATS"].split("|"); seen={}
seg=ida_segment.get_segm_by_name(".text")
for h in idautils.Heads(seg.start_ea, seg.end_ea):
    if not ida_bytes.is_code(ida_bytes.get_flags(h)): continue
    d=ida_lines.tag_remove(ida_lines.generate_disasm_line(h, 0) or '')
    if any(p in d for p in pats):
        f=ida_funcs.get_func(h); fs=f.start_ea if f else 0
        seen.setdefault(fs, []).append("0x%X %s"%(h,d))
for fs,l in sorted(seen.items()):
    out.append("== 0x%X %s (%d)\n  "%(fs, ida_funcs.get_func_name(fs), len(l)) + "\n  ".join(l[:4]))
open(os.environ["XR_OUT"],"w").write("\n".join(out)); ida_pro.qexit(0)
