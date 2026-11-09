#!/usr/bin/env python3
"""Disassembles the game's VU1 microprograms into labelled listings, one per program.

The programs live in .vutext as overlays described by .DVP.ovlytab. The ELF's symbol table still
holds the assembler's labels, so branches and entry points keep their original names.

Usage: vu1dis.py <elf> <output-dir>
"""
import collections
import pathlib
import re
import struct
import sys

UPPER_BC = ['ADD', 'SUB', 'MADD', 'MSUB', 'MAX', 'MINI', 'MUL']
UPPER = {0x1C: 'MULq', 0x1D: 'MAXi', 0x1E: 'MULi', 0x1F: 'MINIi', 0x20: 'ADDq', 0x21: 'MADDq', 0x22: 'ADDi',
         0x23: 'MADDi', 0x24: 'SUBq', 0x25: 'MSUBq', 0x26: 'SUBi', 0x27: 'MSUBi', 0x28: 'ADD', 0x29: 'MADD',
         0x2A: 'MUL', 0x2B: 'MAX', 0x2C: 'SUB', 0x2D: 'MSUB', 0x2E: 'OPMSUB', 0x2F: 'MINI'}
UPPER_SPECIAL_BC = {0: 'ADDA', 4: 'SUBA', 8: 'MADDA', 12: 'MSUBA', 0x18: 'MULA'}
UPPER_SPECIAL = {0x10: 'ITOF0', 0x11: 'ITOF4', 0x12: 'ITOF12', 0x13: 'ITOF15', 0x14: 'FTOI0', 0x15: 'FTOI4',
                 0x16: 'FTOI12', 0x17: 'FTOI15', 0x1C: 'MULAq', 0x1D: 'ABS', 0x1E: 'MULAi', 0x1F: 'CLIP', 0x20: 'ADDAq',
                 0x21: 'MADDAq', 0x22: 'ADDAi', 0x23: 'MADDAi', 0x24: 'SUBAq', 0x25: 'MSUBAq', 0x26: 'SUBAi',
                 0x27: 'MSUBAi', 0x28: 'ADDA', 0x29: 'MADDA', 0x2A: 'MULA', 0x2C: 'SUBA', 0x2D: 'MSUBA', 0x2E: 'OPMULA',
                 0x2F: 'NOP'}
LOWER_SPECIAL = {0x30: 'MOVE', 0x31: 'MR32', 0x34: 'LQI', 0x35: 'SQI', 0x36: 'LQD', 0x37: 'SQD', 0x38: 'DIV', 0x39: 'SQRT',
                 0x3A: 'RSQRT', 0x3B: 'WAITQ', 0x3C: 'MTIR', 0x3D: 'MFIR', 0x3E: 'ILWR', 0x3F: 'ISWR', 0x40: 'RNEXT',
                 0x41: 'RGET', 0x42: 'RINIT', 0x43: 'RXOR', 0x64: 'MFP', 0x68: 'XTOP', 0x69: 'XITOP', 0x6C: 'XGKICK',
                 0x70: 'ESADD', 0x71: 'ERSADD', 0x72: 'ELENG', 0x73: 'ERLENG', 0x74: 'EATANxy', 0x75: 'EATANxz',
                 0x76: 'ESUM', 0x78: 'ESQRT', 0x79: 'ERSQRT', 0x7A: 'ERCPR', 0x7B: 'WAITP', 0x7C: 'ESIN', 0x7D: 'EATAN',
                 0x7E: 'EEXP'}
LOWER = {0x00: 'LQ', 0x01: 'SQ', 0x04: 'ILW', 0x05: 'ISW', 0x08: 'IADDIU', 0x09: 'ISUBIU', 0x10: 'FCEQ', 0x11: 'FCSET',
         0x12: 'FCAND', 0x13: 'FCOR', 0x14: 'FSEQ', 0x15: 'FSSET', 0x16: 'FSAND', 0x17: 'FSOR', 0x18: 'FMEQ', 0x1A: 'FMAND',
         0x1B: 'FMOR', 0x1C: 'FCGET', 0x20: 'B', 0x21: 'BAL', 0x24: 'JR', 0x25: 'JALR', 0x28: 'IBEQ', 0x29: 'IBNE',
         0x2C: 'IBLTZ', 0x2D: 'IBGTZ', 0x2E: 'IBLEZ', 0x2F: 'IBGEZ'}
BC = 'xyzw'


def dest(word):
    bits = (word >> 21) & 15
    return ''.join(c for c, b in zip('xyzw', (8, 4, 2, 1)) if bits & b)


def fields(word):
    return (word >> 16) & 31, (word >> 11) & 31, (word >> 6) & 31


def upper_text(word):
    ft, fs, fd = fields(word)
    d = dest(word)
    op = word & 63
    if op < 0x1C:
        name = UPPER_BC[op >> 2]
        return f'{name}{BC[op & 3]}.{d} vf{fd:02}, vf{fs:02}, vf{ft:02}{BC[op & 3]}'
    if op in UPPER:
        name = UPPER[op]
        if op >= 0x28:
            return f'{name}.{d} vf{fd:02}, vf{fs:02}, vf{ft:02}'
        return f'{name}.{d} vf{fd:02}, vf{fs:02}, {name[-1].upper()}'
    if op >= 0x3C:
        special = (fd << 2) | (op & 3)
        if special < 0x10 or 0x18 <= special < 0x1C:
            base = special & ~3
            return f'{UPPER_SPECIAL_BC[base]}{BC[special & 3]}.{d} ACC, vf{fs:02}, vf{ft:02}{BC[special & 3]}'
        name = UPPER_SPECIAL.get(special)
        if name is None:
            return None
        if name == 'NOP':
            return 'NOP'
        if name == 'CLIP':
            return f'CLIPw.{d} vf{fs:02}, vf{ft:02}w'
        if name.startswith(('ITOF', 'FTOI', 'ABS')):
            return f'{name}.{d} vf{ft:02}, vf{fs:02}'
        if name.endswith(('q', 'i')):
            return f'{name}.{d} ACC, vf{fs:02}, {name[-1].upper()}'
        return f'{name}.{d} ACC, vf{fs:02}, vf{ft:02}'
    return None


def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def lower_text(word, pc, label_of):
    ft, fs, fd = fields(word)
    d = dest(word)
    if word & 0x80000000:
        op = word & 63
        if op == 0x30:
            return f'IADD vi{fd:02}, vi{fs:02}, vi{ft:02}'
        if op == 0x31:
            return f'ISUB vi{fd:02}, vi{fs:02}, vi{ft:02}'
        if op == 0x32:
            return f'IADDI vi{ft:02}, vi{fs:02}, {signed(fd, 5)}'
        if op == 0x34:
            return f'IAND vi{fd:02}, vi{fs:02}, vi{ft:02}'
        if op == 0x35:
            return f'IOR vi{fd:02}, vi{fs:02}, vi{ft:02}'
        if op < 0x3C:
            return None
        name = LOWER_SPECIAL.get((fd << 2) | (op & 3))
        if name is None:
            return None
        fsf, ftf = BC[(word >> 21) & 3], BC[(word >> 23) & 3]
        if name in ('MOVE', 'MR32'):
            return f'{name}.{d} vf{ft:02}, vf{fs:02}'
        if name == 'LQI':
            return f'LQI.{d} vf{ft:02}, (vi{fs:02}++)'
        if name == 'SQI':
            return f'SQI.{d} vf{fs:02}, (vi{ft:02}++)'
        if name == 'LQD':
            return f'LQD.{d} vf{ft:02}, (--vi{fs:02})'
        if name == 'SQD':
            return f'SQD.{d} vf{fs:02}, (--vi{ft:02})'
        if name == 'DIV':
            return f'DIV Q, vf{fs:02}{fsf}, vf{ft:02}{ftf}'
        if name == 'SQRT':
            return f'SQRT Q, vf{ft:02}{ftf}'
        if name == 'RSQRT':
            return f'RSQRT Q, vf{fs:02}{fsf}, vf{ft:02}{ftf}'
        if name in ('WAITQ', 'WAITP'):
            return name
        if name == 'MTIR':
            return f'MTIR vi{ft:02}, vf{fs:02}{fsf}'
        if name == 'MFIR':
            return f'MFIR.{d} vf{ft:02}, vi{fs:02}'
        if name == 'ILWR':
            return f'ILWR.{d} vi{ft:02}, (vi{fs:02})'
        if name == 'ISWR':
            return f'ISWR.{d} vi{ft:02}, (vi{fs:02})'
        if name in ('RNEXT', 'RGET'):
            return f'{name}.{d} vf{ft:02}, R'
        if name in ('RINIT', 'RXOR'):
            return f'{name} R, vf{fs:02}{fsf}'
        if name == 'MFP':
            return f'MFP.{d} vf{ft:02}, P'
        if name in ('XTOP', 'XITOP'):
            return f'{name} vi{ft:02}'
        if name == 'XGKICK':
            return f'XGKICK vi{fs:02}'
        if name.startswith('E'):
            return f'{name} P, vf{fs:02}' + ('' if name in ('ESADD', 'ERSADD', 'ELENG', 'ERLENG', 'ESUM') else fsf)
        return name
    op = word >> 25
    name = LOWER.get(op)
    if name is None:
        return None
    imm11 = signed(word & 0x7FF, 11)
    imm15 = ((word >> 10) & 0x7800) | (word & 0x7FF)
    imm12 = ((word >> 10) & 0x800) | (word & 0x7FF)
    if name in ('LQ', 'ILW'):
        return f'{name}.{d} {"vf" if name == "LQ" else "vi"}{ft:02}, {imm11}(vi{fs:02})'
    if name in ('SQ', 'ISW'):
        return f'{name}.{d} {"vf" if name == "SQ" else "vi"}{fs if name == "SQ" else ft:02}, {imm11}(vi{ft if name == "SQ" else fs:02})'
    if name in ('IADDIU', 'ISUBIU'):
        return f'{name} vi{ft:02}, vi{fs:02}, {imm15}'
    if name == 'FCGET':
        return f'FCGET vi{ft:02}'
    if name in ('FCEQ', 'FCAND', 'FCOR', 'FCSET'):
        return f'{name} {"" if name == "FCSET" else "vi01, "}0x{word & 0xFFFFFF:x}'
    if name.startswith('FS'):
        return f'{name} ' + (f'0x{imm12:x}' if name == 'FSSET' else f'vi{ft:02}, 0x{imm12:x}')
    if name.startswith('FM'):
        return f'{name} vi{ft:02}, vi{fs:02}'
    if name in ('JR', 'JALR'):
        return f'{name} ' + (f'vi{ft:02}, vi{fs:02}' if name == 'JALR' else f'vi{fs:02}')
    target = pc + 8 + imm11 * 8
    where = label_of(target)
    if name == 'B':
        return f'B {where}'
    if name == 'BAL':
        return f'BAL vi{ft:02}, {where}'
    if name in ('IBEQ', 'IBNE'):
        return f'{name} vi{ft:02}, vi{fs:02}, {where}'
    return f'{name} vi{fs:02}, {where}'


def main():
    elf, out = sys.argv[1], pathlib.Path(sys.argv[2])
    data = pathlib.Path(elf).read_bytes()
    shoff = struct.unpack_from('<I', data, 32)[0]
    count, strndx = struct.unpack_from('<HH', data, 48)
    sections = [struct.unpack_from('<10I', data, shoff + 40 * i) for i in range(count)]
    names = sections[strndx][4]

    def name_of(section):
        start = names + section[0]
        return data[start:data.index(b'\0', start)].decode()

    by_name = {name_of(s): s for s in sections}
    symtab = next(s for s in sections if s[1] == 2)
    strtab = sections[symtab[6]][4]
    symbols = []
    for pos in range(symtab[4], symtab[4] + symtab[5], 16):
        name_off, value = struct.unpack_from('<II', data, pos)
        end = data.index(b'\0', strtab + name_off)
        name = data[strtab + name_off:end].decode('latin1')
        if name and not re.match(r'\.(vu|vif|gif|dma)\.\d', name):
            symbols.append((value, name))
    symbols.sort()

    table, strings, text = by_name['.DVP.ovlytab'], by_name['.DVP.ovlystrtab'], by_name['.vutext']
    groups = collections.OrderedDict()
    for offset in range(0, table[5], 12):
        name_addr, lma, vma = struct.unpack_from('<III', data, table[4] + offset)
        start = strings[4] + name_addr - strings[3]
        overlay = data[start:data.index(b'\0', start)].decode()
        group = re.search(r'\.(\d+)\.\d+\.\d+$', overlay).group(1)
        groups.setdefault(group, []).append((lma, vma, by_name[overlay][5]))

    out.mkdir(parents=True, exist_ok=True)
    unknown = 0
    total = 0
    for group, overlays in groups.items():
        first = overlays[0][0]
        before = [n for v, n in symbols if first - 0x40 <= v <= first and n.endswith('Prog')]
        program = before[-1] if before else f'vu1_{group}'
        image = {}
        labels = collections.defaultdict(list)
        for lma, vma, size in overlays:
            for offset in range(0, size - 7, 8):
                lower, upper = struct.unpack_from('<II', data, text[4] + lma - text[3] + offset)
                image[vma + offset] = (lower, upper)
            for value, name in symbols:
                if lma <= value < lma + size:
                    labels[vma + value - lma].append(name)
        referenced = set()

        def label_of(target):
            if target in labels:
                return labels[target][0]
            referenced.add(target)
            return f'L_{target:04x}'

        lines = [f'; {program}: VU1 microprogram, overlay group {group}, {len(image)} instructions', '']
        for pc in sorted(image):
            lower, upper = image[pc]
            flags = ''.join(c for c, bit in (('[I]', 31), ('[E]', 30), ('[M]', 29), ('[D]', 28), ('[T]', 27)) if upper >> bit & 1)
            up = upper_text(upper & 0x07FFFFFF)
            if upper >> 31 & 1:
                low = f'LOI {struct.unpack("<f", struct.pack("<I", lower))[0]:g}'
            else:
                low = lower_text(lower, pc, label_of)
            total += 1
            if up is None or low is None:
                unknown += 1
            lines.append((pc, f'{pc:04x}:  {flags:6} {up or f"<upper {upper:08x}>":34} {low or f"<lower {lower:08x}>"}'))
        rendered = []
        for item in lines:
            if isinstance(item, str):
                rendered.append(item)
                continue
            pc, text_line = item
            for name in labels.get(pc, []):
                rendered.append(f'{name}:')
            if pc in referenced and pc not in labels:
                rendered.append(f'L_{pc:04x}:')
            rendered.append(text_line)
        (out / f'{program}.vsm').write_text('\n'.join(rendered) + '\n')
        code = bytearray(0x4000) # The program as VU1 micro memory, for vu1_check.cpp's synthetic runs.
        for pc, (lower, upper) in image.items():
            struct.pack_into('<II', code, pc, lower, upper)
        (out / f'{program}.bin').write_bytes(code)
        print(f'{program}: group {group}, {len(image)} instructions, {sum(len(v) for v in labels.values())} labels')
    print(f'{total} instructions, {unknown} undecoded')


if __name__ == '__main__':
    main()
