#!/usr/bin/env python3
"""Checks the symbol mapping behind rc::sym against the executables' own code.

Usage: tools/check_addr_mapping.py
Every address that the code of a function unchanged between the versions computes (rcelf.observed_pairs: jal targets,
lui+lo pairs, $gp-relative accesses), named the way tools/addr_convert.py names it (symbol + offset, lit4_..., sec_... +
offset) and evaluated with decomp/generated/eu/rc_addrs.h, must equal the address the Europe code computes at the
same instruction. Values that are the same number in both versions are constants that fall in the address range
(colours, sizes, the random generator's multiplier); they are listed separately and must not be converted.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import addr_convert as ac  # noqa: E402
import rcelf  # noqa: E402

ROOT = ac.ROOT


def load(path):
    return {m.group(1): int(m.group(2), 16)
            for m in re.finditer(r'\b(\w+) = (0x[0-9a-f]+)u', open(path).read())}


def main():
    namer = ac.Namer()
    eu_values = load(os.path.join(ROOT, 'decomp', 'generated', 'eu', 'rc_addrs.h'))
    other = namer.others[0][0]
    checked, same, bad = 0, [], []
    for u, e in sorted(rcelf.observed_pairs(namer.usa, other).items()):
        if not namer.in_range(u):
            continue
        text, kind, _ = namer.name(u)
        if text is None:
            continue
        m = re.fullmatch(r'\(?sym::(\w+)(?: \+ (0x[0-9a-f]+)u\))?', text)
        value = eu_values[m.group(1)] + (int(m.group(2), 16) if m.group(2) else 0)
        checked += 1
        if value == e:
            continue
        (same if u == e else bad).append((u, text, value, e))
    for u, text, value, e in bad:
        print(f'MISMATCH {u:#x} {text}: header {value:#x}, the Europe code uses {e:#x}')
    print(f'{checked} code addresses checked, {len(bad)} mismatches; {len(same)} are the same number in both versions '
          '(constants, not addresses): ' + ' '.join(f'{u:#x}' for u, *_ in same))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
