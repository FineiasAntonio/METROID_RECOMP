#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nv.h"
#include "maxwell.h"
#include "render.h"

enum { CLASS_GPFIFO = 0xb06f, CLASS_3D = 0xb197, CLASS_COMPUTE = 0xb1c0, CLASS_I2M = 0xa140, CLASS_DMA = 0xb0b5, CLASS_2D = 0x902d };

typedef struct {
    uint32_t cls;
    uint32_t regs[0x1000];
    uint32_t i2m_left, i2m_off;
    uint64_t i2m_dst;
} Engine;

static Engine sub[8];
static uint32_t host_regs[0x40];
static uint64_t submits, methods, sem_releases;

void *gpu_ptr(uint64_t va) {
    for (int i = ngpu_maps - 1; i >= 0; i--) {
        GpuMapping *m = &gpu_maps[i];
        if (va >= m->gpu_va && va < m->gpu_va + m->size) return (void *)(uintptr_t)(m->cpu_addr + (va - m->gpu_va));
    }
    return NULL;
}

static uint64_t gpu_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec) * 384 / 625;
}

static void release(uint64_t va, uint32_t payload, int four_words) {
    uint32_t *p = gpu_ptr(va);
    if (!p) {
        if (getenv("GPU_TRACE")) fprintf(stderr, "[gpu] semáforo em VA não mapeado %#lx\n", va);
        return;
    }
    if (four_words) {
        uint64_t t = gpu_ticks();
        memcpy(p + 2, &t, 8);
        p[1] = 0;
    }
    __atomic_store_n(p, payload, __ATOMIC_SEQ_CST);
    sem_releases++;
}

static uint64_t addr2(uint32_t hi, uint32_t lo) { return (uint64_t)(hi & 0xff) << 32 | lo; }

static void i2m_data(Engine *e, uint32_t v) {
    if (!e->i2m_left) return;
    uint8_t *p = gpu_ptr(e->i2m_dst + e->i2m_off);
    uint32_t n = e->i2m_left < 4 ? e->i2m_left : 4;
    if (p) memcpy(p, &v, n);
    e->i2m_off += n;
    e->i2m_left -= n;
}

static void dma_launch(Engine *e, uint32_t v) {
    uint32_t *r = e->regs;
    uint64_t src = addr2(r[0x100], r[0x101]), dst = addr2(r[0x102], r[0x103]);
    uint32_t len = r[0x106], lines = r[0x107], pitch_in = r[0x104], pitch_out = r[0x105];
    int src_linear = v >> 7 & 1, dst_linear = v >> 8 & 1, multi = v >> 9 & 1;
    if (src_linear && dst_linear) {
        if (!multi) lines = 1;
        for (uint32_t l = 0; l < lines; l++) {
            uint8_t *s = gpu_ptr(src + (uint64_t)l * pitch_in), *d = gpu_ptr(dst + (uint64_t)l * pitch_out);
            if (s && d) memmove(d, s, len);
        }
    }
    uint32_t sem = v >> 3 & 3;
    if (sem == 1 || sem == 2) release(addr2(r[0x90], r[0x91]), r[0x92], sem == 2);
}

static uint64_t hist[0x10000];
static uint32_t macro_mem[0x2000], macro_pos[0x80];
static uint32_t macro_params[0x4000], nparams, macro_running = ~0u;
uint64_t stat_draws, stat_clears, stat_macros;
static CbBind cb_binds[5][18];
static int draw_indexed;

static void method(int sc, uint32_t m, uint32_t v);

typedef struct {
    uint32_t r[8], pc, method, inc, param_idx, nparams;
    const uint32_t *params;
    int carry, sc;
    int64_t delayed;
} Mme;

static void mme_send(Mme *x, uint32_t v) {
    method(x->sc, x->method, v);
    x->method = (x->method + x->inc) & 0xfff;
}
static uint32_t mme_fetch(Mme *x) { return x->param_idx < x->nparams ? x->params[x->param_idx++] : 0; }
static void mme_setm(Mme *x, uint32_t a) { x->method = a & 0xfff; x->inc = a >> 12 & 0x3f; }
static void mme_set(Mme *x, uint32_t reg, uint32_t v) { if (reg) x->r[reg] = v; }

static void mme_result(Mme *x, uint32_t op, uint32_t reg, uint32_t v) {
    switch (op) {
    case 0: mme_set(x, reg, mme_fetch(x)); break;
    case 1: mme_set(x, reg, v); break;
    case 2: mme_set(x, reg, v); mme_setm(x, v); break;
    case 3: mme_set(x, reg, mme_fetch(x)); mme_send(x, v); break;
    case 4: mme_set(x, reg, v); mme_send(x, v); break;
    case 5: mme_set(x, reg, mme_fetch(x)); mme_setm(x, v); break;
    case 6: mme_set(x, reg, v); mme_setm(x, v); mme_send(x, mme_fetch(x)); break;
    case 7: mme_set(x, reg, v); mme_setm(x, v & 0xfff); mme_send(x, v >> 12 & 0x3f); break;
    }
}

static uint32_t mme_alu(Mme *x, uint32_t op, uint32_t a, uint32_t b) {
    uint64_t r;
    switch (op) {
    case 0: r = (uint64_t)a + b; x->carry = r >> 32 & 1; return (uint32_t)r;
    case 1: r = (uint64_t)a + b + (x->carry ? 1 : 0); x->carry = r >> 32 & 1; return (uint32_t)r;
    case 2: r = (uint64_t)a - b; x->carry = r < 0x100000000ull; return (uint32_t)r;
    case 3: r = (uint64_t)a - b - (x->carry ? 0 : 1); x->carry = r < 0x100000000ull; return (uint32_t)r;
    case 8: return a ^ b;
    case 9: return a | b;
    case 10: return a & b;
    case 11: return a & ~b;
    case 12: return ~(a & b);
    }
    fprintf(stderr, "[mme] alu op %u inválida\n", op);
    return 0;
}

static int mme_step(Mme *x, int delay) {
    uint32_t base = x->pc;
    uint32_t op = macro_mem[(x->pc / 4) & 0x1fff];
    x->pc += 4;
    if (x->delayed >= 0) { x->pc = (uint32_t)x->delayed; x->delayed = -1; }
    uint32_t kind = op & 7, res = op >> 4 & 7, dst = op >> 8 & 7, ra = op >> 11 & 7, rb = op >> 14 & 7;
    int32_t imm = (int32_t)op >> 14;
    uint32_t bsrc = op >> 17 & 31, bsize = op >> 22 & 31, bdst = op >> 27 & 31, mask = (uint32_t)((1ull << bsize) - 1);
    uint32_t A = x->r[ra], B = x->r[rb];
    switch (kind) {
    case 0: mme_result(x, res, dst, mme_alu(x, op >> 17 & 31, A, B)); break;
    case 1: mme_result(x, res, dst, A + (uint32_t)imm); break;
    case 2: mme_result(x, res, dst, (A & ~(mask << bdst)) | (((B >> bsrc) & mask) << bdst)); break;
    case 3: mme_result(x, res, dst, ((B >> (A & 31)) & mask) << bdst); break;
    case 4: mme_result(x, res, dst, ((B >> bsrc) & mask) << (A & 31)); break;
    case 5: mme_result(x, res, dst, sub[x->sc].regs[(A + (uint32_t)imm) & 0xfff]); break;
    case 7: {
        int taken = (op >> 4 & 1) ? A != 0 : A == 0;
        if (taken) {
            uint32_t target = base + (uint32_t)(imm * 4);
            if (op >> 5 & 1) { x->pc = target; return 1; }
            x->delayed = target;
            return mme_step(x, 1);
        }
        break;
    }
    }
    if (op >> 7 & 1 && !delay) {
        mme_step(x, 1);
        return 0;
    }
    return 1;
}

static void mme_run(int sc, uint32_t idx, const uint32_t *params, uint32_t n) {
    Mme x = {.pc = macro_pos[idx & 0x7f] * 4, .params = params, .nparams = n, .param_idx = 1, .sc = sc, .delayed = -1};
    x.r[1] = n ? params[0] : 0;
    stat_macros++;
    for (int guard = 0; guard < 1 << 20 && mme_step(&x, 0); guard++)
        ;
}

static void method(int sc, uint32_t m, uint32_t v) {
    Engine *e = &sub[sc];
    methods++;
    if (m >= 0xe00) return;
    hist[(e->cls == CLASS_3D ? 0 : e->cls == CLASS_COMPUTE ? 0x2000 : e->cls == CLASS_DMA ? 0x4000 : e->cls == CLASS_2D ? 0x6000 : 0x8000) + (m & 0x1fff)]++;
    if (m < 0x40) {
        host_regs[m] = v;
        if (m == 0) { e->cls = v & 0xffff; return; }
        if (m == 0x07) {
            uint32_t op = v & 0x1f;
            if (op == 2) release(addr2(host_regs[4], host_regs[5]), host_regs[6], !(v >> 24 & 1));
        }
        return;
    }
    if (m < 0x1000) e->regs[m] = v;
    if (e->cls == CLASS_3D) {
        if (m == 0x46) macro_mem[e->regs[0x45]++ & 0x1fff] = v;
        else if (m == 0x48) macro_pos[e->regs[0x47]++ & 0x7f] = v;
        else if (m == 0x585) { stat_draws++; if (render_active()) maxwell_draw(e->regs, gpu_ptr, cb_binds, draw_indexed); }
        else if (m == 0x5f8) draw_indexed = 1;
        else if (m == 0x35e) draw_indexed = 0;
        else if (m >= 0x8e4 && m < 0x8f4) {
            uint64_t a = addr2(e->regs[0x8e1], e->regs[0x8e2]) + e->regs[0x8e3];
            uint32_t *p = gpu_ptr(a);
            if (p) *p = v;
            e->regs[0x8e3] += 4;
        } else if (m >= 0x904 && m < 0x904 + 5 * 8 && (m - 0x904) % 8 == 0) {
            uint32_t st = (m - 0x904) / 8, idx = v >> 4 & 0x1f;
            if (idx < 18) {
                if (v & 1) cb_binds[st][idx] = (CbBind){addr2(e->regs[0x8e1], e->regs[0x8e2]), e->regs[0x8e0]};
                else cb_binds[st][idx] = (CbBind){0, 0};
            }
        }
        else if (m == 0x674) { stat_clears++; if (render_active()) maxwell_clear(e->regs, v, gpu_ptr); }
    }
    switch (e->cls) {
    case CLASS_3D:
    case CLASS_COMPUTE:
    case CLASS_I2M:
        if (m == 0x6c) {
            e->i2m_dst = addr2(e->regs[0x62], e->regs[0x63]);
            e->i2m_left = e->regs[0x60] * (e->regs[0x61] ? e->regs[0x61] : 1);
            e->i2m_off = 0;
        } else if (m == 0x6d) i2m_data(e, v);
        else if (m == 0x6c3 && e->cls != CLASS_I2M) {
            uint32_t op = v & 3;
            if (op == 0 || op == 2) release(addr2(e->regs[0x6c0], e->regs[0x6c1]), op == 0 ? e->regs[0x6c2] : 0, !(v >> 28 & 1));
        }
        break;
    case CLASS_DMA:
        if (m == 0xc0) dma_launch(e, v);
        break;
    }
}

static void call(int sc, uint32_t m, uint32_t v, int last) {
    if (m >= 0xe00 && sub[sc].cls == CLASS_3D) {
        hist[m & 0x1fff]++;
        if (!(m & 1) || macro_running != m >> 1) { macro_running = m >> 1; nparams = 0; }
        if (nparams < 0x4000) macro_params[nparams++] = v;
        if (last) { mme_run(sc, (m - 0xe00) >> 1, macro_params, nparams); nparams = 0; macro_running = ~0u; }
        return;
    }
    method(sc, m, v);
}

static void run_pushbuffer(const uint32_t *w, uint32_t n) {
    for (uint32_t i = 0; i < n;) {
        uint32_t h = w[i++];
        uint32_t m = h & 0x1fff, sc = h >> 13 & 7, count = h >> 16 & 0x1fff, mode = h >> 29;
        switch (mode) {
        case 4: call(sc, m, count, 1); break;
        case 1: case 0:
            for (uint32_t k = 0; k < count && i < n; k++) call(sc, m + k, w[i++], k + 1 == count);
            break;
        case 3: case 2:
            for (uint32_t k = 0; k < count && i < n; k++) call(sc, m, w[i++], k + 1 == count);
            break;
        case 5:
            for (uint32_t k = 0; k < count && i < n; k++) call(sc, k ? m + 1 : m, w[i++], k + 1 == count);
            break;
        default:
            break;
        }
    }
}

typedef struct {
    int syncpt;
    const uint64_t *e;
    uint32_t n;
} Submit;

static void submit_now(void *p);

void gpu_submit(int channel_syncpt, const uint64_t *e, uint32_t n) {
    Submit s = {channel_syncpt, e, n};
    render_run(submit_now, &s);
}

static void submit_now(void *p) {
    Submit *sb = p;
    int channel_syncpt = sb->syncpt;
    const uint64_t *e = sb->e;
    uint32_t n = sb->n;
    submits++;
    for (uint32_t k = 0; k < n; k++) {
        uint64_t va = e[k] & 0xfffffffffcull;
        uint32_t len = (uint32_t)(e[k] >> 42 & 0x1fffff);
        if (!len) continue;
        const uint32_t *pb = gpu_ptr(va);
        if (!pb) {
            if (getenv("GPU_TRACE")) fprintf(stderr, "[gpu] pushbuffer em VA não mapeado %#lx\n", va);
            continue;
        }
        run_pushbuffer(pb, len);
    }
    if (getenv("GPU_TRACE") && !(submits & (submits - 1)))
        fprintf(stderr, "[gpu] %lu submissões, %lu métodos, %lu semáforos (syncpt %d)\n", submits, methods, sem_releases, channel_syncpt);
}

void gpu_dump_hist(void) {
    const char *names[] = {"3d", "compute", "dma", "2d", "outro"};
    for (int i = 0; i < 0xa000; i++)
        if (hist[i]) fprintf(stderr, "[hist] %s %#x %lu\n", names[i >> 13], i & 0x1fff, hist[i]);
}
