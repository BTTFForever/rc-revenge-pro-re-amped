#!/usr/bin/env python3
"""Builds the local track viewer straight from the disc's level files. Levels sharing a world appear once.

Usage: build.py <disc-dir> <gs_tables.bin> <viewer-dir>
"""
import concurrent.futures
import json
import os
import pathlib
import shutil
import subprocess
import sys

here = pathlib.Path(__file__).parent
disc, tables, viewer = pathlib.Path(sys.argv[1]), sys.argv[2], pathlib.Path(sys.argv[3])
data = viewer / 'data'
shutil.rmtree(data, ignore_errors=True)
data.mkdir(parents=True)
shutil.copy(here / 'index.html', viewer / 'index.html')
exporter = str(here.parent / 'bbk_export.py')
MENU_LEVEL = 98


def level_number(path):
    return int(path.parent.name) * 10 + int(path.stem)


def export(path):
    number = level_number(path)
    menu = number == MENU_LEVEL
    stem = 'menu' if menu else f'level_{number:03d}'
    name = 'Menu world' if menu else f'Level {number}'
    command = [sys.executable, '-I', exporter, str(path), tables, name, str(data / f'{stem}.js')] + (['--menu'] if menu else [])
    result = subprocess.run(command, capture_output=True, text=True)
    return number, stem, name, result


files = sorted((disc / 'BB').glob('*/*.BBK'), key=lambda p: (level_number(p) != MENU_LEVEL, level_number(p)))
with concurrent.futures.ThreadPoolExecutor(max_workers=max(2, (os.cpu_count() or 4) - 2)) as pool:
    results = list(pool.map(export, files))

entries, seen = [], {}
for number, stem, name, result in results:
    if result.returncode != 0:
        print(f'{name}: skipped ({(result.stderr or result.stdout).strip().splitlines()[-1]})')
        continue
    print(result.stdout.strip())
    meta_path = data / f'{stem}.js.meta'
    meta = json.loads(meta_path.read_text())
    meta_path.unlink()
    if meta['hash'] in seen:
        original = seen[meta['hash']]
        original['also'] = f"{original['also']}, {name}" if original.get('also') else name
        (data / f'{stem}.js').unlink()
        continue
    entry = {'name': name, 'file': f'{stem}.js', 'tris': meta['tris'], 'props': meta['props'], 'thumb': meta['thumb']}
    seen[meta['hash']] = entry
    entries.append(entry)

(data / 'index.js').write_text('RCVIEW.index = ' + json.dumps(entries) + ';\n')
print(len(entries), 'worlds')
