#!/usr/bin/env python3
"""Checks the renderer's texture decode (render/tex) against the track viewer's on the level files themselves
(renderer plan P1 step 5; RENDERER_FRAME_RECORD.md section 17).

For each BB/<n>/<n>.BBK of a disc: every texture the level's world and placed objects list is decoded twice from the
file's GS memory image: by tools/bbk_export.py (the track viewer's decoder, numpy, the runtime's tables in
gs_tables.bin) and by rc_tex_test's bbk mode (the image uploaded as the game uploads it, 64x64 CT32 tiles from block
0x1e00, through render/tex's own layout, then render/tex's decode). The RGBA texels must be equal byte for byte.
It also prints each level's alpha histogram: the textures holding a texel alpha above 0x80.

Usage: tex_check.py <disc-dir> <gs_tables.bin> <rc_tex_test> [level-number ...]
"""
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import bbk_export as bbk  # noqa: E402

FORMATS = {0: 'CT32', 0x13: 'T8', 0x14: 'T4'}


def shapes(level, menu):
    header = level.u32(level.end - bbk.MENU_HEADER_FROM_END) if menu else level.end - bbk.LEVEL_HEADER_FROM_END
    world = level.u32(header)
    if not 0x100000 <= world < 0x2000000 or level.f32(world) not in (2.0, 3.0):
        return []
    found = [world]
    props = level.u32(header + 4)
    if 0x100000 <= props < 0x2000000:
        for index in range(min(level.u32(props), 4096)):
            tob = level.u32(props + 4 + index * bbk.TSO_SIZE + 0xC)
            if 0x100000 <= tob < 0x2000000 and level.f32(tob) in (2.0, 3.0):
                found.append(tob)
    return found


def textures(level, menu):
    seen = {}
    for shape in shapes(level, menu):
        listing = level.u32(shape + 0x44)
        if not 0x100000 <= listing < 0x2000000:
            continue
        for i in range(min(level.u32(shape + 0x38), 4096)):
            entry = level.u32(listing + 4 * i)
            if not 0x100000 <= entry < 0x2000000:
                continue
            width, height, fmt = level.i32(entry + 8), level.i32(entry + 12), level.i32(entry + 16)
            base, clut = level.i32(entry + 0x2C), level.i32(entry + 0x14)
            if width <= 0 or height <= 0 or width > 1024 or height > 1024 or not 0x1E00 <= base < 0x4000:
                continue
            seen.setdefault((fmt, base, width, height, clut), entry)
    return sorted(seen)


def main():
    disc, tables, tool = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
    wanted = {int(a) for a in sys.argv[4:]}
    files = sorted((disc / 'BB').glob('*/*.BBK'), key=lambda p: int(p.parent.name) * 10 + int(p.stem))
    total = different = 0
    with tempfile.TemporaryDirectory() as work:
        work = pathlib.Path(work)
        for path in files:
            number = int(path.parent.name) * 10 + int(path.stem)
            if wanted and number not in wanted:
                continue
            try:
                level = bbk.Level(path, tables)
            except (SystemExit, zlib.error, struct.error):
                continue
            listed = textures(level, number == 98)
            if not listed:
                print(f'level {number}: no world in this file')
                continue
            # The GS memory image as the file holds it: the second zlib section.
            file = path.read_bytes()
            at = bbk.HEADER_SIZE
            size = struct.unpack_from('<I', file, at)[0]
            at += 4 + size
            size = struct.unpack_from('<I', file, at)[0]
            (work / 'tiles.bin').write_bytes(zlib.decompress(file[at + 4:at + 4 + size]))
            reference, lines, skipped = [], [], 0
            for fmt, base, width, height, clut in listed:
                rgba = level.gs.texture(fmt, base, width, height, clut)
                if rgba is None:
                    skipped += 1
                    continue
                reference.append(((fmt, base, width, height, clut), rgba))
                lines.append(f'{fmt:x} {base:x} {max(1, width // 64):x} {width:x} {height:x} {clut:x}')
            (work / 'list.txt').write_text('\n'.join(lines) + '\n')
            subprocess.run([tool, 'bbk', str(work / 'tiles.bin'), str(work / 'list.txt'), str(work / 'out.bin')], check=True, capture_output=True)
            mine = (work / 'out.bin').read_bytes()
            at = bad = 0
            formats, above = {}, []
            for (fmt, base, width, height, clut), rgba in reference:
                size = width * height * 4
                if mine[at:at + size] != rgba.tobytes():
                    bad += 1
                    if bad <= 3:
                        print(f'  DIFFERENT: {FORMATS.get(fmt, hex(fmt))} {width}x{height} at 0x{base:x} palette 0x{clut:x}')
                at += size
                formats[fmt] = formats.get(fmt, 0) + 1
                alpha = rgba[..., 3]
                if alpha.max() > 0x80:
                    above.append(f'{FORMATS.get(fmt, hex(fmt))} {width}x{height} at 0x{base:x}: {int((alpha > 0x80).sum())} texels, largest 0x{int(alpha.max()):02x}')
            total += len(reference)
            different += bad
            kinds = ', '.join(f'{FORMATS.get(f, hex(f))} {n}' for f, n in sorted(formats.items()))
            print(f'level {number}: {len(reference)} textures ({kinds}), {len(reference) - bad} equal, {bad} different, '
                  f'{skipped} in a format the viewer does not decode; alpha above 0x80 in {len(above)}')
            for line in above:
                print(f'  above 0x80: {line}')
    print(f'{total} textures, {different} different')
    print('PASS' if total and not different else 'FAIL')
    sys.exit(0 if total and not different else 1)


if __name__ == '__main__':
    main()
