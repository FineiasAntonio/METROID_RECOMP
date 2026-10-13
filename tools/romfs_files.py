#!/usr/bin/env python3
import bisect
import re
import struct
import sys

path = 'extracted/romfs.bin'
with open(path, 'rb') as f:
    hdr = struct.unpack('<10Q', f.read(0x50))
    dir_meta_off, dir_meta_size, file_meta_off, file_meta_size, data_off = hdr[3], hdr[4], hdr[7], hdr[8], hdr[9]
    f.seek(dir_meta_off)
    dmeta = f.read(dir_meta_size)
    f.seek(file_meta_off)
    fmeta = f.read(file_meta_size)


def dir_path(off):
    parts = []
    while off != 0:
        parent, _, _, _, _, nlen = struct.unpack_from('<6I', dmeta, off)
        parts.append(dmeta[off + 0x18:off + 0x18 + nlen].decode())
        off = parent
    return '/'.join(reversed(parts))


files = []
o = 0
while o < len(fmeta):
    parent, _, foff, fsize, _, nlen = struct.unpack_from('<IIQQII', fmeta, o)
    name = fmeta[o + 0x20:o + 0x20 + nlen].decode()
    files.append((data_off + foff, fsize, f'{dir_path(parent)}/{name}'))
    o += (0x20 + nlen + 3) & ~3
files.sort()
starts = [x[0] for x in files]


def which(off):
    i = bisect.bisect_right(starts, off) - 1
    if i >= 0 and off < files[i][0] + max(files[i][1], 1):
        return files[i][2]
    return '?'


if __name__ == '__main__':
    print(f'{len(files)} arquivos', file=sys.stderr)
    for line in sys.stdin:
        m = re.search(r'off=(0x[0-9a-f]+) size=(0x[0-9a-f]+)', line)
        if m:
            print(line.split()[0], which(int(m.group(1), 16)), m.group(2))
