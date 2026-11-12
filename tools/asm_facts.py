#!/usr/bin/env python3
"""Prints a fact sheet read from the MIPS instructions of functions, to check Ghidra's pseudo-C against.

Usage (run from fork/):
  tools/asm_facts.py NAME [NAME...]        facts for these functions
  tools/asm_facts.py --unit RDR_SHAPE      facts for every function of a source unit (the 'cu' of symbols.json)
  tools/asm_facts.py --unit ALL --warnings-only    only the warnings, for the whole program

Facts per function: frame size; stack locals passed by address; absolute data addresses built by lui+addiu/ori or
lui+load/store (each with nearest symbol); $gp-relative accesses (read/write); calls with the constant or stack-address
arguments in $a0-$a3/$t0-$t3 (delay slot included); madd.s/msub.s and sqrt.s/rsqrt.s use.
Register tracking is straight-line only: a register keeps its constant until overwritten, and the call-clobbered ones
are forgotten after a call. Branch targets are not merged, so a value set in one arm can leak into another.

Warnings:
  label     a Ghidra label (_dma_N, &_something) stands for different addresses in different functions (or twice in one).
            Heuristic: per function, take the distinct underscore labels of its pseudo-C in order of appearance and the
            unnamed ('?', not a global, not a function) data addresses of its instructions in order of appearance.
            One label with one address, or as many labels as addresses (paired in order), gives an assignment. A
            label with differing assigned addresses across the program is flagged in each function using it. A label
            with several candidate addresses inside one function is flagged as ambiguous when it is the only label.
  empty     Ghidra's body is only 'return;' but the instructions store something.
  delay     the last 'jr $ra' has a store in its delay slot (Ghidra often drops it).
"""
import bisect
import json
import pathlib
import re
import sys
from collections import defaultdict

GP = 0x32f570
REGS = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
        's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']
REG = {n: i for i, n in enumerate(REGS)}
REG['s8'] = 30
CLOBBERED = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25]
ARG_REGS = [4, 5, 6, 7, 8, 9, 10, 11]
NO_DEST = re.compile(r'^(s[bhwdq]|s[wd][lr]|sc|b\w*|j|jr|nop|mult\w*|div\w*|sync|c\.\w+\.s|ctc\d|mt\w+|break|syscall)$')
INSN = re.compile(r'^\s*// (0x[0-9a-f]+): 0x[0-9a-f]+\s+(\S+)\s*(.*?)(?: \(Delay Slot\))?\s*$')
MEM = re.compile(r'^\$(\w+), (-?0x[0-9A-Fa-f]+|-?\d+)\(\$(\w+)\)$')
SIZES = {'u8': 1, 'u16': 2, 'u32': 4, 'int': 4, 'ee_float': 4, 'ee_fnptr': 4, 'uint8_t': 1, 'uint16_t': 2,
         'uint32_t': 4, 'int32_t': 4, 'float': 4, 's32': 4, 's16': 2, 's8': 1}


def num(text):
    return int(text, 0)


class Program:
    """Symbols, globals and Ghidra text of the whole game."""

    def __init__(self):
        self.symbols = json.load(open('decomp/generated/symbols.json'))
        self.by_name = {s['name']: s for s in self.symbols}
        sizeof = {m.group(1): num(m.group(2)) for m in re.finditer(
            r'sizeof\(rc::(\w+)\) == (0x[0-9a-f]+)', open('decomp/generated/rc_layout_check.cpp').read())}
        self.globals = {}  # address -> (name, size or None)
        for m in re.finditer(r'inline (.+?) &(\w+)\(\) \{ return \*reinterpret_cast<(.+?)>\(guest_host\((0x[0-9a-f]+)u\)\); \}',
                             open('decomp/generated/rc_globals.h').read()):
            self.globals[num(m.group(4))] = (m.group(2), self.type_size(m.group(3), sizeof))
        self.global_names = {n for n, _ in self.globals.values()}
        self.global_addrs = sorted(self.globals)
        self.functions = {}  # address -> (name, size); the output/ files cover SDK code too
        self.files = {}  # name -> (address, path)
        for path in sorted(pathlib.Path('output').glob('*_0x*.cpp')):
            m = re.match(r'(.+)_0x([0-9a-f]+)\.cpp$', path.name)
            address = int(m.group(2), 16)
            self.files[m.group(1)] = (address, path)
            self.functions[address] = [m.group(1), 4]
        for s in self.symbols:
            self.functions[s['address']] = [s['name'], s['size']]
        for address, (name, path) in {a: (n, p) for n, (a, p) in self.files.items()}.items():
            if self.functions[address][1] == 4:  # only the file's own range is known
                m = re.search(r'// Address: (0x[0-9a-f]+) - (0x[0-9a-f]+)', path.read_text(errors='replace')[:600])
                if m:
                    self.functions[address][1] = num(m.group(2)) - num(m.group(1))
        self.function_addrs = sorted(self.functions)
        self.pseudo = {}  # address -> Ghidra text
        text = open('decomp/generated/pseudo_all.c', errors='replace').read()
        for part in re.split(r'(?=^/\* ==== )', text, flags=re.M):
            m = re.match(r'/\* ==== (\S+) @ ([0-9a-f]+) \*/', part)
            if m:
                self.pseudo[int(m.group(2), 16)] = part

    @staticmethod
    def type_size(type_text, sizeof):
        dims = re.findall(r'\[(\d+)\]', type_text)
        base = re.sub(r'\(\*\)|\[\d+\]', '', type_text).strip()
        size = SIZES.get(base) or sizeof.get('_' + base) or sizeof.get(base)
        if not size:
            return None
        for d in dims:
            size *= int(d)
        return size

    def target(self, text):
        """Address of a jal operand: 0x..., func_XXXX, or a function name."""
        if text.startswith('0x'):
            return num(text)
        if text.startswith('func_'):
            return int(text[5:], 16)
        if text in self.files:
            return self.files[text][0]
        return None

    def resolve(self, address):
        """Nearest symbol: global, global+offset, function, function+offset, or ?."""
        i = bisect.bisect_right(self.global_addrs, address) - 1
        if i >= 0:
            start = self.global_addrs[i]
            name, size = self.globals[start]
            nxt = self.global_addrs[i + 1] if i + 1 < len(self.global_addrs) else start + 0x10000
            if address < start + (min(size, nxt - start) if size else nxt - start):
                return (name, address - start, 'global')
        i = bisect.bisect_right(self.function_addrs, address) - 1
        if i >= 0:
            start = self.function_addrs[i]
            name, size = self.functions[start]
            if address < start + max(size, 4):
                return (name, address - start, 'function')
        return (None, 0, '?')

    def describe(self, address):
        name, off, kind = self.resolve(address)
        if kind == '?':
            return f'? 0x{address:x}'
        return f'0x{address:x} {name}' + (f'+0x{off:x}' if off else '') + ('()' if kind == 'function' else '')


def parse_listing(path):
    out = []
    for line in open(path, errors='replace'):
        m = INSN.match(line)
        if m:
            out.append((int(m.group(1), 16), m.group(2), m.group(3), '(Delay Slot)' in line))
    return out


def analyse(prog, name):
    address, path = prog.files[name]
    code = parse_listing(path)
    regs = {0: ('c', 0), 28: ('c', GP), 29: ('sp', 0)}  # reg -> ('c', value) | ('sp', offset)
    facts = {'frame': None, 'locals': [], 'abs': [], 'gp': [], 'calls': [], 'fp': set(), 'stores': 0,
             'last_jr': None, 'code': code}
    pending = None  # call waiting for its delay slot

    def get(r):
        return regs.get(r)

    def setreg(r, v):
        if r:
            if v is None:
                regs.pop(r, None)
            else:
                regs[r] = v

    for index, (pc, op, args, _) in enumerate(code):
        parts = [a.strip() for a in args.split(',')] if args else []
        if op in ('madd.s', 'msub.s', 'madda.s', 'msuba.s', 'sqrt.s', 'rsqrt.s'):
            facts['fp'].add(op)
        mem = MEM.match(args) if args else None
        if mem and (op[0] in 'ls') and op not in ('sll', 'sra', 'srl', 'slt', 'sltu', 'sltiu', 'slti', 'sllv'):
            base = REG.get(mem.group(3))
            off = num(mem.group(2))
            write = op[0] == 's'
            if write:
                facts['stores'] += 1
            b = get(base) if base is not None else None
            if base == 28:
                facts['gp'].append((pc, op, GP + off, write))
            elif b and b[0] == 'c':
                facts['abs'].append((pc, b[1] + off, 'store' if write else 'load', op))
            if not write and mem.group(1) in REG:
                setreg(REG[mem.group(1)], None)
        elif op == 'lui' and len(parts) == 2:
            setreg(REG[parts[0][1:]], ('c', (num(parts[1]) & 0xffff) << 16))
        elif op in ('addiu', 'addi', 'daddiu') and len(parts) == 3:
            rd, rs, imm = REG[parts[0][1:]], REG[parts[1][1:]], num(parts[2])
            if rd == 29 and rs == 29:
                if facts['frame'] is None and imm < 0:
                    facts['frame'] = -imm
                setreg(rd, ('sp', 0))
            elif rs == 29:
                facts['locals'].append((pc, parts[0], imm, rd in (4, 5, 6, 7)))
                setreg(rd, ('sp', imm))
            else:
                v = get(rs)
                if v and v[0] == 'c':
                    value = (v[1] + imm) & 0xffffffff
                    setreg(rd, ('c', value))
                    if rs != 0 and value >= 0x100000:
                        facts['abs'].append((pc, value, 'addr', op))
                else:
                    setreg(rd, None)
        elif op == 'ori' and len(parts) == 3:
            v = get(REG[parts[1][1:]])
            if v and v[0] == 'c':
                value = v[1] | num(parts[2])
                setreg(REG[parts[0][1:]], ('c', value))
                if REG[parts[1][1:]] != 0 and value >= 0x100000:
                    facts['abs'].append((pc, value, 'addr', op))
            else:
                setreg(REG[parts[0][1:]], None)
        elif op in ('or', 'addu', 'daddu') and len(parts) == 3 and parts[2] == '$zero':
            setreg(REG[parts[0][1:]], get(REG[parts[1][1:]]))
        elif op == 'jal' and parts:
            pending = [pc, prog.target(parts[0])]
        elif op == 'jalr':
            pending = [pc, None]
        elif parts and parts[0].startswith('$') and not NO_DEST.match(op) and parts[0][1:] in REG:
            setreg(REG[parts[0][1:]], None)
        if op == 'jr' and parts == ['$ra']:
            delay = code[index + 1] if index + 1 < len(code) else None
            facts['last_jr'] = (pc, delay)
        if pending and pc != pending[0]:  # the delay slot has just been processed
            known = []
            for r in ARG_REGS:
                v = get(r)
                if v:
                    known.append((REGS[r], v))
            facts['calls'].append((pending[0], pending[1], known, {r: v for r, v in zip(ARG_REGS, [get(r) for r in ARG_REGS])}))
            for r in CLOBBERED:
                regs.pop(r, None)
            pending = None
    return facts


def store_in_delay(facts):
    last = facts['last_jr']
    if last and last[1]:
        pc, op, args, _ = last[1]
        if op[0] == 's' and MEM.match(args) and op not in ('sll', 'sra', 'srl', 'slt', 'sltu', 'sltiu', 'slti'):
            return last[1]
    return None


def ghidra_body(prog, address):
    text = prog.pseudo.get(address)
    if text is None:
        return None
    body = text[text.find('{') + 1:text.rfind('}')]
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    body = re.sub(r'^\s*[\w ]+\**\s*\*?\w+(\[\d+\])?;\s*$', '', body, flags=re.M)  # local declarations
    return re.sub(r'\s+', ' ', body).strip()


def ghidra_labels(prog, address):
    """Underscore labels of a function's pseudo-C in order of first appearance."""
    text = prog.pseudo.get(address, '')
    text = text[text.find('{'):] if '{' in text else text
    seen = []
    for m in re.finditer(r'(?<![\w.])(_dma_\d+|&_[A-Za-z_]\w*)', text):
        label = m.group(1).lstrip('&')
        if label not in prog.global_names and label not in prog.by_name and label not in seen:
            seen.append(label)
    return seen


def pseudo_calls(prog, address):
    """(callee, [argument texts]) for each call in the pseudo-C, in text order."""
    text = prog.pseudo.get(address, '')
    text = text[text.find('{'):]
    calls = []
    for m in re.finditer(r'\b([A-Za-z_]\w*)\(', text):
        if m.group(1) in ('if', 'while', 'for', 'switch', 'sizeof', 'return'):
            continue
        depth, i, args, cur = 1, m.end(), [], ''
        while i < len(text) and depth:
            c = text[i]
            depth += c == '('
            depth -= c == ')'
            if depth == 1 and c == ',':
                args.append(cur)
                cur = ''
            elif depth:
                cur += c
            i += 1
        args.append(cur)
        calls.append((m.group(1), [a.strip() for a in args]))
    return calls


def label_args(prog, name, facts, labels):
    """Pairs labels passed as call arguments with the constant held by the same argument register in the nth call
    to the same callee. Returns label -> [addresses] in order of discovery."""
    found = {}
    asm = defaultdict(list)
    for pc, target, known, regs in facts['calls']:
        asm[prog.functions[target][0] if target in prog.functions else None].append(regs)
    seen = defaultdict(int)
    for callee, args in pseudo_calls(prog, prog.files[name][0]):
        index = seen[callee]
        seen[callee] += 1
        for k, arg in enumerate(args[:8]):
            m = re.fullmatch(r'(?:\([^()]*\)\s*)*&?(_dma_\d+|_\w+)', arg)
            if m and m.group(1) in labels and index < len(asm[callee]):
                v = asm[callee][index].get(ARG_REGS[k])
                if v and v[0] == 'c' and prog.resolve(v[1])[2] == '?':
                    found.setdefault(m.group(1), [])
                    if v[1] not in found[m.group(1)]:
                        found[m.group(1)].append(v[1])
    return found


def unnamed_addresses(prog, facts):
    seen = []
    for _, value, _, _ in facts['abs']:
        if prog.resolve(value)[2] == '?' and 0x100000 <= value < 0x2000000 and value not in seen:
            seen.append(value)
    return seen


def print_facts(prog, name, facts, warnings):
    address = prog.files[name][0]
    print(f'== {name} @0x{address:x}  ({len(facts["code"])} instructions)')
    print(f'  frame: {"%d bytes (0x%x)" % (facts["frame"], facts["frame"]) if facts["frame"] else "none (no addiu $sp,$sp,-N)"}')
    for pc, reg, off, is_arg in facts['locals']:
        print(f'  local: 0x{pc:x} {reg} = $sp+0x{off:x}' + ('  (argument, passed by address)' if is_arg else ''))
    seen = set()
    for pc, value, kind, op in facts['abs']:
        if (value, kind) in seen:
            continue
        seen.add((value, kind))
        print(f'  abs {kind:5s} 0x{pc:x} {prog.describe(value)}')
    for pc, op, value, write in facts['gp']:
        print(f'  gp  {"write" if write else "read "} 0x{pc:x} {op} {prog.describe(value)}')
    for pc, target, known, _ in facts['calls']:
        callee = prog.describe(target) if target else 'indirect (jalr)'
        arg = ', '.join(f'{r}=' + (f'sp+0x{v[1]:x}' if v[0] == 'sp' else prog.describe(v[1]) if v[1] >= 0x100000 else f'{v[1]:#x}')
                        for r, v in known)
        print(f'  call 0x{pc:x} {callee}' + (f'  [{arg}]' if arg else ''))
    print(f'  fp: {", ".join(sorted(facts["fp"])) if facts["fp"] else "no madd/msub/sqrt/rsqrt"}')
    for w in warnings:
        print(f'  WARNING {w}')
    print()


def main():
    args = sys.argv[1:]
    warnings_only = '--warnings-only' in args
    args = [a for a in args if a != '--warnings-only']
    if not args:
        print(__doc__)
        return 1
    prog = Program()
    if args[0] == '--unit':
        names = [s['name'] for s in prog.symbols if args[1] == 'ALL' or s.get('cu') == args[1]]
        names = [n for n in names if n in prog.files]
    else:
        names = args
        for n in names:
            if n not in prog.files:
                sys.exit(f'{n}: no output/{n}_0x*.cpp')
    facts = {n: analyse(prog, n) for n in names}
    warnings = defaultdict(list)
    # labels: name -> {function -> [addresses]}
    assigned = defaultdict(dict)
    for n, f in facts.items():
        labels = ghidra_labels(prog, prog.files[n][0])
        if not labels:
            continue
        sure = label_args(prog, n, f, labels)
        for label, a in sure.items():
            assigned[label][n] = a
        left = [l for l in labels if l not in sure]
        used = {a for v in sure.values() for a in v}
        addrs = [a for a in unnamed_addresses(prog, f) if a not in used]
        if left and len(left) == len(addrs):
            for label, a in zip(left, addrs):
                assigned[label][n] = [a]
    for label, uses in assigned.items():
        distinct = {a for v in uses.values() for a in v}
        if len(distinct) < 2:
            continue
        for n, v in uses.items():
            others = {m: w for m, w in uses.items() if m != n and not set(w) & set(v)}
            hex_ = lambda l: ', '.join(f'0x{a:x}' for a in l)
            if len(v) > 1:
                warnings[n].append(f'label: {label} stands for {len(v)} addresses in this function: {hex_(v)}')
            if others:
                sample = ', '.join(f'{m} 0x{w[0]:x}' for m, w in sorted(others.items())[:3])
                warnings[n].append(f'label: {label} is {hex_(v)} here but elsewhere {sample}'
                                   + (f' (+{len(others) - 3} more)' if len(others) > 3 else ''))
    for n, f in facts.items():
        address = prog.files[n][0]
        body = ghidra_body(prog, address)
        if body is not None and re.fullmatch(r'(return;)?', body) and f['stores']:
            stores = [f'0x{pc:x} {op} {a}' for pc, op, a, _ in f['code'] if op[0] == 's' and MEM.match(a)
                      and op not in ('sll', 'sra', 'srl', 'slt', 'sltu', 'sltiu', 'slti')]
            warnings[n].append('empty: Ghidra body is empty but the code stores: ' + '; '.join(stores[:3]))
        d = store_in_delay(f)
        if d:
            m = MEM.match(d[2])
            note = ''
            if m and m.group(3) == 'gp':
                note = f' = {prog.describe(GP + num(m.group(2)))}'
                gname = prog.resolve(GP + num(m.group(2)))[0]
                body = ghidra_body(prog, address) or ''
                note += ' (named in Ghidra text)' if gname and re.search(r'\b' + re.escape(gname) + r'\b', body) else ' (NOT named in Ghidra text)'
            warnings[n].append(f'delay: store in the delay slot of the final jr $ra: 0x{d[0]:x} {d[1]} {d[2]}{note}')
    for n in names:
        if warnings_only:
            for w in warnings.get(n, []):
                print(f'{n} @0x{prog.files[n][0]:x} [{prog.by_name.get(n, {}).get("cu", "?")}] {w}')
        else:
            print_facts(prog, n, facts[n], warnings.get(n, []))
    return 0


if __name__ == '__main__':
    sys.exit(main())
