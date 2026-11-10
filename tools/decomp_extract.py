#!/usr/bin/env python3
"""Prints the Ghidra output for the functions of the given source units that are not native yet, leaf first.

Each function is headed by its address, its call-depth level ('-' if it can block), and its signature from the debug data.
Usage: decomp_extract.py UNIT [UNIT...]   (run from fork/)
"""
import json
import pathlib
import re
import sys


def main():
    units = set(sys.argv[1:])
    symbols = {f['address']: f for f in json.load(open('decomp/generated/symbols.json'))}
    ready = {}
    for line in open('decomp/generated/order.txt'):
        if not line.startswith('#'):
            level, address, _, _ = line.split()
            ready[int(address, 16)] = int(level)
    done = set()
    for path in pathlib.Path('decomp/src').glob('*.cpp'):
        done |= {int(a, 16) for a in re.findall(r'RC_DECOMP_FN\w*\(\w+,\s*(0x[0-9a-f]+)\)', path.read_text())}
    text = open('decomp/generated/pseudo_all.c', errors='replace').read()
    found = []
    for part in re.split(r'(?=^/\* ==== )', text, flags=re.M):
        match = re.match(r'/\* ==== (\S+) @ ([0-9a-f]+) \*/', part)
        if not match:
            continue
        address = int(match.group(2), 16)
        info = symbols.get(address, {})
        if address in done or info.get('cu') not in units:
            continue
        body = re.sub(r'^\s*/\*.*?\*/\s*\n', '', part, flags=re.M)
        body = re.sub(r'^\s*(?:undefined\d?|u?int|u?short|u?char|bool|float|long|ulong|int\d+|uint\d+|int\d+_t|s32|frac|byte) +\*?[a-zA-Z_]\w*;\n', '', body, flags=re.M)
        body = re.sub(r'\n\s*\n+', '\n', body)
        params = ', '.join(f"{p['type_text']} {p['name']}" for p in info.get('params', []))
        found.append((ready.get(address, 99), address,
                      f"// {match.group(1)} @0x{address:x} L{ready.get(address, '-')} {info.get('ret_text')}({params})\n{body}"))
    for _, _, item in sorted(found, key=lambda f: (f[0], f[1])):
        print(item)


if __name__ == '__main__':
    main()
