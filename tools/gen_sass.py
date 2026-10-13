#!/usr/bin/env python3
import re
from pathlib import Path

rows = []
for line in Path('tools/maxwell.inc').read_text().splitlines():
    m = re.match(r'INST\((\w+),\s*"([^"]+)",\s*"([01\- ]+)"\)', line)
    if m:
        bits = m.group(3).replace(' ', '')
        mask = int(''.join('0' if c == '-' else '1' for c in bits), 2) << (64 - len(bits))
        val = int(bits.replace('-', '0'), 2) << (64 - len(bits))
        rows.append((m.group(1), mask, val, bits.count('-')))
rows.sort(key=lambda r: r[3])
out = ['#pragma once', '#include <stdint.h>', 'enum {'] + [f'    OP_{n},' for n, *_ in rows] + ['    OP_INVALID', '};',
       'static const struct { uint64_t mask, val; int op; const char *name; } sass_ops[] = {']
out += [f'    {{{m:#x}ull, {v:#x}ull, OP_{n}, "{n}"}},' for n, m, v, _ in rows]
out += ['};', '']
Path('rt/sass_ops.h').write_text('\n'.join(out))
print(len(rows), 'opcodes')
