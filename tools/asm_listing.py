#!/usr/bin/env python3
"""An annotated listing of a function's instructions, read from the recompiled output's comments: branch labels,
the symbols of the addresses the code builds (lui pairs, $gp), call targets and decoded jump tables.

Usage (from fork/):
  tools/asm_listing.py NAME [usa|eu]     the listing of one version
  tools/asm_listing.py NAME --diff       USA against Europe with addresses, labels and call targets normalised: what
                                         is left is register use, order, constants and code (for Europe variants)
"""
import bisect
import difflib
import pathlib
import re
import sys

sys.path.insert(0, 'tools')
import rcelf  # noqa: E402


def run(name, ver):
    out = {'usa': 'output', 'eu': 'output_eu'}[ver]
    elf = rcelf.Elf(rcelf.game_elf(ver))
    GP = {'usa': 0x32f570, 'eu': 0x32f7f0}[ver]
    syms = {}
    for m in re.finditer(r'inline constexpr uint32_t (\w+) = (0x[0-9a-f]+)u;', open(f'decomp/generated/{ver}/rc_addrs.h').read()):
        syms.setdefault(int(m.group(2), 16), m.group(1))
    addrs = sorted(syms)
    def desc(a):
        i = bisect.bisect_right(addrs, a) - 1
        if i < 0: return hex(a)
        s = addrs[i]; off = a - s
        return f'{syms[s]}' + (f'+{off:#x}' if off else '')
    paths = list(pathlib.Path(out).glob(f'{name}_0x*.cpp'))
    src = paths[0].read_text()
    INSN = re.compile(r'^\s*// (0x[0-9a-f]+): (0x[0-9a-f]+)\s+(\S+)\s*(.*?)(?: \(Delay Slot\))?\s*$')
    lines = []
    for l in src.splitlines():
        m = INSN.match(l)
        if m: lines.append((int(m.group(1), 16), m.group(3), m.group(4)))
    BR = re.compile(r'\. \+ 4 \+ \((-?0x[0-9A-Fa-f]+) << 2\)')
    fixed = []
    for pc, op, a in lines:
        m = BR.search(a)
        if m:
            a = BR.sub('L%x' % (pc + 4 + 4 * int(m.group(1), 16)), a)
        fixed.append((pc, op, a))
    have = {pc for pc, _, _ in fixed}
    start = fixed[0][0]; end = fixed[-1][0] + 4
    for pc in range(start, end, 4):
        if pc not in have:
            w = elf.word(pc)
            fixed.append((pc, 'nop' if w == 0 else '?? %08x' % w, ''))
    lines = sorted(fixed)
    targets = set()
    for pc, op, a in lines:
        t = re.search(r'L([0-9a-f]+)$', a)
        if t: targets.add(int(t.group(1), 16))
    END = syms and max(a for a, n in syms.items() if n == '_end')
    NODEST = re.compile(r'^(s[bhwdq]|swc1|sqc2|s[wd][lr]|b\w*|j|jr|jal|jalr|nop|mult\w*|div\w*|sync|c\.\w+\.s|ctc\d|mt\w+|break|syscall|madd\w*)$')
    vals = {}
    rawlui = {}
    jt = {}
    def good(v): return 0x100000 <= v < END + 0x100000
    for pc, op, a in lines:
        ann = ''
        regs = [x.strip() for x in a.split(',')]
        if pc in targets: print(f'L{pc:x}:')
        mm = re.match(r'^\$(\w+), (-?0x[0-9A-Fa-f]+|-?\d+)\(\$(\w+)\)$', a)
        if mm:
            base = '$' + mm.group(3); off = int(mm.group(2), 0)
            if base == '$gp': ann = desc(GP + off)
            elif base in vals and good((vals[base] + off) & 0xffffffff): ann = desc((vals[base] + off) & 0xffffffff)
        newval = None
        if op == 'lui': rawlui[regs[0]] = int(regs[1], 0) << 16
        if op in ('addu', 'daddu') and len(regs) == 3:
            for r in regs[1:]:
                if r in vals: rawlui[regs[0]] = vals[r]
                elif r in rawlui: rawlui[regs[0]] = rawlui[r]
        if op == 'lw' and mm and '$' + mm.group(3) in rawlui and '$' + mm.group(3) not in vals:
            jt[regs[0]] = (rawlui['$' + mm.group(3)] + int(mm.group(2), 0)) & 0xffffffff
        if op == 'lui':
            newval = int(regs[1], 0) << 16
        elif op in ('addiu', 'daddiu', 'ori') and len(regs) == 3 and regs[1] in vals:
            v = (vals[regs[1]] | int(regs[2], 0)) if op == 'ori' else (vals[regs[1]] + int(regs[2], 0)) & 0xffffffff
            newval = v
            if good(v): ann = desc(v)
        if op in ('jal', 'j'):
            t = re.search(r'([0-9A-Fa-f]+)$', a)
            if t: ann = desc(int(t.group(1), 16))
        print(f'  {pc:x}: {op:10} {a}' + (f'   ; {ann}' if ann else ''))
        if regs and regs[0].startswith('$') and not NODEST.match(op):
            if newval is None: vals.pop(regs[0], None)
            else: vals[regs[0]] = newval
        if op == 'jr' and a != '$ra':
            t = jt.get(a)
            if t is None: print('   ; jump table? unknown base')
            else:
                k = 0; ents = []
                while True:
                    w = elf.word(t + 4 * k)
                    if not (start <= w < end) or k > 64: break
                    ents.append(f'{k}:L{w:x}'); k += 1
                print(f'   ; jump table {t:#x} ({desc(t)}): ' + ' '.join(ents))


def listing(name, ver):
    import io, contextlib
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        run(name, ver)
    return buf.getvalue().splitlines()


def normalise(line):
    line = re.sub(r'^  [0-9a-f]+: ', '', line)
    line = re.sub(r'func_[0-9A-F]+', 'func', line)
    # A lui of an address in the image (0x10-0x3f) is normalised; other values (float constants) are kept.
    line = re.sub(r'^(lui +\$\w+, )0x(?:[1-3][0-9A-F])$', r'\1HI', line)
    line = re.sub(r'(-?0x[0-9A-Fa-f]+|-?\d+)(\(\$\w+\) +;)', r'LO\2', line)
    line = re.sub(r'(addiu +\$\w+, \$\w+, )-?0x[0-9A-Fa-f]+( +;)', r'\1LO\2', line)
    line = re.sub(r'^L[0-9a-f]+:', 'L:', line)
    line = re.sub(r'L[0-9a-f]+', 'L', line)
    line = re.sub(r'jump table 0x[0-9a-f]+ \([^)]*\)', 'jump table', line)
    return line


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    name = sys.argv[1]
    if len(sys.argv) > 2 and sys.argv[2] == '--diff':
        usa = [normalise(l) for l in listing(name, 'usa')]
        eu = [normalise(l) for l in listing(name, 'eu')]
        for line in difflib.unified_diff(usa, eu, 'usa', 'eu', lineterm='', n=1):
            print(line)
        return 0
    run(name, sys.argv[2] if len(sys.argv) > 2 else 'usa')
    return 0


if __name__ == '__main__':
    sys.exit(main())
