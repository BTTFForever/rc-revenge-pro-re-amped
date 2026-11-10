#!/usr/bin/env python3
"""Shows the decompilation's progress per source file of the original game.

Usage: decomp_progress.py [--files]   (run from fork/)
"""
import collections
import json
import pathlib
import re
import sys

ROOT = pathlib.Path('decomp')
PSEUDO = ROOT / 'generated' / 'pseudo_all.c'


def main():
    done = set()
    for path in (ROOT / 'src').glob('*.cpp'):
        done |= set(int(a, 16) for a in re.findall(r'RC_DECOMP_FN(?:_BLOCKING)?\(\w+,\s*(0x[0-9a-f]+)\)', path.read_text()))
    ready = {}
    for line in (ROOT / 'generated' / 'order.txt').read_text().splitlines():
        if not line.startswith('#'):
            level, address, size, name = line.split()
            ready[int(address, 16)] = int(level)
    unit = {f['address']: f.get('cu', 'unknown') for f in json.load(open(ROOT / 'generated' / 'symbols.json'))}
    files = collections.defaultdict(lambda: {'total': 0, 'lines': 0, 'ready': 0, 'done': 0})
    current, lines = None, 0

    def flush():
        if current is None:
            return
        entry = files[unit.get(current, '(libraries)')]
        entry['total'] += 1
        entry['lines'] += lines
        entry['ready'] += current in ready
        entry['done'] += current in done

    for line in PSEUDO.read_text(errors='replace').splitlines():
        match = re.match(r'/\* ==== (\S+) @ ([0-9a-f]+) \*/', line)
        if match:
            flush()
            current, lines = int(match.group(2), 16), 0
        else:
            lines += 1
    flush()
    total = sum(f['total'] for f in files.values())
    print(f"{sum(f['done'] for f in files.values())} of {total} functions native, {sum(f['lines'] for f in files.values())} pseudo-C lines")
    if '--files' in sys.argv:
        for name, f in sorted(files.items(), key=lambda kv: -kv[1]['lines']):
            print(f"{name:24} {f['total']:5} fn {f['lines']:7} lines  {f['ready']:4} ready  {f['done']:4} done")


if __name__ == '__main__':
    main()
