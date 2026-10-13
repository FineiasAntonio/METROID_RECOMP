#!/usr/bin/env python3
import collections
import ctypes
import random
import struct
import subprocess
import sys
from pathlib import Path

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_PROT_ALL
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_SP, UC_ARM64_REG_NZCV, UC_ARM64_REG_Q0, UC_ARM64_REG_FPCR, UC_ARM64_REG_X29, UC_ARM64_REG_X30, UC_ARM64_REG_CPACR_EL1

sys.path.insert(0, str(Path(__file__).parent))
import lift

PC = 0x400000
MEM, MEMSZ = 0x10000000, 0x20000
OUT = Path('build/difftest')
PER_MNEMONIC = int(sys.argv[1]) if len(sys.argv) > 1 else 24
STATES = 4

SKIP_PREFIX = ('b', 'bl', 'b.', 'cbz', 'cbnz', 'tbz', 'tbnz', 'br', 'blr', 'ret', 'svc', 'brk', 'mrs', 'msr', 'hint',
               'udf', 'hlt', 'dc', 'ic', 'sys', 'clrex', 'dmb', 'dsb', 'isb', 'nop', 'yield', 'wfe', 'wfi', 'sev', 'sevl',
               'ldxr', 'ldxp', 'ldaxr', 'ldaxp', 'stxr', 'stxp', 'stlxr', 'stlxp', 'ldxrb', 'ldxrh', 'ldaxrb', 'ldaxrh',
               'stxrb', 'stxrh', 'stlxrb', 'stlxrh', 'prfm', 'prfum', 'ldr.lit', 'adr', 'adrp')


def gpr(i): return UC_ARM64_REG_X0 + i if i < 29 else (UC_ARM64_REG_X29 if i == 29 else UC_ARM64_REG_X30)


def collect():
    nsos, bases = lift.layout()
    words = collections.Counter()
    for m in lift.MODULES:
        n = nsos[m]
        for a, s in lift.load_map(m):
            for pc in range(a, a + s, 4):
                words[struct.unpack_from('<I', n.img, pc)[0]] += 1
    names = lift.mnemonics(list(words))
    by = collections.defaultdict(list)
    for w, nm in names.items():
        base = nm.split('.')[0]
        if base in SKIP_PREFIX or nm.startswith('b.'):
            continue
        if w & 0x3b000000 == 0x18000000:
            continue
        by[nm].append(w)
    rnd = random.Random(1)
    tests = []
    for nm, ws in sorted(by.items()):
        ws.sort(key=lambda w: -words[w])
        pick = ws[:PER_MNEMONIC // 2] + rnd.sample(ws, min(len(ws), PER_MNEMONIC // 2))
        for w in dict.fromkeys(pick):
            tests.append((nm, w))
    return tests


def build(tests):
    OUT.mkdir(parents=True, exist_ok=True)
    body, ok = [], []
    for i, (nm, w) in enumerate(tests):
        cx = lift.Ctx('t', PC, 4, set(), b'', PC)
        cx.unresolved_br = 0
        cx.skip = None
        try:
            s = lift.lift(w, PC, cx)
        except lift.Unimpl:
            continue
        if 'goto' in s or 'dispatch' in s or 'return' in s:
            continue
        body.append(f'static void t{i}(CPU *c) {{ {s} }}')
        ok.append(i)
    table = ', '.join(f't{i}' for i in ok)
    idx = ', '.join(str(i) for i in ok)
    src = f'''#include "cpu.h"
#include <sys/mman.h>
void dispatch(CPU *c, uint64_t t) {{ (void)c; (void)t; }}
void unimpl(CPU *c, uint64_t pc, uint32_t w) {{ (void)c; (void)pc; (void)w; }}
void svc(CPU *c, uint32_t i) {{ (void)c; (void)i; }}
void guest_brk(CPU *c, uint64_t pc) {{ (void)c; (void)pc; }}
uint64_t sysreg_read(CPU *c, uint32_t k) {{ (void)c; (void)k; return 0; }}
void trace_enter(CPU *c, uint64_t f) {{ (void)c; (void)f; }}
void trace_exit(CPU *c, uint64_t f) {{ (void)c; (void)f; }}
void sysreg_write(CPU *c, uint32_t k, uint64_t v) {{ (void)c; (void)k; (void)v; }}
{chr(10).join(body)}
static void (*const fns[])(CPU *) = {{{table}}};
static const int ids[] = {{{idx}}};
static CPU cpu;
int setup(void) {{ return mmap((void *){MEM:#x}, {MEMSZ:#x}, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *){MEM:#x}; }}
int count(void) {{ return {len(ok)}; }}
int id(int k) {{ return ids[k]; }}
void run(int k, uint64_t *x, uint8_t *v, uint32_t *nzcv) {{
    for (int i = 0; i < 31; i++) cpu.x[i] = x[i];
    cpu.sp = x[31];
    memcpy(cpu.v, v, 512);
    cpu.nf = *nzcv >> 31 & 1; cpu.zf = *nzcv >> 30 & 1; cpu.cf = *nzcv >> 29 & 1; cpu.vf = *nzcv >> 28 & 1;
    fns[k](&cpu);
    for (int i = 0; i < 31; i++) x[i] = cpu.x[i];
    x[31] = cpu.sp;
    memcpy(v, cpu.v, 512);
    *nzcv = (uint32_t)cpu.nf << 31 | (uint32_t)cpu.zf << 30 | (uint32_t)cpu.cf << 29 | (uint32_t)cpu.vf << 28;
}}
'''
    (OUT / 't.c').write_text(src)
    subprocess.run(['clang', '-O1', '-shared', '-fPIC', '-fno-strict-aliasing', '-w', '-Irt', str(OUT / 't.c'), '-o', str(OUT / 't.so'), '-lm'], check=True)
    return ctypes.CDLL(str((OUT / 't.so').resolve()))


def rand_state(rnd, mem_mode, w=0):
    if mem_mode:
        x = [MEM + MEMSZ // 2 + rnd.randrange(-0x800, 0x800) * 16 for _ in range(32)]
        for i in rnd.sample(range(31), 6):
            x[i] = rnd.choice([0, 1, 2, 3, 4, 7, 8, 16, (1 << 64) - 1, (1 << 64) - 8, 0xffffffff, 0x80000000, rnd.getrandbits(5)])
        rn, rm = (w >> 5) & 31, (w >> 16) & 31
        if rm != rn and rm != 31:
            x[rm] = rnd.choice([0, 1, 8, 16, 0x40, rnd.getrandbits(8), (1 << 64) - 16])
        x[rn] = MEM + MEMSZ // 2 + rnd.randrange(-0x400, 0x400) * 16
    else:
        pool = [0, 1, 2, (1 << 63), (1 << 64) - 1, 0x7fffffff, 0x80000000, 0xffffffff, (1 << 63) - 1]
        x = [rnd.choice(pool) if rnd.random() < 0.2 else rnd.getrandbits(64) if rnd.random() < 0.7 else rnd.getrandbits(rnd.choice([8, 16, 32])) for _ in range(32)]
        x[31] = MEM + MEMSZ // 2
    fl = [1.0, -1.0, 0.5, 2.0, 0.0, -0.0, 3.25, 1e10, -1e-10, 65504.0, 1e-40, float('inf')]
    v = bytearray()
    for _ in range(32):
        kind = rnd.random()
        if kind < 0.3:
            v += struct.pack('<4f', *[rnd.choice(fl) * rnd.choice([1, 3, 0.1]) for _ in range(4)])
        elif kind < 0.5:
            v += struct.pack('<2d', *[rnd.choice(fl) * rnd.uniform(-100, 100) for _ in range(2)])
        elif kind < 0.6:
            v += struct.pack('<4f', *[rnd.uniform(-1000, 1000) for _ in range(4)])
        else:
            v += rnd.getrandbits(128).to_bytes(16, 'little')
    nzcv = rnd.getrandbits(4) << 28
    return x, bytes(v), nzcv


def is_nan_diff(a, b):
    for size, fmt in ((4, '<f'), (8, '<d'), (2, '<e')):
        ok = True
        for o in range(0, 16, size):
            x, y = a[o:o + size], b[o:o + size]
            if x == y:
                continue
            fx, fy = struct.unpack(fmt, x)[0], struct.unpack(fmt, y)[0]
            if not (fx != fx and fy != fy):
                ok = False
                break
        if ok:
            return True
    return False


def main():
    tests = collect()
    lib = build(tests)
    assert lib.setup()
    memv = (ctypes.c_uint8 * MEMSZ).from_address(MEM)
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    uc.mem_map(PC, 0x1000, UC_PROT_ALL)
    uc.mem_map(MEM, MEMSZ, UC_PROT_ALL)
    uc.reg_write(UC_ARM64_REG_CPACR_EL1, 3 << 20)
    rnd = random.Random(7)
    fails = collections.defaultdict(list)
    nanonly = collections.Counter()
    tested = collections.Counter()
    n = lib.count()
    for k in range(n):
        i = lib.id(k)
        nm, w = tests[i]
        mem_mode = ((w >> 25) & 5) == 4
        uc.mem_write(PC, struct.pack('<I', w))
        for _ in range(STATES):
            x, v, nzcv = rand_state(rnd, mem_mode, w)
            mem0 = bytes(rnd.getrandbits(8) for _ in range(256)) * (MEMSZ // 256)
            uc.mem_write(MEM, mem0)
            ctypes.memmove(MEM, mem0, MEMSZ)
            for r in range(31):
                uc.reg_write(gpr(r), x[r])
            uc.reg_write(UC_ARM64_REG_SP, x[31])
            uc.reg_write(UC_ARM64_REG_NZCV, nzcv)
            uc.reg_write(UC_ARM64_REG_FPCR, 0)
            for r in range(32):
                uc.reg_write(UC_ARM64_REG_Q0 + r, int.from_bytes(v[r * 16:r * 16 + 16], 'little'))
            try:
                uc.emu_start(PC, PC + 4, count=1)
            except Exception as e:
                fails[nm].append((w, f'unicorn: {e}'))
                break
            ex = [uc.reg_read(gpr(r)) for r in range(31)] + [uc.reg_read(UC_ARM64_REG_SP)]
            ev = b''.join(uc.reg_read(UC_ARM64_REG_Q0 + r).to_bytes(16, 'little') for r in range(32))
            enz = uc.reg_read(UC_ARM64_REG_NZCV) & 0xf0000000
            emem = bytes(uc.mem_read(MEM, MEMSZ))
            cx = (ctypes.c_uint64 * 32)(*x)
            cv = (ctypes.c_uint8 * 512).from_buffer_copy(v)
            cn = ctypes.c_uint32(nzcv)
            lib.run(k, cx, cv, ctypes.byref(cn))
            gx, gv, gn, gmem = list(cx), bytes(cv), cn.value, bytes(memv)
            diffs = []
            for r in range(32):
                if gx[r] != ex[r]:
                    diffs.append(f'{"sp" if r == 31 else f"x{r}"}: c={gx[r]:#x} uc={ex[r]:#x}')
            nan = True
            for r in range(32):
                a, b = gv[r * 16:r * 16 + 16], ev[r * 16:r * 16 + 16]
                if a != b:
                    if not is_nan_diff(a, b):
                        nan = False
                    diffs.append(f'v{r}: c={a.hex()} uc={b.hex()}')
            if gn != enz:
                nan = False
                diffs.append(f'nzcv: c={gn >> 28:04b} uc={enz >> 28:04b}')
            if gmem != emem:
                nan = False
                o = next(j for j in range(MEMSZ) if gmem[j] != emem[j])
                diffs.append(f'mem@{MEM + o:#x}: c={gmem[o:o + 8].hex()} uc={emem[o:o + 8].hex()}')
            if any(not d.startswith('v') for d in diffs):
                nan = False
            if diffs:
                if nan:
                    nanonly[nm] += 1
                else:
                    fails[nm].append((w, '; '.join(diffs[:4])))
                    break
        tested[nm] += 1
    words = [w for (nm, w) in tests]
    print(f'{n} instruções testadas ({len(tested)} mnemônicos), {sum(len(v) for v in fails.values())} falhas em {len(fails)} mnemônicos')
    names = lift.mnemonics(sorted({w for v in fails.values() for w, _ in v}))
    full = {}
    if names:
        txt = '\n'.join(' '.join(f'0x{b:02x}' for b in w.to_bytes(4, 'little')) for w in names)
        out = subprocess.run(['llvm-mc', '--disassemble', '-triple=aarch64', '-mattr=+neon,+crc,+crypto'], input=txt, capture_output=True, text=True).stdout
        lines = [l.strip() for l in out.splitlines() if l.strip() and not l.strip().startswith('.')]
        full = dict(zip(names, lines))
    for nm in sorted(fails, key=lambda k: -len(fails[k])):
        for w, d in fails[nm][:2]:
            print(f'  {nm:14s} {w:#010x} {full.get(w, ""):40s} {d}')
    if nanonly:
        print('diferenças só em payload de NaN:', dict(nanonly))
    del words


if __name__ == '__main__':
    main()
