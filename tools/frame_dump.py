#!/usr/bin/env python3
"""Inspects a frame record dump (.rcfr; RENDERER_FRAME_RECORD.md section 6.4). Python standard library only.

Usage:
  frame_dump.py info <dump>                  header, segments, cycles, sizes by chunk and record type
  frame_dump.py list <dump> [cycle]          the events, one line each (all cycles, or one by its number)
  frame_dump.py tags <dump> <cycle>          the side tags of a cycle (names from TNAM, payloads decoded), every kick's
                                             labels and resolved spans, begin/end pairs, unresolved anchored tags
  frame_dump.py kick <dump> <cycle> <index>  a kick's registers, spans, and the first stream bytes from the rebuilt pages
  frame_dump.py png <dump> <cycle> <out.png> [--opaque]   the cycle's reference picture (exact-mode dumps)
  frame_dump.py verify <dump>                chunk CRCs, record sizes, the index against a scan
Exit 0 ok, 1 a check failed, 2 unusable dump."""
import struct
import sys
import zlib

CHUNK = struct.Struct('<4sIIIQQ')  # id, flags, crc32, reserved, storedBytes, rawBytes
HEADER = struct.Struct('<8s13I16s16s40s4xQ')
KEYF = struct.Struct('<QQQII')
REC = struct.Struct('<HHI')
KICK = struct.Struct('<IIIIIIIIBBHIIIIIIIQQQ')
END = struct.Struct('<QQQQQQQQIIIIII')
HW = struct.Struct('<IIQII')
TAG = struct.Struct('<IHBBIIII')
REC_NAMES = {1: 'PAGE', 2: 'HW', 3: 'CRT', 4: 'KICK', 5: 'TAG', 6: 'ODD', 7: 'END', 8: 'NOTE'}
ODD_NAMES = {1: 'VIF0 kick', 2: 'FIFO store', 3: 'GS direct write', 4: 'native GIF path', 5: 'PATH2', 6: 'local->host',
             7: 'hook off the EE thread'}
GS_PRIV = {0x00: 'PMODE', 0x10: 'SMODE1', 0x20: 'SMODE2', 0x30: 'SRFSH', 0x40: 'SYNCH1', 0x50: 'SYNCH2', 0x60: 'SYNCV',
           0x70: 'DISPFB1', 0x80: 'DISPLAY1', 0x90: 'DISPFB2', 0xa0: 'DISPLAY2', 0xb0: 'EXTBUF', 0xc0: 'EXTDATA',
           0xd0: 'EXTWRITE', 0xe0: 'BGCOLOR', 0x1000: 'CSR', 0x1010: 'IMR', 0x1040: 'BUSDIR', 0x1080: 'SIGLBLID'}
PAGE = 4096
RAM_PAGES = 8192


def pad8(n):
    return (8 - (n & 7)) & 7


class Dump:
    def __init__(self, path):
        self.path = path
        self.data = open(path, 'rb').read()
        self.chunks = []  # (offset, id, flags, crc, stored, raw)
        self.partial = False
        self.saw_end = False
        if len(self.data) < CHUNK.size + HEADER.size:
            raise ValueError('too short')
        cid, flags, crc, _, stored, raw = CHUNK.unpack_from(self.data, 0)
        if cid != b'RCFR':
            raise ValueError('not a frame record (no RCFR chunk)')
        h = HEADER.unpack_from(self.data, CHUNK.size)
        if h[0] != b'RCFREC1\0':
            raise ValueError('bad magic')
        names = ('magic', 'formatVersion', 'headerBytes', 'gameVersion', 'flags', 'width', 'height', 'fieldRateMilliHz',
                 'ramBytes', 'scratchpadBytes', 'pageBytes', 'gsMemBytes', 'emuStateVersion', 'pad', 'elfName',
                 'gsBackend', 'build', 'indexOffset')
        self.header = dict(zip(names, h))
        self.header_crc_ok = zlib.crc32(self.data[CHUNK.size:CHUNK.size + raw]) == crc
        at = CHUNK.size + stored + pad8(stored)
        while at < len(self.data):
            if at + CHUNK.size > len(self.data):
                self.partial = True
                break
            cid, flags, crc, _, stored, raw = CHUNK.unpack_from(self.data, at)
            if at + CHUNK.size + stored > len(self.data):
                self.partial = True
                break
            self.chunks.append((at, cid.decode('latin-1'), flags, crc, stored, raw))
            if cid == b'ENDF':
                self.saw_end = True
                break
            at += CHUNK.size + stored + pad8(stored)

    def payload(self, chunk):
        at, cid, flags, crc, stored, raw = chunk
        body = self.data[at + CHUNK.size:at + CHUNK.size + stored]
        if flags & 1:
            body = zlib.decompress(body)
        if len(body) != raw:
            raise ValueError(f'{cid} at {at}: {len(body)} bytes, header says {raw}')
        if zlib.crc32(body) != crc:
            raise ValueError(f'{cid} at {at}: CRC mismatch')
        return body

    def tag_names(self):
        """The TNAM table as {(table, value): name}."""
        names = {}
        for c in self.chunks:
            if c[1] == 'TNAM':
                for line in self.payload(c).decode('utf-8', 'replace').splitlines():
                    parts = line.split()
                    if len(parts) == 3:
                        names[(parts[0], int(parts[1]))] = parts[2]
        return names

    def complete(self):
        return self.saw_end and self.header['indexOffset'] != 0 and not self.partial

    @staticmethod
    def records(body):
        at = 0
        while at + REC.size <= len(body):
            rtype, flags, size = REC.unpack_from(body, at)
            if size < REC.size or size & 7 or at + size > len(body):
                raise ValueError(f'bad record size {size} at {at}')
            yield rtype, body[at + REC.size:at + size]
            at += size
        if at != len(body):
            raise ValueError('trailing bytes after the last record')

    @staticmethod
    def keyframe(body):
        kh = KEYF.unpack_from(body, 0)
        at = KEYF.size
        states = []
        for _ in range(kh[4]):
            cid, flags, crc, _, stored, raw = CHUNK.unpack_from(body, at)
            b = body[at + CHUNK.size:at + CHUNK.size + stored]
            if flags & 1:
                b = zlib.decompress(b)
            states.append((cid.decode('latin-1'), stored, raw, zlib.crc32(b) == crc, b))
            at += CHUNK.size + stored + pad8(stored)
        return kh, states

    def cycles(self):
        """Yields (chunk, cycle number, records) for every CYCL chunk, and keyframes as ('KEYF', chunk)."""
        for c in self.chunks:
            if c[1] == 'KEYF':
                yield 'KEYF', c, None
            elif c[1] == 'CYCL':
                recs = list(self.records(self.payload(c)))
                end = [END.unpack_from(b) for t, b in recs if t == 7]
                yield 'CYCL', c, (end[-1][0] if end else None, recs)


def hw_name(address):
    phys = address & 0x1fffffff
    if 0x12000000 <= phys < 0x12002000:
        return GS_PRIV.get((phys - 0x12000000) & ~7, f'GS+{phys - 0x12000000:#x}') + ('.hi' if phys & 4 else '')
    if 0x10003c00 <= phys < 0x10003d00:
        return f'VIF1+{phys - 0x10003c00:#x}'
    if 0x10003000 <= phys < 0x10003100:
        return f'GIF+{phys - 0x10003000:#x}'
    return f'{address:#010x}'


def describe(rtype, b):
    if rtype == 1:
        first, count = struct.unpack_from('<II', b)
        where = f'RAM 0x{first * PAGE:07x}' if first < RAM_PAGES else f'SPR 0x{(first - RAM_PAGES) * PAGE:04x}'
        return f'PAGE {count} page(s) from {where} ({count * 4} KB)'
    if rtype == 2:
        address, width, value, ra, _ = HW.unpack_from(b)
        return f'HW   {hw_name(address):10s} = {value:#x} ({width}-bit, ra {ra:#x})'
    if rtype == 3:
        i, m, f, _ = struct.unpack_from('<iiiI', b)
        return f'CRT  interlaced {i} mode {m} ffmd {f}'
    if rtype == 4:
        k = KICK.unpack_from(b)
        index, chcr, madr, qwc, tadr = k[0:5]
        ch, mode = k[8], k[9]
        kind = 'chain' if mode == 1 else 'normal'
        src = f'TADR {tadr:#x}' if mode == 1 else f'MADR {madr:#x} QWC {qwc}'
        return (f'KICK #{index} ch{ch} {kind} {src}: {k[12]} bytes, {k[11]} tags, hash {k[19]:016x}, ra {k[7]:#x}'
                + (f', {k[16]} spans' if k[16] else '') + (f', {k[17]} labels' if k[17] else ''))
    if rtype == 5:
        seq, kind, op, depth, anchor, parent, nbytes, _ = TAG.unpack_from(b)
        return f'TAG  #{seq} kind {kind} {("set", "begin", "end")[op] if op < 3 else op} depth {depth} anchor {anchor:#x} ({nbytes} bytes)'
    if rtype == 6:
        kind, address, value = struct.unpack_from('<IIQ', b)
        return f'ODD  {ODD_NAMES.get(kind, kind)} address {address:#x} value {value:#x}'
    if rtype == 7:
        e = END.unpack_from(b)
        return (f'END  cycle {e[0]} game frame {e[1] if e[1] != 2**64 - 1 else "-"} field {e[2]} host {e[3] / 1e6:.1f} ms '
                f'DISPFB2 {e[4]:#x} FBP {e[10]:#x}, {e[11]} kicks')
    if rtype == 8:
        return 'NOTE ' + b.rstrip(b'\0').decode('utf-8', 'replace')
    return f'type {rtype} ({len(b)} bytes)'


def cmd_info(d):
    h = d.header
    print(f'{d.path}: {len(d.data) / 1048576:.1f} MB, format {h["formatVersion"]}, '
          f'{"Europe" if h["gameVersion"] else "USA"} {h["elfName"].rstrip(b"\0").decode()}, '
          f'{h["width"]}x{h["height"]} at {h["fieldRateMilliHz"] / 1000:.3f} Hz, backend {h["gsBackend"].rstrip(b"\0").decode()}, '
          f'build {h["build"].rstrip(b"\0").decode()}, {"exact" if h["flags"] & 1 else "plain"}, '
          f'{"complete" if d.complete() else ("cut short" if d.partial else "not closed")}')
    by_chunk = {}
    for c in d.chunks:
        s = by_chunk.setdefault(c[1], [0, 0, 0])
        s[0] += 1
        s[1] += c[4]
        s[2] += c[5]
    for cid, (n, stored, raw) in by_chunk.items():
        print(f'  {cid}: {n} chunk(s), {raw / 1048576:.2f} MB raw, {stored / 1048576:.2f} MB stored')
    by_rec = {}
    cycles = []
    segments = 0
    for kind, c, info in d.cycles():
        if kind == 'KEYF':
            kh, states = d.keyframe(d.payload(c))
            final = kh[3] & 2
            segments += 0 if final else 1
            print(f'  keyframe{" (final)" if final else ""} before cycle {kh[0]}: ' +
                  ', '.join(f'{s[0]} {s[2] // 1024} KB -> {s[1] // 1024} KB' for s in states))
            continue
        number, recs = info
        cycles.append(number)
        for t, b in recs:
            s = by_rec.setdefault(t, [0, 0])
            s[0] += 1
            s[1] += len(b) + REC.size
    n = max(1, len(cycles))
    print(f'  {segments} segment(s), {len(cycles)} cycle(s)' + (f' ({cycles[0]}-{cycles[-1]})' if cycles else ''))
    for t in sorted(by_rec):
        count, size = by_rec[t]
        print(f'  {REC_NAMES.get(t, t):5s} {count:8d} records {size / 1048576:9.2f} MB raw ({size / n / 1024:.1f} KB a cycle)')
    return 0


def cmd_list(d, which=None):
    for kind, c, info in d.cycles():
        if kind == 'KEYF':
            if which is None:
                kh, states = d.keyframe(d.payload(c))
                print(f'-- keyframe{" (final)" if kh[3] & 2 else ""}: {", ".join(s[0] for s in states)}')
            continue
        number, recs = info
        if which is not None and number != which:
            continue
        print(f'-- cycle {number}')
        for t, b in recs:
            print('  ' + describe(t, b))
    return 0


def parse_tag(b):
    seq, kind, op, depth, anchor, parent, nbytes, _ = TAG.unpack_from(b)
    return {'seq': seq, 'kind': kind, 'op': op, 'depth': depth, 'anchor': anchor, 'parent': parent,
            'payload': bytes(b[TAG.size:TAG.size + nbytes])}


def tag_text(t, names):
    """One tag in words: kind, op and the payload decoded (render/gfx/tags.h)."""
    n = lambda table, v: names.get((table, v), f'{table}{v}')  # noqa: E731
    kind, op, p = t['kind'], t['op'], t['payload']
    text = f'#{t["seq"]} {n("kind", kind)} {n("op", op)}'
    if op == 2:
        return text + f' (of #{t["parent"]})'
    if kind == 1 and len(p) >= 24:
        gf, rf, scene, flags, _ = struct.unpack_from('<QIIII', p)
        text += f' game frame {gf} rdr frame {rf} scene {scene:04x}'
    elif kind == 2 and len(p) >= 104:
        v = struct.unpack_from('<I4if3f9f5fIII', p)
        text += (f' view {v[0]} scissor {v[1]},{v[2]}-{v[3]},{v[4]} y offset {v[5]:g} camera {v[24]:#x} at '
                 f'({v[6]:.1f}, {v[7]:.1f}, {v[8]:.1f}) dVal {v[18]:g} z {v[21]:g}-{v[22]:g} angle {v[23]:#x}')
    elif kind in (4, 5) and len(p) >= 16:
        dk, obj, part, ordinal = struct.unpack_from('<IIII', p)
        text += f' {n("draw", dk)} object {obj:#x} part {part} ordinal {ordinal}'
        if kind == 5 and len(p) >= 40:
            element, anchor = struct.unpack_from('<II', p, 16)
            text += f' hud {n("hud", element)} anchor {n("anchor", anchor)}'
        if kind == 4 and len(p) >= 72 and struct.unpack_from('<I', p, 20)[0]:
            m = struct.unpack_from('<12f', p, 24)
            text += f' at ({m[9]:.1f}, {m[10]:.1f}, {m[11]:.1f})'
    elif kind == 6 and len(p) >= 16:
        what, obj, a, b2 = struct.unpack_from('<IIII', p)
        text += f' {n("kick", what)} object {obj:#x} a {a:#x} b {b2:#x}'
    elif kind == 7:
        text += ' "' + p.rstrip(b'\0').decode('utf-8', 'replace') + '"'
    if t['anchor']:
        text += f' anchor {t["anchor"]:#x}'
    return text + f' depth {t["depth"]}'


def cmd_tags(d, which):
    names = d.tag_names()
    known = {}  # seq -> tag, from TAGP and every cycle up to `which` + 2
    cycles = []
    for kind, c, info in d.cycles():
        if kind == 'KEYF':
            kh, states = d.keyframe(d.payload(c))
            for s in states:
                if s[0] == 'TAGP':
                    for t, b in d.records(s[4]):
                        if t == 5:
                            tg = parse_tag(b)
                            known.setdefault(tg['seq'], tg)
            continue
        number, recs = info
        if number is None or number > which + 2:
            break
        for t, b in recs:
            if t == 5:
                tg = parse_tag(b)
                tg['cycle'] = number
                known[tg['seq']] = tg
        cycles.append((number, recs))
    target = [r for n, r in cycles if n == which]
    if not target:
        print(f'no cycle {which}')
        return 1
    resolved = set()
    for number, recs in cycles:
        for t, b in recs:
            if t == 4:
                k = KICK.unpack_from(b)
                for i in range(k[16]):
                    resolved.add(struct.unpack_from('<IIIB', b, KICK.size + i * 16)[2])
    ntags = 0
    pairs_ok = pairs_open = 0
    for t, b in target[0]:
        if t == 5:
            ntags += 1
            print('  TAG  ' + tag_text(parse_tag(b), names))
        elif t == 4:
            k = KICK.unpack_from(b)
            ch, mode, nbytes = k[8], k[9], k[12]
            labels = [struct.unpack_from('<I', b, KICK.size + k[16] * 16 + 4 * i)[0] for i in range(k[17])]
            label_text = ', '.join(tag_text(known[s], names).split(' depth')[0] if s in known else f'#{s} (not held)' for s in labels)
            print(f'  KICK #{k[0]} ch{ch} {"chain" if mode == 1 else "normal"}: {nbytes} bytes'
                  + (f'; labels: {label_text}' if labels else '; no label'))
            spans = [struct.unpack_from('<IIIB', b, KICK.size + i * 16) for i in range(k[16])]
            for so, ga, seq, op in spans:
                tg = known.get(seq)
                desc = tag_text(tg, names).split(' anchor')[0] if tg else f'#{seq} (not held)'
                if tg and op == 2 and tg['parent'] in known:
                    desc += ': ' + tag_text(known[tg['parent']], names).split(' anchor')[0].split(' ', 1)[1]
                print(f'      span stream +{so:#x} of {nbytes:#x} guest {ga:#x}: {desc}')
                if op == 1:
                    if any(o == 2 and s2 in known and known[s2]['parent'] == seq and o2 >= so
                           for o2, _, s2, o in spans):
                        pairs_ok += 1
                    else:
                        pairs_open += 1
    made_here = [tg for tg in known.values() if tg.get('cycle') == which and tg['anchor'] and tg['op'] in (1, 2)
                 and tg['kind'] != 6]
    unresolved = [tg for tg in made_here if tg['seq'] not in resolved]
    print(f'{ntags} tag(s) in cycle {which}; {pairs_ok} begin/end pair(s) resolved in one kick, {pairs_open} begin(s) without '
          f'their end in the kick; {len(made_here)} anchored tag(s) made in this cycle, {len(unresolved)} not resolved by a kick '
          f'up to cycle {which + 2}' + ('' if cycles[-1][0] >= which + 2 else ' (the dump ends before it)'))
    for tg in unresolved[:20]:
        print('  unresolved ' + tag_text(tg, names))
    return 0


def cmd_kick(d, which, index):
    ram = {}
    for kind, c, info in d.cycles():
        if kind == 'KEYF':
            kh, _ = d.keyframe(d.payload(c))
            if not kh[3] & 2:
                ram = {}
            continue
        number, recs = info
        for t, b in recs:
            if t == 1:
                first, count = struct.unpack_from('<II', b)
                for i in range(count):
                    ram[first + i] = b[8 + i * PAGE:8 + (i + 1) * PAGE]
            elif t == 4 and number == which:
                k = KICK.unpack_from(b)
                if k[0] != index:
                    continue
                names = ('index', 'chcr', 'madr', 'qwc', 'tadr', 'asr0', 'asr1', 'callerRa', 'channel', 'mode', 'kflags',
                         'dmaTags', 'streamBytes', 'gifBytes', 'mxcsr', 'fbrst', 'spanCount', 'labelCount', 'field',
                         'streamHash', 'gifHash')
                for name, value in zip(names, k):
                    print(f'  {name:12s} {value:#x}' if isinstance(value, int) and name not in ('index', 'dmaTags', 'streamBytes', 'spanCount', 'labelCount', 'field') else f'  {name:12s} {value}')
                at = KICK.size
                for i in range(k[16]):
                    so, ga, seq, op = struct.unpack_from('<IIIB', b, at + i * 16)
                    print(f'  span stream+{so:#x} guest {ga:#x} tag #{seq} {"begin" if op == 1 else "end"}')
                start = (k[4] if k[9] == 1 else k[2])
                phys = start & 0x1fffffff
                scratch = 0x70000000 <= (start & 0x7fffffff) < 0x70004000
                page = (RAM_PAGES + ((start & 0x7fffffff) - 0x70000000) // PAGE) if scratch else phys // PAGE
                off = ((start & 0x7fffffff) - 0x70000000) % PAGE if scratch else phys % PAGE
                if page in ram:
                    first = ram[page][off:off + 64]
                    print(f'  first bytes at {"TADR" if k[9] == 1 else "MADR"} {start:#x}:')
                    for i in range(0, len(first), 16):
                        print('    ' + ' '.join(f'{x:02x}' for x in first[i:i + 16]))
                else:
                    print(f'  page of {start:#x} not in the rebuilt image')
                return 0
    print(f'no kick {index} in cycle {which}')
    return 1


def cmd_png(d, which, out, opaque):
    w, h = d.header['width'], d.header['height']
    prev = None
    for c in d.chunks:
        if c[1] == 'CYCL':
            recs = list(d.records(d.payload(c)))
            ends = [END.unpack_from(b) for t, b in recs if t == 7]
            prev = ends[-1][0] if ends else None
        elif c[1] == 'REFI' and prev == which:
            px = bytearray(d.payload(c))
            if opaque:
                px[3::4] = b'\xff' * (len(px) // 4)
            rows = b''.join(b'\0' + bytes(px[y * w * 4:(y + 1) * w * 4]) for y in range(h))

            def chunk(tag, body):
                return struct.pack('>I', len(body)) + tag + body + struct.pack('>I', zlib.crc32(tag + body))
            png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + \
                chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')
            open(out, 'wb').write(png)
            print(f'wrote {out}')
            return 0
    print(f'no reference picture for cycle {which} (plain-mode dumps have none)')
    return 1


def cmd_verify(d):
    problems = []
    if not d.header_crc_ok:
        problems.append('RCFR chunk CRC mismatch')
    keys, cycl_offsets, key_offsets = 0, [], []
    for c in d.chunks:
        if c[1] == 'CYCL':
            cycl_offsets.append(c[0])
        try:
            body = d.payload(c)
            if c[1] == 'CYCL':
                recs = list(d.records(body))
                if not recs or recs[-1][0] != 7:
                    problems.append(f'CYCL at {c[0]} does not end with END')
            elif c[1] == 'KEYF':
                kh, states = d.keyframe(body)
                keys += 1
                if not kh[3] & 2:
                    key_offsets.append(c[0])
                for s in states:
                    if not s[3]:
                        problems.append(f'keyframe at {c[0]}: state {s[0]} CRC mismatch')
            elif c[1] == 'INDX' and c[0] == d.header['indexOffset']:
                nseg, ncyc = struct.unpack_from('<QQ', body)
                segs = [struct.unpack_from('<QQQ', body, 16 + 24 * i) for i in range(nseg)]
                cyc = [struct.unpack_from('<QQ', body, 16 + 24 * nseg + 16 * i) for i in range(ncyc)]
                d.index = (segs, cyc)
        except Exception as e:  # noqa: BLE001 - report every problem
            problems.append(str(e))
    index = getattr(d, 'index', None)
    notes = []
    if d.header['indexOffset'] and index is None:
        if d.partial:
            notes.append('the index was cut off with the end of the file')
        else:
            problems.append('the header names an index that is not there')
    if index is not None:
        if [c[0] for c in index[1]] != cycl_offsets:
            problems.append('the index does not match the CYCL chunks')
        if [s[0] for s in index[0]] != key_offsets[:len(index[0])]:
            problems.append('the index does not match the KEYF chunks')
    state = 'complete' if d.complete() else ('cut short (partial chunk at the end)' if d.partial else 'not closed')
    print(f'{d.path}: {len(d.chunks) + 1} chunks, {keys} keyframes, {len(cycl_offsets)} cycles, {state}: '
          + ('ok' if not problems else 'FAILED'))
    for p in problems:
        print('  ' + p)
    for n in notes:
        print('  note: ' + n)
    return 1 if problems else 0


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd, path = argv[1], argv[2]
    try:
        d = Dump(path)
    except (OSError, ValueError) as e:
        print(f'{path}: {e}')
        return 2
    if cmd == 'info':
        return cmd_info(d)
    if cmd == 'list':
        return cmd_list(d, int(argv[3]) if len(argv) > 3 else None)
    if cmd == 'tags' and len(argv) > 3:
        return cmd_tags(d, int(argv[3]))
    if cmd == 'kick' and len(argv) > 4:
        return cmd_kick(d, int(argv[3]), int(argv[4]))
    if cmd == 'png' and len(argv) > 4:
        return cmd_png(d, int(argv[3]), argv[4], '--opaque' in argv)
    if cmd == 'verify':
        return cmd_verify(d)
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
