#!/usr/bin/env bash
python3 tools/disasm.py "$@" | python3 -c "
import sys,re,subprocess
lines=sys.stdin.read().splitlines()
tg=[]
for l in lines:
    m=re.search(r'\sbl\s+#(-?\d+)',l)
    tg.append(hex(int(l.split()[0],16)+int(m.group(1))) if m else None)
syms=subprocess.run(['python3','tools/sym.py']+[t for t in tg if t],capture_output=True,text=True).stdout.splitlines()
it=iter(syms)
for l,t in zip(lines,tg):
    print(l+('   ; '+next(it).split(' ',1)[1][:100] if t else ''))
"
