#!/usr/bin/env python3
import bisect
import collections
import csv
import struct
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from funcmap import Nso

MODULES = ['rtld', 'main', 'subsdk0', 'sdk']
BASE0, ALIGN = 0x07e00000, 0x200000
SHARD = 1500
TRACE = {int(a, 16) for a in __import__('os').environ.get('TRACE_FUNCS', '').split(',') if a}


class Unimpl(Exception):
    pass


def bits(w, hi, lo): return (w >> lo) & ((1 << (hi - lo + 1)) - 1)
def sx(v, n): return v - (1 << n) if v >> (n - 1) & 1 else v


def lit(v, M=64):
    v &= (1 << M) - 1
    return f'{v:#x}ull' if M == 64 else f'{v:#x}u'


def X(n): return '0ull' if n == 31 else f'c->x[{n}]'
def XS(n): return 'c->sp' if n == 31 else f'c->x[{n}]'
def W(n): return '0u' if n == 31 else f'(uint32_t)c->x[{n}]'
def WS(n): return '(uint32_t)c->sp' if n == 31 else f'(uint32_t)c->x[{n}]'
def R(n, sf, sp=False): return (XS if sp else X)(n) if sf else (WS if sp else W)(n)
def UT(M): return f'uint{M}_t'
def ST(M): return f'int{M}_t'


def wr(d, sf, e, sp=False):
    if d == 31 and not sp:
        return f'(void)({e});'
    dst = 'c->sp' if d == 31 else f'c->x[{d}]'
    return f'{dst} = {e};' if sf else f'{dst} = (uint32_t)({e});'


COND = ['c->zf', 'c->cf', 'c->nf', 'c->vf', '(c->cf && !c->zf)', '(c->nf == c->vf)',
        '(!c->zf && c->nf == c->vf)', '1']


def cond(cc):
    e = COND[cc >> 1]
    return f'!{e}' if cc & 1 and cc != 15 else e


def ror_py(v, r, n): return ((v >> r) | (v << (n - r))) & ((1 << n) - 1) if r else v


def replicate(v, e, M):
    out = 0
    for i in range(0, M, e):
        out |= v << i
    return out


def bitmasks(N, imms, immr, imm, M):
    ln = ((N << 6) | (~imms & 0x3f)).bit_length() - 1
    if ln < 1:
        raise Unimpl('bitmask')
    levels = (1 << ln) - 1
    if imm and (imms & levels) == levels:
        raise Unimpl('bitmask')
    S, Rr = imms & levels, immr & levels
    d = (S - Rr) & levels
    e = 1 << ln
    welem = ror_py((1 << (S + 1)) - 1, Rr, e)
    telem = (1 << (d + 1)) - 1
    return replicate(welem, e, M), replicate(telem, e, M)


def shift(t, e, a, M):
    if a == 0:
        return e
    return [f'({e} << {a})', f'({e} >> {a})', f'({UT(M)})(({ST(M)}){e} >> {a})', f'ror{M}({e}, {a})'][t]


def extend(opt, e, a):
    x = [f'(uint64_t)(uint8_t){e}', f'(uint64_t)(uint16_t){e}', f'(uint64_t)(uint32_t){e}', f'{e}',
         f'(uint64_t)(int8_t){e}', f'(uint64_t)(int16_t){e}', f'(uint64_t)(int32_t){e}', f'{e}'][opt]
    return f'({x} << {a})' if a else f'({x})'


FT = {0: ('S', 'float', 'f'), 1: ('D', 'double', 'df'), 3: ('H', '_Float16', 'hf')}


def fty(t):
    if t not in FT:
        raise Unimpl('ftype')
    return FT[t]


def V(n, t): return f'c->v[{n}].{fty(t)[2]}[0]'
def wV(d, t, e): return f'wv{fty(t)[0]}(c, {d}, {e});'


def vfp_imm(imm8):
    b54 = (imm8 >> 4) & 3
    e = b54 - 3 if imm8 & 0x40 else b54 + 1
    v = (1 + (imm8 & 0xf) / 16) * 2.0 ** e
    return -v if imm8 & 0x80 else v


class Ctx:
    def __init__(self, mod, start, size, funcs, img, base):
        self.mod, self.start, self.end = mod, start, start + size
        self.funcs, self.img, self.base = funcs, img, base
        self.labels = set()

    def inside(self, t): return self.start <= t < self.end

    def jump(self, t):
        if self.inside(t):
            self.labels.add(t)
            return f'goto L_{t:x};'
        if t in self.funcs:
            return f'{{ f_{t:08x}(c); return; }}'
        return f'{{ dispatch(c, {lit(t)}); return; }}'

    def rd(self, a, n): return int.from_bytes(self.img[a - self.base:a - self.base + n], 'little')


def lift(w, pc, cx):
    op0 = bits(w, 28, 25)
    if op0 in (8, 9):
        return dp_imm(w, pc)
    if op0 in (10, 11):
        return branch_sys(w, pc, cx)
    if op0 & 5 == 4:
        return ldst(w, pc, cx)
    if op0 & 7 == 5:
        return dp_reg(w)
    if op0 & 7 == 7:
        return simd_fp(w)
    raise Unimpl('op0')


def dp_imm(w, pc):
    sf, M = bits(w, 31, 31), 64 if w >> 31 else 32
    rd, rn = bits(w, 4, 0), bits(w, 9, 5)
    op = bits(w, 25, 23)
    if op in (0, 1):
        imm = sx(bits(w, 23, 5) << 2 | bits(w, 30, 29), 21)
        v = ((pc & ~0xfff) + (imm << 12)) if w >> 31 else pc + imm
        return wr(rd, 1, lit(v))
    if op == 2:
        S, sub, imm = bits(w, 29, 29), bits(w, 30, 30), bits(w, 21, 10) << (12 * bits(w, 22, 22))
        if bits(w, 22, 22) and bits(w, 23, 23):
            raise Unimpl('addimm')
        a = R(rn, sf, True)
        if S:
            return wr(rd, sf, f'adds{M}(c, {a}, {lit(~imm if sub else imm, M)}, {int(sub)})')
        return wr(rd, sf, f'{a} {"-" if sub else "+"} {lit(imm, M)}', True)
    if op == 4:
        opc, N = bits(w, 30, 29), bits(w, 22, 22)
        if not sf and N:
            raise Unimpl('logimm')
        wm, _ = bitmasks(N, bits(w, 15, 10), bits(w, 21, 16), True, M)
        a = R(rn, sf)
        e = f'{a} {"&|^&"[opc]} {lit(wm, M)}'
        if opc == 3:
            return wr(rd, sf, f'logic{M}(c, {e})')
        return wr(rd, sf, e, True)
    if op == 5:
        opc, hw, imm = bits(w, 30, 29), bits(w, 22, 21), bits(w, 20, 5)
        if opc == 1 or (not sf and hw > 1):
            raise Unimpl('movw')
        s = hw * 16
        if opc == 0:
            return wr(rd, sf, lit(~(imm << s), M))
        if opc == 2:
            return wr(rd, sf, lit(imm << s, M))
        return wr(rd, sf, f'({R(rd, sf)} & {lit(~(0xffff << s), M)}) | {lit(imm << s, M)}')
    if op == 6:
        opc, N, immr, imms = bits(w, 30, 29), bits(w, 22, 22), bits(w, 21, 16), bits(w, 15, 10)
        if opc == 3 or N != sf:
            raise Unimpl('bfm')
        wm, tm = bitmasks(N, imms, immr, False, M)
        src = R(rn, sf)
        rot = f'ror{M}({src}, {immr})' if immr else src
        if opc == 2:
            return wr(rd, sf, f'({rot} & {lit(wm & tm, M)})')
        if opc == 0:
            top = f'(({UT(M)})0 - (({src} >> {imms}) & 1))'
            return wr(rd, sf, f'(({top} & {lit(~tm, M)}) | ({rot} & {lit(wm & tm, M)}))')
        dst = R(rd, sf)
        return wr(rd, sf, f'(({dst} & {lit(~tm, M)}) | ((({dst} & {lit(~wm, M)}) | ({rot} & {lit(wm, M)})) & {lit(tm, M)}))')
    if op == 7:
        if bits(w, 30, 29) or bits(w, 22, 22) != sf or bits(w, 21, 21):
            raise Unimpl('extr')
        rm, lsb = bits(w, 20, 16), bits(w, 15, 10)
        if lsb == 0:
            return wr(rd, sf, R(rm, sf))
        return wr(rd, sf, f'(({R(rm, sf)} >> {lsb}) | ({R(rn, sf)} << {M - lsb}))')
    raise Unimpl('dpimm')


SYSREG_DC_ZVA = (3 - 2) << 14 | 3 << 11 | 7 << 7 | 4 << 3 | 1


def branch_sys(w, pc, cx):
    if bits(w, 30, 26) == 0b00101:
        t = pc + sx(bits(w, 25, 0), 26) * 4
        if w >> 31 and t == pc + 8:
            cx.skip = pc + 4
            return f'c->x[30] = {lit(pc + 4)};'
        if w >> 31:
            call = f'f_{t:08x}(c);' if t in cx.funcs else f'dispatch(c, {lit(t)});'
            return f'c->x[30] = {lit(pc + 4)}; {call}'
        return cx.jump(t)
    if bits(w, 31, 24) == 0x54 and not bits(w, 4, 4):
        t = pc + sx(bits(w, 23, 5), 19) * 4
        return f'if ({cond(bits(w, 3, 0))}) {cx.jump(t)}'
    if bits(w, 30, 25) == 0b011010:
        t = pc + sx(bits(w, 23, 5), 19) * 4
        r = R(bits(w, 4, 0), bits(w, 31, 31))
        return f'if ({r} {"!=" if bits(w, 24, 24) else "=="} 0) {cx.jump(t)}'
    if bits(w, 30, 25) == 0b011011:
        b = bits(w, 31, 31) << 5 | bits(w, 23, 19)
        t = pc + sx(bits(w, 18, 5), 14) * 4
        return f'if ((({X(bits(w, 4, 0))} >> {b}) & 1) {"!=" if bits(w, 24, 24) else "=="} 0) {cx.jump(t)}'
    if bits(w, 31, 25) == 0b1101011:
        opc, rn = bits(w, 24, 21), bits(w, 9, 5)
        if bits(w, 20, 16) != 0x1f or bits(w, 15, 10) or bits(w, 4, 0):
            raise Unimpl('bxr')
        if opc == 0:
            return br(w, pc, cx, rn)
        if opc == 1:
            return f'{{ uint64_t t = {X(rn)}; c->x[30] = {lit(pc + 4)}; dispatch(c, t); }}'
        if opc == 2:
            return 'return;'
        raise Unimpl('bxr')
    if bits(w, 31, 21) == 0b11010100000 and bits(w, 4, 0) == 1:
        return f'svc(c, {bits(w, 20, 5)});'
    if bits(w, 31, 21) == 0b11010100001 and bits(w, 4, 0) == 0:
        return f'guest_brk(c, {lit(pc)});'
    if w & 0xfffff01f == 0xd503201f:
        return ';'
    if w & 0xfffff0ff in (0xd503309f, 0xd50330bf, 0xd50330df):
        return '__atomic_thread_fence(__ATOMIC_SEQ_CST);'
    if w & 0xfffff0ff == 0xd503305f:
        return 'c->excl_addr = ~0ull;'
    if bits(w, 31, 20) == 0xd53:
        return wr(bits(w, 4, 0), 1, f'sysreg_read(c, {bits(w, 19, 5)})')
    if bits(w, 31, 20) == 0xd51:
        return f'sysreg_write(c, {bits(w, 19, 5)}, {X(bits(w, 4, 0))});'
    if bits(w, 31, 19) == 0b1101010100001:
        if bits(w, 18, 5) == SYSREG_DC_ZVA & 0x3fff:
            return f'memset((void *)(uintptr_t)({X(bits(w, 4, 0))} & ~63ull), 0, 64);'
        return ';'
    raise Unimpl('brsys')


def br(w, pc, cx, rn):
    targets = jump_table(pc, cx, rn)
    reg = X(rn)
    if not targets:
        cx.unresolved_br += 1
        return f'{{ dispatch(c, {reg}); return; }}'
    cases = ' '.join(f'case {lit(t)}: {cx.jump(t)}' for t in sorted(set(targets)))
    return f'switch ({reg}) {{ {cases} }} {{ dispatch(c, {reg}); return; }}'


def jump_table(pc, cx, rn):
    lo = max(cx.start, pc - 4 * 24)
    ws = []
    for a in range(pc - 4, lo - 1, -4):
        w = cx.rd(a, 4)
        if w & 0xfc000000 == 0x14000000 or w & 0xfffffc1f in (0xd61f0000, 0xd65f0000):
            break
        ws.append((a, w))

    def find(pred):
        for a, w in ws:
            r = pred(a, w)
            if r is not None:
                return a, r
        return None, None

    def addr_of(reg, before):
        for a, w in ws:
            if a >= before or bits(w, 4, 0) != reg:
                continue
            if w & 0x9f000000 == 0x10000000:
                return a + sx(bits(w, 23, 5) << 2 | bits(w, 30, 29), 21)
            if w & 0xffc00000 == 0x91000000:
                base = addr_of(bits(w, 9, 5), a)
                return None if base is None else base + bits(w, 21, 10)
            if w & 0x9f000000 == 0x90000000:
                return (a & ~0xfff) + (sx(bits(w, 23, 5) << 2 | bits(w, 30, 29), 21) << 12)
            return None
        return None

    a_add, add = find(lambda a, w: w if bits(w, 4, 0) == rn and (w & 0xffe00000 in (0x8b000000, 0x8b200000)) else None)
    if add is None:
        return None
    rb, ro = bits(add, 9, 5), bits(add, 20, 16)
    if add & 0x00200000:
        sh = bits(add, 12, 10)
        if bits(add, 15, 13) not in (3, 6, 7):
            return None
    else:
        if bits(add, 23, 22):
            return None
        sh = bits(add, 15, 10)
    a_ld, ld = find(lambda a, w: w if a < a_add and bits(w, 4, 0) == ro and w & 0x3b200c00 == 0x38200800 else None)
    if ld is None:
        return None
    size, opc = bits(ld, 31, 30), bits(ld, 23, 22)
    signed = opc in (2, 3)
    rt_, ri = bits(ld, 9, 5), bits(ld, 20, 16)
    lsh = size if bits(ld, 12, 12) else 0
    table = addr_of(rt_, a_ld)
    base = addr_of(rb, a_add)
    if base is None:
        return None
    superset = None
    if not signed:
        span = (1 << (8 << size)) << sh
        superset = [t for t in range(base, min(base + span, cx.end), 4) if cx.inside(t)] or None
    if table is None:
        return superset
    _, cmp = find(lambda a, w: w if a < a_ld and w & 0x7f80001f == 0x7100001f and bits(w, 22, 22) == 0 and bits(w, 9, 5) == ri else None)
    if cmp is None:
        return superset
    n = bits(cmp, 21, 10) + 1
    if n > 4096:
        return None
    esz = 1 << size
    out = []
    for i in range(n):
        e = cx.rd(table + (i << lsh), esz)
        if signed:
            e = sx(e, esz * 8)
        t = (base + (e << sh)) & (2 ** 64 - 1)
        if not cx.inside(t) or t & 3:
            return None
        out.append(t)
    return out


def ldst(w, pc, cx):
    if not bits(w, 31, 31) and bits(w, 29, 24) == 0b001100:
        return ldst_multi(w)
    if not bits(w, 31, 31) and bits(w, 29, 24) == 0b001101:
        return ldst_single(w)
    if bits(w, 29, 24) == 0b001000:
        return excl(w)
    if bits(w, 29, 27) == 0b011 and bits(w, 25, 24) == 0:
        return ld_lit(w, pc)
    if bits(w, 29, 27) == 0b101:
        return ldst_pair(w)
    if bits(w, 29, 27) == 0b111:
        return ldst_reg(w)
    raise Unimpl('ldst')


def excl(w):
    size, o2, L, o1, rs, o0, rt2, rn, rt = (bits(w, 31, 30), bits(w, 23, 23), bits(w, 22, 22), bits(w, 21, 21),
                                            bits(w, 20, 16), bits(w, 15, 15), bits(w, 14, 10), bits(w, 9, 5), bits(w, 4, 0))
    a = XS(rn)
    N = 8 << size
    if o2:
        if o1 or not o0:
            raise Unimpl('ldar')
        if L:
            return wr(rt, 1, f'__atomic_load_n((uint{N}_t *)(uintptr_t){a}, __ATOMIC_SEQ_CST)')
        return f'__atomic_store_n((uint{N}_t *)(uintptr_t){a}, (uint{N}_t){X(rt)}, __ATOMIC_SEQ_CST);'
    if o1:
        if size == 3:
            if L:
                return f'{{ uint64_t lo, hi; ldxp64(c, {a}, &lo, &hi); {wr(rt, 1, "lo")} {wr(rt2, 1, "hi")} }}'
            return wr(rs, 0, f'stxp64(c, {a}, {X(rt)}, {X(rt2)})')
        if size == 2:
            if L:
                return f'{{ uint64_t v = ldx64(c, {a}); {wr(rt, 0, "v")} {wr(rt2, 0, "v >> 32")} }}'
            return wr(rs, 0, f'stx64(c, {a}, ((uint64_t){X(rt2)} << 32) | (uint32_t){X(rt)})')
        raise Unimpl('ldxp')
    if L:
        return wr(rt, 1, f'ldx{N}(c, {a})')
    return wr(rs, 0, f'stx{N}(c, {a}, (uint{N}_t){X(rt)})')


def ld_lit(w, pc):
    opc, Vb, rt = bits(w, 31, 30), bits(w, 26, 26), bits(w, 4, 0)
    a = lit(pc + sx(bits(w, 23, 5), 19) * 4)
    if Vb:
        if opc == 2:
            return f'c->v[{rt}] = MV({a});'
        if opc == 3:
            raise Unimpl('ldlit')
        return f'wv64(c, {rt}, M{32 << opc}({a}));'
    if opc == 0:
        return wr(rt, 1, f'M32({a})')
    if opc == 1:
        return wr(rt, 1, f'M64({a})')
    if opc == 2:
        return wr(rt, 1, f'(uint64_t)(int32_t)M32({a})')
    return ';'


def mem_op(size, opc, Vb, rt, a):
    if Vb:
        q = size == 0 and opc & 2
        if opc & 2 and not q:
            raise Unimpl('ldst-v')
        if q:
            return f'c->v[{rt}] = MV({a});' if opc & 1 else f'MV({a}) = c->v[{rt}];'
        N = 8 << size
        if opc & 1:
            return f'wv64(c, {rt}, M{N}({a}));'
        return f'M{N}({a}) = c->v[{rt}].{"bhsd"[size]}[0];'
    N = 8 << size
    if opc == 0:
        return f'M{N}({a}) = (uint{N}_t){X(rt)};'
    if opc == 1:
        return wr(rt, 1, f'M{N}({a})')
    if opc == 2:
        if size == 3:
            return ';'
        return wr(rt, 1, f'(uint64_t)(int{N}_t)M{N}({a})')
    if size >= 2:
        raise Unimpl('ldst-opc')
    return wr(rt, 0, f'(uint32_t)(int{N}_t)M{N}({a})')


def scale_of(size, opc, Vb): return 4 if Vb and size == 0 and opc & 2 else size


def ldst_reg(w):
    size, Vb, opc, rn, rt = bits(w, 31, 30), bits(w, 26, 26), bits(w, 23, 22), bits(w, 9, 5), bits(w, 4, 0)
    sc = scale_of(size, opc, Vb)
    base = XS(rn)
    if bits(w, 25, 24) == 1:
        return f'{{ uint64_t a = {base} + {lit(bits(w, 21, 10) << sc)}; {mem_op(size, opc, Vb, rt, "a")} }}'
    if bits(w, 25, 24):
        raise Unimpl('ldst-reg')
    if bits(w, 21, 21):
        if bits(w, 11, 10) != 2:
            raise Unimpl('atomic')
        opt, S = bits(w, 15, 13), bits(w, 12, 12)
        if not opt & 2:
            raise Unimpl('ldst-ext')
        off = extend(opt, X(bits(w, 20, 16)), sc if S else 0)
        return f'{{ uint64_t a = {base} + {off}; {mem_op(size, opc, Vb, rt, "a")} }}'
    imm = sx(bits(w, 20, 12), 9)
    mode = bits(w, 11, 10)
    if mode in (0, 2):
        return f'{{ uint64_t a = {base} + {lit(imm)}; {mem_op(size, opc, Vb, rt, "a")} }}'
    if mode == 1:
        return f'{{ uint64_t a = {base}; {mem_op(size, opc, Vb, rt, "a")} {XS(rn)} = a + {lit(imm)}; }}'
    return f'{{ uint64_t a = {base} + {lit(imm)}; {mem_op(size, opc, Vb, rt, "a")} {XS(rn)} = a; }}'


def ldst_pair(w):
    opc, Vb, mode, L = bits(w, 31, 30), bits(w, 26, 26), bits(w, 24, 23), bits(w, 22, 22)
    rt2, rn, rt = bits(w, 14, 10), bits(w, 9, 5), bits(w, 4, 0)
    if Vb:
        if opc == 3:
            raise Unimpl('ldp-v')
        sc = 2 + opc
    else:
        if opc == 3 or (opc == 1 and not L):
            raise Unimpl('ldp')
        sc = 3 if opc == 2 else 2
    imm = sx(bits(w, 21, 15), 7) << sc
    n = 1 << sc

    def one(r, a):
        if Vb:
            size, o = (0, 2 | L) if sc == 4 else (sc, L)
            return mem_op(size, o, 1, r, a)
        if opc == 1:
            return mem_op(2, 2, 0, r, a)
        return mem_op(sc, L, 0, r, a)

    if L and not Vb and rt == rt2:
        raise Unimpl('ldp-same')
    body = f'{one(rt, "a")} {one(rt2, f"a + {n}")}'
    if L and not Vb and rt == rn and rn != 31:
        body = f'{one(rt2, f"a + {n}")} {one(rt, "a")}'
    if mode in (0, 2):
        return f'{{ uint64_t a = {XS(rn)} + {lit(imm)}; {body} }}'
    if mode == 1:
        return f'{{ uint64_t a = {XS(rn)}; {body} {XS(rn)} = a + {lit(imm)}; }}'
    return f'{{ uint64_t a = {XS(rn)} + {lit(imm)}; {body} {XS(rn)} = a; }}'


def dp_reg(w):
    sf, M = bits(w, 31, 31), 64 if w >> 31 else 32
    rd, rn, rm = bits(w, 4, 0), bits(w, 9, 5), bits(w, 20, 16)
    if bits(w, 28, 24) == 0b01010:
        opc, t, N, a = bits(w, 30, 29), bits(w, 23, 22), bits(w, 21, 21), bits(w, 15, 10)
        if not sf and a > 31:
            raise Unimpl('log')
        m = shift(t, R(rm, sf), a, M)
        if N:
            m = f'~{m}'
        e = f'{R(rn, sf)} {"&|^&"[opc]} {m}'
        return wr(rd, sf, f'logic{M}(c, {e})' if opc == 3 else e)
    if bits(w, 28, 24) == 0b01011:
        sub, S = bits(w, 30, 30), bits(w, 29, 29)
        if bits(w, 21, 21):
            if bits(w, 23, 22) or bits(w, 12, 10) > 4:
                raise Unimpl('addext')
            m = extend(bits(w, 15, 13), X(rm), bits(w, 12, 10))
            a = R(rn, sf, True)
            if not sf:
                m = f'(uint32_t){m}'
            sp = not S
        else:
            t, amt = bits(w, 23, 22), bits(w, 15, 10)
            if t == 3 or (not sf and amt > 31):
                raise Unimpl('addsh')
            m, a, sp = shift(t, R(rm, sf), amt, M), R(rn, sf), False
        if S:
            return wr(rd, sf, f'adds{M}(c, {a}, {f"~{m}" if sub else m}, {int(sub)})')
        return wr(rd, sf, f'{a} {"-" if sub else "+"} {m}', sp)
    if bits(w, 28, 21) == 0b11010000:
        if bits(w, 15, 10):
            raise Unimpl('adc')
        sub, S = bits(w, 30, 30), bits(w, 29, 29)
        m = f'~{R(rm, sf)}' if sub else R(rm, sf)
        if S:
            return wr(rd, sf, f'adds{M}(c, {R(rn, sf)}, {m}, c->cf)')
        return wr(rd, sf, f'{R(rn, sf)} + {m} + c->cf')
    if bits(w, 28, 21) == 0b11010010:
        if not bits(w, 29, 29) or bits(w, 10, 10) or bits(w, 4, 4):
            raise Unimpl('ccmp')
        sub = bits(w, 30, 30)
        m = lit(rm, M) if bits(w, 11, 11) else R(rm, sf)
        cmp_ = f'adds{M}(c, {R(rn, sf)}, {f"~{m}" if sub else m}, {int(sub)})'
        return f'if ({cond(bits(w, 15, 12))}) (void){cmp_}; else setnzcv(c, {bits(w, 3, 0)});'
    if bits(w, 28, 21) == 0b11010100:
        if bits(w, 29, 29) or bits(w, 11, 11):
            raise Unimpl('csel')
        k = bits(w, 30, 30) << 1 | bits(w, 10, 10)
        m = R(rm, sf)
        m = [m, f'{m} + 1', f'~{m}', f'({UT(M)})0 - {m}'][k]
        return wr(rd, sf, f'{cond(bits(w, 15, 12))} ? {R(rn, sf)} : ({UT(M)})({m})')
    if bits(w, 30, 21) == 0b0011010110:
        o = bits(w, 15, 10)
        a, b = R(rn, sf), R(rm, sf)
        if o == 2:
            return wr(rd, sf, f'udiv{M}({a}, {b})')
        if o == 3:
            return wr(rd, sf, f'sdiv{M}(({ST(M)}){a}, ({ST(M)}){b})')
        if 8 <= o <= 11:
            s = f'({b} & {M - 1})'
            return wr(rd, sf, [f'{a} << {s}', f'{a} >> {s}', f'({UT(M)})(({ST(M)}){a} >> {s})', f'ror{M}({a}, {s})'][o - 8])
        if 16 <= o <= 23 and (o & 3 == 3) == bool(sf):
            return wr(rd, 0, f'crc32{"c" if o & 4 else ""}_{8 << (o & 3)}({W(rn)}, {R(rm, sf)})')
        raise Unimpl('dp2')
    if bits(w, 30, 21) == 0b1011010110 and bits(w, 20, 16) == 0:
        o, a = bits(w, 15, 10), R(rn, sf)
        if o == 0:
            return wr(rd, sf, f'rbit{M}({a})')
        if o == 1:
            return wr(rd, sf, f'rev16_64({a})')
        if o == 2:
            return wr(rd, sf, f'rev32_64({a})' if sf else f'__builtin_bswap32({a})')
        if o == 3 and sf:
            return wr(rd, sf, f'__builtin_bswap64({a})')
        if o == 4:
            return wr(rd, sf, f'clz{M}({a})')
        if o == 5:
            return wr(rd, sf, f'cls{M}({a})')
        raise Unimpl('dp1')
    if bits(w, 28, 24) == 0b11011:
        op31, o0, ra = bits(w, 23, 21), bits(w, 15, 15), bits(w, 14, 10)
        if bits(w, 30, 29):
            raise Unimpl('dp3')
        sg = '-' if o0 else '+'
        if op31 == 0:
            return wr(rd, sf, f'{R(ra, sf)} {sg} {R(rn, sf)} * {R(rm, sf)}')
        if not sf:
            raise Unimpl('dp3')
        if op31 in (1, 5):
            cast = '(uint64_t)(int32_t)' if op31 == 1 else '(uint64_t)(uint32_t)'
            return wr(rd, 1, f'{X(ra)} {sg} {cast}{X(rn)} * {cast}{X(rm)}')
        if op31 == 2 and not o0:
            return wr(rd, 1, f'(uint64_t)(((__int128)(int64_t){X(rn)} * (int64_t){X(rm)}) >> 64)')
        if op31 == 6 and not o0:
            return wr(rd, 1, f'(uint64_t)(((unsigned __int128){X(rn)} * {X(rm)}) >> 64)')
        raise Unimpl('dp3')
    raise Unimpl('dpreg')


FRINT = {8: 0, 9: 1, 10: 2, 11: 3, 12: 4, 14: 0, 15: 0}


def simd_fp(w):
    if bits(w, 30, 29) == 0 and bits(w, 28, 24) == 0b11110:
        if bits(w, 31, 31) and bits(w, 15, 10):
            raise Unimpl('fp')
        if bits(w, 21, 21):
            return fp_dp(w)
        return fp_fixed(w)
    if bits(w, 31, 24) == 0b00011111:
        t, o1, o0 = bits(w, 23, 22), bits(w, 21, 21), bits(w, 15, 15)
        rm, ra, rn, rd = bits(w, 20, 16), bits(w, 14, 10), bits(w, 9, 5), bits(w, 4, 0)
        n, m, a = V(rn, t), V(rm, t), V(ra, t)
        f = 'fmaf' if t == 0 else 'fma'
        cast = '(_Float16)' if t == 3 else ''
        sn = '-' if o0 != o1 else ''
        sa = '-' if o1 else ''
        return wV(rd, t, f'{cast}{f}({sn}{n}, {m}, {sa}{a})')
    return simd(w)


def fp_dp(w):
    t, rm, rn, rd = bits(w, 23, 22), bits(w, 20, 16), bits(w, 9, 5), bits(w, 4, 0)
    sf = bits(w, 31, 31)
    if bits(w, 15, 10) == 0:
        return fp_int(w)
    if bits(w, 14, 10) == 0b10000:
        o = bits(w, 20, 15)
        n = V(rn, t)
        if o == 0:
            return wV(rd, t, n)
        if o == 1:
            return wV(rd, t, f'fabs({n})')
        if o == 2:
            return wV(rd, t, f'-{n}')
        if o == 3:
            return wV(rd, t, f'sqrtf({n})' if t == 0 else f'sqrt({n})')
        if o in (4, 5, 7):
            return wV(rd, {4: 0, 5: 1, 7: 3}[o], f'({fty({4: 0, 5: 1, 7: 3}[o])[1]}){n}')
        if o in FRINT:
            return wV(rd, t, f'({fty(t)[1]})fround({n}, {FRINT[o]})')
        raise Unimpl('fp1')
    if bits(w, 13, 10) == 0b1000:
        o = bits(w, 4, 0)
        if o & 7:
            raise Unimpl('fcmp')
        return f'fcmp(c, {V(rn, t)}, {"0.0" if o & 8 else V(rm, t)});'
    if bits(w, 12, 10) == 0b100:
        if bits(w, 9, 5):
            raise Unimpl('fmovi')
        v = vfp_imm(bits(w, 20, 13))
        return wV(rd, t, f'({fty(t)[1]}){v.hex()}')
    k = bits(w, 11, 10)
    if k == 1:
        return f'if ({cond(bits(w, 15, 12))}) fcmp(c, {V(rn, t)}, {V(rm, t)}); else setnzcv(c, {bits(w, 3, 0)});'
    if k == 3:
        return wV(rd, t, f'{cond(bits(w, 15, 12))} ? {V(rn, t)} : {V(rm, t)}')
    if k == 2:
        o = bits(w, 15, 12)
        a, b = V(rn, t), V(rm, t)
        ty = fty(t)[1]
        e = {0: f'{a} * {b}', 1: f'{a} / {b}', 2: f'{a} + {b}', 3: f'{a} - {b}',
             4: f'({ty})fmaxa({a}, {b})', 5: f'({ty})fmina({a}, {b})',
             6: f'({ty})fmaxnm({a}, {b})', 7: f'({ty})fminnm({a}, {b})', 8: f'-({a} * {b})'}.get(o)
        if e is None:
            raise Unimpl('fp2')
        return wV(rd, t, e)
    raise Unimpl('fpdp')


def fp_int(w):
    sf, t, rmode, op = bits(w, 31, 31), bits(w, 23, 22), bits(w, 20, 19), bits(w, 18, 16)
    rn, rd = bits(w, 9, 5), bits(w, 4, 0)
    M = 64 if sf else 32
    if op in (6, 7):
        if rmode == 0:
            if t == 0 and not sf:
                return wr(rd, 0, f'c->v[{rn}].s[0]') if op == 6 else f'wv64(c, {rd}, {W(rn)});'
            if t == 1 and sf:
                return wr(rd, 1, f'c->v[{rn}].d[0]') if op == 6 else f'wv64(c, {rd}, {X(rn)});'
            if t == 3:
                return wr(rd, sf, f'c->v[{rn}].h[0]') if op == 6 else f'wv64(c, {rd}, (uint16_t){X(rn)});'
        if rmode == 1 and t == 2 and sf:
            return wr(rd, 1, f'c->v[{rn}].d[1]') if op == 6 else f'c->v[{rd}].d[1] = {X(rn)};'
        raise Unimpl('fmovg')
    if op in (2, 3) and rmode == 0:
        src = f'({ST(M)}){R(rn, sf)}' if op == 2 else R(rn, sf)
        return wV(rd, t, f'({fty(t)[1]}){src}')
    if op in (0, 1):
        return wr(rd, sf, f'fcvt{"su"[op]}{M}({V(rn, t)}, {rmode})')
    if op in (4, 5) and rmode == 0:
        return wr(rd, sf, f'fcvt{"su"[op - 4]}{M}({V(rn, t)}, 4)')
    raise Unimpl('fpint')


def fp_fixed(w):
    sf, t, rmode, op, scale = bits(w, 31, 31), bits(w, 23, 22), bits(w, 20, 19), bits(w, 18, 16), bits(w, 15, 10)
    rn, rd = bits(w, 9, 5), bits(w, 4, 0)
    M = 64 if sf else 32
    fb = 64 - scale
    if rmode == 0 and op in (2, 3):
        src = f'({ST(M)}){R(rn, sf)}' if op == 2 else R(rn, sf)
        return wV(rd, t, f'({fty(t)[1]})((double){src} / {float(2 ** fb).hex()})')
    if rmode == 3 and op in (0, 1):
        return wr(rd, sf, f'fcvt{"su"[op]}{M}((double){V(rn, t)} * {float(2 ** fb).hex()}, 3)')
    raise Unimpl('fpfixed')


UF = {8: 'b', 16: 'h', 32: 's', 64: 'd'}
SF = {8: 'sb', 16: 'sh', 32: 'ss', 64: 'sd'}
FF = {16: 'hf', 32: 'f', 64: 'df'}
FTY = {16: '_Float16', 32: 'float', 64: 'double'}


def vop(d, lanes, stmt, regs, keep=False, q=1):
    decl = ''.join(f'V128 {k} = c->v[{r}]; ' for k, r in regs)
    init = f'c->v[{d}]' if keep else '{0}'
    hi = '' if q or not keep else ' r.d[1] = 0;'
    loop = stmt if lanes == 1 else f'for (int i = 0; i < {lanes}; i++) {{ {stmt} }}'
    if lanes == 1:
        loop = loop.replace('[i]', '[0]')
    return f'{{ {decl}V128 r = {init}; {loop}{hi} c->v[{d}] = r; }}'


def sat(e, E, signed):
    if E == 64:
        raise Unimpl('sat64')
    if signed:
        lo, hi = -(1 << (E - 1)), (1 << (E - 1)) - 1
    else:
        lo, hi = 0, (1 << E) - 1
    return f'{{ int64_t t = {e}; r.{SF[E] if signed else UF[E]}[i] = t > {hi}ll ? {hi}ll : t < {lo}ll ? {lo}ll : t; }}'


INT3 = {
    (0, 0b10000): '{T} = a.{T}[i] + b.{T}[i];', (1, 0b10000): '{T} = a.{T}[i] - b.{T}[i];',
    (0, 0b10001): '{T} = (a.{T}[i] & b.{T}[i]) ? -1 : 0;', (1, 0b10001): '{T} = a.{T}[i] == b.{T}[i] ? -1 : 0;',
    (0, 0b00110): '{T} = a.{S}[i] > b.{S}[i] ? -1 : 0;', (1, 0b00110): '{T} = a.{T}[i] > b.{T}[i] ? -1 : 0;',
    (0, 0b00111): '{T} = a.{S}[i] >= b.{S}[i] ? -1 : 0;', (1, 0b00111): '{T} = a.{T}[i] >= b.{T}[i] ? -1 : 0;',
    (0, 0b10011): '{T} = a.{T}[i] * b.{T}[i];',
    (0, 0b10010): '{T} = o.{T}[i] + a.{T}[i] * b.{T}[i];', (1, 0b10010): '{T} = o.{T}[i] - a.{T}[i] * b.{T}[i];',
    (0, 0b01100): '{T} = a.{S}[i] > b.{S}[i] ? a.{T}[i] : b.{T}[i];', (1, 0b01100): '{T} = a.{T}[i] > b.{T}[i] ? a.{T}[i] : b.{T}[i];',
    (0, 0b01101): '{T} = a.{S}[i] < b.{S}[i] ? a.{T}[i] : b.{T}[i];', (1, 0b01101): '{T} = a.{T}[i] < b.{T}[i] ? a.{T}[i] : b.{T}[i];',
    (0, 0b01000): '{{ int s = (int8_t)b.{T}[i]; r.{T}[i] = s >= 0 ? (s < {E} ? a.{T}[i] << s : 0) : (-s < {E} ? a.{S}[i] >> -s : a.{S}[i] >> ({E} - 1)); }}',
    (1, 0b01000): '{{ int s = (int8_t)b.{T}[i]; r.{T}[i] = s >= 0 ? (s < {E} ? a.{T}[i] << s : 0) : (-s < {E} ? a.{T}[i] >> -s : 0); }}',
    (0, 0b00000): '{T} = ((int64_t)a.{S}[i] + b.{S}[i]) >> 1;', (1, 0b00000): '{T} = ((uint64_t)a.{T}[i] + b.{T}[i]) >> 1;',
    (0, 0b00010): '{T} = ((int64_t)a.{S}[i] + b.{S}[i] + 1) >> 1;', (1, 0b00010): '{T} = ((uint64_t)a.{T}[i] + b.{T}[i] + 1) >> 1;',
    (0, 0b01110): '{T} = a.{S}[i] > b.{S}[i] ? a.{S}[i] - b.{S}[i] : b.{S}[i] - a.{S}[i];',
    (1, 0b01110): '{T} = a.{T}[i] > b.{T}[i] ? a.{T}[i] - b.{T}[i] : b.{T}[i] - a.{T}[i];',
    (0, 0b01111): '{T} = o.{T}[i] + (a.{S}[i] > b.{S}[i] ? a.{S}[i] - b.{S}[i] : b.{S}[i] - a.{S}[i]);',
    (1, 0b01111): '{T} = o.{T}[i] + (a.{T}[i] > b.{T}[i] ? a.{T}[i] - b.{T}[i] : b.{T}[i] - a.{T}[i]);',
}
SAT3 = {(0, 0b00001): ('(int64_t)a.{S}[i] + b.{S}[i]', 1), (1, 0b00001): ('(int64_t)a.{T}[i] + b.{T}[i]', 0),
        (0, 0b00101): ('(int64_t)a.{S}[i] - b.{S}[i]', 1), (1, 0b00101): ('(int64_t)a.{T}[i] - b.{T}[i]', 0)}
PAIR3 = {(0, 0b10111): '+', (0, 0b10100): 'smax', (1, 0b10100): 'umax', (0, 0b10101): 'smin', (1, 0b10101): 'umin'}

FP3 = {
    (0, 0, 0b11010): '{F} = a.{F}[i] + b.{F}[i];', (0, 1, 0b11010): '{F} = a.{F}[i] - b.{F}[i];',
    (1, 0, 0b11011): '{F} = a.{F}[i] * b.{F}[i];', (1, 0, 0b11111): '{F} = a.{F}[i] / b.{F}[i];',
    (0, 0, 0b11110): '{F} = fmaxa(a.{F}[i], b.{F}[i]);', (0, 1, 0b11110): '{F} = fmina(a.{F}[i], b.{F}[i]);',
    (0, 0, 0b11000): '{F} = fmaxnm(a.{F}[i], b.{F}[i]);', (0, 1, 0b11000): '{F} = fminnm(a.{F}[i], b.{F}[i]);',
    (0, 0, 0b11001): '{F} = {FMA}(a.{F}[i], b.{F}[i], o.{F}[i]);', (0, 1, 0b11001): '{F} = {FMA}(-a.{F}[i], b.{F}[i], o.{F}[i]);',
    (1, 1, 0b11010): '{F} = fabs(a.{F}[i] - b.{F}[i]);',
    (0, 0, 0b11100): '{T} = a.{F}[i] == b.{F}[i] ? -1 : 0;', (1, 0, 0b11100): '{T} = a.{F}[i] >= b.{F}[i] ? -1 : 0;',
    (1, 1, 0b11100): '{T} = a.{F}[i] > b.{F}[i] ? -1 : 0;',
    (1, 0, 0b11101): '{T} = fabs(a.{F}[i]) >= fabs(b.{F}[i]) ? -1 : 0;', (1, 1, 0b11101): '{T} = fabs(a.{F}[i]) > fabs(b.{F}[i]) ? -1 : 0;',
    (0, 0, 0b11111): '{F} = {RS}(a.{F}[i], b.{F}[i]);', (0, 1, 0b11111): '{F} = {RQ}(a.{F}[i], b.{F}[i]);',
    (0, 0, 0b11011): '{F} = a.{F}[i] * b.{F}[i];',
}
FPAIR3 = {(1, 0, 0b11010): '+', (1, 0, 0b11110): 'fmaxa', (1, 1, 0b11110): 'fmina',
          (1, 0, 0b11000): 'fmaxnm', (1, 1, 0b11000): 'fminnm'}


def fmt(t, E, fe=None):
    return t.format(T=UF[E], S=SF[E], E=E, F=FF.get(fe or E, 'f'), FMA='fmaf' if (fe or E) == 32 else 'fma',
                    RS='frecpsf' if (fe or E) == 32 else 'frecps', RQ='frsqrtsf' if (fe or E) == 32 else 'frsqrts')


def stmt3(tpl, E, fe=None):
    s = fmt(tpl, E, fe)
    tgt = s.split(' = ', 1)
    if s.startswith('{'):
        return s
    return f'r.{tgt[0]}[i] = {tgt[1]}'


def three_same(w, scalar):
    Q, U, size, rm, op, rn, rd = bits(w, 30, 30), bits(w, 29, 29), bits(w, 23, 22), bits(w, 20, 16), bits(w, 15, 11), bits(w, 9, 5), bits(w, 4, 0)
    regs = [('a', rn), ('b', rm), ('o', rd)]
    if op >= 0b11000:
        a_, sz = bits(w, 23, 23), bits(w, 22, 22)
        E = 64 if sz else 32
        if not Q and sz and not scalar:
            raise Unimpl('fp3')
        lanes = 1 if scalar else (64 << Q) // E
        if not scalar and (U, a_, op) in FPAIR3:
            f = FPAIR3[(U, a_, op)]
            F = FF[E]
            h = lanes // 2
            ex = (lambda x, y: f'{x} {f} {y}') if f in '+*' else (lambda x, y: f'{f}({x}, {y})')
            st = f'r.{F}[i] = i < {h} ? {ex(f"a.{F}[2*i]", f"a.{F}[2*i+1]")} : {ex(f"b.{F}[2*(i-{h})]", f"b.{F}[2*(i-{h})+1]")};'
            return vop(rd, lanes, st, regs)
        k = (U, a_, op)
        if scalar and k == (0, 0, 0b11011):
            raise Unimpl('fmulx')
        if k not in FP3:
            raise Unimpl('fp3')
        return vop(rd, lanes, stmt3(FP3[k], E), regs)
    E = 8 << size
    lanes = 1 if scalar else (64 << Q) // E
    if scalar and size != 3 and op not in (0b00001, 0b00101):
        raise Unimpl('int3-scalar')
    if op == 0b00011 and not scalar:
        l2 = 2 if Q else 1
        k = U << 2 | size
        st = ['r.d[i] = a.d[i] & b.d[i];', 'r.d[i] = a.d[i] & ~b.d[i];', 'r.d[i] = a.d[i] | b.d[i];', 'r.d[i] = a.d[i] | ~b.d[i];',
              'r.d[i] = a.d[i] ^ b.d[i];', 'r.d[i] = (o.d[i] & a.d[i]) | (~o.d[i] & b.d[i]);',
              'r.d[i] = (o.d[i] & ~b.d[i]) | (a.d[i] & b.d[i]);', 'r.d[i] = (o.d[i] & b.d[i]) | (a.d[i] & ~b.d[i]);'][k]
        if k == 2 and rn == rm:
            return vop(rd, l2, 'r.d[i] = a.d[i];', [('a', rn)])
        return vop(rd, l2, st, regs)
    if (U, op) in SAT3:
        e, sg = SAT3[(U, op)]
        return vop(rd, lanes, sat(fmt(e, E), E, sg), regs)
    if (U, op) in PAIR3 and not scalar:
        f = PAIR3[(U, op)]
        T = SF[E] if f[0] == 's' else UF[E]
        h = lanes // 2
        ex = (lambda x, y: f'{x} + {y}') if f == '+' else (lambda x, y: f'({x} {">" if "max" in f else "<"} {y} ? {x} : {y})')
        st = f'r.{UF[E]}[i] = i < {h} ? {ex(f"a.{T}[2*i]", f"a.{T}[2*i+1]")} : {ex(f"b.{T}[2*(i-{h})]", f"b.{T}[2*(i-{h})+1]")};'
        return vop(rd, lanes, st, regs)
    if (U, op) not in INT3 or (E == 64 and op == 0b10011):
        raise Unimpl('int3')
    return vop(rd, lanes, stmt3(INT3[(U, op)], E), regs)


def lane_info(imm5):
    k = (imm5 & -imm5).bit_length() - 1
    if k > 3:
        raise Unimpl('imm5')
    return 8 << k, imm5 >> (k + 1)


def copy(w):
    Q, op, imm5, imm4, rn, rd = bits(w, 30, 30), bits(w, 29, 29), bits(w, 20, 16), bits(w, 14, 11), bits(w, 9, 5), bits(w, 4, 0)
    E, idx = lane_info(imm5)
    T = UF[E]
    lanes = (64 << Q) // E
    if op:
        src = imm4 >> (E.bit_length() - 4)
        return f'c->v[{rd}].{T}[{idx}] = c->v[{rn}].{T}[{src}];'
    if imm4 == 0:
        return vop(rd, lanes, f'r.{T}[i] = a.{T}[{idx}];', [('a', rn)])
    if imm4 == 1:
        return vop(rd, lanes, f'r.{T}[i] = (uint{E}_t){X(rn)};', [])
    if imm4 == 3 and Q:
        return f'c->v[{rd}].{T}[{idx}] = (uint{E}_t){X(rn)};'
    if imm4 == 7:
        return wr(rd, Q, f'c->v[{rn}].{T}[{idx}]')
    if imm4 == 5:
        return wr(rd, Q, f'(uint64_t)c->v[{rn}].{SF[E]}[{idx}]')
    raise Unimpl('copy')


def expand_imm(op, cmode, imm8):
    c3 = cmode >> 1
    if c3 < 4:
        return replicate(imm8 << (8 * c3), 32, 64), 32
    if c3 < 6:
        return replicate(imm8 << (8 * (c3 & 1)), 16, 64), 16
    if c3 == 6:
        v = (imm8 << 8 | 0xff) if not cmode & 1 else (imm8 << 16 | 0xffff)
        return replicate(v, 32, 64), 32
    if not cmode & 1:
        if not op:
            return replicate(imm8, 8, 64), 8
        return sum(0xff << (8 * i) for i in range(8) if imm8 >> i & 1), 64
    a, b, rest = imm8 >> 7, imm8 >> 6 & 1, imm8 & 0x3f
    if not op:
        v = a << 31 | (b ^ 1) << 30 | (0x1f if b else 0) << 25 | rest << 19
        return replicate(v, 32, 64), 32
    return a << 63 | (b ^ 1) << 62 | (0xff if b else 0) << 54 | rest << 48, 64


def mod_imm(w):
    Q, op, cmode, rd = bits(w, 30, 30), bits(w, 29, 29), bits(w, 15, 12), bits(w, 4, 0)
    imm8 = bits(w, 18, 16) << 5 | bits(w, 9, 5)
    if bits(w, 11, 11):
        raise Unimpl('fmov16')
    v, _ = expand_imm(op, cmode, imm8)
    hi = 'd[1]' if Q else None
    orr_bic = (cmode < 12 and cmode & 1)
    if orr_bic:
        o = '&= ~' if op else '|= '
        s = f'c->v[{rd}].d[0] {o}{lit(v)};'
        return s + (f' c->v[{rd}].d[1] {o}{lit(v)};' if Q else f' c->v[{rd}].d[1] = 0;')
    if op and cmode < 14:
        v = ~v & (2 ** 64 - 1)
    if op and cmode == 15 and not Q:
        raise Unimpl('fmovd')
    return f'{{ V128 r = {{0}}; r.d[0] = {lit(v)}; r.d[1] = {lit(v) if Q else "0"}; c->v[{rd}] = r; }}'


def ext(w):
    Q, rm, imm4, rn, rd = bits(w, 30, 30), bits(w, 20, 16), bits(w, 14, 11), bits(w, 9, 5), bits(w, 4, 0)
    L = 8 << Q
    return vop(rd, L, f'r.b[i] = i + {imm4} < {L} ? a.b[i + {imm4}] : b.b[i + {imm4} - {L}];', [('a', rn), ('b', rm)])


def two_misc(w, scalar):
    Q, U, size, op, rn, rd = bits(w, 30, 30), bits(w, 29, 29), bits(w, 23, 22), bits(w, 16, 12), bits(w, 9, 5), bits(w, 4, 0)
    regs = [('a', rn)]
    sz = size & 1
    if op in (0b10110, 0b10111) and not U and not scalar and size >> 1 == 0:
        if op == 0b10111:
            src, dst = (16, 32) if not sz else (32, 64)
            n = 128 // dst
            return vop(rd, n, f'r.{FF[dst]}[i] = ({FTY[dst]})a.{FF[src]}[i + {n if Q else 0}];', regs)
        src, dst = (32, 16) if not sz else (64, 32)
        n = 64 // dst
        return vop(rd, n, f'r.{FF[dst]}[i + {n if Q else 0}] = ({FTY[dst]})a.{FF[src]}[i];', regs, keep=bool(Q))
    if op == 0b10010 and not U and not scalar:
        E = 8 << size
        n = 64 // E
        return vop(rd, n, f'r.{UF[E]}[i + {n if Q else 0}] = (uint{E}_t)a.{UF[2 * E]}[i];', regs, keep=bool(Q))
    if op >= 0b01100 and size >> 1 or op in (0b11101, 0b11010, 0b11011, 0b11100, 0b11000, 0b11001) and not size >> 1 or op == 0b11111:
        E = 64 if sz else 32
        if not Q and sz and not scalar:
            raise Unimpl('fp2m')
        lanes = 1 if scalar else (64 << Q) // E
        F, T, S, ty = FF[E], UF[E], SF[E], FTY[E]
        hi = size >> 1
        cv = {0b11010: (0, 1), 0b11011: (2, 3), 0b11100: (4, None)}
        st = None
        if op == 0b11101 and not hi:
            st = f'r.{F}[i] = ({ty})a.{S if not U else T}[i];'
        elif op == 0b11101 and hi and not U:
            st = f'r.{F}[i] = frecpe{E}(a.{F}[i]);'
        elif op == 0b11101 and hi and U:
            st = f'r.{F}[i] = frsqrte{E}(a.{F}[i]);'
        elif op in cv and cv[op][hi] is not None:
            mode = cv[op][hi]
            st = f'r.{T}[i] = fcvt{"su"[U]}{E}(a.{F}[i], {mode});'
        elif op == 0b01111 and hi:
            st = f'r.{F}[i] = {"-" if U else "fabs"}(a.{F}[i]);'
        elif op == 0b11111 and hi and U:
            st = f'r.{F}[i] = {"sqrtf" if E == 32 else "sqrt"}(a.{F}[i]);'
        elif op in (0b11000, 0b11001):
            mode = {(0, 0, 0): 0, (0, 0, 1): 2, (0, 1, 0): 1, (0, 1, 1): 3, (1, 0, 0): 4, (1, 0, 1): 0, (1, 1, 1): 0}.get((U, hi, op & 1))
            if mode is None:
                raise Unimpl('frint')
            st = f'r.{F}[i] = ({ty})fround(a.{F}[i], {mode});'
        elif op in (0b01100, 0b01101, 0b01110) and hi:
            cmp_ = {(0, 0b01100): '>', (0, 0b01101): '==', (0, 0b01110): '<', (1, 0b01100): '>=', (1, 0b01101): '<='}.get((U, op))
            if cmp_ is None:
                raise Unimpl('fcmz')
            st = f'r.{T}[i] = a.{F}[i] {cmp_} 0 ? -1 : 0;'
        if st is None:
            raise Unimpl('fp2m')
        return vop(rd, lanes, st, regs)
    E = 8 << size
    lanes = 1 if scalar else (64 << Q) // E
    T, S = UF[E], SF[E]
    if scalar and size != 3:
        raise Unimpl('int2m-scalar')
    st = {
        (0, 0b01000): f'r.{T}[i] = a.{S}[i] > 0 ? -1 : 0;', (1, 0b01000): f'r.{T}[i] = a.{S}[i] >= 0 ? -1 : 0;',
        (0, 0b01001): f'r.{T}[i] = a.{T}[i] == 0 ? -1 : 0;', (1, 0b01001): f'r.{T}[i] = a.{S}[i] <= 0 ? -1 : 0;',
        (0, 0b01010): f'r.{T}[i] = a.{S}[i] < 0 ? -1 : 0;',
        (0, 0b01011): f'r.{T}[i] = a.{S}[i] < 0 ? -a.{T}[i] : a.{T}[i];', (1, 0b01011): f'r.{T}[i] = -a.{T}[i];',
    }.get((U, op))
    if op == 0b00101 and size == 0:
        st = 'r.b[i] = ~a.b[i];' if U else 'r.b[i] = __builtin_popcount(a.b[i]);'
    if op == 0b00000 and not U:
        g = 64 // E
        st = f'r.{T}[i] = a.{T}[(i / {g}) * {g} + {g - 1} - i % {g}];'
    if op == 0b00000 and U and E < 32:
        g = 32 // E
        st = f'r.{T}[i] = a.{T}[(i / {g}) * {g} + {g - 1} - i % {g}];'
    if op == 0b00001 and not U and E == 8:
        st = 'r.b[i] = a.b[i ^ 1];'
    if st is None:
        raise Unimpl('int2m')
    return vop(rd, lanes, st, regs)


def across(w):
    Q, U, size, op, rn, rd = bits(w, 30, 30), bits(w, 29, 29), bits(w, 23, 22), bits(w, 16, 12), bits(w, 9, 5), bits(w, 4, 0)
    if op in (0b01100, 0b01111) and U:
        if size & 1 or not Q:
            raise Unimpl('fpv')
        f = {(0b01100, 0): 'fmaxnm', (0b01100, 1): 'fminnm', (0b01111, 0): 'fmaxa', (0b01111, 1): 'fmina'}[(op, size >> 1)]
        return f'{{ V128 a = c->v[{rn}]; wvS(c, {rd}, (float){f}({f}(a.f[0], a.f[1]), {f}(a.f[2], a.f[3]))); }}'
    E = 8 << size
    n = (64 << Q) // E
    T, S = UF[E], SF[E]
    src = S if not U else T
    if op == 0b11011 and not U:
        return f'{{ V128 a = c->v[{rn}]; uint64_t s = 0; for (int i = 0; i < {n}; i++) s += a.{T}[i]; wv64(c, {rd}, (uint{E}_t)s); }}'
    if op == 0b00011:
        return f'{{ V128 a = c->v[{rn}]; int64_t s = 0; for (int i = 0; i < {n}; i++) s += a.{src}[i]; wv64(c, {rd}, (uint{2 * E}_t)s); }}'
    if op in (0b01010, 0b11010):
        cmp_ = '>' if op == 0b01010 else '<'
        return f'{{ V128 a = c->v[{rn}]; int64_t s = a.{src}[0]; for (int i = 1; i < {n}; i++) if (a.{src}[i] {cmp_} s) s = a.{src}[i]; wv64(c, {rd}, (uint{E}_t)s); }}'
    raise Unimpl('across')


def indexed(w, scalar):
    Q, U, size, L, Mb, rm, op, H, rn, rd = (bits(w, 30, 30), bits(w, 29, 29), bits(w, 23, 22), bits(w, 21, 21), bits(w, 20, 20),
                                           bits(w, 19, 16), bits(w, 15, 12), bits(w, 11, 11), bits(w, 9, 5), bits(w, 4, 0))
    regs = [('a', rn), ('b', None), ('o', rd)]
    if size >> 1 and op in (0b0001, 0b0101, 0b1001):
        sz = size & 1
        E = 64 if sz else 32
        if sz and L:
            raise Unimpl('fidx')
        idx = H if sz else H << 1 | L
        m = Mb << 4 | rm
        lanes = 1 if scalar else (64 << Q) // E
        F = FF[E]
        fma = 'fmaf' if E == 32 else 'fma'
        st = {0b0001: f'r.{F}[i] = {fma}(a.{F}[i], b.{F}[{idx}], o.{F}[i]);',
              0b0101: f'r.{F}[i] = {fma}(-a.{F}[i], b.{F}[{idx}], o.{F}[i]);',
              0b1001: f'r.{F}[i] = a.{F}[i] * b.{F}[{idx}];'}[op]
        if U and op != 0b1001 or U and op == 0b1001:
            raise Unimpl('fmulx-idx')
        return vop(rd, lanes, st, [('a', rn), ('b', m), ('o', rd)])
    if scalar or size not in (1, 2):
        raise Unimpl('idx')
    E = 8 << size
    idx = H << 2 | L << 1 | Mb if size == 1 else H << 1 | L
    m = rm if size == 1 else Mb << 4 | rm
    T, S = UF[E], SF[E]
    lanes = (64 << Q) // E
    if op == 0b1000 and not U:
        return vop(rd, lanes, f'r.{T}[i] = a.{T}[i] * b.{T}[{idx}];', [('a', rn), ('b', m)])
    if op in (0b0000, 0b0100) and U:
        sg = '+' if op == 0 else '-'
        return vop(rd, lanes, f'r.{T}[i] = o.{T}[i] {sg} a.{T}[i] * b.{T}[{idx}];', [('a', rn), ('b', m), ('o', rd)])
    if op in (0b1010, 0b0010, 0b0110):
        D = 2 * E
        n = 64 // E
        src = S if not U else T
        cast = f'(int{D}_t)' if not U else f'(uint{D}_t)'
        e = f'{cast}a.{src}[i + {n if Q else 0}] * b.{src}[{idx}]'
        st = {0b1010: f'r.{UF[D]}[i] = {e};', 0b0010: f'r.{UF[D]}[i] = o.{UF[D]}[i] + {e};', 0b0110: f'r.{UF[D]}[i] = o.{UF[D]}[i] - {e};'}[op]
        return vop(rd, n, st, [('a', rn), ('b', m), ('o', rd)])
    raise Unimpl('idx')


def shift_imm(w, scalar):
    Q, U, immh, immb, op, rn, rd = bits(w, 30, 30), bits(w, 29, 29), bits(w, 22, 19), bits(w, 18, 16), bits(w, 15, 11), bits(w, 9, 5), bits(w, 4, 0)
    E = 8 << (immh.bit_length() - 1)
    v = immh << 3 | immb
    rs, ls = 2 * E - v, v - E
    if scalar and E != 64 and op not in (0b11100, 0b11111):
        raise Unimpl('shs')
    lanes = 1 if scalar else (64 << Q) // E
    T, S = UF[E], SF[E]
    regs = [('a', rn), ('o', rd)]
    src = T if U else S
    if op == 0b00000:
        return vop(rd, lanes, f'r.{T}[i] = {"0" if rs == E and U else f"a.{src}[i] >> {min(rs, E - 1)}"};', regs)
    if op == 0b00010:
        return vop(rd, lanes, f'r.{T}[i] = o.{T}[i] + {"0" if rs == E and U else f"(a.{src}[i] >> {min(rs, E - 1)})"};', regs)
    if op == 0b01010 and not U:
        return vop(rd, lanes, f'r.{T}[i] = a.{T}[i] << {ls};', regs)
    if op == 0b10100 and not scalar:
        if E == 64:
            raise Unimpl('shll')
        n = 64 // E
        return vop(rd, n, f'r.{UF[2 * E] if U else SF[2 * E]}[i] = ({"u" if U else ""}int{2 * E}_t)a.{src}[i + {n if Q else 0}] << {ls};', regs)
    if op in (0b10000, 0b10001) and not U and not scalar:
        if E == 64:
            raise Unimpl('shrn')
        n = 64 // E
        rnd = f' + ((uint{2 * E}_t)1 << {rs - 1})' if op & 1 else ''
        return vop(rd, n, f'r.{T}[i + {n if Q else 0}] = (uint{E}_t)((a.{UF[2 * E]}[i]{rnd}) >> {rs});', regs, keep=bool(Q))
    if op == 0b11100 and E >= 32:
        F, ty = FF[E], FTY[E]
        return vop(rd, lanes, f'r.{F}[i] = ({ty})((double)a.{src}[i] / {float(2 ** rs).hex()});', regs)
    if op == 0b11111 and E >= 32:
        F = FF[E]
        return vop(rd, lanes, f'r.{T}[i] = fcvt{"su"[U]}{E}((double)a.{F}[i] * {float(2 ** rs).hex()}, 3);', regs)
    raise Unimpl('shimm')


def permute(w):
    Q, size, rm, op, rn, rd = bits(w, 30, 30), bits(w, 23, 22), bits(w, 20, 16), bits(w, 14, 12), bits(w, 9, 5), bits(w, 4, 0)
    E = 8 << size
    n = (64 << Q) // E
    T = UF[E]
    h = n // 2
    regs = [('a', rn), ('b', rm)]
    p = op >> 2
    if op & 3 == 1:
        return vop(rd, n, f'r.{T}[i] = i < {h} ? a.{T}[2*i+{p}] : b.{T}[2*(i-{h})+{p}];', regs)
    if op & 3 == 2:
        return vop(rd, n, f'r.{T}[i] = (i & 1) ? b.{T}[(i & ~1) + {p}] : a.{T}[(i & ~1) + {p}];', regs)
    if op & 3 == 3:
        return vop(rd, n, f'r.{T}[i] = (i & 1) ? b.{T}[i / 2 + {p * h}] : a.{T}[i / 2 + {p * h}];', regs)
    raise Unimpl('perm')


def tbl(w):
    Q, rm, ln, op, rn, rd = bits(w, 30, 30), bits(w, 20, 16), bits(w, 14, 13), bits(w, 12, 12), bits(w, 9, 5), bits(w, 4, 0)
    n = 8 << Q
    tb = ' '.join(f't[{k}] = c->v[{(rn + k) % 32}];' for k in range(ln + 1))
    other = 'o.b[i]' if op else '0'
    return (f'{{ V128 t[4]; {tb} V128 m = c->v[{rm}], o = c->v[{rd}], r = {{0}}; '
            f'for (int i = 0; i < {n}; i++) {{ unsigned k = m.b[i]; r.b[i] = k < {16 * (ln + 1)} ? t[k >> 4].b[k & 15] : {other}; }} c->v[{rd}] = r; }}')


def three_diff(w):
    Q, U, size, rm, op, rn, rd = bits(w, 30, 30), bits(w, 29, 29), bits(w, 23, 22), bits(w, 20, 16), bits(w, 15, 12), bits(w, 9, 5), bits(w, 4, 0)
    if size == 3:
        raise Unimpl('3diff')
    E = 8 << size
    D = 2 * E
    n = 64 // E
    off = n if Q else 0
    src = UF[E] if U else SF[E]
    cast = f'(uint{D}_t)' if U else f'(int{D}_t)'
    a, b = f'{cast}a.{src}[i + {off}]', f'{cast}b.{src}[i + {off}]'
    R_ = f'r.{UF[D]}[i]'
    regs = [('a', rn), ('b', rm), ('o', rd)]
    st = {0b1100: f'{R_} = {a} * {b};', 0b1000: f'{R_} = o.{UF[D]}[i] + {a} * {b};', 0b1010: f'{R_} = o.{UF[D]}[i] - {a} * {b};',
          0b0000: f'{R_} = {a} + {b};', 0b0010: f'{R_} = {a} - {b};',
          0b0001: f'{R_} = a.{UF[D]}[i] + {b};', 0b0011: f'{R_} = a.{UF[D]}[i] - {b};'}.get(op)
    if st is None:
        raise Unimpl('3diff')
    return vop(rd, n, st, regs)


def ldst_multi(w):
    Q, L, post, rm, op, size, rn, rt = bits(w, 30, 30), bits(w, 22, 22), bits(w, 23, 23), bits(w, 20, 16), bits(w, 15, 12), bits(w, 11, 10), bits(w, 9, 5), bits(w, 4, 0)
    spec = {0b0111: (1, 1), 0b1010: (2, 1), 0b0110: (3, 1), 0b0010: (4, 1), 0b1000: (1, 2), 0b0100: (1, 3), 0b0000: (1, 4)}.get(op)
    if spec is None:
        raise Unimpl('ldst-multi')
    rpt, sel = spec
    E = 8 << size
    if E == 64 and not Q and sel > 1:
        raise Unimpl('ldst-multi')
    n = (64 << Q) // E
    T = UF[E]
    nb = E // 8
    total = rpt * sel * n * nb
    body = []
    for r in range(rpt):
        for e in range(n):
            for s in range(sel):
                reg = (rt + r * sel + s) % 32
                off = ((r * n + e) * sel + s) * nb
                body.append(f'c->v[{reg}].{T}[{e}] = M{E}(a + {off});' if L else f'M{E}(a + {off}) = c->v[{reg}].{T}[{e}];')
    if L and not Q:
        body += [f'c->v[{(rt + k) % 32}].d[1] = 0;' for k in range(rpt * sel)]
    wb = ''
    if post:
        wb = f' {XS(rn)} = a + {lit(total) if rm == 31 else X(rm)};'
    return f'{{ uint64_t a = {XS(rn)}; {" ".join(body)}{wb} }}'


def ldst_single(w):
    Q, L, R_, post, rm, op, S, size, rn, rt = (bits(w, 30, 30), bits(w, 22, 22), bits(w, 21, 21), bits(w, 23, 23), bits(w, 20, 16),
                                              bits(w, 15, 13), bits(w, 12, 12), bits(w, 11, 10), bits(w, 9, 5), bits(w, 4, 0))
    sel = (op & 1) << 1 | R_
    sel += 1
    sc = op >> 1
    if sc == 3:
        if not L or S:
            raise Unimpl('ld1r')
        E = 8 << size
        n = (64 << Q) // E
        body = ' '.join(f'{{ uint{E}_t v = M{E}(a + {s * E // 8}); V128 r = {{0}}; for (int i = 0; i < {n}; i++) r.{UF[E]}[i] = v; c->v[{(rt + s) % 32}] = r; }}' for s in range(sel))
        total = sel * E // 8
    else:
        if sc == 2:
            if size & 2:
                raise Unimpl('ldst1')
            E, idx = (32, Q << 1 | S) if size == 0 else (64, Q)
        elif sc == 1:
            if size & 1:
                raise Unimpl('ldst1')
            E, idx = 16, Q << 2 | S << 1 | size >> 1
        else:
            E, idx = 8, Q << 3 | S << 2 | size
        nb = E // 8
        body = ' '.join((f'c->v[{(rt + s) % 32}].{UF[E]}[{idx}] = M{E}(a + {s * nb});' if L else f'M{E}(a + {s * nb}) = c->v[{(rt + s) % 32}].{UF[E]}[{idx}];') for s in range(sel))
        total = sel * nb
    wb = f' {XS(rn)} = a + {lit(total) if rm == 31 else X(rm)};' if post else ''
    return f'{{ uint64_t a = {XS(rn)}; {body}{wb} }}'


def crypto(w):
    if bits(w, 31, 24) == 0x4e and bits(w, 21, 17) == 0b10100 and bits(w, 11, 10) == 2 and bits(w, 23, 22) == 0:
        op, rn, rd = bits(w, 16, 12), bits(w, 9, 5), bits(w, 4, 0)
        f = {4: 'aese', 5: 'aesd', 6: 'aesmc', 7: 'aesimc'}.get(op)
        if f is None:
            raise Unimpl('aes')
        if op < 6:
            return f'c->v[{rd}] = {f}(c->v[{rd}], c->v[{rn}]);'
        return f'c->v[{rd}] = {f}(c->v[{rn}]);'
    raise Unimpl('crypto')


def simd(w):
    top = bits(w, 31, 31)
    if top:
        raise Unimpl('simd')
    s = bits(w, 30, 30)
    if bits(w, 31, 24) == 0x4e and bits(w, 23, 17) == 0b0010100 and bits(w, 11, 10) == 2:
        return crypto(w)
    scalar = bits(w, 30, 30) == 1 and bits(w, 28, 28) == 1
    vec = bits(w, 28, 28) == 0
    if not scalar and not vec:
        raise Unimpl('simd')
    b2824 = bits(w, 28, 24)
    if vec and b2824 == 0b01110 and bits(w, 21, 21) and bits(w, 10, 10):
        return three_same(w, False)
    if scalar and b2824 == 0b11110 and bits(w, 21, 21) and bits(w, 10, 10):
        return three_same(w, True)
    if vec and bits(w, 28, 21) == 0b01110000 and not bits(w, 15, 15) and bits(w, 10, 10):
        return copy(w)
    if scalar and bits(w, 29, 21) == 0b011110000 and bits(w, 15, 10) == 1:
        E, idx = lane_info(bits(w, 20, 16))
        return f'wv64(c, {bits(w, 4, 0)}, c->v[{bits(w, 9, 5)}].{UF[E]}[{idx}]);'
    if vec and bits(w, 28, 19) == 0b0111100000 and bits(w, 10, 10):
        return mod_imm(w)
    if vec and bits(w, 29, 21) == 0b101110000 and not bits(w, 15, 15) and not bits(w, 10, 10):
        return ext(w)
    if b2824 in (0b01110, 0b11110) and bits(w, 21, 17) == 0b10000 and bits(w, 11, 10) == 2:
        return two_misc(w, scalar)
    if vec and b2824 == 0b01110 and bits(w, 21, 17) == 0b11000 and bits(w, 11, 10) == 2:
        return across(w)
    if b2824 in (0b01111, 0b11111) and not bits(w, 10, 10):
        return indexed(w, scalar)
    if bits(w, 28, 23) in (0b011110, 0b111110) and bits(w, 22, 19) and bits(w, 10, 10):
        return shift_imm(w, scalar)
    if vec and bits(w, 29, 24) == 0b001110 and not bits(w, 21, 21) and not bits(w, 15, 15) and bits(w, 11, 10) == 2:
        return permute(w)
    if vec and bits(w, 29, 24) == 0b001110 and bits(w, 23, 21) == 0 and not bits(w, 15, 15) and bits(w, 11, 10) == 0:
        return tbl(w)
    if vec and b2824 == 0b01110 and bits(w, 21, 21) and bits(w, 11, 10) == 0:
        return three_diff(w)
    if scalar and bits(w, 28, 24) == 0b11110 and bits(w, 21, 17) == 0b11000 and bits(w, 11, 10) == 2 and bits(w, 16, 12) == 0b11011:
        return f'wv64(c, {bits(w, 4, 0)}, c->v[{bits(w, 9, 5)}].d[0] + c->v[{bits(w, 9, 5)}].d[1]);'
    if scalar and bits(w, 28, 24) == 0b11110 and bits(w, 21, 17) == 0b11000 and bits(w, 11, 10) == 2 and bits(w, 29, 29):
        op, sz = bits(w, 16, 12), bits(w, 22, 22)
        F = 'df' if sz else 'f'
        n, d = bits(w, 9, 5), bits(w, 4, 0)
        if op == 0b01101 and not bits(w, 23, 23):
            return f'wv{"D" if sz else "S"}(c, {d}, c->v[{n}].{F}[0] + c->v[{n}].{F}[1]);'
        f = {(0b01100, 0): 'fmaxnm', (0b01100, 1): 'fminnm', (0b01111, 0): 'fmaxa', (0b01111, 1): 'fmina'}.get((op, bits(w, 23, 23)))
        if f:
            return f'wv{"D" if sz else "S"}(c, {d}, {f}(c->v[{n}].{F}[0], c->v[{n}].{F}[1]));'
    raise Unimpl('simd')


def mnemonics(words):
    if not words:
        return {}
    text = '\n'.join(' '.join(f'0x{b:02x}' for b in w.to_bytes(4, 'little')) for w in words)
    out = subprocess.run(['llvm-mc', '--disassemble', '--show-encoding', '-triple=aarch64',
                          '-mattr=+v8a,+fp-armv8,+neon,+crc,+crypto'],
                         input=text, capture_output=True, text=True).stdout
    names = {}
    for l in out.splitlines():
        if 'encoding: [' not in l:
            continue
        enc = l.split('encoding: [')[1].split(']')[0].split(',')
        ins = l.split('//')[0].split()
        arr = next((t.split('.')[-1].rstrip(',}') for t in ins[1:] if '.' in t and t.startswith(('v', '{v'))), '')
        names[int.from_bytes(bytes(int(b, 16) for b in enc), 'little')] = ins[0] + (f'.{arr}' if arr else '')
    return names


def write_if_changed(path, text):
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def layout():
    bases, b = {}, BASE0
    nsos = {m: Nso(f'extracted/nso/{m}.raw') for m in MODULES}
    for m in MODULES:
        bases[m] = b
        b = (bases[m] + bss_end(nsos[m]) + ALIGN - 1) // ALIGN * ALIGN
    return nsos, bases


def bss_end(n):
    m = struct.unpack_from('<I', n.img, 4)[0]
    return m + struct.unpack_from('<i', n.img, m + 12)[0]


def load_map(mod):
    with open(f'build/funcmap/{mod}.csv') as f:
        return [(int(r['addr'], 16), int(r['size'])) for r in csv.DictReader(f)]


def lift_module(mod, n, base, write):
    fmap = [(a + base, s) for a, s in load_map(mod)]
    funcs = {a for a, _ in fmap}
    out = Path(f'build/lifted/{mod}')
    if write:
        out.mkdir(parents=True, exist_ok=True)
    stats = collections.Counter()
    unimpl_words = collections.Counter()
    shard, shard_no = [], 0
    starts = sorted(funcs)
    for idx, (fa, size) in enumerate(fmap):
        cx = Ctx(mod, fa, size, funcs, n.img, base)
        cx.unresolved_br = 0
        body = []
        cx.skip = None
        for pc in range(fa, fa + size, 4):
            w = struct.unpack_from('<I', n.img, pc - base)[0]
            if pc == cx.skip:
                continue
            stats['insn'] += 1
            try:
                s = lift(w, pc, cx)
            except Unimpl as e:
                stats['unimpl'] += 1
                stats['u:' + e.args[0]] += 1
                unimpl_words[w] += 1
                s = f'unimpl(c, {lit(pc)}, {w:#x}u);'
            body.append((pc, s))
        stats['br_unresolved'] += cx.unresolved_br
        nxt = fa + size
        tail = f'f_{nxt:08x}(c);' if nxt in funcs else ''
        if write:
            lines = [f'void f_{fa:08x}(CPU *c) {{']
            if fa in TRACE:
                lines.append(f'  trace_enter(c, {lit(fa)});')
                body = [(pc, st.replace('return;', f'{{ trace_exit(c, {lit(fa)}); return; }}')) for pc, st in body]
            for pc, s in body:
                if pc in cx.labels:
                    lines.append(f'L_{pc:x}:')
                lines.append(f'  {s}')
            lines.append(f'  {tail}')
            lines.append('}')
            shard.append('\n'.join(lines))
            if len(shard) == SHARD or idx == len(fmap) - 1:
                write_if_changed(out / f's{shard_no:04d}.c', '#include "funcs.h"\n\n' + '\n\n'.join(shard) + '\n')
                shard, shard_no = [], shard_no + 1
    if write:
        write_if_changed(out / 'funcs.h', '#include "cpu.h"\n' + ''.join(f'void f_{a:08x}(CPU *c);\n' for a in starts))
        write_if_changed(out / 'table.c', 
            '#include "funcs.h"\n#include "table.h"\n'
            f'const FuncEntry {mod}_funcs[] = {{\n' + ''.join(f'  {{{a:#x}ull, f_{a:08x}}},\n' for a in starts) + '};\n'
            f'const unsigned long {mod}_nfuncs = {len(starts)};\n')
    return stats, unimpl_words


def main():
    write = '--stats' not in sys.argv
    nsos, bases = layout()
    total_u = collections.Counter()
    for m in MODULES:
        stats, uw = lift_module(m, nsos[m], bases[m], write)
        total_u.update(uw)
        pct = 100 * (1 - stats['unimpl'] / stats['insn'])
        cats = ', '.join(f'{k[2:]}={v}' for k, v in stats.most_common() if k.startswith('u:'))
        print(f'{m} @ {bases[m]:#x}: {stats["insn"]} instr, {pct:.2f}% traduzidas, BR sem tabela={stats["br_unresolved"]} | {cats}')
    names = mnemonics(list(total_u))
    byname = collections.Counter()
    for w, k in total_u.items():
        byname[names.get(w, '?')] += k
    print('top não implementadas:', ', '.join(f'{k}={v}' for k, v in byname.most_common(40)))
    if write:
        write_if_changed(Path('build/lifted/layout.h'), ''.join(f'#define BASE_{m.upper()} {bases[m]:#x}ull\n' for m in MODULES))


if __name__ == '__main__':
    main()
