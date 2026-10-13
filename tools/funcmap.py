#!/usr/bin/env python3
import bisect
import csv
import struct
import sys
from pathlib import Path


def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def u64(b, o): return struct.unpack_from('<Q', b, o)[0]


def uleb(b, o):
    r = s = 0
    while True:
        c = b[o]; o += 1
        r |= (c & 0x7f) << s; s += 7
        if c < 0x80: return r, o


def sleb(b, o):
    r = s = 0
    while True:
        c = b[o]; o += 1
        r |= (c & 0x7f) << s; s += 7
        if c < 0x80:
            if c & 0x40: r -= 1 << s
            return r, o


def read_enc(b, o, enc, datarel=0):
    if enc == 0xff: return None, o
    fmt = enc & 0x0f
    size, signed = {0x0: (8, False), 0x1: (0, False), 0x2: (2, False), 0x3: (4, False), 0x4: (8, False),
                    0x9: (0, True), 0xa: (2, True), 0xb: (4, True), 0xc: (8, True)}[fmt]
    if size == 0:
        v, n = (sleb if signed else uleb)(b, o)
    else:
        v = int.from_bytes(b[o:o + size], 'little', signed=signed); n = o + size
    app = enc & 0x70
    if app == 0x10: v += o
    elif app == 0x30: v += datarel
    elif app not in (0x00,): raise ValueError(f'enc {enc:#x}')
    return v, n


class Nso:
    def __init__(self, path):
        d = Path(path).read_bytes()
        assert d[:4] == b'NSO0', path
        self.segs = []
        end = 0
        for i in range(3):
            foff, dst, size, _ = struct.unpack_from('<4I', d, 0x10 + i * 16)
            self.segs.append((dst, size, foff))
            end = max(end, dst + size)
        self.img = bytearray(end)
        for dst, size, foff in self.segs:
            self.img[dst:dst + size] = d[foff:foff + size]
        self.text = (self.segs[0][0], self.segs[0][0] + self.segs[0][1])
        ro = self.segs[1][0]
        stro, strsz, symo, symsz = struct.unpack_from('<4I', d, 0x90)
        self.dynstr = (ro + stro, strsz)
        self.dynsym = (ro + symo, symsz)
        m = u32(self.img, 4)
        assert self.img[m:m + 4] == b'MOD0'
        self.ehf = (m + struct.unpack_from('<i', self.img, m + 16)[0], m + struct.unpack_from('<i', self.img, m + 20)[0])

    def in_text(self, a): return self.text[0] <= a < self.text[1]


def eh_frame_funcs(n):
    b, hdr = n.img, n.ehf[0]
    if n.ehf[0] == n.ehf[1]: return {}
    assert b[hdr] == 1
    pe, ce, te = b[hdr + 1], b[hdr + 2], b[hdr + 3]
    _, o = read_enc(b, hdr + 4, pe)
    count, o = read_enc(b, o, ce)
    cies, out = {}, {}
    for _ in range(count):
        _, o = read_enc(b, o, te, hdr)
        fde, o = read_enc(b, o, te, hdr)
        length = u32(b, fde); p = fde + 4
        cie = p - u32(b, p); p += 4
        if cie not in cies: cies[cie] = parse_cie(b, cie)
        R = cies[cie]
        start, p = read_enc(b, p, R)
        size, _ = read_enc(b, p, R & 0x0f)
        out[start] = size
    return out


def parse_cie(b, o):
    o += 8
    ver = b[o]; o += 1
    z = o; o = b.index(0, o); aug = b[z:o].decode(); o += 1
    _, o = uleb(b, o); _, o = sleb(b, o)
    if ver == 1: o += 1
    else: _, o = uleb(b, o)
    R = 0
    if aug.startswith('z'):
        _, o = uleb(b, o)
        for c in aug[1:]:
            if c == 'R': R = b[o]; o += 1
            elif c == 'L': o += 1
            elif c == 'P': e = b[o]; _, o = read_enc(b, o + 1, e)
            elif c in 'SB': pass
            else: raise ValueError(aug)
    return R


def dyn_funcs(n):
    b = n.img
    so, ssz = n.dynsym
    sto, _ = n.dynstr
    out = {}
    for o in range(so, so + ssz, 24):
        name, info, _, shndx, val, size = struct.unpack_from('<IBBHQQ', b, o)
        if info & 0xf == 2 and shndx and n.in_text(val):
            end = b.index(0, sto + name)
            out[val] = (size, b[sto + name:end].decode())
    return out


def bl_targets(n):
    b, (lo, hi) = n.img, n.text
    out = set()
    for pc in range(lo, hi, 4):
        w = u32(b, pc)
        if w & 0xfc000000 == 0x94000000:
            imm = w & 0x3ffffff
            if imm & 0x2000000: imm -= 1 << 26
            t = pc + imm * 4
            if n.in_text(t) and t != pc + 8: out.add(t)
            if t == pc + 8:
                p = pc + 4 + struct.unpack_from('<i', b, pc + 4)[0]
                if n.in_text(p) and not p & 3: out.add(p)
    return out


def reloc_targets(n, eh):
    b = n.img
    m = u32(b, 4)
    o = m + struct.unpack_from('<i', b, m + 4)[0]
    tags = {}
    while True:
        t, v = struct.unpack_from('<qQ', b, o)
        o += 16
        if t == 0:
            break
        tags[t] = v
    ehs = sorted(eh)
    out = set()
    for off, sz in ((tags.get(7), tags.get(8)), (tags.get(23), tags.get(2))):
        for r in range(off or 0, (off or 0) + (sz or 0), 24):
            ro, info, add = struct.unpack_from('<QQq', b, r)
            t = info & 0xffffffff
            v = add if t == 1027 else u64(b, ro) if t == 1026 else None
            if v is None or not n.in_text(v) or v & 3:
                continue
            i = bisect.bisect_right(ehs, v) - 1
            if i >= 0 and ehs[i] < v < ehs[i] + eh[ehs[i]]:
                continue
            out.add(v)
    return out


def tail_targets(n, starts):
    b, (lo, hi) = n.img, n.text
    out = set()
    for pc in range(lo, hi, 4):
        w = u32(b, pc)
        if w & 0xfc000000 != 0x14000000:
            continue
        imm = w & 0x3ffffff
        if imm & 0x2000000: imm -= 1 << 26
        t = pc + imm * 4
        if not n.in_text(t):
            continue
        f = bisect.bisect_right(starts, pc) - 1
        g = bisect.bisect_right(starts, t) - 1
        if f != g:
            out.add(t)
    return out


def build_map(path):
    n = Nso(path)
    eh, dyn, bl = eh_frame_funcs(n), dyn_funcs(n), bl_targets(n)
    rel = reloc_targets(n, eh)
    starts = sorted(set(eh) | set(dyn) | bl | rel | {n.text[0]})
    tail = tail_targets(n, starts) - set(starts)
    starts = sorted(set(starts) | tail)
    rows = []
    for i, a in enumerate(starts):
        nxt = starts[i + 1] if i + 1 < len(starts) else n.text[1]
        size = eh.get(a) or (dyn[a][0] if a in dyn else 0) or nxt - a
        src = ''.join(c for c, s in (('e', eh), ('s', dyn), ('b', bl), ('r', rel), ('t', tail)) if a in s) or 'x'
        rows.append((a, size, dyn[a][1] if a in dyn else f'sub_{a:08x}', src))
    return n, eh, dyn, bl, rows


def check(n, eh, dyn, rows):
    assert all(n.in_text(a) and n.in_text(a + s - 1) for a, s in eh.items()), 'eh_frame fora de .text'
    both = [a for a in dyn if a in eh]
    bad = [a for a in both if dyn[a][0] and dyn[a][0] != eh[a]]
    assert len(bad) <= len(both) * 0.01, f'{len(bad)} tamanhos divergentes symtab vs eh_frame'
    starts = [r[0] for r in rows]
    assert starts == sorted(set(starts))
    covered, last = 0, 0
    for a, s, *_ in rows:
        covered += max(0, a + s - max(a, last)); last = max(last, a + s)
    return covered / (n.text[1] - n.text[0])


def main():
    out = Path('build/funcmap'); out.mkdir(parents=True, exist_ok=True)
    for path in sys.argv[1:] or sorted(Path('extracted/nso').glob('*.raw')):
        n, eh, dyn, bl, rows = build_map(path)
        cov = check(n, eh, dyn, rows)
        name = Path(path).stem
        with open(out / f'{name}.csv', 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['addr', 'size', 'name', 'src'])
            for a, s, nm, src in rows:
                w.writerow([f'{a:#010x}', s, nm, src])
        only_bl = sum(1 for r in rows if r[3] == 'b')
        print(f'{name}: {len(rows)} funções | eh_frame {len(eh)} | símbolos {len(dyn)} | só-BL {only_bl} | cobertura .text {cov:.1%}')


if __name__ == '__main__':
    main()
