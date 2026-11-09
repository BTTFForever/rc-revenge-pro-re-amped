#!/usr/bin/env python3
"""Writes one 16 KB VU1 code image per .DVP overlay group of a PS2 ELF, for vu1_recomp.

Usage: make_vu1_images.py <elf> <output-dir>

The bytes each overlay uploads live in .vutext at the overlay's load address. Space after
a group stays zero: the runtime matches compiled images by longest prefix, so stale code
left above a shorter program still matches.
"""
import pathlib
import re
import struct
import sys

CODE_SIZE = 0x4000


def main():
    elf, out = sys.argv[1], pathlib.Path(sys.argv[2])
    data = pathlib.Path(elf).read_bytes()
    shoff = struct.unpack_from('<I', data, 32)[0]
    count, strndx = struct.unpack_from('<HH', data, 48)
    sections = [struct.unpack_from('<10I', data, shoff + 40 * i) for i in range(count)]
    names = sections[strndx][4]

    def name(section):
        start = names + section[0]
        return data[start:data.index(b'\0', start)].decode()

    by_name = {name(s): s for s in sections}
    table, strings, text = by_name['.DVP.ovlytab'], by_name['.DVP.ovlystrtab'], by_name['.vutext']
    groups = {}
    for offset in range(0, table[5], 12):
        name_addr, lma, vma = struct.unpack_from('<III', data, table[4] + offset)
        start = strings[4] + name_addr - strings[3]
        overlay = data[start:data.index(b'\0', start)].decode()
        group = re.search(r'\.(\d+)\.\d+\.\d+$', overlay).group(1)
        groups.setdefault(group, []).append((lma, vma, by_name[overlay][5]))

    out.mkdir(parents=True, exist_ok=True)
    for group, overlays in groups.items():
        image = bytearray(CODE_SIZE)
        for lma, vma, size in overlays:
            source = text[4] + lma - text[3]
            image[vma:vma + size] = data[source:source + size]
        (out / f'vu1_group_{group}.bin').write_bytes(image)
        print(group, len(overlays), 'overlays,', sum(o[2] for o in overlays), 'bytes')


main()
