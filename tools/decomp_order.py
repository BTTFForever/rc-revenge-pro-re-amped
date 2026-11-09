#!/usr/bin/env python3
"""Lists the game functions that cannot block, from the ELF's own code, leaf first.

These can be compared against their recompiled originals call by call, because the comparison runs
each call twice from one state. A function is listed only when everything it can reach is already
known not to block.

Usage: decomp_order.py <elf> <functions.csv> <symbols.json> <output.txt>
"""
import csv
import json
import re
import struct
import sys

TEXT_BASE, TEXT_OFFSET, GAME_END = 0x100000, 0x1000, 0x1B0430
# Library routines that never block or call back into the game.
NON_BLOCKING = re.compile(r'^(memcpy|memset|memmove|memcmp|memchr|memclr|strlen|strcmp|strncmp|strcpy|strncpy|strcat|'
                          r'strrchr|sprintf|vsprintf|printf|scePrintf|sqrt\w*|sin\w*|cos\w*|atan\w*|'
                          r'__\w+[sdt][fi]\d?|__(?:div|mod|udiv|umod|mul|ashl|ashr|lshr|neg|cmp|ucmp|fix|float|'
                          r'extend|trunc|add|sub|lt|le|gt|ge|eq|ne|unord)\w*)$')


def main():
    elf, table, symbols, out = sys.argv[1:5]
    data = open(elf, 'rb').read()
    real_size = {f['address']: f['size'] for f in json.load(open(symbols)) if f.get('size')}
    functions = sorted((start, start + real_size.get(start, end - start), name) for start, end, name in
                       ((int(r['Start'], 16), int(r['End'], 16), r['Name']) for r in csv.DictReader(open(table))))
    start_of = {f[0]: f[2] for f in functions}
    calls, opaque = {}, set()
    for start, end, name in functions:
        if not TEXT_BASE <= start < GAME_END:
            continue
        targets = set()
        for address in range(start, end, 4):
            word = struct.unpack_from('<I', data, address - TEXT_BASE + TEXT_OFFSET)[0]
            opcode = word >> 26
            if opcode in (2, 3):
                target = ((address + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
                if not start <= target < end:
                    targets.add(target)
            if opcode == 0 and (word & 63) == 9:
                opaque.add(name)
            if opcode == 0 and (word & 63) == 8 and ((word >> 21) & 31) != 31:
                opaque.add(name)  # a jump table is fine inside a function, but it is not checked here
            if opcode == 0 and (word & 63) == 12:
                opaque.add(name)  # syscall
        calls[start] = targets
    ready, level = set(), {}
    depth = 0
    while True:
        batch = []
        for start, end, name in functions:
            if start not in calls or start in ready or name in opaque:
                continue
            good = True
            for target in calls[start]:
                if target in ready or target == start:
                    continue
                callee = start_of.get(target)
                if callee and target >= GAME_END and NON_BLOCKING.match(callee):
                    continue
                good = False
                break
            if good:
                batch.append(start)
        if not batch:
            break
        for start in batch:
            ready.add(start)
            level[start] = depth
        depth += 1
    with open(out, 'w') as handle:
        handle.write('# level address bytes name\n')
        for start, end, name in sorted((f for f in functions if f[0] in ready), key=lambda f: (level[f[0]], f[0])):
            handle.write(f'{level[start]} 0x{start:x} {end - start} {name}\n')
    total = sum(1 for f in functions if f[0] in calls)
    bytes_ready = sum(f[1] - f[0] for f in functions if f[0] in ready)
    bytes_all = sum(f[1] - f[0] for f in functions if f[0] in calls)
    print(f'{len(ready)} of {total} game functions ready now, {bytes_ready} of {bytes_all} bytes, {depth} levels')
    print(f'{len(opaque)} use indirect jumps, calls or syscalls and are held back')


if __name__ == '__main__':
    main()
