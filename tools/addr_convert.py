#!/usr/bin/env python3
"""Finds the guest addresses written as hex literals in the native code and rewrites them as symbols (rc::sym).

Usage (from fork/):
  tools/addr_convert.py report          classify every literal; write the ones that need a person to
                                        build-*/addr_review_todo.txt (one line each, with its source line)
  tools/addr_convert.py apply           rewrite the literals classified as addresses, in place
  tools/addr_convert.py check           exit 1 if a literal in the image's address range is neither converted nor
                                        listed in tools/addr_review.txt, or if code names a recompiled function as
                                        Name_0x<address> (run before every commit; see NOTES.md)

Files: decomp/**/*.cpp and *.h (not decomp/generated), overrides/*.cpp. Comments and string literals are not touched.

Classification of a hex literal with value v (USA addresses: the code was written against SLUS_201.53):
  - outside [0x100000, end of the image]: not an address, left alone.
  - the address argument of a registration macro (RC_DECOMP_FN*(name, 0x...)): removed, the macro takes the function
    by name (rc::fns, tools/gen_addrs.py).
  - listed in tools/addr_review.txt (file, value, the source line with blanks squeezed): the reviewed decision,
    'addr' or 'keep' (keep = not an address, e.g. a colour, a size or a mask that falls in the range).
  - the start of a function or of a sized object, or inside one (or a .lit4 entry, or an unnamed place in .rodata,
    .data or .sdata) and used where only an address makes sense (the argument of guest_call, at<>, mem<>,
    guest_host, ptr32<>, decompOr, replaceFunction, callOriginalFloat, constant): an address.
  - anything else in the range: to review. 'report' lists these; a person reads the code and records the decision.
Symbolic form: sym::<key> or (sym::<key> + 0x<offset>u), where the key is the containing symbol (functions, sized
objects), lit4_<usa address> for a float literal, or sec_<section> + offset for an unnamed place in a section whose
contents are the same at the same offset in every version (checked against the other versions' executables: the
conversion is refused if the place does not hold the same bytes there, or if the symbol was resized or, inside a
function, the function changed).
"""
import os
import re
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_addrs  # noqa: E402
import rcelf  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
USA = rcelf.game_elf('usa')
OTHERS = [rcelf.game_elf('eu')]
REVIEW = os.path.join(ROOT, 'tools', 'addr_review.txt')
CHANGED = {rcelf.game_elf('eu'): os.path.join(ROOT, 'decomp', 'generated', 'eu', 'changed_functions.txt')}

LITERAL = re.compile(r'(?<![\w.])0[xX]([0-9a-fA-F]+)([uUlL]*)(?![\w.])')
REG = re.compile(r'(RC_DECOMP_FN\w*)\(\s*(\w+)\s*,\s*0[xX][0-9a-fA-F]+[uU]?\s*(,|\))')
ADDRESS_CONTEXT = re.compile(r'(?:guest_call\s*(?:<[^<>()]*>)?|\bat\s*<[^()]*>|\bmem\s*<[^()]*>|guest_host|ptr32\s*<[^()]*>|'
                             r'decompOr|replaceFunction|callOriginalFloat|\bconstant)\s*\(\s*$')
SKIP_PREFIXES = ('$L', 'gcc2_compiled', '__gnu_compiled')


def source_files():
    out = []
    for base, dirs, files in os.walk(os.path.join(ROOT, 'decomp')):
        if os.path.relpath(base, ROOT).startswith(os.path.join('decomp', 'generated')):
            continue
        out += [os.path.join(base, f) for f in files if f.endswith(('.cpp', '.h'))]
    out += [os.path.join(ROOT, 'overrides', f) for f in os.listdir(os.path.join(ROOT, 'overrides')) if f.endswith('.cpp')]
    return sorted(out)


def code_mask(text):
    """True for each character that is code (not inside a comment or a string/char literal)."""
    mask = bytearray(b'\1') * len(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            mask[i:j] = b'\0' * (j - i)
            i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            mask[i:j] = b'\0' * (j - i)
            i = j
        elif c == '"' or (c == "'" and not (i > 0 and text[i - 1].isalnum())):
            j = i + 1
            if c == '"' and i > 0 and text[i - 1] == 'R':  # raw string R"x(...)x"
                d = text[j:text.index('(', j)]
                j = text.index(')' + d + '"', j) + len(d) + 2
            else:
                while j < n and text[j] != c:
                    j += 2 if text[j] == '\\' else 1
                j += 1
            mask[i:j] = b'\0' * (j - i)
            i = j
        else:
            i += 1
    return mask


def squeeze(line):
    return ' '.join(line.split())


def load_review():
    decisions = {}
    if not os.path.exists(REVIEW):
        return decisions
    for raw in open(REVIEW):
        if not raw.strip() or raw.startswith('#'):
            continue
        decision, path, value, line = raw.rstrip('\n').split('\t')[:4]
        decisions[(path, int(value, 16), line)] = decision
    return decisions


class Namer:
    """Names a USA address as a symbol + offset, checked against the other versions."""

    def __init__(self):
        self.usa = rcelf.Elf(USA)
        self.others = []
        for path in OTHERS:
            if not os.path.exists(path):
                continue
            other = rcelf.Elf(path)
            changed = set()
            if os.path.exists(CHANGED.get(path, '')):
                changed = {l.split('\t')[0] for l in open(CHANGED[path]) if l.strip() and not l.startswith('#')}
            self.others.append((other, rcelf.VersionMap(self.usa, other), changed))
        self.lit4_maps = {o.path: rcelf.lit4_map(self.usa, o) for o, _, _ in self.others}
        self.observed = {o.path: rcelf.observed_pairs(self.usa, o) for o, _, _ in self.others}
        self.image_end = self.usa.section['.bss'].end
        self.lit4 = self.usa.section['.lit4']
        self.func_starts = {f.value for f in self.usa.functions if f.size}
        self.object_starts = {s.value for s in self.usa.placed if s.kind == 'OBJECT' and s.size}
        self.ends = defaultdict(list)
        for s in self.usa.placed:
            if s.size and s.kind in ('OBJECT', 'FUNC'):
                self.ends[s.value + s.size].append(s)

    def in_range(self, v):
        return 0x100000 <= v <= self.image_end

    def name(self, v):
        """(symbolic text, kind, problem or None). kind: 'function', 'in-function', 'object', 'in-object', 'lit4',
        'section', 'end'."""
        u = self.usa
        if v == self.image_end:
            return 'sym::image_end', 'end', None
        if self.lit4.addr <= v < self.lit4.end:
            if v % 4:
                return None, 'lit4', 'misaligned lit4'
            problem = None
            for other, m, changed in self.others:
                if v not in self.lit4_maps[other.path]:
                    problem = f'lit4 {v:#x} not in {os.path.basename(other.path)}'
            return f'sym::lit4_{v:06x}', 'lit4', problem
        sym = u.containing(v)
        sec = u.section_of(v)
        if sym is not None and (sym.size == 0 or not sym.value <= v < sym.value + sym.size):
            if sym.value != v:
                sym = None  # only a label before it: use the section instead
        if sym is not None and sym.name.startswith(SKIP_PREFIXES):
            sym = None
        if sym is None:
            if sec is None:
                return None, 'outside', 'not in a section'
            off = v - sec.addr
            text = f'(sym::sec_{sec.name[1:]} + {off:#x}u)' if off else f'sym::sec_{sec.name[1:]}'
            problem = None
            for other, m, _ in self.others:
                osec = other.section[sec.name]
                seen = self.observed[other.path].get(v)
                if off == 0:
                    continue  # the section's start
                if osec.size != sec.size:
                    problem = f'{sec.name} resized in {os.path.basename(other.path)}'
                elif seen is not None:
                    if seen != osec.addr + off:
                        problem = f'the code reads {v:#x} at {seen:#x} in {os.path.basename(other.path)}'
                elif sec.kind != rcelf.SHT_NOBITS and u.read(v, 4) != other.read(osec.addr + off, 4):
                    problem = f'{sec.name}+{off:#x} differs in {os.path.basename(other.path)}'
            return text, 'section', problem
        off = v - sym.value
        ident = gen_addrs.ident(sym.key)
        text = f'(sym::{ident} + {off:#x}u)' if off else f'sym::{ident}'
        kind = ('function' if off == 0 else 'in-function') if sym.kind == 'FUNC' else ('object' if off == 0 else 'in-object')
        problem = None
        for other, m, changed in self.others:
            mapped, how = m.map(v)
            if mapped is None:
                problem = f'{how} in {os.path.basename(other.path)}'
            elif kind == 'in-function' and sym.key in changed:
                problem = f'inside {sym.key}, which changed in {os.path.basename(other.path)}'
            # One past the end of an object is also the start of this one: both readings must agree.
            for prev in self.ends.get(v, []):
                other_prev = other.by_key.get(prev.key)
                if mapped is not None and other_prev is not None and other_prev.value + other_prev.size != mapped:
                    problem = f'{v:#x} is the end of {prev.key} and the start of {sym.key}: they part in {os.path.basename(other.path)}'
        return text, kind, problem


def scan(namer, review):
    """Yields (path, offset, end, value, suffix, line, category, symbolic, problem)."""
    for path in source_files():
        text = open(path).read()
        rel = os.path.relpath(path, ROOT)
        mask = code_mask(text)
        reg_spans = {}
        for m in REG.finditer(text):
            if mask[m.start()]:
                reg_spans[m.start(0)] = m
        reg_literal = set()
        for m in reg_spans.values():
            lit = LITERAL.search(text, m.start(2) + len(m.group(2)))
            reg_literal.add(lit.start())
        lines = text.split('\n')
        line_starts = [0]
        for l in lines:
            line_starts.append(line_starts[-1] + len(l) + 1)
        import bisect
        for m in LITERAL.finditer(text):
            if not mask[m.start()]:
                continue
            v = int(m.group(1), 16)
            if not namer.in_range(v):
                continue
            ln = bisect.bisect_right(line_starts, m.start()) - 1
            line = lines[ln]
            if m.start() in reg_literal:
                yield rel, m.start(), m.end(), v, m.group(2), ln + 1, line, 'registration', None, None
                continue
            symbolic, kind, problem = namer.name(v)
            key = (rel, v, squeeze(line))
            if key in review:
                cat = 'reviewed-' + review[key]
            else:
                before = text[line_starts[ln]:m.start()]
                if kind in ('function', 'object', 'end'):
                    cat = 'exact'
                elif ADDRESS_CONTEXT.search(before):
                    cat = 'context'
                else:
                    cat = 'review'
            yield rel, m.start(), m.end(), v, m.group(2), ln + 1, line, cat, symbolic, problem


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else 'report'
    namer = Namer()
    review = load_review()
    items = list(scan(namer, review))
    counts = Counter(i[7] for i in items)
    problems = [i for i in items if i[9] and i[7] in ('exact', 'context', 'reviewed-addr')]
    if mode == 'report':
        print(f'{len(items)} literals in the address range: ' + ', '.join(f'{k} {v}' for k, v in sorted(counts.items())))
        todo = [i for i in items if i[7] == 'review']
        out = os.path.join(ROOT, 'build-decomp', 'addr_review_todo.txt')
        with open(out, 'w') as f:
            for rel, _, _, v, _, ln, line, cat, symbolic, problem in todo:
                f.write(f'?\t{rel}\t{v:#x}\t{squeeze(line)}\t# {rel}:{ln} -> {symbolic}' + (f' PROBLEM {problem}' if problem else '') + '\n')
        print(f'{len(todo)} to review in {out}')
        for i in problems:
            print(f'PROBLEM {i[0]}:{i[5]} {i[3]:#x} -> {i[8]}: {i[9]}')
        return
    if mode == 'check':
        bad = [i for i in items if i[7] in ('review', 'exact', 'context', 'registration', 'reviewed-addr', 'reviewed-addr-changed')]
        for rel, _, _, v, _, ln, line, cat, symbolic, problem in bad:
            print(f'{rel}:{ln}: raw address {v:#x} ({cat}): write it as {symbolic or "a symbol"}'
                  ' or, if it is not an address, add it to tools/addr_review.txt as keep')
        # A recompiled function named directly (Name_0x<address>) is also version-specific: use rc::fns::Name.
        for path in source_files():
            text = open(path).read()
            mask = code_mask(text)
            for m in re.finditer(r'\b\w+_0x[0-9a-fA-F]{5,8}\b', text):
                if mask[m.start()]:
                    print(f'{os.path.relpath(path, ROOT)}:{text.count(chr(10), 0, m.start()) + 1}: {m.group(0)} names a '
                          'recompiled function by its USA address: use rc::fns::<name>.original')
                    bad.append(m)
        if bad:
            sys.exit(1)
        print(f'ok: no raw address literals ({counts.get("reviewed-keep", 0)} reviewed non-addresses)')
        return
    if mode == 'apply':
        if problems:
            for i in problems:
                print(f'PROBLEM {i[0]}:{i[5]} {i[3]:#x} -> {i[8]}: {i[9]}')
            sys.exit('not applied: resolve the problems first (review them as keep, or fix the mapping)')
        by_file = defaultdict(list)
        for i in items:
            if i[7] in ('registration', 'exact', 'context', 'reviewed-addr', 'reviewed-addr-changed'):
                by_file[i[0]].append(i)
        total = 0
        for rel, edits in by_file.items():
            path = os.path.join(ROOT, rel)
            text = open(path).read()
            for rel_, start, end, v, suffix, ln, line, cat, symbolic, problem in sorted(edits, key=lambda e: -e[1]):
                if cat == 'registration':
                    # Remove ", 0x..." from the macro call.
                    comma = text.rindex(',', 0, start)
                    text = text[:comma] + text[end:]
                else:
                    text = text[:start] + symbolic + text[end:]
                total += 1
            open(path, 'w').write(text)
        print(f'{total} literals rewritten in {len(by_file)} files')


if __name__ == '__main__':
    main()
