#!/usr/bin/env python3
"""Sums the output of `rc_replay <dump> --consumer vifstat [--with-emu]` over several dumps
(RENDERER_3D_DESIGN.md section 3).

  tools/vif_agg.py out/*.txt                 every V line summed (n, m) and every X line's maximum
  tools/vif_agg.py --per-dump KEY out/*.txt  one row per dump for the "total" / "max" key KEY (count, sum, mean, max)
"""
import collections
import os
import sys


def read(path):
    sums, maxes = {}, {}
    for line in open(path, errors="replace"):
        line = line.rstrip("\n")
        if line.startswith("V "):
            cat, key, n, m = line[2:].split("\t")
            sums[(cat, key)] = (int(n), int(m))
        elif line.startswith("X "):
            cat, key, v = line[2:].split("\t")
            maxes[(cat, key)] = int(v)
    return sums, maxes


def main():
    args = sys.argv[1:]
    if args and args[0] == "--per-dump":
        key = args[1]
        print(f"{'dump':28} {'kicks':>7} {'sum':>12} {'mean':>10} {'max':>8}")
        for path in args[2:]:
            sums, maxes = read(path)
            n, m = sums.get(("total", key), (0, 0))
            print(f"{os.path.basename(path)[:-4]:28} {n:7d} {m:12d} {m / n if n else 0:10.1f} {maxes.get(('max', key), 0):8d}")
        return
    total = collections.defaultdict(lambda: [0, 0])
    top = collections.defaultdict(int)
    for path in args:
        sums, maxes = read(path)
        for k, (n, m) in sums.items():
            total[k][0] += n
            total[k][1] += m
        for k, v in maxes.items():
            top[k] = max(top[k], v)
    for (cat, key), (n, m) in sorted(total.items()):
        print(f"V {cat}\t{key}\t{n}\t{m}")
    for (cat, key), v in sorted(top.items()):
        print(f"X {cat}\t{key}\t{v}")


if __name__ == "__main__":
    main()
