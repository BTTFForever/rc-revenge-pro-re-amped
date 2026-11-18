#!/usr/bin/env python3
"""Reads a PS2 CD/DVD image without mounting it: converts a raw image to an ISO and extracts its files.

Usage:
  iso_extract.py boot <image>               prints the boot executable's name from SYSTEM.CNF (e.g. SLUS_201.53)
  iso_extract.py kind <image>               prints iso (2048-byte sectors) or raw (2352), and the resolved path
  iso_extract.py to-iso <image> <out.iso>   writes the 2048-byte-sector ISO of a raw (2352-byte sector) .bin image
  iso_extract.py extract <image> <out dir>  extracts every file (ISO 9660 primary volume descriptor)
  iso_extract.py sha1 <file>                prints the file's SHA-1

<image> is a 2048-byte-sector .iso or a raw 2352-byte-sector .bin (Mode 1 or Mode 2 Form 1); a .cue is followed to its
.bin. Standard library only.
"""
import hashlib
import os
import re
import sys

SECTOR = 2048
RAW = 2352
SYNC = b'\x00' + b'\xff' * 10 + b'\x00'


def resolve(path):
    """A .cue's first FILE entry, otherwise the path itself."""
    if path.lower().endswith('.cue'):
        text = open(path, encoding='latin-1').read()
        m = re.search(r'FILE\s+"([^"]+)"', text) or re.search(r'FILE\s+(\S+)', text)
        if not m:
            sys.exit(f'{path}: no FILE line')
        return os.path.join(os.path.dirname(os.path.abspath(path)), m.group(1))
    return path


class Image:
    def __init__(self, path):
        self.path = resolve(path)
        self.f = open(self.path, 'rb')
        size = os.path.getsize(self.path)
        head = self.f.read(16)
        self.raw = size % RAW == 0 and head[:12] == SYNC
        if self.raw:
            mode = head[15]
            if mode == 1:
                self.offset = 16
            elif mode == 2:
                self.offset = 24  # Mode 2 Form 1: sub-header, then 2048 bytes of user data
            else:
                sys.exit(f'{self.path}: unknown sector mode {mode}')
            self.sectors = size // RAW
        else:
            if size % SECTOR:
                sys.exit(f'{self.path}: neither a 2048-byte-sector ISO nor a raw 2352-byte-sector image')
            self.sectors = size // SECTOR

    def sector(self, lba, count=1):
        if not self.raw:
            self.f.seek(lba * SECTOR)
            return self.f.read(count * SECTOR)
        out = bytearray()
        for n in range(lba, lba + count):
            self.f.seek(n * RAW + self.offset)
            out += self.f.read(SECTOR)
        return bytes(out)

    def read(self, lba, size):
        return self.sector(lba, (size + SECTOR - 1) // SECTOR)[:size]

    def walk(self):
        """(path, lba, size) for every file, depth first; a directory is (path + '/', None, 0)."""
        pvd = self.sector(16)
        if pvd[0] != 1 or pvd[1:6] != b'CD001':
            sys.exit(f'{self.path}: no ISO 9660 primary volume descriptor')
        root = pvd[156:156 + 34]
        yield from self._dir(int.from_bytes(root[2:6], 'little'), int.from_bytes(root[10:14], 'little'), '', 0)

    def _dir(self, lba, size, prefix, depth):
        if depth > 16:
            return
        data = self.read(lba, size)
        pos = 0
        while pos < len(data):
            n = data[pos]
            if n == 0:  # records do not cross sectors: skip to the next one
                pos = (pos // SECTOR + 1) * SECTOR
                continue
            rec = data[pos:pos + n]
            pos += n
            name_len = rec[32]
            name = rec[33:33 + name_len]
            if name in (b'\x00', b'\x01'):
                continue
            name = name.decode('latin-1').split(';')[0]
            extent, length, flags = int.from_bytes(rec[2:6], 'little'), int.from_bytes(rec[10:14], 'little'), rec[25]
            if '/' in name or name in ('.', '..'):
                sys.exit(f'bad file name {name!r}')
            if flags & 2:
                yield prefix + name + '/', None, 0
                yield from self._dir(extent, length, prefix + name + '/', depth + 1)
            else:
                yield prefix + name, extent, length


def boot_name(img):
    for path, lba, size in img.walk():
        if lba is not None and path.upper() == 'SYSTEM.CNF':
            text = img.read(lba, size).decode('latin-1')
            m = re.search(r'BOOT2\s*=\s*cdrom0:\\?([^;\s]+)', text, re.I)
            if m:
                return m.group(1)
    sys.exit(f'{img.path}: no SYSTEM.CNF with a BOOT2 line (not a PS2 disc?)')


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    if cmd == 'sha1':
        h = hashlib.sha1()
        with open(sys.argv[2], 'rb') as f:
            for block in iter(lambda: f.read(1 << 20), b''):
                h.update(block)
        print(h.hexdigest())
        return
    img = Image(sys.argv[2])
    if cmd == 'boot':
        print(boot_name(img))
    elif cmd == 'kind':
        print('raw' if img.raw else 'iso', os.path.abspath(img.path))
    elif cmd == 'to-iso':
        if not img.raw:
            sys.exit(f'{img.path} is already a 2048-byte-sector image')
        with open(sys.argv[3], 'wb') as out:
            for lba in range(0, img.sectors, 256):
                out.write(img.sector(lba, min(256, img.sectors - lba)))
    elif cmd == 'extract':
        outdir = sys.argv[3]
        count = 0
        for path, lba, size in img.walk():
            dest = os.path.join(outdir, path)
            if lba is None:
                os.makedirs(dest, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, 'wb') as out:
                for start in range(0, size, 256 * SECTOR):
                    chunk = min(256 * SECTOR, size - start)
                    out.write(img.read(lba + start // SECTOR, chunk))
            count += 1
        print(f'{count} files extracted to {outdir}')
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
