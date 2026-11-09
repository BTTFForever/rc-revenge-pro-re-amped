#!/usr/bin/env python3
"""Reads one level file from the disc (BB/<n>/<n>.BBK) and writes its world for the track viewer.

A BBK file is a 0x14EC-byte header followed by zlib sections in order: sound, a GS memory image, a game
data image with absolute addresses already applied, and handles. The world is the shape the level header
points to: a DMA chain of the data TRdr_VUShapeProg consumes. Each texture block is four quadwords of GS
state, then ten per triangle: normal, three positions, three (s, t, q), three integer colours.

Usage: bbk_export.py <file.BBK> <gs_tables.bin> <name> <output.js> [--menu]
"""
import base64
import hashlib
import io
import json
import struct
import sys
import zlib

import numpy as np
from PIL import Image, ImageDraw

HEADER_SIZE = 0x14EC
DRAM_HEADER = 0x14CC
VRAM_HEADER = 0x11B8
GS_IMAGE_START = 0x1E0000
TILE_BYTES = 0x4000
TSO_SIZE = 0x3C
LEVEL_HEADER_FROM_END = 0x70
MENU_HEADER_FROM_END = 0x140


def clut_swizzle(index):
    return (index & 0xE7) | ((index & 0x08) << 1) | ((index & 0x10) >> 1)


class GsMemory:
    """GS local memory, built the way the game uploads it: 64x64 32-bit tiles from 0x1E0000."""

    def __init__(self, tables_path, image):
        self.formats = {}
        data = open(tables_path, 'rb').read()
        at = 0
        while at < len(data):
            psm, sx, sy, count = struct.unpack_from('<4I', data, at)
            self.formats[psm] = (sx, sy, np.frombuffer(data, np.uint16, count, at + 16).astype(np.uint32))
            at += 16 + count * 2
        self.words = np.zeros(1 << 20, np.uint32)
        tile = 0
        for start in range(0, len(image) - TILE_BYTES + 1, TILE_BYTES):
            address = GS_IMAGE_START + tile * TILE_BYTES
            tile += 1
            if address >= 0x400000:
                break
            x, y = np.meshgrid(np.arange(64), np.arange(64))
            index = self.pixel_index(0, address >> 8, 1, x, y)
            self.words[index & 0xFFFFF] = np.frombuffer(image, np.uint32, 64 * 64, start).reshape(64, 64)
        self.bytes = self.words.view(np.uint8)

    def pixel_index(self, psm, base, width, x, y):
        sx, sy, table = self.formats[psm]
        per_row = (width * 64) >> sx
        page = (base >> 5) + (y >> sy) * per_row + (x >> sx)
        entry = ((base & 31) << (sx + sy)) + ((y & ((1 << sy) - 1)) << sx) + (x & ((1 << sx) - 1))
        return (page << (sx + sy)) + table[entry]

    def texture(self, fmt, base, width, height, clut_base):
        x, y = np.meshgrid(np.arange(width), np.arange(height))
        buffer_width = max(1, width // 64)
        index = self.pixel_index(fmt, base, buffer_width, x, y)
        if fmt == 0:
            return self.words[index & 0xFFFFF].view(np.uint8).reshape(height, width, 4).copy()
        if fmt == 0x13:
            values = self.bytes[index & 0x3FFFFF]
            slots = np.array([clut_swizzle(i) for i in range(256)])
            palette = self.words[self.pixel_index(0, clut_base, 1, slots % 16, slots // 16) & 0xFFFFF]
        elif fmt == 0x14:
            packed = self.bytes[(index >> 1) & 0x3FFFFF]
            values = (packed >> ((index & 1) * 4)) & 15
            slots = np.arange(16)
            palette = self.words[self.pixel_index(0, clut_base, 1, slots % 8, slots // 8) & 0xFFFFF]
        else:
            return None
        return palette[values].view(np.uint8).reshape(height, width, 4).copy()


class Level:
    def __init__(self, path, tables):
        file = open(path, 'rb').read()
        if struct.unpack_from('<I', file, 4)[0] != 0xFADED007:
            raise SystemExit('not a level file')
        sections, at = [], HEADER_SIZE
        while at + 4 <= len(file) and len(sections) < 4:
            size = struct.unpack_from('<I', file, at)[0]
            sections.append(zlib.decompress(file[at + 4:at + 4 + size]))
            at += 4 + size
        self.end = struct.unpack_from('<I', file, DRAM_HEADER + 0xC)[0]
        base = struct.unpack_from('<I', file, DRAM_HEADER + 0x10)[0]
        self.data = bytearray(1 << 25)
        self.data[base & 0x1FFFFFF:(base & 0x1FFFFFF) + len(sections[2])] = sections[2]
        self.gs = GsMemory(tables, sections[1])

    def u32(self, address):
        return struct.unpack_from('<I', self.data, address & 0x1FFFFFF)[0]

    def i32(self, address):
        return struct.unpack_from('<i', self.data, address & 0x1FFFFFF)[0]

    def f32(self, address):
        return struct.unpack_from('<f', self.data, address & 0x1FFFFFF)[0]


def vif_runs(level, start, limit=200000):
    """Follows the DMA chain and yields VU memory (1024 quadwords) as each microprogram run would see it."""
    at = start
    memory = bytearray(16 * 1024)
    dirty = False
    for _ in range(limit):
        tag = level.u32(at)
        qwc = tag & 0xFFFF
        identifier = (tag >> 28) & 7
        words = struct.unpack_from('<%dI' % (2 + qwc * 4), level.data, (at + 8) & 0x1FFFFFF)
        i = 0
        while i < len(words):
            code = words[i]
            i += 1
            cmd = (code >> 24) & 0x7F
            immediate = code & 0xFFFF
            num = (code >> 16) & 0xFF
            if cmd in (0x14, 0x15, 0x17):
                if dirty:
                    yield bytes(memory)
                    dirty = False
            elif cmd == 0x20:
                i += 1
            elif cmd in (0x30, 0x31):
                i += 4
            elif cmd == 0x4A:
                i += (num or 256) * 2
            elif cmd in (0x50, 0x51):
                i += (immediate or 65536) * 4
            elif cmd >= 0x60:
                count = num or 256
                vn, vl = (cmd >> 2) & 3, cmd & 3
                if vn == 3 and vl == 0:
                    address = immediate & 0x3FF
                    for quad in range(count):
                        memory[(address + quad) * 16 % 16384:(address + quad) * 16 % 16384 + 16] = struct.pack('<4I', *words[i:i + 4])
                        i += 4
                    dirty = True
                else:
                    i += ((32 >> vl) * (vn + 1) * count + 31) // 32
        if identifier in (6, 7):  # The chain ends with RET, or END.
            break
        at += (qwc + 1) * 16
    if dirty:
        yield bytes(memory)


def add_triangle(group, buffer, offset, rotation, translation):
    """Adds the ten-quadword triangle at a byte offset in buffer."""
    normal = np.array(struct.unpack_from('<3f', buffer, offset), np.float32)
    winding = struct.unpack_from('<h', buffer, offset + 12)[0]
    if rotation is not None:
        normal = rotation[0] * normal[0] + rotation[1] * normal[1] + rotation[2] * normal[2]
    for corner in range(3):
        p = struct.unpack_from('<3f', buffer, offset + (1 + corner) * 16)
        st = struct.unpack_from('<3f', buffer, offset + (4 + corner) * 16)
        c = struct.unpack_from('<4i', buffer, offset + (7 + corner) * 16)
        if rotation is not None:
            p = rotation[0] * p[0] + rotation[1] * p[1] + rotation[2] * p[2] + translation
        group['pos'].append(p)
        group['uv'].append(st if abs(st[2]) > 1e-9 else (st[0], st[1], 1.0))
        group['col'].append(c)
        group['nrm'].append((normal[0], normal[1], normal[2], 1.0 if winding != 0 else 0.0))


def read_triangles(level, shape, groups, rotation=None, translation=None):
    """Adds a shape's triangles to groups, keyed by texture. rotation and translation place a prop."""
    triangles = 0
    listing = level.u32(shape + 0x44)
    by_base = {}
    for i in range(min(level.u32(shape + 0x38), 4096)):
        entry = level.u32(listing + 4 * i)
        if entry:
            by_base.setdefault(level.i32(entry + 0x2C), entry)
    for memory in vif_runs(level, level.u32(shape + 0x48)):
        at = 0
        while at < 1024:
            count = struct.unpack_from('<h', memory, at * 16 + 8)[0]
            if count == 0:
                break
            if count < 0:
                at += 1 - count
                continue
            # The block starts with the GS TEX0 value, whose low bits are the texture's address.
            texture = by_base.get(struct.unpack_from('<I', memory, at * 16)[0] & 0x3FFF, 0)
            group = groups.setdefault(texture, {'pos': [], 'uv': [], 'col': [], 'nrm': []})
            tri = at + 4
            for _ in range(count):
                if tri + 10 > 1024:
                    break
                add_triangle(group, memory, tri * 16, rotation, translation)
                tri += 10
                triangles += 1
            at += 4 + count * 10

    # Translucent polygons are kept in their own list: four quadwords of GS state, then one triangle each.
    alpha = level.u32(shape + 0x54)
    if 0x100000 <= alpha < 0x2000000:
        for index in range(min(level.u32(alpha), 65536)):
            start = (alpha + 16 + index * 14 * 16) & 0x1FFFFFF
            texture = by_base.get(struct.unpack_from('<I', level.data, start)[0] & 0x3FFF, 0)
            group = groups.setdefault(texture, {'pos': [], 'uv': [], 'col': [], 'nrm': []})
            add_triangle(group, level.data, start + 4 * 16, rotation, translation)
            triangles += 1
    return triangles


def place_props(level, header, groups):
    """Adds the level's placed objects. Animated and special ones are left out."""
    placed = 0
    if not 0x100000 <= header < 0x2000000:
        return placed
    for index in range(min(level.u32(header), 4096)):
        entry = header + 4 + index * TSO_SIZE
        tob = level.u32(entry + 0xC)
        if not 0x100000 <= tob < 0x2000000 or level.f32(tob) not in (2.0, 3.0) or level.u32(tob + 0x48) == 0:
            continue
        pos = np.array(struct.unpack_from('<3i', level.data, entry + 0xC + 4), np.float32) / 256.0
        m = np.array(struct.unpack_from('<9h', level.data, entry + 0xC + 0x18), np.float32).reshape(3, 3) / 4096.0
        try:
            read_triangles(level, tob, groups, m, pos)
            placed += 1
        except struct.error:
            continue
    return placed


def bleed_colour(rgba, passes=24):
    """Spreads colour into fully transparent texels so that filtering does not darken the edges of cut-outs."""
    filled = rgba[..., 3] > 0
    if filled.all() or not filled.any():
        return
    colour = rgba[..., :3].astype(np.float32)
    for _ in range(passes):
        if filled.all():
            break
        total = np.zeros_like(colour)
        count = np.zeros(filled.shape, np.float32)
        for axis, step in ((0, 1), (0, -1), (1, 1), (1, -1)):
            total += np.roll(colour * filled[..., None], step, axis)
            count += np.roll(filled, step, axis)
        grow = ~filled & (count > 0)
        colour[grow] = total[grow] / count[grow][:, None]
        filled = filled | grow
    colour[~filled] = colour[filled].mean(axis=0)
    rgba[..., :3] = colour.astype(np.uint8)


def decode_texture(level, texture):
    width, height, fmt = level.i32(texture + 8), level.i32(texture + 12), level.i32(texture + 16)
    if width <= 0 or height <= 0 or width > 1024 or height > 1024:
        return None
    rgba = level.gs.texture(fmt, level.i32(texture + 0x2C), width, height, level.i32(texture + 0x14))
    if rgba is None:
        return None
    # Alpha here is how much of what is behind shows through, on a scale where 128 is all of it, so a texture
    # that is all zero is solid. Two values mark a cut-out's background; anything else needs blending.
    values = np.unique(rgba[..., 3])
    if len(values) == 1 and values[0] == 0:
        rgba[..., 3] = 255
        return rgba, 0
    mode = 1 if len(values) <= 2 else 2
    if mode == 1:
        rgba[..., 3] = np.where(rgba[..., 3] == values[0], 255, 0)
    else:
        rgba[..., 3] = 255 - np.minimum(rgba[..., 3].astype(np.uint16) * 255 // 128, 255).astype(np.uint8)
    bleed_colour(rgba)
    return rgba, mode


def thumbnail(pos, colours, low, high, size=320):
    image = Image.new('RGB', (size, size), (14, 16, 22))
    draw = ImageDraw.Draw(image)
    span = max(high[0] - low[0], high[2] - low[2], 1.0)
    scale = (size - 16) / span
    # Seen from above, so the lowest parts are drawn first. The world is left-handed: +x is right and +z is up.
    for i in np.argsort(pos[:, :, 1].mean(axis=1)):
        draw.polygon([(8 + (v[0] - low[0]) * scale, size - 8 - (v[2] - low[2]) * scale) for v in pos[i]], fill=tuple(int(c) for c in colours[i]))
    buffer = io.BytesIO()
    image.save(buffer, 'PNG')
    return 'data:image/png;base64,' + base64.b64encode(buffer.getvalue()).decode()


def main():
    path, tables, name, output = sys.argv[1:5]
    level = Level(path, tables)
    if '--menu' in sys.argv:
        header = level.u32(level.end - MENU_HEADER_FROM_END)
    else:
        header = level.end - LEVEL_HEADER_FROM_END
    world = level.u32(header)
    if not 0x100000 <= world < 0x2000000 or level.f32(world) not in (2.0, 3.0):
        raise SystemExit('no world in this file')
    groups = {}
    world_triangles = read_triangles(level, world, groups)
    props = place_props(level, level.u32(header + 4), groups)

    parts, positions, colours, facing = [], [], [], []
    digest = hashlib.sha1()
    total = 0
    for texture, group in sorted(groups.items()):
        if not group['pos']:
            continue
        pos = np.array(group['pos'], np.float32)
        uv = np.array(group['uv'], np.float32)
        col = np.clip(np.array(group['col'], np.int32), 0, 255).astype(np.uint8)
        nrm = np.array(group['nrm'], np.float32)
        total += len(pos) // 3
        digest.update(pos.tobytes())
        image, average = '', np.array([128.0, 128.0, 128.0])
        decoded = decode_texture(level, texture) if texture else None
        mode = 0
        if decoded is not None:
            rgba, mode = decoded
            buffer = io.BytesIO()
            Image.fromarray(rgba, 'RGBA').save(buffer, 'PNG')
            image = 'data:image/png;base64,' + base64.b64encode(buffer.getvalue()).decode()
            average = rgba[..., :3].reshape(-1, 3).mean(axis=0)
        positions.append(pos.reshape(-1, 3, 3))
        colours.append(np.clip(col[:, :3].reshape(-1, 3, 3).mean(axis=1) * average / 128.0, 0, 255))
        # Visible from straight above, by the game's own facing rule, and not translucent.
        triangle_normals = nrm[::3]
        facing.append(((triangle_normals[:, 3] < 0.5) == (triangle_normals[:, 1] > 0)) & (mode != 2))
        parts.append('{p:"%s",u:"%s",c:"%s",n:"%s",m:%d,t:"%s"}' % (base64.b64encode(pos.tobytes()).decode(), base64.b64encode(uv.tobytes()).decode(),
                                                     base64.b64encode(col.tobytes()).decode(), base64.b64encode(nrm.tobytes()).decode(), mode, image))

    all_pos = np.concatenate(positions)
    points = all_pos.reshape(-1, 3)
    low, high = points.min(axis=0), points.max(axis=0)
    core_low, core_high = np.percentile(points, 3, axis=0), np.percentile(points, 97, axis=0)
    inside = np.all((all_pos[:, :, [0, 2]] >= core_low[[0, 2]] - 1) & (all_pos[:, :, [0, 2]] <= core_high[[0, 2]] + 1), axis=(1, 2))
    inside &= np.concatenate(facing)
    thumb = thumbnail(all_pos[inside], np.concatenate(colours)[inside], core_low, core_high)
    print(f'{name}: {world_triangles} world triangles, {total} with {props} props, {len(parts)} textures')

    with open(output, 'w') as out:
        out.write('RCVIEW.add({name:%s,tris:%d,min:%s,max:%s,cmin:%s,cmax:%s,groups:[' % (
            json.dumps(name), total, [float(x) for x in low], [float(x) for x in high], [float(x) for x in core_low], [float(x) for x in core_high]))
        out.write(',\n'.join(parts))
        out.write(']});\n')
    with open(output + '.meta', 'w') as out:
        json.dump({'name': name, 'tris': total, 'props': props, 'thumb': thumb, 'hash': digest.hexdigest()}, out)


if __name__ == '__main__':
    main()
