#!/usr/bin/env python3
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from lift import layout

nsos, bases = layout()
addr, count = int(sys.argv[1], 16), int(sys.argv[2]) if len(sys.argv) > 2 else 16
mod = max((m for m in bases if bases[m] <= addr), key=lambda m: bases[m])
n, off = nsos[mod], addr - bases[mod]
bs = ' '.join(f'0x{x:02x}' for x in n.img[off:off + count * 4])
out = subprocess.run(['llvm-mc', '--disassemble', '-triple=aarch64'], input=bs, capture_output=True, text=True).stdout
lines = [l.strip() for l in out.splitlines() if l.strip() and not l.strip().startswith('.')]
for i, l in enumerate(lines):
    print(f'{addr + 4 * i:#x} {mod}+{off + 4 * i:#x}: {l}')
