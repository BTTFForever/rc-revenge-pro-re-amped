#!/usr/bin/env python3
"""gs_survey.py LOG [LOG ...]: summarises GS survey captures (RC_GS_SURVEY=<log>, runtime gs_survey.cpp) as Markdown.

Each LOG is the stream file; LOG.agg beside it holds the aggregates. The capture's name is the file's stem and its
version the folder's name (e.g. survey/usa/level5.log). Sections: captures and scenes, the frame order check, kicks and
buffer high-water marks, traffic outside DMA channels 1 and 2, display registers, register values by path, draw states
with the blend classification, textures, uploads.

Record formats (gs_survey.h): S (one per segment: from one frame boundary, the game's buffer swap, to the next, or 30
fields without one), F (the frame boundary: frame, scene, game counters), I (per-kick intervals, detail segments only),
P (GS privileged register writes), C (SetGsCrt), E (first sighting of an event), G (named address ranges), N (counter
names). Aggregates: R (distinct register values), D (draw states), T (textures), U (uploads), E (event counts), M (maxima).
Scene = mode << 12 | level << 4 | split << 1 | movie, mode as mainVarG: 0 boot, 1 front end, 2 Track Editor, 3 race, 4 demo.
"""
import collections
import os
import re
import sys

MODES = {0: 'boot', 1: 'front end', 2: 'Track Editor', 3: 'race', 4: 'demo race'}
PATHS = {1: 'PATH1 (VU1)', 2: 'PATH2 (VIF DIRECT)', 3: 'PATH3 (GIF ch2)', 4: 'EE direct'}
PSM = {0x00: 'CT32', 0x01: 'CT24', 0x02: 'CT16', 0x0a: 'CT16S', 0x13: 'T8', 0x14: 'T4', 0x1b: 'T8H', 0x24: 'T4HL',
       0x2c: 'T4HH', 0x30: 'Z32', 0x31: 'Z24', 0x32: 'Z16', 0x3a: 'Z16S'}
ABD = ['Cs', 'Cd', '0', '?']
CC = ['As', 'Ad', 'FIX', '?']
ATST = ['NEVER', 'ALWAYS', 'LESS', 'LEQUAL', 'EQUAL', 'GEQUAL', 'GREATER', 'NOTEQUAL']
AFAIL = ['KEEP', 'FB_ONLY', 'ZB_ONLY', 'RGB_ONLY']
ZTST = ['NEVER', 'ALWAYS', 'GEQUAL', 'GREATER']
PRIMT = ['point', 'line', 'linestrip', 'tri', 'tristrip', 'trifan', 'sprite', 'bad']


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


def scene_class(scene):
    mode = scene >> 12
    name = MODES.get(mode, f'mode {mode}')
    if mode in (3, 4):
        name += f' L{(scene >> 4) & 0xff}'
    if scene & 2:
        name += ' split'
    if scene & 1:
        name += ' +movie'
    return name


def scene_kind(scene):
    """Coarser than scene_class: levels merged."""
    mode = scene >> 12
    name = MODES.get(mode, f'mode {mode}')
    if scene & 2:
        name += ' split'
    if scene & 1:
        name += ' +movie'
    return name


def d_prim(p):
    flags = [n for n, b in (('IIP', 3), ('TME', 4), ('FGE', 5), ('ABE', 6), ('AA1', 7), ('FST', 8), ('CTXT2', 9), ('FIX', 10)) if p >> b & 1]
    return PRIMT[p & 7] + ('+' + '+'.join(flags) if flags else '')


def d_alpha(v):
    a, b, c, d, fix = bits(v, 0, 2), bits(v, 2, 2), bits(v, 4, 2), bits(v, 6, 2), bits(v, 32, 8)
    cs = CC[c] if c != 2 else f'FIX {fix:#x}'
    return f'({ABD[a]}-{ABD[b]})*{cs}+{ABD[d]}'


def d_test(v):
    out = []
    if v & 1:
        out.append(f'A {ATST[bits(v, 1, 3)]} {bits(v, 4, 8):#x} fail {AFAIL[bits(v, 12, 2)]}')
    else:
        out.append('A off')
    if v >> 14 & 1:
        out.append(f'DATE {bits(v, 15, 1)}')
    out.append(f'Z {ZTST[bits(v, 17, 2)]}' if v >> 16 & 1 else 'Z off')
    return ', '.join(out)


def d_tex0(v, full=True):
    psm = bits(v, 20, 6)
    s = f'{PSM.get(psm, hex(psm))}'
    if full:
        s += f' {1 << bits(v, 26, 4)}x{1 << bits(v, 30, 4)} TBP {bits(v, 0, 14):#x} TBW {bits(v, 14, 6)}'
    s += f' TCC {bits(v, 34, 1)} TFX {["MODULATE", "DECAL", "HIGHLIGHT", "HIGHLIGHT2"][bits(v, 35, 2)]}'
    if psm in (0x13, 0x14, 0x1b, 0x24, 0x2c):
        s += f' CPSM {PSM.get(bits(v, 51, 4), hex(bits(v, 51, 4)))} CSM{bits(v, 55, 1) + 1} CSA {bits(v, 56, 5)} CLD {bits(v, 61, 3)}'
        if full:
            s += f' CBP {bits(v, 37, 14):#x}'
    return s


def d_tex1(v):
    filt = ['NEAREST', 'LINEAR', 'NEAREST_MIPMAP_NEAREST', 'NEAREST_MIPMAP_LINEAR', 'LINEAR_MIPMAP_NEAREST',
            'LINEAR_MIPMAP_LINEAR', '6?', '7?']
    k = bits(v, 32, 12)
    k = k - 4096 if k & 0x800 else k
    return (f'LCM {bits(v, 0, 1)} MXL {bits(v, 2, 3)} MMAG {filt[bits(v, 5, 1)]} MMIN {filt[bits(v, 6, 3)]} '
            f'MTBA {bits(v, 9, 1)} L {bits(v, 19, 2)} K {k / 16:g}')


def d_clamp(v):
    m = ['REPEAT', 'CLAMP', 'REGION_CLAMP', 'REGION_REPEAT']
    s = f'{m[bits(v, 0, 2)]}/{m[bits(v, 2, 2)]}'
    if bits(v, 0, 2) >= 2 or bits(v, 2, 2) >= 2:
        s += f' U {bits(v, 4, 10)}..{bits(v, 14, 10)} V {bits(v, 24, 10)}..{bits(v, 34, 10)}'
    return s


def d_frame(v):
    return f'FBP {bits(v, 0, 9):#x} FBW {bits(v, 16, 6)} {PSM.get(bits(v, 24, 6), "?")} FBMSK {bits(v, 32, 32):#x}'


def d_zbuf(v):
    return f'ZBP {bits(v, 0, 9):#x} {PSM.get(0x30 | bits(v, 24, 4), "?")} ZMSK {bits(v, 32, 1)}'


def d_scissor(v):
    return f'x {bits(v, 0, 11)}..{bits(v, 16, 11)} y {bits(v, 32, 11)}..{bits(v, 48, 11)}'


def d_xyoffset(v):
    return f'{bits(v, 0, 16) / 16:g}, {bits(v, 32, 16) / 16:g}'


def d_texa(v):
    return f'TA0 {bits(v, 0, 8):#x} AEM {bits(v, 15, 1)} TA1 {bits(v, 32, 8):#x}'


def d_bitblt(v):
    return (f'S {bits(v, 0, 14):#x}/{bits(v, 16, 6)}/{PSM.get(bits(v, 24, 6), "?")} '
            f'D {bits(v, 32, 14):#x}/{bits(v, 48, 6)}/{PSM.get(bits(v, 56, 6), "?")}')


def decode(reg, v):
    return {
        'PRIM': lambda: d_prim(v), 'ALPHA_1': lambda: d_alpha(v), 'ALPHA_2': lambda: d_alpha(v),
        'TEST_1': lambda: d_test(v), 'TEST_2': lambda: d_test(v), 'TEX0_1': lambda: d_tex0(v), 'TEX0_2': lambda: d_tex0(v),
        'TEX1_1': lambda: d_tex1(v), 'TEX1_2': lambda: d_tex1(v), 'CLAMP_1': lambda: d_clamp(v), 'CLAMP_2': lambda: d_clamp(v),
        'FRAME_1': lambda: d_frame(v), 'FRAME_2': lambda: d_frame(v), 'ZBUF_1': lambda: d_zbuf(v), 'ZBUF_2': lambda: d_zbuf(v),
        'SCISSOR_1': lambda: d_scissor(v), 'SCISSOR_2': lambda: d_scissor(v), 'XYOFFSET_1': lambda: d_xyoffset(v),
        'XYOFFSET_2': lambda: d_xyoffset(v), 'TEXA': lambda: d_texa(v), 'BITBLTBUF': lambda: d_bitblt(v),
        'TRXREG': lambda: f'{bits(v, 0, 12)}x{bits(v, 32, 12)}', 'TRXDIR': lambda: ['host->local', 'local->host', 'local->local', 'off'][v & 3],
        'MIPTBP1_1': lambda: f'{bits(v, 0, 14):#x}/{bits(v, 20, 14):#x}/{bits(v, 40, 14):#x}',
        'MIPTBP2_1': lambda: f'{bits(v, 0, 14):#x}/{bits(v, 20, 14):#x}/{bits(v, 40, 14):#x}',
        'FOGCOL': lambda: f'{v & 0xffffff:#08x}', 'PABE': lambda: str(v & 1), 'FBA_1': lambda: str(v & 1),
        'FBA_2': lambda: str(v & 1), 'DTHE': lambda: str(v & 1), 'COLCLAMP': lambda: str(v & 1),
        'PRMODECONT': lambda: str(v & 1), 'SCANMSK': lambda: str(v & 3),
    }.get(reg, lambda: f'{v:#x}')()


# ---- blend classification -------------------------------------------------------------------------------------------

def coefs(alpha):
    """GS colour = (A-B)*C + D as (s1, s0, d1, d0): Cs coefficient s1*c + s0, Cd coefficient d1*c + d0."""
    a, b, d = bits(alpha, 0, 2), bits(alpha, 2, 2), bits(alpha, 6, 2)
    s1 = (a == 0) - (b == 0)
    d1 = (a == 1) - (b == 1)
    return s1, int(d == 0), d1, int(d == 1)


def dst_factor(d1, d0, c):
    """The blend op and destination factor for a Cd coefficient d1*c + d0 (the source term is premultiplied in the shader,
    so its coefficient never limits fixed-function blending). c is 'As', 'Ad' or a FIX value / 128."""
    form = (d1, d0)
    if form == (0, 0):
        return 'blend off', None
    if form == (0, 1):
        return 'add, dst ONE', None
    cname = c if isinstance(c, str) else f'{c:g}'
    if form == (1, 0):
        return f'add, dst factor {cname}', c
    if form == (-1, 1):
        return f'add, dst factor 1-{cname}', c
    if form == (-1, 0):
        return f'subtract, dst factor {cname}', c
    return None, c


def classify(prim, alpha, test, fba, pabe, colclamp, dthe, mina, maxa, high=0):
    """How one draw state maps onto Vulkan blending: a short verdict and notes on what fixed function can't do."""
    notes = []
    verdict = 'fixed function'
    if prim >> 6 & 1:
        s1, s0, d1, d0 = coefs(alpha)
        cc = bits(alpha, 4, 2)
        fix = bits(alpha, 32, 8)
        c = 'As' if cc == 0 else 'Ad' if cc == 1 else fix / 128
        op, factor = dst_factor(d1, d0, c)
        if op and (s1, s0) == (-1, 0):  # the source term is subtracted
            if op.startswith('add'):
                op = 'reverse-subtract' + op[3:]
            elif op.startswith('subtract'):
                op = None
        if op is None:
            verdict = 'NOT fixed function'
            notes.append('destination coefficient outside {0, 1, c, 1-c, -c}')
        else:
            notes.append(op)
            if factor == 'As':
                notes.append('As as a blend factor (dual-source SRC1_ALPHA = As*255/128): exact only while As <= 0x80'
                             + (f'; {high} prims had a vertex alpha above 0x80' if high else ''))
            elif factor == 'Ad':
                verdict = 'NOT fixed function'
                notes.append('Ad as a blend factor: GS 0x80 = 1.0 (needs a scaled alpha store or a destination read)')
            elif isinstance(factor, float) and factor > 1:
                verdict = 'NOT fixed function'
                notes.append('FIX > 0x80')
        if s1 != 0 and cc == 0 and not (factor == 'As'):
            notes.append('As only in the source term (shader)')
        if pabe & 1:
            # Pixels with As < 0x80 are written unblended (Cs, dst 0); the rest blend with As/128 >= 1.
            if cc == 0:
                at = (d1 + d0)  # destination coefficient at As = 0x80
                if at == 0 and (s1 + s0) == 1:
                    notes.append('PABE: both cases are "Cs, dst 0" while As <= 0x80, so blending is off; any As > 0x80 '
                                 'extrapolates past Cs (destination coefficient < 0): not fixed function')
                    verdict = 'fixed function if As <= 0x80' + (' (vertex A > 0x80 seen)' if high else '')
                else:
                    verdict = 'NOT fixed function'
                    notes.append('PABE with C = As: blended pixels have As >= 0x80, factor >= 1')
            elif cc == 2:
                dd = d1 * fix / 128 + d0
                if 0 <= dd <= 1:
                    notes.append(f'PABE: per-pixel destination factor 0 or {dd:g}: dual-source blending')
                else:
                    verdict = 'NOT fixed function'
                    notes.append(f'PABE: per-pixel switch between add and a {dd:g} destination coefficient')
            else:
                verdict = 'NOT fixed function'
                notes.append('PABE with C = Ad')
        if not colclamp & 1:
            verdict = 'NOT fixed function'
            notes.append('COLCLAMP 0: colour wraps')
    if test & 1 and bits(test, 12, 2) != 0 and bits(test, 1, 3) != 1:
        notes.append(f'alpha test fail {AFAIL[bits(test, 12, 2)]}: two draws (colour-only for failing pixels)')
    if test >> 14 & 1:
        notes.append('DATE: destination alpha test')
    if fba & 1:
        notes.append('FBA: alpha MSB forced on write')
    if dthe & 1:
        notes.append('DTHE: dithering')
    if prim >> 7 & 1:
        notes.append('AA1: GS antialiasing')
    if prim >> 5 & 1:
        notes.append('FGE: fog')
    return verdict, notes


# ---- reading --------------------------------------------------------------------------------------------------------

KV = re.compile(r'(\S+?)=(\S+)')


def parse_paths(fields):
    out = {}
    for k, v in fields.items():
        if re.fullmatch(r'p\d', k):
            nums = [int(x) for x in v.split('/')]
            out[int(k[1])] = dict(zip(('packets', 'bytes', 'maxpk', 'tags', 'regw', 'verts', 'prims', 'img'), nums))
    return out


def parse_fbp(v):
    res = {}
    for item in v.split(','):
        if ':' in item:
            f, n = item.split(':')
            res[int(f, 16)] = int(n)
    return res


class Capture:
    def __init__(self, path):
        self.path = path
        self.name = os.path.splitext(os.path.basename(path))[0]
        self.version = os.path.basename(os.path.dirname(os.path.abspath(path)))
        self.segs = []
        self.frames = []
        self.priv = collections.Counter()
        self.privfirst = {}
        self.crt = []
        self.events = []
        self.counter_names = []
        self.ranges = {}
        with open(path, errors='replace') as f:
            for line in f:
                t = line[:1]
                if t == 'S':
                    head = line.split()
                    fields = dict(KV.findall(line))
                    seg = {'idle': ' idle' in line, 'scene': int(fields.get('scene', '0'), 16),
                           'swap': int(fields.get('swap', '0')), 'paths': parse_paths(fields), 'kicks': {}, 'lf': {},
                           'disp': int(fields['disp'], 16) if 'disp' in fields else None, 'fields': fields}
                    fl = fields.get('fld', '0-0').split('-')
                    seg['fld0'], seg['fld1'] = int(fl[0]), int(fl[1])
                    for k, v in fields.items():
                        if k.startswith('k.'):
                            n, b = v.split('/')
                            seg['kicks'][k[2:]] = (int(n), int(b))
                        elif k.startswith('lf.'):
                            seg['lf'][k[3:]] = parse_fbp(v)
                    if 'up' in fields:
                        u = fields['up'].split('/')
                        seg['uploads'], seg['fbuploads'] = int(u[0]), int(u[1])
                    self.segs.append(seg)
                elif t == 'F':
                    parts = line.split()
                    self.frames.append([int(x) for x in parts[3:]])
                elif t == 'P':
                    m = re.match(r'P (\w+)=(\w+)', line)
                    if m:
                        self.priv[(m.group(1), int(m.group(2), 16))] += 1
                elif t == 'C':
                    self.crt.append(line.strip())
                elif t == 'E':
                    self.events.append(line.strip())
                elif t == 'N':
                    self.counter_names = line.split()[1].split(',')
                elif t == 'G':
                    p = line.split()
                    self.ranges[p[3]] = (int(p[4], 16), int(p[5], 16))
        self.R, self.D, self.T, self.U, self.E, self.M = [], [], [], [], [], []
        agg = path + '.agg'
        if os.path.exists(agg):
            with open(agg) as f:
                for line in f:
                    p = line.split()
                    if not p or p[0] == '#':
                        continue
                    if p[0] == 'R':
                        self.R.append((int(p[1], 16), int(p[2]), p[3], int(p[4], 16), int(p[5])))
                    elif p[0] == 'D':
                        v = [int(x, 16) for x in p[3:18]]
                        extra = p[18:]
                        mina, maxa = (int(extra[2], 16), int(extra[3], 16)) if len(extra) >= 4 else (0, 0xff)
                        high = int(extra[4]) if len(extra) >= 5 else 0
                        self.D.append((int(p[1], 16), int(p[2]), v, int(extra[0]), mina, maxa, high))
                    elif p[0] == 'T':
                        self.T.append((int(p[1], 16), int(p[2]), [int(x, 16) for x in p[3:10]], int(p[10])))
                    elif p[0] == 'U':
                        self.U.append((int(p[1], 16), int(p[2]), int(p[3]), int(p[4], 16), int(p[5], 16), int(p[6], 16), int(p[7])))
                    elif p[0] == 'E':
                        self.E.append((int(p[1]), ' '.join(p[2:])))
                    elif p[0] == 'M':
                        self.M.append(line.strip())


# ---- report ---------------------------------------------------------------------------------------------------------

def table(head, rows):
    out = ['| ' + ' | '.join(head) + ' |', '|' + '---|' * len(head)]
    out += ['| ' + ' | '.join(str(c) for c in r) + ' |' for r in rows]
    return '\n'.join(out)


def frame_order(cap):
    """For swap segments (K1, flip, K2 in one segment): does K1 of frame N draw into the buffer K2 of frame N-1 drew
    into, is that the buffer the flip shows, and does K2 of frame N draw into the other one?"""
    res = collections.Counter()
    prev_k2 = None
    for seg in cap.segs:
        lf = seg['lf']
        k2 = set(lf.get('K2', {}))
        k1 = set(lf.get('K1', {}))
        if seg['idle'] or not k2:
            prev_k2 = None
            continue
        res['segments with a K2 draw'] += 1
        if len(k2) != 1:
            res['K2 drew into more than one buffer'] += 1
        if seg['disp'] is not None and prev_k2 is not None:
            res['flip shows the buffer K2 of the frame before drew'] += seg['disp'] in prev_k2
            res['flip shows another buffer'] += seg['disp'] not in prev_k2
            res['K2 draws into the buffer not shown'] += seg['disp'] not in k2
        if k1 and prev_k2 is not None:
            res['K1 drew'] += 1
            res['K1 drew over the previous K2 buffer'] += k1 <= prev_k2
        prev_k2 = k2
    return res


def report(caps):
    out = []
    w = out.append
    w('## Captures\n')
    rows = []
    for c in caps:
        kinds = collections.Counter(scene_kind(s['scene']) for s in c.segs)
        levels = sorted({(s['scene'] >> 4) & 0xff for s in c.segs if (s['scene'] >> 12) in (3, 4)})
        rows.append([c.version, c.name, len(c.segs), sum(not s['idle'] for s in c.segs), sum(s['idle'] for s in c.segs),
                     ', '.join(f'{k} {n}' for k, n in kinds.most_common()), ','.join(map(str, levels))])
    w(table(['ver', 'capture', 'segments', 'swaps', 'idle', 'segments by scene', 'levels'], rows))

    w('\n\n## Frame order\n')
    rows = []
    total = collections.Counter()
    for c in caps:
        r = frame_order(c)
        total.update(r)
        if r:
            rows.append([c.version, c.name] + [r[k] for k in FO_KEYS])
    rows.append(['all', ''] + [total[k] for k in FO_KEYS])
    w(table(['ver', 'capture'] + FO_KEYS, rows))

    w('\n\n## Kicks per segment (swap segments; idle segments separately)\n')
    rows = []
    for idle in (False, True):
        stats = collections.defaultdict(lambda: [0, 0, 0, 0])  # segments with, total kicks, max kicks, max bytes in a segment
        nseg = 0
        for c in caps:
            for s in c.segs:
                if s['idle'] != idle:
                    continue
                nseg += 1
                for k, (n, b) in s['kicks'].items():
                    st = stats[k]
                    st[0] += 1
                    st[1] += n
                    st[2] = max(st[2], n)
                    st[3] = max(st[3], b)
        for k, st in sorted(stats.items()):
            rows.append(['idle' if idle else 'swap', k, st[0], f'{st[1] / max(st[0], 1):.1f}', st[2], st[3]])
    w(table(['segment', 'kick', 'segments with it', 'kicks per such segment', 'max kicks', 'max bytes in one segment'], rows))

    w('\n\n## Paths per segment\n')
    rows = []
    for p in (1, 2, 3, 4):
        segs = [s['paths'][p] for c in caps for s in c.segs if p in s['paths']]
        if not segs:
            rows.append([PATHS[p], 0, '', '', '', '', ''])
            continue
        rows.append([PATHS[p], len(segs), max(x['bytes'] for x in segs), max(x['maxpk'] for x in segs),
                     max(x['regw'] for x in segs), max(x['prims'] for x in segs), max(x['img'] for x in segs)])
    w(table(['path', 'segments with it', 'max bytes', 'max packet', 'max register writes', 'max primitives', 'max image bytes'], rows))

    w('\n\n## Maxima by capture (aggregates)\n')
    for c in caps:
        w(f'\n{c.version} {c.name}:\n')
        for m in c.M:
            if 'maxbytes=0 ' not in m and 'maxpacket=0 ' not in m:
                w(f'    {m}')

    w('\n\n## Game buffer counters (max over captures)\n')
    names = next((c.counter_names for c in caps if c.counter_names), [])
    rows = []
    for c in caps:
        if not c.frames:
            continue
        cols = list(zip(*c.frames))
        mx = [max(col) for col in cols[: len(names)]]
        # currDmaTag counts from 1000 on odd frames.
        tags = [v - 1000 if v >= 1000 else v for v in cols[1]] if len(cols) > 1 else [0]
        rows.append([c.version, c.name] + mx[:1] + [max(tags)] + mx[2:])
    w(table(['ver', 'capture', names[0] + ' (qw)', 'tags used'] + names[2:], rows))

    w('\n\n## Events (traffic outside the planned kicks, unusual state)\n')
    ev = collections.defaultdict(lambda: collections.Counter())
    for c in caps:
        for n, text in c.E:
            ev[text][f'{c.version} {c.name}'] += n
    for text, where in sorted(ev.items()):
        w(f'- `{text}`: ' + ', '.join(f'{k} ({n})' for k, n in sorted(where.items())))

    w('\n\n## Display: SetGsCrt and privileged registers\n')
    crts = collections.Counter((c.version, l.split(' fld')[0]) for c in caps for l in c.crt)
    for (v, l), n in sorted(crts.items()):
        w(f'- {v}: `{l}` x{n}')
    priv = collections.defaultdict(collections.Counter)
    for c in caps:
        for (reg, val), n in c.priv.items():
            priv[(c.version, reg)][val] += n
    rows = []
    for (v, reg), vals in sorted(priv.items()):
        rows.append([v, reg, ', '.join(f'{d_priv(reg, x)} x{n}' for x, n in sorted(vals.items()))])
    w('\n' + table(['ver', 'register', 'values (writes)'], rows))

    w('\n\n## Register values by path\n')
    regs = collections.defaultdict(lambda: collections.defaultdict(lambda: [0, set(), set()]))
    for c in caps:
        for scene, path, reg, val, n in c.R:
            if reg in ('TEX0_1', 'TEX0_2', 'TRXPOS', 'BITBLTBUF', 'MIPTBP1_1', 'MIPTBP2_1', 'LABEL', 'SIGNAL', 'FINISH'):
                continue
            e = regs[(reg, path)][val]
            e[0] += n
            e[1].add(scene_kind(scene))
            e[2].add(c.version)
    rows = []
    for (reg, path), vals in sorted(regs.items()):
        for val, (n, kinds, vers) in sorted(vals.items(), key=lambda x: -x[1][0]):
            rows.append([reg, path, f'{val:#x}', decode(reg, val), n, ', '.join(sorted(kinds)), '/'.join(sorted(vers))])
    w(table(['register', 'path', 'value', 'meaning', 'writes', 'scenes', 'ver'], rows))

    w('\n\n## Draw states (primitives drawn, by path)\n')
    w('Key: PRIM (with PRMODE when PRMODECONT is 0), ALPHA (only when ABE), TEST, FBA, PABE, TEX0 format bits, TEX1, '
      'CLAMP modes, FRAME, ZBUF, COLCLAMP, DTHE, TEXA, FOGCOL, DIMX. Here grouped without FRAME/ZBUF/TEX0/TEX1/CLAMP.\n')
    groups = collections.defaultdict(lambda: [0, set(), 0xff, 0, set(), collections.Counter(), 0])
    for c in caps:
        for scene, path, v, prims, mina, maxa, high in c.D:
            prim, alpha, test, fba, pabe = v[0], v[1], v[2], v[3], v[4]
            key = (path, prim & 0x7f7, alpha, test, fba & 1, pabe & 1, v[10] & 1, v[11] & 1)
            g = groups[key]
            g[0] += prims
            g[1].add(scene_kind(scene))
            g[2] = min(g[2], mina)
            g[3] = max(g[3], maxa)
            g[4].add(c.version)
            g[5][f'{c.name}'] += prims
            g[6] += high
    rows = []
    for key, g in sorted(groups.items(), key=lambda x: (x[0][0], -x[1][0])):
        path, prim, alpha, test, fba, pabe, colclamp, dthe = key
        verdict, notes = classify(prim, alpha, test, fba, pabe, colclamp, dthe, g[2], g[3], g[6])
        rows.append([path, d_prim(prim), d_alpha(alpha) if prim >> 6 & 1 else '-', d_test(test), pabe, fba, colclamp, dthe,
                     g[0], f'{g[2]:#x}-{g[3]:#x}', g[6], ', '.join(sorted(g[1])), verdict, '; '.join(notes)])
    w(table(['path', 'PRIM', 'ALPHA', 'TEST', 'PABE', 'FBA', 'COLCLAMP', 'DTHE', 'prims', 'vertex A', 'prims A>0x80', 'scenes', 'Vulkan', 'notes'], rows))

    w('\n\n## Frame and Z buffers drawn into (draw states)\n')
    fz = collections.defaultdict(lambda: [0, set()])
    for c in caps:
        for scene, path, v, prims, mina, maxa, high in c.D:
            e = fz[(path, v[8], v[9])]
            e[0] += prims
            e[1].add(scene_kind(scene))
    rows = [[p, d_frame(f), d_zbuf(z), n, ', '.join(sorted(k))] for (p, f, z), (n, k) in sorted(fz.items())]
    w(table(['path', 'FRAME', 'ZBUF', 'prims', 'scenes'], rows))

    w('\n\n## Textures sampled\n')
    fmt = collections.defaultdict(lambda: [0, 0, set(), set()])
    tex1s = collections.defaultdict(lambda: [0, set()])
    clamps = collections.Counter()
    below = []
    distinct = set()
    for c in caps:
        for scene, path, v, prims in c.T:
            tex0, tex1, clamp, m1, m2, texa, texclut = v
            psm = bits(tex0, 20, 6)
            key = (path, PSM.get(psm, hex(psm)), f'{1 << bits(tex0, 26, 4)}x{1 << bits(tex0, 30, 4)}',
                   (f'{PSM.get(bits(tex0, 51, 4), "?")} CSM{bits(tex0, 55, 1) + 1} CSA{bits(tex0, 56, 5)} CLD{bits(tex0, 61, 3)}'
                    if psm in (0x13, 0x14, 0x1b, 0x24, 0x2c) else '-'),
                   f'TCC{bits(tex0, 34, 1)} TFX{bits(tex0, 35, 2)}')
            e = fmt[key]
            e[0] += prims
            e[1] += 1
            e[2].add(scene_kind(scene))
            e[3].add(c.version)
            t = tex1s[(path, tex1)]
            t[0] += prims
            t[1].add(scene_kind(scene))
            clamps[(path, d_clamp(clamp))] += prims
            distinct.add((c.version, tex0 & ~(7 << 61)))
            if bits(tex0, 0, 14) < 0x1e00:
                below.append((c.version, c.name, d_tex0(tex0)))
    rows = [[k[0], k[1], k[2], k[3], k[4], e[1], e[0], ', '.join(sorted(e[2])), '/'.join(sorted(e[3]))]
            for k, e in sorted(fmt.items(), key=lambda x: (x[0][0], -x[1][0]))]
    w(table(['path', 'PSM', 'size', 'CLUT', 'colour', 'TEX0 values', 'prims', 'scenes', 'ver'], rows))
    w(f'\n\nDistinct TEX0 values (CLD masked): {len(distinct)}. Sampled below block 0x1e00 (frame or Z buffer memory): '
      f'{len(below)}' + (': ' + '; '.join(f'{v} {n} {t}' for v, n, t in below[:10]) if below else '') + '\n')
    rows = [[p, f'{v:#x}', d_tex1(v), n, ', '.join(sorted(k))] for (p, v), (n, k) in sorted(tex1s.items())]
    w(table(['path', 'TEX1', 'meaning', 'prims', 'scenes'], rows))
    w('\n')
    rows = [[p, m, n] for (p, m), n in sorted(clamps.items())]
    w(table(['path', 'CLAMP', 'prims'], rows))

    w('\n\n## Uploads (BITBLTBUF/TRXDIR)\n')
    up = collections.defaultdict(lambda: [0, set(), set(), set()])
    for c in caps:
        for scene, path, d, bb, pos, reg, n in c.U:
            dbp = bits(bb, 32, 14)
            dest = 'frame/Z memory (<0x1e00)' if dbp < 0x1e00 else 'texture memory'
            key = (path, ['host->local', 'local->host', 'local->local', '3'][d], dest, PSM.get(bits(bb, 56, 6), '?'),
                   f'{bits(reg, 0, 12)}x{bits(reg, 32, 12)}', bits(bb, 48, 6))
            e = up[key]
            e[0] += n
            e[1].add(scene_kind(scene))
            e[2].add(c.version)
            e[3].add(dbp)
    rows = [[k[0], k[1], k[2], k[3], k[4], k[5], e[0], len(e[3]), ', '.join(sorted(e[1])), '/'.join(sorted(e[2]))]
            for k, e in sorted(up.items(), key=lambda x: (x[0][2], -x[1][0]))]
    w(table(['path', 'dir', 'destination', 'PSM', 'size', 'DBW', 'count', 'distinct DBP', 'scenes', 'ver'], rows))
    return '\n'.join(out)


FO_KEYS = ['segments with a K2 draw', 'K2 drew into more than one buffer', 'flip shows the buffer K2 of the frame before drew',
           'flip shows another buffer', 'K2 draws into the buffer not shown', 'K1 drew', 'K1 drew over the previous K2 buffer']


def d_priv(reg, v):
    if reg.startswith('DISPFB'):
        return f'FBP {bits(v, 0, 9):#x} FBW {bits(v, 9, 6)} {PSM.get(bits(v, 15, 5), "?")} x{bits(v, 32, 11)} y{bits(v, 43, 11)}'
    if reg.startswith('DISPLAY'):
        return f'DX {bits(v, 0, 12)} DY {bits(v, 12, 11)} MAGH {bits(v, 23, 4) + 1} MAGV {bits(v, 27, 2) + 1} DW {bits(v, 32, 12) + 1} DH {bits(v, 44, 11) + 1}'
    if reg == 'SMODE2':
        return f'INT {v & 1} FFMD {v >> 1 & 1} (SCE_GS_{"FRAME" if v >> 1 & 1 else "FIELD"})'
    if reg == 'PMODE':
        return f'EN1 {v & 1} EN2 {v >> 1 & 1} MMOD {v >> 5 & 1} AMOD {v >> 6 & 1} SLBG {v >> 7 & 1} ALP {bits(v, 8, 8):#x}'
    return f'{v:#x}'


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    caps = []
    for p in sys.argv[1:]:
        with open(p, errors='replace') as f:
            if not f.readline().startswith('# gs survey'):
                print(f'(skipped {p}: not a survey stream)', file=sys.stderr)
                continue
        caps.append(Capture(p))
    print(report(caps))
    return 0


if __name__ == '__main__':
    sys.exit(main())
