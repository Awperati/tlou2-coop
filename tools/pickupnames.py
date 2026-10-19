"""Offline check of the pickup loot rule (config/pickups.ini) against the game's own level data.
Reads every *-ingame.pak in the given world-*.psarc archives (read-only), collects pickup-like
process names, and classifies them the way PickupPolicy does (src/core/pickups.h):
collectible (per player) is checked first, then story item (always shared), else loot.
  python -I tools/pickupnames.py <pickups.ini> <world-*.psarc> [...]
Prints counts per class, samples, and names worth a second look (keys/notes classed as loot).
"""
import collections, os, re, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dsar import Psarc  # noqa: E402

NAME = re.compile(rb"[a-z0-9][a-z0-9\-_]{5,80}")
PICKUPISH = ("pickup", "coll-", "obj-col-", "card-", "journal", "-note", "coin", "artifact", "safe-", "-key", "door-key", "obj-the-")


def load_rules(ini):
    rules = {"AlwaysShared": [], "PerPlayer": []}
    for line in open(ini, encoding="utf-8"):
        line = line.split(";")[0].strip()
        if "=" in line:
            k, v = (s.strip() for s in line.split("=", 1))
            if k in rules:
                rules[k] = [p.strip() for p in v.split(",") if p.strip()]
    return rules


def classify(name, rules):
    if any(p in name for p in rules["PerPlayer"]):
        return "collectible (per player)"
    if any(p in name for p in rules["AlwaysShared"]):
        return "story item (always shared)"
    return "loot (shared in mixed)"


def main():
    rules = load_rules(sys.argv[1])
    seen = {}
    for path in sys.argv[2:]:
        p = Psarc(path)
        level = os.path.basename(path)
        for n, k in p.names.items():
            if not n.endswith("-ingame.pak"):
                continue
            for m in NAME.finditer(p.entry(k)):
                s = m.group().decode()
                if any(t in s for t in PICKUPISH):
                    seen.setdefault(s, level)
    by = collections.defaultdict(list)
    for s in sorted(seen):
        by[classify(s, rules)].append(s)
    for cls, names in sorted(by.items()):
        print(f"{cls}: {len(names)}")
        for s in names[:12]:
            print(f"    {s}  ({seen[s]})")
    odd = [s for s in by["loot (shared in mixed)"] if re.search(r"key|note|letter|journal|card|coin|safe|artifact|collect", s)]
    print(f"loot names that look like keys/collectibles: {len(odd)}")
    for s in odd[:40]:
        print(f"    {s}  ({seen[s]})")


if __name__ == "__main__":
    main()
