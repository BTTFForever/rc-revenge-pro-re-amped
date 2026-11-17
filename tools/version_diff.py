#!/usr/bin/env python3
"""Compares the game's functions and data between two versions of the executable, by symbol.

Usage: version_diff.py <usa elf> <other elf> <out dir>
       e.g. tools/version_diff.py ../disc/SLUS_201.53 ../disc_eu/SLES_500.77 decomp/generated/eu

Method (exact; see NOTES.md, "Europe version"):
 1. The symbol tables must list the same symbols in the same order (keys from rcelf.py).
 2. For each function (FUNC symbol with a size), by key:
    - "identical": same size and the same bytes.
    - otherwise the words are compared with the address fields masked: the 26-bit target of j/jal, and the 16-bit
      immediate of lui and of every instruction that can carry the low half of an address (addiu, daddiu, ori, all
      loads and stores; rcelf.LO_OPS). Different sizes or masked words: "different" (same size) or "resized".
    - for masked-equal functions every masked-out field is then explained or reported. The addresses the code computes
      (j/jal targets, lui+lo pairs tracked in straight-line order, $gp-relative accesses: rcelf.references) are
      computed in both versions; each USA address is mapped to the other version by symbol + offset (rcelf.VersionMap)
      and must equal the other version's address. A $gp-relative access to .lit4 (float literals, no symbols) is
      explained if the 4 bytes read are the same in both. A differing immediate that is not part of a computed address
      (addiu v0,$0,60 against 50, an ori mask, a lui that loads a float constant) is a constant change. A function with
      every field explained is "relocated"; one with any constant change or unexplained address is "constants".
      An address computed the same way in both versions whose mapping says it should have moved is also reported.
    Functions that are "identical" or "relocated" behave the same in both versions given the symbol mapping. All
    others are listed in <out dir>/changed_functions.txt; their USA natives are not registered in that version.
 3. Data: each OBJECT symbol (and each initialised section without one) is compared word by word, a word counting as
    equal if it is the same or if both are addresses that map onto each other. Objects that differ are listed.
Writes <out dir>/changed_functions.txt (machine-readable: name, category, details) and <out dir>/version_diff.txt
(the full report), and prints a summary.
"""
import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rcelf  # noqa: E402


def explain_function(m, fa, fb):
    a, b = m.a, m.b
    wa, wb = a.code(fa), b.code(fb)
    if len(wa) != len(wb):
        return 'resized', [f'size {fa.size:#x} -> {fb.size:#x}']
    if wa == wb and fa.value == fb.value:
        notes = []
    if any(rcelf.masked(x) != rcelf.masked(y) for x, y in zip(wa, wb)):
        n = sum(rcelf.masked(x) != rcelf.masked(y) for x, y in zip(wa, wb))
        return 'different', [f'{n} of {len(wa)} instructions differ beyond address fields']
    ra = rcelf.references(a, wa, fa.value)
    rb = rcelf.references(b, wb, fb.value)
    notes = []
    explained = set()
    rb_by_index = {(i, k): t for i, k, t in rb}

    def check(ta, tb, kind):
        """None if the pair is explained, else why not."""
        if ta == tb:
            return None  # the same value in both: the same behaviour, whatever it stands for
        mapped, how = m.map(ta)
        if mapped == tb:
            return None
        if kind == 'gp':
            sa = a.section_of(ta)
            if sa is not None and sa.name == '.lit4' and a.read(ta, 4) == b.read(tb, 4):
                return None
        if kind in ('jal', 'j') and fa.value <= ta < fa.value + fa.size and tb - fb.value == ta - fa.value:
            return None  # a jump inside the function
        sym, off = m.symbolic(ta)
        where = f'{sym.key}+{off:#x}' if sym else '?'
        return f'{ta:#x} ({where}) -> {tb:#x}, map gives ' + (f'{mapped:#x}' if mapped is not None else how)

    # Every lui of the function, per register, for the lo halves whose lui the straight-line tracking lost (a value
    # set before a branch and used after a join that passed another write of the register).
    luis = {}
    for i, (x, y) in enumerate(zip(wa, wb)):
        if x >> 26 == rcelf.LUI:
            luis.setdefault((x >> 16) & 31, []).append((i, (x & 0xFFFF) << 16, (y & 0xFFFF) << 16))

    def lenient(i):
        x, y = wa[i], wb[i]
        op = x >> 26
        # Any lui of the function: the base register may be a copy (move, addiu) of the one the lui set, or the
        # straight-line tracking took a lui from another path.
        for j, ha, hb in [l for reg in luis.values() for l in reg]:
            ta = (ha | (x & 0xFFFF)) if op == 0x0D else (ha + rcelf.sext16(x & 0xFFFF)) & 0xFFFFFFFF
            tb = (hb | (y & 0xFFFF)) if op == 0x0D else (hb + rcelf.sext16(y & 0xFFFF)) & 0xFFFFFFFF
            if ta != tb and a.section_of(ta) is not None and check(ta, tb, 'hi/lo') is None:
                return j
        return None

    for i, kind, ta in ra:
        tb = rb_by_index.get((i, kind))
        if tb is None:
            notes.append(f'+{4 * i:#x} {kind} {ta:#x}: no counterpart')
            continue
        explained.add(i)
        why = check(ta, tb, kind)
        if why and kind == 'hi/lo' and lenient(i) is not None:
            why = None
        if why:
            notes.append(f'+{4 * i:#x} {kind} {why}')
    lui_explained = set()
    for i, (x, y) in enumerate(zip(wa, wb)):
        if x == y or i in explained:
            continue
        op = x >> 26
        if op in rcelf.LO_OPS:
            j = lenient(i)
            if j is not None:
                explained.add(i)
                lui_explained.add(j)
    for i, (x, y) in enumerate(zip(wa, wb)):
        if x == y or i in explained or i in lui_explained:
            continue
        op = x >> 26
        if op == rcelf.LUI:
            # Explained if a lo consumer of this register follows (its full address was checked above).
            reg = (x >> 16) & 31
            used = False
            for j in range(i + 1, min(len(wa), i + 64)):
                w = wa[j]
                o = w >> 26
                if o in rcelf.LO_OPS and ((w >> 21) & 31) == reg and j in explained:
                    used = True
                    break
                if o == rcelf.LUI and ((w >> 16) & 31) == reg:
                    break
            if used:
                continue
            notes.append(f'+{4 * i:#x} constant: lui {x & 0xFFFF:#06x} -> {y & 0xFFFF:#06x}')
        else:
            notes.append(f'+{4 * i:#x} constant: op {op:#04x} imm {x & 0xFFFF:#06x} -> {y & 0xFFFF:#06x}')
    if not notes:
        return ('identical' if wa == wb else 'relocated'), []
    return 'constants', notes


def compare_data(m):
    a, b = m.a, m.b
    out = []
    for name in ('.data', '.rodata', '.lit4', '.sdata'):
        sa, sb = a.section[name], b.section[name]
        objs = [s for s in a.placed if sa.addr <= s.value < sa.end and s.kind == 'OBJECT' and s.size]
        covered = set()
        for s in objs:
            t = b.by_key.get(s.key)
            if t is None:
                out.append((name, s.key, 'missing'))
                continue
            if t.size != s.size:
                out.append((name, s.key, f'size {s.size:#x} -> {t.size:#x}'))
                continue
            da, db = a.read(s.value, s.size), b.read(t.value, t.size)
            diffs = 0
            first = None
            for off in range(0, s.size - s.size % 4, 4):
                x = int.from_bytes(da[off:off + 4], 'little')
                y = int.from_bytes(db[off:off + 4], 'little')
                if x == y and not (a.section_of(x) and m.map(x)[0] not in (None, x)):
                    continue
                if a.section_of(x) is not None and m.map(x)[0] == y:
                    continue
                diffs += 1
                first = first if first is not None else off
            if s.size % 4 and da[-(s.size % 4):] != db[-(s.size % 4):]:
                diffs += 1
            if diffs:
                out.append((name, s.key, f'{diffs} words differ, first at +{first:#x}' if first is not None else 'tail bytes differ'))
            covered.update(range(s.value, s.value + s.size, 4))
        # Words outside every sized object (string literals, unnamed tables), compared at the same section offset.
        if sa.size == sb.size:
            runs = []
            for off in range(0, sa.size - sa.size % 4, 4):
                if sa.addr + off in covered:
                    continue
                x = int.from_bytes(a.read(sa.addr + off, 4), 'little')
                y = int.from_bytes(b.read(sb.addr + off, 4), 'little')
                if x == y or (a.section_of(x) is not None and m.map(x)[0] == y):
                    continue
                if runs and runs[-1][1] == sa.addr + off:
                    runs[-1][1] += 4
                else:
                    runs.append([sa.addr + off, sa.addr + off + 4])
            for lo, hi in runs:
                sym, off = m.symbolic(lo)
                where = f' (after {sym.key}+{off:#x})' if sym else ''
                out.append((name, f'{lo:#x}-{hi:#x}{where}', 'unnamed words differ'))
        else:
            out.append((name, '(section)', f'size {sa.size:#x} -> {sb.size:#x}: not compared outside objects'))
    return out


def main():
    ua, ub, outdir = sys.argv[1:4]
    a, b = rcelf.Elf(ua), rcelf.Elf(ub)
    m = rcelf.VersionMap(a, b)
    report = []
    if [s.name for s in a.symbols] != [s.name for s in b.symbols]:
        sys.exit('the symbol tables differ in order or names: keys would not match')
    cats = {}
    details = {}
    for fa in a.functions:
        if fa.size == 0:
            continue
        fb = b.by_key[fa.key]
        cat, notes = explain_function(m, fa, fb)
        cats[fa.key] = cat
        details[fa.key] = (fa, fb, notes)
    count = Counter(cats.values())
    report.append(f'# {os.path.basename(ua)} against {os.path.basename(ub)}: {len(cats)} functions with a size')
    for k in ('identical', 'relocated', 'constants', 'different', 'resized'):
        report.append(f'#   {k}: {count[k]}')
    changed = [k for k in cats if cats[k] not in ('identical', 'relocated')]
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, 'changed_functions.txt'), 'w') as f:
        f.write('# Generated by tools/version_diff.py: functions that do not behave the same in both versions\n')
        f.write('# (key, category, USA address, other address, sizes, first note). Their USA natives are not registered\n')
        f.write('# in the other version.\n')
        for k in sorted(changed, key=lambda k: details[k][0].value):
            fa, fb, notes = details[k]
            f.write(f'{k}\t{cats[k]}\t{fa.value:#x}\t{fb.value:#x}\t{fa.size:#x}\t{fb.size:#x}\t{notes[0] if notes else ""}\n')
    report.append('')
    for k in sorted(changed, key=lambda k: details[k][0].value):
        fa, fb, notes = details[k]
        report.append(f'{k} {cats[k]} {fa.value:#x}/{fb.value:#x} size {fa.size:#x}/{fb.size:#x}')
        for n in notes[:40]:
            report.append(f'    {n}')
        if len(notes) > 40:
            report.append(f'    ... {len(notes) - 40} more')
    report.append('')
    report.append('# Data objects whose contents differ (pointers compared through the address map):')
    for sec, key, why in compare_data(m):
        report.append(f'{sec} {key}: {why}')
    with open(os.path.join(outdir, 'version_diff.txt'), 'w') as f:
        f.write('\n'.join(report) + '\n')
    print('\n'.join(report[:6]))
    print(f'changed: {len(changed)} (see {outdir}/changed_functions.txt and version_diff.txt)')


if __name__ == '__main__':
    main()
