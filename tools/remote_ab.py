"""Summarise a remote_ab.ps1 output: per setting, her speed and gap while the remote moves.
  python tools/remote_ab.py re/clips/ab_<toggle>.txt
"""
import re, sys

blocks, cur = {}, None
for ln in open(sys.argv[1], encoding='utf-8-sig'):
    if ln.startswith('== '):
        cur = ln[3:].strip()
        blocks[cur] = {'her': [], 'gap': [], 'tele': 0, 'snap': 0, 'gaits': [], 'dead': 0}
        continue
    if cur is None:
        continue
    b = blocks[cur]
    if 'teleport' in ln: b['tele'] += 1
    if 'snap turn' in ln: b['snap'] += 1
    if 'WARNING' in ln or 'alive 0' in ln: b['dead'] += 1
    m = re.search(r'speed ([0-9.]+) m/s, her speed ([0-9.]+), gap ([0-9.]+) m.*?gait (-?\d)', ln)
    if m:
        sp, her, gap = map(float, m.groups()[:3])
        if sp >= 1.0:
            b['her'].append(her); b['gap'].append(gap); b['gaits'].append(int(m.group(4)))
for k, b in blocks.items():
    g = sorted(b['gap'])
    if not g:
        print(f'{k}: no moving samples'); continue
    h = b['her']
    flips = sum(1 for i in range(1, len(b['gaits'])) if b['gaits'][i] != b['gaits'][i - 1])
    print(f"{k}: n={len(g)} her mean {sum(h)/len(h):.2f} m/s | gap mean {sum(g)/len(g):.2f} median {g[len(g)//2]:.1f} "
          f"p90 {g[int(len(g)*.9)]:.1f} max {g[-1]:.1f} m | gait flips {flips} | teleports {b['tele']} snaps {b['snap']} dead {b['dead']}")
