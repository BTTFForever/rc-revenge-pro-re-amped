#!/usr/bin/env python3
"""Writes a PS2Recomp function map (the ghidra_output CSV: Name,Start,End,Size) from an executable's symbol table.

Usage: function_map_from_elf.py <elf> <output.csv> [<reference elf> <reference.csv>]

There is no Ghidra project for the Europe executable, and the map only needs the function starts: the recompiler takes
each function's end from the ELF symbol's size (ElfParser's authoritative ends), so a map from the symbol table gives
the same functions. Rows: every FUNC symbol with a size, in address order, named as in the symbol table; the entry
point is written as ENTRYPOINT, as Ghidra's export names it.

With a reference executable and its map (the USA ones), the reference map's rows that are not FUNC symbols (Ghidra's
entry_<address> rows: 4-byte nops after a function that some branch reaches) are carried over through the symbol
mapping of tools/rcelf.py, so the recompiler makes the same set of functions for both versions.
"""
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rcelf  # noqa: E402


def main():
    elf_path, out_path = sys.argv[1:3]
    elf = rcelf.Elf(elf_path)
    rows = {}
    for f in elf.functions:
        if f.size and f.value not in rows:
            name = 'ENTRYPOINT' if f.name == '_start' else f.name
            rows[f.value] = (name, f.value, f.value + f.size)
    extra = 0
    if len(sys.argv) > 4:
        ref = rcelf.Elf(sys.argv[3])
        mapping = rcelf.VersionMap(ref, elf)
        ref_starts = {f.value for f in ref.functions if f.size}
        with open(sys.argv[4], newline='') as handle:
            reader = csv.reader(handle)
            next(reader)
            for name, start, end, _ in reader:
                a, b = int(start, 16), int(end, 16)
                if a in ref_starts:
                    continue
                mapped, how = mapping.map(a)
                if mapped is None:
                    sys.exit(f'{name} at {a:#x}: {how}')
                rows[mapped] = (name if not name.startswith('entry_') else f'entry_{mapped:08x}', mapped, mapped + (b - a))
                extra += 1
    with open(out_path, 'w', newline='') as handle:
        writer = csv.writer(handle, lineterminator='\n')
        writer.writerow(['Name', 'Start', 'End', 'Size'])
        for name, start, end in sorted(rows.values(), key=lambda r: r[1]):
            writer.writerow([name, f'0x{start:08X}', f'0x{end:08X}', end - start])
    print(f'{len(rows)} functions ({extra} carried over from the reference map)')


if __name__ == '__main__':
    main()
