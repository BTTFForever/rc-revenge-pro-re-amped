"""Shared ELF reading for the version tools (tools/version_diff.py, tools/gen_addrs.py, tools/addr_convert.py).

Reads the sections, the symbol table and the bytes of one of the game's executables (SLUS_201.53, SLES_500.77) with
only the standard library. Symbols are keyed so that the same symbol can be found in the other version:
  - a name that occurs once in the symbol table is its own key;
  - a name that occurs several times (static functions and variables of the same name in different files, local
    labels) is keyed as name#n, n counting its occurrences in symbol table order. Both executables come from the same
    sources and linker script, and their symbol tables list the same 5,525 entries in the same order (checked by
    version_diff.py), so the n-th occurrence is the same symbol in both.
"""
import os
import struct
from bisect import bisect_right
from collections import Counter

ELF_NAMES = {'usa': 'SLUS_201.53', 'eu': 'SLES_500.77'}


def game_elf(version):
    """The executable tools/setup.sh extracted for a version: game/<version>/disc/<name>."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(root, 'game', version, 'disc', ELF_NAMES[version])

SHT_NOBITS = 8
STT_NAMES = {0: 'NOTYPE', 1: 'OBJECT', 2: 'FUNC', 3: 'SECTION', 4: 'FILE'}


class Section:
    def __init__(self, name, kind, addr, offset, size, flags):
        self.name, self.kind, self.addr, self.offset, self.size, self.flags = name, kind, addr, offset, size, flags

    @property
    def end(self):
        return self.addr + self.size

    @property
    def loaded(self):
        return self.flags & 2 and self.addr != 0  # SHF_ALLOC


class Symbol:
    def __init__(self, index, name, value, size, kind, bind, shndx):
        self.index, self.name, self.value, self.size, self.kind, self.bind, self.shndx = index, name, value, size, kind, bind, shndx
        self.key = name

    def __repr__(self):
        return f'{self.key}@{self.value:#x}+{self.size:#x}'


class Elf:
    def __init__(self, path):
        self.path = path
        self.data = open(path, 'rb').read()
        d = self.data
        (shoff,) = struct.unpack_from('<I', d, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2E)
        raw = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * shentsize) for i in range(shnum)]
        strtab = raw[shstrndx]

        def cstr(base, off):
            end = d.index(b'\0', base + off)
            return d[base + off:end].decode('latin-1')

        self.sections = []
        for r in raw:
            self.sections.append(Section(cstr(strtab[4], r[0]), r[1], r[3], r[4], r[5], r[2]))
        self.section = {s.name: s for s in self.sections}
        symtab = self.section['.symtab']
        strs = self.section['.strtab']
        self.symbols = []
        for i in range(symtab.size // 16):
            name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', d, symtab.offset + i * 16)
            self.symbols.append(Symbol(i, cstr(strs.offset, name), value, size, STT_NAMES.get(info & 15, info & 15), info >> 4, shndx))
        counts = Counter(s.name for s in self.symbols)
        seen = Counter()
        for s in self.symbols:
            if counts[s.name] > 1:
                s.key = f'{s.name}#{seen[s.name]}'
                seen[s.name] += 1
        self.by_key = {s.key: s for s in self.symbols}
        self.functions = sorted((s for s in self.symbols if s.kind == 'FUNC' and s.shndx != 0), key=lambda s: s.value)
        # Symbols that name a place in the loaded image: functions, objects and labels, not sections, files or
        # absolute values.
        self.placed = sorted((s for s in self.symbols if s.kind in ('FUNC', 'OBJECT', 'NOTYPE') and 0 < s.shndx < 0xff00
                              and s.name and self.sections[s.shndx].loaded), key=lambda s: (s.value, -s.size, s.index))
        self._placed_addr = [s.value for s in self.placed]
        (self.gp,) = [s.value for s in self.symbols if s.name == '_gp']
        text = self.section['.text']
        self.text = text

    def section_of(self, address):
        for s in self.sections:
            if s.loaded and s.addr <= address < s.end:
                return s
        return None

    def read(self, address, size):
        s = self.section_of(address)
        if s is None or s.kind == SHT_NOBITS:
            return None
        off = s.offset + address - s.addr
        return self.data[off:off + size]

    def word(self, address):
        b = self.read(address, 4)
        return struct.unpack('<I', b)[0] if b and len(b) == 4 else None

    def code(self, function):
        return struct.unpack(f'<{function.size // 4}I', self.read(function.value, function.size - function.size % 4))

    def containing(self, address):
        """The best symbol for an address: the smallest sized symbol that contains it, else the nearest symbol at or
        before it in the same section (a label, or a symbol whose size does not cover the address)."""
        sec = self.section_of(address)
        if sec is None:
            return None
        i = bisect_right(self._placed_addr, address)
        best = None
        j = i - 1
        while j >= 0 and j >= i - 64:
            s = self.placed[j]
            if s.value <= address < s.value + s.size and (best is None or s.size < best.size):
                best = s
            j -= 1
        if best is not None:
            return best
        j = i - 1
        while j >= 0:
            s = self.placed[j]
            if sec is not None and not (sec.addr <= s.value < sec.end):
                return None
            # Prefer named symbols over compiler labels ($L.., gcc2_compiled.) at the same address.
            if not s.name.startswith(('$L', 'gcc2_compiled', '__gnu_compiled')):
                return s
            j -= 1
        return None

    def symbols_at(self, address):
        i = bisect_right(self._placed_addr, address)
        out = []
        j = i - 1
        while j >= 0 and self.placed[j].value == address:
            out.append(self.placed[j])
            j -= 1
        return out


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


# Opcodes whose 16-bit immediate can be the low half of an address (%lo or %gp_rel): addiu, daddiu, ori and every
# load and store, including the FPU's, the EE's 128-bit lq/sq and the VU0 macro mode's lqc2/sqc2.
LO_OPS = {0x09, 0x19, 0x0D, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E,
          0x31, 0x35, 0x36, 0x37, 0x39, 0x3D, 0x3E, 0x3F, 0x1A, 0x1B, 0x1E, 0x1F}
LUI = 0x0F


def masked(word):
    """The masked form compared by version_diff.py: j/jal targets and lui/LO_OPS immediates cleared."""
    op = word >> 26
    if op in (2, 3):
        return word & 0xFC000000
    if op == LUI or op in LO_OPS:
        return word & 0xFFFF0000
    return word


def references(elf, words, base):
    """Addresses a function's code computes: (index, kind, address) for each jal/j target, each lo carrier whose
    base register holds a lui value set earlier in the function (straight-line tracking, reset at a write of the
    register by any other instruction), and each $gp-relative access. kind is 'jal', 'j', 'hi/lo', 'gp'."""
    hi = {}
    out = []
    for i, w in enumerate(words):
        op = w >> 26
        rs, rt = (w >> 21) & 31, (w >> 16) & 31
        imm = w & 0xFFFF
        if op in (2, 3):
            target = ((base + 4 * i + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            out.append((i, 'jal' if op == 3 else 'j', target))
            continue
        if op == LUI:
            hi[rt] = imm << 16
            continue
        if op in LO_OPS:
            if rs == 28:
                out.append((i, 'gp', (elf.gp + sext16(imm)) & 0xFFFFFFFF))
            elif rs in hi:
                full = (hi[rs] | imm) if op == 0x0D else (hi[rs] + sext16(imm)) & 0xFFFFFFFF
                out.append((i, 'hi/lo', full))
            # addiu, daddiu, ori and the loads write rt; stores only read it.
            if op in (0x09, 0x19, 0x0D, 0x1A, 0x1B, 0x1E, 0x31, 0x35, 0x36, 0x37) or 0x20 <= op <= 0x27:
                if op not in (0x31, 0x35, 0x36):  # lwc1, ldc1, lqc2 write an FPU/VU register
                    hi.pop(rt, None)
            continue
        # Any other instruction that writes a GPR ends what we know of it.
        if op == 0:
            hi.pop((w >> 11) & 31, None)
        elif op in (0x08, 0x0A, 0x0B, 0x0C, 0x0E, 0x18, 0x1C):
            hi.pop(rt, None)
        elif op == 0x11 and rs in (0, 1, 2):  # mfc1, dmfc1, cfc1
            hi.pop(rt, None)
        elif op == 0x12 and rs in (1, 2):  # qmfc2, cfc2
            hi.pop(rt, None)
        elif op == 0x10:  # mfc0
            hi.pop(rt, None)
    return out


class VersionMap:
    """Maps an address of one version to the same place in the other: symbol + offset through the symbol keys, else
    the same offset in the same section (all sections but .text and .lit4 have the same sizes in both versions)."""

    def __init__(self, a, b):
        self.a, self.b = a, b

    def symbolic(self, address):
        """(symbol, offset) in version a, or (None, None)."""
        s = self.a.containing(address)
        if s is None:
            return None, None
        return s, address - s.value

    def map(self, address):
        """(address in b, how) or (None, why)."""
        s, off = self.symbolic(address)
        if s is not None:
            t = self.b.by_key.get(s.key)
            if t is None:
                return None, f'{s.key} missing'
            if s.size and t.size != s.size and off >= min(s.size, t.size):
                return None, f'{s.key} resized {s.size:#x}->{t.size:#x}'
            return t.value + off, 'symbol'
        sa = self.a.section_of(address)
        if sa is None:
            ends = [t for t in self.a.symbols if t.value == address and t.name in ('_end', 'end') and t.key in self.b.by_key]
            if ends:  # _end, _fbss and the like, at the end of a section
                return self.b.by_key[ends[0].key].value, 'symbol'
            return None, 'outside the image'
        sb = self.b.section.get(sa.name)
        if sb is None or sb.size != sa.size:
            return None, f'{sa.name} resized'
        return sb.addr + address - sa.addr, 'section'


def observed_pairs(a, b):
    """Every address the code of a function that is the same in both versions (masked words equal) computes, paired
    with what the same instruction computes in b: {address in a: address in b}. Addresses with two different
    partners are left out (a straight-line tracking slip)."""
    pairs, bad = {}, set()
    for fa in a.functions:
        fb = b.by_key.get(fa.key)
        if not fa.size or fb is None or fb.size != fa.size:
            continue
        wa, wb = a.code(fa), b.code(fb)
        if any(masked(x) != masked(y) for x, y in zip(wa, wb)):
            continue
        rb = {(i, k): t for i, k, t in references(b, wb, fb.value)}
        for i, kind, ta in references(a, wa, fa.value):
            tb = rb.get((i, kind))
            if tb is None or a.section_of(ta) is None:
                continue
            if pairs.setdefault(ta, tb) != tb:
                bad.add(ta)
    for t in bad:
        pairs.pop(t, None)
    return pairs


def lit4_map(a, b):
    """.lit4 address in a -> b. The section is a concatenation of each object file's float pool and lost a word in the
    Europe executable, so it is aligned word by word (difflib on the values); an aligned block of changed values maps
    one to one. Every $gp-relative read seen in the code of unchanged functions must agree with the alignment."""
    import difflib
    sa, sb = a.section['.lit4'], b.section['.lit4']
    wa = [a.read(x, 4) for x in range(sa.addr, sa.end, 4)]
    wb = [b.read(x, 4) for x in range(sb.addr, sb.end, 4)]
    out = {}
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, wa, wb, autojunk=False).get_opcodes():
        if tag == 'equal' or (tag == 'replace' and i2 - i1 == j2 - j1):
            for k in range(i2 - i1):
                out[sa.addr + 4 * (i1 + k)] = sb.addr + 4 * (j1 + k)
    for x, y in observed_pairs(a, b).items():
        if sa.addr <= x < sa.end and out.get(x) != y:
            raise SystemExit(f'.lit4 {x:#x}: the alignment gives {out.get(x)}, the code reads {y:#x}')
    return out
