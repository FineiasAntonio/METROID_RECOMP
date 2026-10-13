#!/usr/bin/env python3
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from lift import layout

nsos, bases = layout()


def imports(mod):
    n = nsos[mod]
    b = n.img
    m = struct.unpack_from('<I', b, 4)[0]
    o = m + struct.unpack_from('<i', b, m + 4)[0]
    tags = {}
    while True:
        t, v = struct.unpack_from('<qQ', b, o)
        o += 16
        if t == 0:
            break
        tags[t] = v
    out = {}
    for off, sz in ((tags.get(23), tags.get(2)), (tags.get(7), tags.get(8))):
        for r in range(off or 0, (off or 0) + (sz or 0), 24):
            ro, info, _ = struct.unpack_from('<QQq', b, r)
            s = info >> 32
            if s:
                nm = struct.unpack_from('<I', b, tags[6] + s * 24)[0]
                out[ro] = b[tags[5] + nm:b.index(0, tags[5] + nm)].decode()
    return out


for a in sys.argv[1:]:
    a = int(a, 16)
    mod = max((m for m in bases if bases[m] <= a), key=lambda m: bases[m])
    off = a - bases[mod]
    w = struct.unpack_from('<2I', nsos[mod].img, off)
    page = ((off & ~0xfff) + ((((w[0] >> 5) & 0x7ffff) << 2 | (w[0] >> 29) & 3) << 12))
    got = page + ((w[1] >> 10) & 0xfff) * 8
    name = imports(mod).get(got, '?')
    print(hex(a), subprocess.run(['llvm-cxxfilt'], input=name, capture_output=True, text=True).stdout.strip())
