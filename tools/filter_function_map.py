#!/usr/bin/env python3
"""Removes Ghidra map entries that fall inside a real ELF function.

Usage: filter_function_map.py <elf> <input.csv> <output.csv>

ExportPS2Functions.java exports every branch target as an entry_* record. Those records
become fragments that stop at the next boundary, so a resumed thread can run off the end
of one into an address with no registered function. With exact ELF symbols, each function
is emitted whole and its own resume table handles resumes inside it.
"""
import bisect
import csv
import subprocess
import sys

elf, source, target = sys.argv[1:4]
functions = []
for line in subprocess.run(['readelf', '-sW', elf], capture_output=True, text=True, check=True).stdout.splitlines():
    parts = line.split()
    if len(parts) >= 8 and parts[3] == 'FUNC' and parts[6] != 'UND' and int(parts[2]) > 0:
        functions.append((int(parts[1], 16), int(parts[1], 16) + int(parts[2])))
functions = sorted(set(functions))
starts = [start for start, _ in functions]


def inside(address):
    index = bisect.bisect_right(starts, address) - 1
    return index >= 0 and functions[index][0] < address < functions[index][1]


with open(source, newline='') as handle:
    reader = csv.reader(handle)
    header, rows = next(reader), list(reader)
kept = [row for row in rows if not inside(int(row[1], 16))]
with open(target, 'w', newline='') as handle:
    writer = csv.writer(handle, lineterminator='\n')
    writer.writerow(header)
    writer.writerows(kept)
print(f'{len(rows)} entries, kept {len(kept)}, removed {len(rows) - len(kept)}')
