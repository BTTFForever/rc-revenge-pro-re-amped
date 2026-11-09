#!/usr/bin/env python3
"""Turns the debug data in the game's ELF into C++ headers and a symbol database.

The ELF carries an ECOFF .mdebug section with stabs embedded in it. For every game source file this
reads the types, functions, parameters, locals and globals, merges them across files and writes:

  rc_types.h         every struct, union, enum and typedef, in namespace rc, with the PS2 layout
  rc_layout_check.cpp  an offsetof and sizeof assert per type; compiling it proves the layout holds
  rc_globals.h       an accessor for every global at its guest address
  symbols/<file>.txt   each function's signature, locals and statement boundaries, for reading
  symbols.json       the same data for other tools

Pointers are ptr32<T> and floats are ee_float (rc_guest.h), so a struct is byte for byte what the
game and its data files expect.

Usage: stabs2c.py <elf> <output-dir>
"""
import collections
import json
import os
import re
import struct
import sys
import zlib

STAB_BASE = 0x8F300
N_RSYM, N_FUN, N_STSYM, N_LCSYM, N_LSYM, N_PSYM, N_LBRAC, N_RBRAC = 0x40, 0x24, 0x26, 0x28, 0x80, 0xA0, 0xC0, 0xE0
ST_LABEL = 5
N_GSYM = 0x20
SYMBOL_STABS = {N_GSYM, N_RSYM, N_FUN, N_STSYM, N_LCSYM, N_LSYM, N_PSYM, N_LBRAC, N_RBRAC}

CPP_KEYWORDS = set("""alignas alignof and asm auto bitand bitor bool break case catch char char8_t char16_t char32_t
class compl concept const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype
default delete do double dynamic_cast else enum explicit export extern false float for friend goto if inline int long
mutable namespace new noexcept not nullptr operator or private protected public register reinterpret_cast requires
return short signed sizeof static static_assert static_cast struct switch template this thread_local throw true try
typedef typeid typename union unsigned using virtual void volatile wchar_t while xor""".split())

BASE_TYPES = {
    'int': 'int32_t', 'char': 'char', 'long int': 'int64_t', 'unsigned int': 'uint32_t',
    'long unsigned int': 'uint64_t', 'long long int': 'int64_t', 'long long unsigned int': 'uint64_t',
    'short int': 'int16_t', 'short unsigned int': 'uint16_t', 'signed char': 'int8_t',
    'unsigned char': 'uint8_t', 'float': 'ee_float', 'double': 'double', 'long double': 'double', 'void': 'void',
    '__int128': 'ee_int128', '__int128 unsigned': 'ee_uint128',
}
BASE_SIZES = {'int32_t': 4, 'char': 1, 'int64_t': 8, 'uint32_t': 4, 'uint64_t': 8, 'int16_t': 2, 'uint16_t': 2,
              'int8_t': 1, 'uint8_t': 1, 'ee_float': 4, 'double': 8, 'ee_int128': 16, 'ee_uint128': 16}


def safe(name):
    return name + '_' if name in CPP_KEYWORDS else name


class Node:
    """A type as one source file's stabs define it."""

    def __init__(self, kind, **fields):
        self.kind = kind
        self.__dict__.update(fields)


class StabParser:
    """Parses the type strings of one source file. Type numbers are local to the file."""

    def __init__(self):
        self.types = {}

    def parse_symbol(self, text):
        """Returns (name, descriptor, Node or None) for 'name:<desc><type>', or None."""
        colon = text.find(':')
        if colon < 0:
            return None
        self.s = text
        self.i = colon + 1
        name = text[:colon]
        desc = ''
        if self.i < len(text) and text[self.i].isalpha():
            desc = text[self.i]
            self.i += 1
            if desc == 'T' and self.i < len(text) and text[self.i] == 't':
                self.i += 1
        node = self.parse_type()
        return name, desc, node

    def num(self):
        match = re.compile(r'-?\d+').match(self.s, self.i)
        self.i = match.end()
        return int(match.group(0), 8) if match.group(0).startswith('0') and len(match.group(0)) > 1 else int(match.group(0))

    def eat(self, char):
        if self.s[self.i] != char:
            raise ValueError(f'expected {char!r} at {self.i} in {self.s[:200]}')
        self.i += 1

    def parse_type(self):
        char = self.s[self.i]
        if char.isdigit() or char == '-':
            number = int(re.compile(r'\d+').match(self.s, self.i).group(0))
            self.i += len(str(number))
            if self.i < len(self.s) and self.s[self.i] == '=':
                self.i += 1
                ref = self.types.setdefault(number, Node('pending', num=number))
                node = self.parse_definition(number)
                ref.__dict__.clear()
                ref.__dict__.update(node.__dict__)
                ref.num = number
                return ref
            return self.types.setdefault(number, Node('pending', num=number))
        return self.parse_definition(None)

    def parse_definition(self, number):
        char = self.s[self.i]
        if char.isdigit():
            target = self.parse_type()
            if target is self.types.get(number):
                return Node('void_self')
            return Node('alias', target=target)
        self.i += 1
        if char == 'r':
            self.parse_type()
            self.eat(';')
            low = self.range_bound()
            self.eat(';')
            high = self.range_bound()
            self.eat(';')
            return Node('range', low=low, high=high)
        if char == '*':
            return Node('ptr', target=self.parse_type())
        if char == 'k' or char == 'B':
            return self.parse_type()
        if char == 'f':
            return Node('func', ret=self.parse_type())
        if char == 'a':
            self.eat('r')
            self.parse_type()
            self.eat(';')
            low = self.num()
            self.eat(';')
            high = self.num()
            self.eat(';')
            return Node('array', low=low, high=high, elem=self.parse_type())
        if char in 'su':
            size = self.num()
            fields = []
            while self.s[self.i] != ';':
                end = self.s.index(':', self.i)
                fname = self.s[self.i:end]
                self.i = end + 1
                ftype = self.parse_type()
                self.eat(',')
                bit = self.num()
                self.eat(',')
                bits = self.num()
                self.eat(';')
                fields.append((fname, ftype, bit, bits))
            self.i += 1
            return Node('union' if char == 'u' else 'struct', size=size, fields=fields, tag=None)
        if char == 'e':
            items = []
            while self.s[self.i] != ';':
                end = self.s.index(':', self.i)
                ename = self.s[self.i:end]
                self.i = end + 1
                value = self.num()
                self.eat(',')
                items.append((ename, value))
            self.i += 1
            return Node('enum', items=items, tag=None)
        if char == 'x':
            what = self.s[self.i]
            end = self.s.index(':', self.i + 1)
            tag = self.s[self.i + 1:end]
            self.i = end + 1
            return Node('xref', what=what, tag=tag)
        raise ValueError(f'unknown type {char!r} in {self.s[:200]}')

    def range_bound(self):
        match = re.compile(r'-?\d+').match(self.s, self.i)
        self.i = match.end()
        return match.group(0)


class Merger:
    """Combines the per-file type tables into one set of named C++ declarations."""

    def __init__(self):
        self.records = {}      # cpp name -> {'kind','body','fields',...}
        self.typedefs = {}     # cpp name -> target spelling
        self.enums = {}        # cpp name -> items
        self.conflicts = collections.Counter()
        self.enumerators = {}  # name -> value

    def resolve(self, node):
        seen = 0
        while node.kind in ('alias',) and getattr(node, 'typedef_name', None) is None and seen < 64:
            node = node.target
            seen += 1
        return node

    def spell(self, node, cu, for_pointer=False):
        """The C++ spelling of a type, without declarators; arrays are handled by the caller."""
        if getattr(node, 'typedef_name', None):
            if for_pointer:
                target = self.strip_typedefs(node)
                if target.kind in ('struct', 'union'):
                    return self.record_name(target, cu)
            return self.typedef_spelling(node, cu)
        if node.kind == 'alias':
            return self.spell(node.target, cu, for_pointer)
        if node.kind == 'base':
            return node.cpp
        if node.kind in ('void_self', 'pending'):
            return 'void'
        if node.kind == 'range':
            return {'1': 'int32_t'}.get(node.high, 'int32_t')
        if node.kind == 'ptr':
            target = self.resolve(node.target)
            if target.kind == 'func':
                return 'ee_fnptr'
            return f'ptr32<{self.spell(node.target, cu, True)}>'
        if node.kind in ('struct', 'union'):
            return self.record_name(node, cu)
        if node.kind == 'enum':
            return self.enum_name(node, cu)
        if node.kind == 'xref':
            word = {'s': 'struct', 'u': 'union', 'e': 'enum'}[node.what]
            if node.what == 'e':
                return 'int32_t'
            self.records.setdefault(safe(node.tag), {'kind': word, 'incomplete': True, 'size': 0, 'fields': []})
            return safe(node.tag)
        if node.kind == 'array':
            return self.spell(node.elem, cu)
        if node.kind == 'func':
            return 'ee_fnptr'
        raise ValueError(node.kind)

    def strip_typedefs(self, node):
        for _ in range(64):
            if node.kind == 'alias':
                node = node.target
            else:
                break
        return node

    def typedef_spelling(self, node, cu):
        name = node.typedef_name
        node.typedef_name = None
        target = self.declarator(node, cu)
        node.typedef_name = name
        cpp = safe(name)
        if target == cpp:
            return cpp
        existing = self.typedefs.get(cpp)
        if existing is None:
            self.typedefs[cpp] = target
        elif self.canon(existing) != self.canon(target):
            self.conflicts['typedef ' + name] += 1
            alt = f'{cpp}__{cu}'
            self.typedefs.setdefault(alt, target)
            return alt
        return cpp

    def canon(self, spelling):
        for _ in range(16):
            if spelling not in self.typedefs:
                break
            spelling = self.typedefs[spelling]
        return spelling

    def declarator(self, node, cu):
        """Spelling that keeps array bounds, for use after 'using name ='."""
        return self.field_spelling(node, cu)

    def strip_for_array(self, node):
        return node

    def record_name(self, node, cu):
        if getattr(node, 'cpp_name', None):
            return node.cpp_name
        tag = node.tag
        node.cpp_name = '<in progress>'
        name = safe(tag) if tag else None
        placeholder = f'anon_{id(node):x}'
        node.cpp_name = name or placeholder
        fields = []
        for fname, ftype, bit, bits in node.fields:
            fields.append((safe(fname) if fname else f'_anon{len(fields)}', self.field_spelling(ftype, cu), bit, bits))
        body = (node.kind, node.size, tuple(fields))
        if name is None:
            digest = zlib.crc32(repr(body).replace(placeholder, 'self').encode())
            name = f'anon_{digest:08x}'
            fields = [(f, t.replace(placeholder, name), b, n) for f, t, b, n in fields]
            body = (node.kind, node.size, tuple(fields))
        existing = self.records.get(name)
        if existing and not existing.get('incomplete') and existing['body'] != body:
            self.conflicts['record ' + name] += 1
            if self.conflicts['record ' + name] == 1:
                diff = [(a, b) for a, b in zip(existing['body'][2], body[2]) if a != b][:2]
                print('  first difference in', name, ':', diff, file=sys.stderr)
            name = f'{name}__{cu}'
            fields = [(f, t, b, n) for f, t, b, n in fields]
            body = (node.kind, node.size, tuple(fields))
        node.cpp_name = name
        if name not in self.records or self.records[name].get('incomplete'):
            self.records[name] = {'kind': node.kind, 'size': node.size, 'fields': fields, 'body': body, 'cu': cu}
        return name

    def field_spelling(self, node, cu):
        bounds = ''
        walk = node
        while True:
            walk = self.unname_if_alias_array(walk)
            if walk.kind == 'array':
                bounds += f'[{walk.high - walk.low + 1}]'
                walk = walk.elem
            else:
                break
        return self.spell(walk, cu) + bounds

    def unname_if_alias_array(self, node):
        if getattr(node, 'typedef_name', None):
            return node
        if node.kind == 'alias' and self.resolve(node.target).kind == 'array':
            return node.target
        return node

    def enum_name(self, node, cu):
        if getattr(node, 'cpp_name', None):
            return node.cpp_name
        items = []
        key = tuple(node.items)
        for ename, value in node.items:
            cpp = safe(ename)
            if self.enumerators.get(cpp, key) != key:
                self.conflicts['enumerator ' + ename] += 1
                cpp = f'{cpp}__{node.tag or format(zlib.crc32(repr(key).encode()), "08x")}'
            self.enumerators[cpp] = key
            items.append((cpp, value))
        name = safe(node.tag) if node.tag else f'anon_enum_{zlib.crc32(repr(items).encode()):08x}'
        if name in self.enums and self.enums[name] != items:
            self.conflicts['enum ' + name] += 1
            name = f'{name}__{cu}'
        self.enums[name] = items
        node.cpp_name = name
        return name


def read_elf_symbols(data):
    """Reads the ELF .symtab: name -> (address, size, type)."""
    shoff = struct.unpack_from('<I', data, 32)[0]
    shentsize, shnum = struct.unpack_from('<HH', data, 46)
    symbols = {}
    for index in range(shnum):
        fields = struct.unpack_from('<10I', data, shoff + index * shentsize)
        if fields[1] != 2:
            continue
        offset, size, link = fields[4], fields[5], fields[6]
        strtab = struct.unpack_from('<10I', data, shoff + link * shentsize)[4]
        for pos in range(offset, offset + size, 16):
            name_off, value, sym_size, info = struct.unpack_from('<IIIB', data, pos)
            end = data.index(b'\0', strtab + name_off)
            name = data[strtab + name_off:end].decode('latin1')
            if name:
                symbols[name] = (value, sym_size, info & 15)
    return symbols


def mdebug_header(data):
    shoff = struct.unpack_from('<I', data, 32)[0]
    shentsize, shnum = struct.unpack_from('<HH', data, 46)
    for index in range(shnum):
        fields = struct.unpack_from('<10I', data, shoff + index * shentsize)
        if fields[1] == 0x70000005:
            offset = fields[4]
            header = struct.unpack_from('<hh23i', data, offset)
            names = ('ilineMax cbLine cbLineOff idnMax cbDnOff ipdMax cbPdOff isymMax cbSymOff ioptMax cbOptOff iauxMax '
                     'cbAuxOff issMax cbSsOff issExtMax cbSsExtOff ifdMax cbFdOff crfd cbRfdOff iextMax cbExtOff').split()
            return dict(zip(names, header[2:]))
    raise SystemExit('no .mdebug section')


def join_continuations(syms):
    """GCC splits a long stab and ends each piece but the last with a backslash."""
    joined = []
    pending = ''
    for text, value, st, index in syms:
        if text.endswith('\\') and index >= STAB_BASE:
            pending += text[:-1]
            continue
        joined.append((pending + text, value, st, index))
        pending = ''
    return joined


def is_game_file(name):
    return name.startswith('src\\') and name.endswith('.c')


def clean_cu(name):
    return re.sub(r'[^A-Za-z0-9_]', '_', name.split('\\')[-1].rsplit('.', 1)[0])


def name_types(parser, merger, name, desc, node):
    """Attaches a 'name:t' or 'name:T' symbol to the type it names."""
    if node is None:
        return
    if desc == 't' and name in ('u_long128', 'long128'):
        # Declared with mode(TI); the stabs record only the int it was written as.
        node.__dict__.clear()
        node.__dict__.update(Node('base', cpp='ee_uint128').__dict__)
        return
    if desc == 't':
        if node.kind == 'range' and name in BASE_TYPES or name in BASE_TYPES and node.kind in ('void_self', 'alias', 'struct', 'range'):
            node.__dict__.clear()
            node.__dict__.update(Node('base', cpp=BASE_TYPES[name]).__dict__)
            return
        if name.startswith('complex '):
            return
        if node.kind in ('struct', 'union', 'enum') and getattr(node, 'tag', None) is None and name.strip():
            node.tag = name
            return
        wrapper = Node('alias', target=node)
        return_name = name
        if node.kind == 'alias' or node.kind == 'ptr' or node.kind == 'array' or node.kind == 'base':
            node.typedef_name = return_name
        else:
            node.typedef_name = return_name
        return wrapper
    if desc == 'T' and node.kind in ('struct', 'union', 'enum') and name.strip():
        node.tag = name


def main():
    elf_path, out_dir = sys.argv[1], sys.argv[2]
    data = open(elf_path, 'rb').read()
    header = mdebug_header(data)
    elf_symbols = read_elf_symbols(data)

    def string(offset):
        end = data.index(b'\0', offset)
        return data[offset:end].decode('latin1')

    merger = Merger()
    functions, globals_ = [], {}
    os.makedirs(os.path.join(out_dir, 'symbols'), exist_ok=True)
    parse_errors = 0
    cu_names = []
    for fd_index in range(header['ifdMax']):
        fd = struct.unpack_from('<10I2H7I', data, header['cbFdOff'] + 72 * fd_index)
        iss_base, isym_base, csym = fd[2], fd[4], fd[5]
        syms = []
        for k in range(csym):
            iss, value, bits = struct.unpack_from('<III', data, header['cbSymOff'] + 12 * (isym_base + k))
            syms.append((string(header['cbSsOff'] + iss_base + iss), value, bits & 63, bits >> 12))
        source = next((s[0] for s in syms if s[0].startswith('src\\') and s[0].endswith('.c')), None)
        if source is None or not is_game_file(source):
            continue
        cu = clean_cu(source)
        cu_names.append(cu)
        syms = join_continuations(syms)
        parser = StabParser()
        current, depth, blocks = None, 0, []
        labels = {}
        for text, value, st, index in syms:
            kind = index - STAB_BASE if index >= STAB_BASE else None
            if kind is None:
                if st == ST_LABEL and current is None:
                    pass
                continue
            if kind not in SYMBOL_STABS:
                continue
            try:
                parsed = parser.parse_symbol(text) if ':' in text else None
            except (ValueError, IndexError, AttributeError) as error:
                parse_errors += 1
                if parse_errors < 8:
                    print('parse failure:', error, file=sys.stderr)
                continue
            if kind == N_LSYM and parsed and parsed[1] in ('t', 'T'):
                name_types(parser, merger, *parsed)
                continue
            if kind == N_FUN:
                if parsed:
                    name, desc, node = parsed
                    current = {'name': name, 'static': desc == 'f', 'address': value, 'ret': node, 'params': [],
                               'locals': [], 'cu': cu, 'source': source}
                    functions.append(current)
                    depth, blocks = 0, []
                elif current is not None:
                    current['size'] = value
                    current = None
                continue
            if kind == N_LBRAC:
                depth += 1
                continue
            if kind == N_RBRAC:
                depth -= 1
                continue
            if not parsed:
                continue
            name, desc, node = parsed
            if kind in (N_STSYM, N_LCSYM) and desc in ('S', 'V'):
                if current is not None:
                    current['locals'].append({'name': name, 'type': node, 'where': f'static@0x{value:x}', 'depth': depth})
                globals_.setdefault(name, (value, node, cu, 'S'))
            elif desc == 'G':
                symbol = elf_symbols.get(name)
                if symbol:
                    globals_.setdefault(name, (symbol[0], node, cu, 'G'))
            elif current is not None:
                if desc in ('p', 'P', 'R'):
                    current['params'].append({'name': name, 'type': node, 'reg' if desc != 'p' else 'stack': value})
                elif desc == 'r':
                    current['locals'].append({'name': name, 'type': node, 'where': f'reg{value}', 'depth': depth})
                elif desc == '' and kind == N_LSYM:
                    current['locals'].append({'name': name, 'type': node, 'where': f'sp+0x{value:x}', 'depth': depth})

        # Spell everything for this file while its type table is alive.
        for function in (f for f in functions if f['cu'] == cu and 'ret_text' not in f):
            function['ret_text'] = merger.spell(function['ret'], cu) if function['ret'] is not None else 'void'
            for item in function['params'] + function['locals']:
                item['type_text'] = merger.field_spelling(item['type'], cu)
                item.pop('type')
            function.pop('ret')
        for name, (value, node, gcu, how) in list(globals_.items()):
            if gcu == cu and isinstance(node, Node):
                globals_[name] = (value, merger.field_spelling(node, cu), gcu, how)
        for node in parser.types.values():
            if node.kind in ('struct', 'union') and getattr(node, 'tag', None):
                merger.spell(node, cu)
            elif node.kind == 'enum':
                merger.spell(node, cu)
            elif getattr(node, 'typedef_name', None):
                merger.spell(node, cu)

    # Statement boundaries from $LM labels.
    print(f'files {len(cu_names)}, functions {len(functions)}, records {len(merger.records)}, '
          f'typedefs {len(merger.typedefs)}, enums {len(merger.enums)}, globals {len(globals_)}, '
          f'parse failures {parse_errors}, conflicts {sum(merger.conflicts.values())}')
    for name, count in merger.conflicts.most_common(20):
        print('  conflict:', name, count)

    write_types(merger, out_dir)
    write_globals(globals_, out_dir)
    write_symbols(functions, out_dir)


class Layout:
    """Natural alignment of a spelled type, to spot fields the original aligned by hand."""

    def __init__(self, merger):
        self.merger = merger

    def canon(self, spelling):
        return self.merger.canon(spelling)

    def align(self, spelling):
        for _ in range(8):
            spelling = self.canon(re.sub(r'(\[\d+\])+$', '', spelling))
            if not spelling.endswith(']'):
                break
        if spelling.startswith('ptr32<') or spelling == 'ee_fnptr':
            return 4
        if spelling in BASE_SIZES:
            return BASE_SIZES[spelling]
        if spelling in self.merger.enums:
            return 4
        record = self.merger.records.get(spelling)
        if record:
            return max([self.align(f[1]) for f in record.get('fields', ())] or [1])
        return 1


def value_deps(spelling):
    """Names a type needs complete when spelled like this. ptr32<...> only needs a declaration."""
    stripped = re.sub(r'ptr32<.*>', '', spelling)
    return set(re.findall(r'[A-Za-z_]\w*', stripped))


def pointer_deps(spelling):
    return set(re.findall(r'[A-Za-z_]\w*', spelling))


def write_types(merger, out_dir):
    decls = {}
    for name, record in merger.records.items():
        decls[name] = ('record', record)
    for name, items in merger.enums.items():
        decls[name] = ('enum', items)
    for name, target in merger.typedefs.items():
        if name not in decls:
            decls[name] = ('typedef', target)

    layout = Layout(merger)
    order, state = [], {}

    def visit(name):
        if name not in decls or state.get(name) == 2:
            return
        if state.get(name) == 1:
            return
        state[name] = 1
        kind, item = decls[name]
        if kind == 'record':
            for _, spelling, _, _ in item.get('fields', ()):
                for dep in value_deps(spelling):
                    visit(dep)
                for dep in pointer_deps(spelling):
                    if dep in decls and decls[dep][0] == 'typedef':
                        visit(dep)
        elif kind == 'typedef':
            for dep in pointer_deps(item):
                if dep in decls and decls[dep][0] != 'record':
                    visit(dep)
                elif dep in value_deps(item):
                    visit(dep)
        state[name] = 2
        order.append(name)

    for name in sorted(decls):
        visit(name)

    lines = ['// Generated by tools/stabs2c.py from the debug data in the game ELF. Do not edit.', '#pragma once',
             '#include "rc_guest.h"', '', 'namespace rc {', '']
    for name in sorted(decls):
        if decls[name][0] == 'record':
            lines.append(f"{decls[name][1]['kind']} {name};")
    lines.append('')
    asserts = ['// Generated by tools/stabs2c.py. Compiling this proves every struct has the PS2 layout.',
               '#include <cstddef>', '#include "rc_types.h"', '']
    for name in order:
        kind, item = decls[name]
        if kind == 'enum':
            underlying = 'uint32_t' if any(value > 0x7FFFFFFF for _, value in item) else 'int32_t'
            lines.append(f'enum {name} : {underlying} {{')
            lines.extend(f'    {ename} = {value},' for ename, value in item)
            lines.append('};\n')
        elif kind == 'typedef':
            match = re.match(r'(.*?)((?:\[\d+\])+)$', item)
            if match:
                lines.append(f'typedef {match.group(1)} {name}{match.group(2)};')
            else:
                lines.append(f'typedef {item} {name};')
        else:
            if item.get('incomplete'):
                continue
            lines.append(f"{item['kind']} {name} {{")
            field_names = [f[0] for f in item['fields']]
            cursor = 0
            pad = 0
            record_align = 1
            for fname, spelling, bit, bits in item['fields']:
                match = re.match(r'(.*?)((?:\[\d+\])+)$', spelling)
                base, bounds = (match.group(1), match.group(2)) if match else (spelling, '')
                raw_base = base
                for other in field_names:
                    if re.search(rf'\b{re.escape(other)}\b', base):
                        base = re.sub(rf'(?<![:\w]){re.escape(other)}\b', f'::rc::{other}', base)
                size = BASE_SIZES.get(layout.canon(raw_base)) or BASE_SIZES.get(raw_base)
                if size and not bounds and bits != size * 8:
                    lines.append(f'    {base} {fname} : {bits}; // bit {bit}')
                    cursor = (bit + bits + 7) // 8
                    continue
                offset = bit // 8
                natural = layout.align(raw_base)
                record_align = max(record_align, natural)
                aligned = (cursor + natural - 1) // natural * natural
                prefix = ''
                if item['kind'] == 'struct' and offset != aligned:
                    if offset % 16 == 0 and (cursor + 15) // 16 * 16 == offset:
                        prefix = 'alignas(16) '
                        record_align = 16
                    elif offset > aligned or offset % natural == 0 and offset > cursor:
                        lines.append(f'    uint8_t _pad{pad}[0x{offset - cursor:x}];')
                        pad += 1
                    else:
                        print(f'  cannot place {name}.{fname} at 0x{offset:x}', file=sys.stderr)
                lines.append(f'    {prefix}{base} {fname}{bounds}; // 0x{offset:x}')
                cursor = offset + bits // 8
                asserts.append(f'static_assert(offsetof(rc::{name}, {fname}) == 0x{bit // 8:x});')
            if item['kind'] == 'struct':
                end = (cursor + record_align - 1) // record_align * record_align
                if item['size'] > end:
                    lines.append(f"    uint8_t _tail[0x{item['size'] - cursor:x}];")
            lines.append('};')
            asserts.append(f"static_assert(sizeof(rc::{name}) == 0x{item['size']:x});")
            lines.append('')
    lines.append('} // namespace rc')
    open(os.path.join(out_dir, 'rc_types.h'), 'w').write('\n'.join(lines) + '\n')
    open(os.path.join(out_dir, 'rc_layout_check.cpp'), 'w').write('\n'.join(asserts) + '\n')


def write_globals(globals_, out_dir):
    lines = ['// Generated by tools/stabs2c.py: every game global at its guest address. Use as G(name).', '#pragma once',
             '#include "rc_types.h"', '', 'namespace rc::globals {']
    for name, (address, spelling, cu, how) in sorted(globals_.items()):
        if not isinstance(spelling, str) or address == 0:
            continue
        match = re.match(r'(.*?)((?:\[\d+\])+)$', spelling)
        if match:
            target = f'{match.group(1)} (&)' + match.group(2)
            lines.append(f"inline auto &{safe(name)}() {{ return *reinterpret_cast<{match.group(1)} (*){match.group(2)}>"
                         f"(guest_host(0x{address:x}u)); }} // {cu}")
        else:
            lines.append(f"inline {spelling} &{safe(name)}() {{ return *reinterpret_cast<{spelling} *>"
                         f"(guest_host(0x{address:x}u)); }} // {cu}")
    lines.append('} // namespace rc::globals')
    lines.append('#define G(name) (::rc::globals::name())')
    open(os.path.join(out_dir, 'rc_globals.h'), 'w').write('\n'.join(lines) + '\n')


def write_symbols(functions, out_dir):
    by_cu = collections.defaultdict(list)
    for function in functions:
        by_cu[function['cu']].append(function)
    for cu, items in by_cu.items():
        lines = [f'// {items[0]["source"]}: {len(items)} functions', '']
        for function in sorted(items, key=lambda f: f['address']):
            params = ', '.join(f"{p['type_text']} {p['name']}" for p in function['params'])
            storage = 'static ' if function['static'] else ''
            lines.append(f"{storage}{function['ret_text']} {function['name']}({params}); "
                         f"// 0x{function['address']:x}, {function.get('size', 0)} bytes")
            for local in function['locals']:
                lines.append(f"    {'  ' * local['depth']}{local['type_text']} {local['name']}; // {local['where']}")
            lines.append('')
        open(os.path.join(out_dir, 'symbols', f'{cu}.txt'), 'w').write('\n'.join(lines) + '\n')
    with open(os.path.join(out_dir, 'symbols.json'), 'w') as handle:
        json.dump(functions, handle, indent=0)


if __name__ == '__main__':
    main()
