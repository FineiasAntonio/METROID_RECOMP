#!/usr/bin/env python3
import collections
import re
import struct
import sys
from pathlib import Path

TABLE = []
for line in (Path(__file__).parent / 'maxwell.inc').read_text().splitlines():
    m = re.match(r'INST\((\w+),\s*"([^"]+)",\s*"([01\- ]+)"\)', line)
    if m:
        bits = m.group(3).replace(' ', '')
        mask = int(''.join('0' if c == '-' else '1' for c in bits), 2) << (64 - len(bits))
        val = int(bits.replace('-', '0'), 2) << (64 - len(bits))
        TABLE.append((m.group(1), mask, val, bits.count('-')))
TABLE.sort(key=lambda t: t[3])


def decode(w):
    for name, mask, val, _ in TABLE:
        if w & mask == val:
            return name
    return '???'


def program(path):
    d = Path(path).read_bytes()
    words = struct.unpack(f'<{len(d) // 8}Q', d)
    out = []
    for i in range(10, len(words)):
        if (i - 10) % 4 == 0:
            continue
        w = words[i]
        name = decode(w)
        out.append((i, w, name))
        if name == 'BRA' and (w >> 20 & 0xffffff) in (0xfffff0, 0xfffff8):
            break
    return out


if __name__ == '__main__':
    files = sys.argv[1:] or sorted(Path('build/shaders').glob('*.bin'))
    hist = collections.Counter()
    for f in files:
        prog = program(f)
        hist.update(n for _, _, n in prog)
        print(f'{Path(f).name}: {len(prog)} instruções')
    print(', '.join(f'{k}={v}' for k, v in hist.most_common()))
