#!/usr/bin/env python3
import bisect
import csv
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from lift import MODULES, layout

_, bases = layout()
tables = {}
for m in MODULES:
    rows = [(int(r['addr'], 16) + bases[m], r['name']) for r in csv.DictReader(open(f'build/funcmap/{m}.csv'))]
    tables[m] = rows


def sym(a):
    m = max((m for m in bases if bases[m] <= a), key=lambda m: bases[m])
    rows = tables[m]
    i = bisect.bisect_right([r[0] for r in rows], a) - 1
    name = subprocess.run(['llvm-cxxfilt'], input=rows[i][1], capture_output=True, text=True).stdout.strip()
    return f'{a:#x} {m}:{name}+{a - rows[i][0]:#x}'


text = ' '.join(sys.argv[1:]) or sys.stdin.read()
for h in re.findall(r'0x[0-9a-f]{7,}', text):
    print(sym(int(h, 16)))
