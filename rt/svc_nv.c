#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "ipc.h"
#include "nv.h"

#define SVC(name, id) static void name##_h(Session *s, void *st, Request *rq, Response *rs); \
    const Service name = {id, name##_h}; \
    static void name##_h(Session *s, void *st, Request *rq, Response *rs)

enum { NV_OK = 0, NV_NOT_SUPPORTED = 2, NV_BAD_PARAM = 4, NV_CONFIG_NOT_FOUND = 0x30006 };
enum { DEV_NONE, DEV_NVMAP, DEV_CTRL, DEV_CTRL_GPU, DEV_AS, DEV_GPU, DEV_OTHER_CHANNEL, DEV_OTHER };

typedef struct {
    int type, syncpt;
    KObject *events[8];
} NvFd;

static NvFd fds[256];
NvMapHandle nvmap[4096];
static int nvmap_next = 1;
GpuMapping gpu_maps[65536];
int ngpu_maps;
static uint64_t gpu_va_next = 0x400000000ull;
static uint32_t syncpt_max[192];
static int syncpt_next = 20;
static KObject *ctrl_events[64];

uint32_t syncpt_value(int id) { return __atomic_load_n(&syncpt_max[id], __ATOMIC_SEQ_CST); }

static const struct { const char *path; int type; } devices[] = {
    {"/dev/nvmap", DEV_NVMAP}, {"/dev/nvhost-ctrl", DEV_CTRL}, {"/dev/nvhost-ctrl-gpu", DEV_CTRL_GPU},
    {"/dev/nvhost-as-gpu", DEV_AS}, {"/dev/nvhost-gpu", DEV_GPU}, {"/dev/nvhost-nvdec", DEV_OTHER_CHANNEL},
    {"/dev/nvhost-vic", DEV_OTHER_CHANNEL}, {"/dev/nvhost-nvjpg", DEV_OTHER_CHANNEL}, {"/dev/nvhost-dbg-gpu", DEV_OTHER},
    {"/dev/nvhost-prof-gpu", DEV_OTHER}, {"/dev/nvhost-display", DEV_OTHER_CHANNEL},
};

static void bad_ioctl(int type, uint32_t cmd) {
    fprintf(stderr, "[nv] ioctl %#x não implementado (dispositivo %d, nr %#x, tamanho %#x)\n", cmd, type, cmd & 0xff, cmd >> 16 & 0x3fff);
    abort();
}

static uint64_t gpu_va_alloc(uint64_t size, uint64_t align) {
    if (align < 0x20000) align = 0x20000;
    uint64_t va = (gpu_va_next + align - 1) & ~(align - 1);
    gpu_va_next = va + ((size + 0xffff) & ~0xffffull);
    return va;
}

static uint32_t ioctl_nvmap(uint32_t cmd, uint8_t *d) {
    uint32_t *w = (uint32_t *)d;
    switch (cmd & 0xff) {
    case 0x01: {
        int h = nvmap_next++;
        nvmap[h] = (NvMapHandle){.size = w[0], .id = (uint32_t)h};
        w[1] = (uint32_t)h;
        return NV_OK;
    }
    case 0x03:
        w[1] = w[0];
        return NV_OK;
    case 0x04: {
        NvMapHandle *m = &nvmap[w[0]];
        m->align = w[3];
        m->kind = d[0x10];
        m->addr = *(uint64_t *)(d + 0x18);
        return NV_OK;
    }
    case 0x05: {
        NvMapHandle *m = &nvmap[w[0]];
        *(uint64_t *)(d + 8) = m->addr;
        w[4] = (uint32_t)m->size;
        w[5] = 0;
        return NV_OK;
    }
    case 0x09: {
        NvMapHandle *m = &nvmap[w[0]];
        uint32_t v[] = {0, (uint32_t)m->size, m->align, (uint32_t)m->addr, 0x40000000, m->kind, 0};
        w[2] = w[1] < 7 ? v[w[1]] : 0;
        return NV_OK;
    }
    case 0x0e:
        w[0] = nvmap[w[1]].id;
        return NV_OK;
    }
    bad_ioctl(DEV_NVMAP, cmd);
    return 0;
}

static uint32_t ioctl_ctrl(uint32_t cmd, uint8_t *d, NvFd *f) {
    uint32_t *w = (uint32_t *)d;
    (void)f;
    switch (cmd & 0xff) {
    case 0x14: w[1] = syncpt_value((int)w[0]); return NV_OK;
    case 0x1b: return NV_CONFIG_NOT_FOUND;
    case 0x1c: case 0x1d: case 0x1e:
        w[3] = syncpt_value((int)w[0]);
        return NV_OK;
    case 0x1f: case 0x20: case 0x21: return NV_OK;
    }
    bad_ioctl(DEV_CTRL, cmd);
    return 0;
}

static uint32_t ioctl_ctrl_gpu(uint32_t cmd, uint8_t *d) {
    uint32_t *w = (uint32_t *)d;
    switch (cmd & 0xff) {
    case 0x01: w[0] = 0x1; return NV_OK;
    case 0x02: {
        uint32_t z[] = {0x20, 0x20, 0x400, 0x800, 0x20, 0x20, 0xc0, 0x20, 0x40, 0x10};
        memcpy(d, z, sizeof z);
        return NV_OK;
    }
    case 0x03: case 0x04: case 0x0d: return NV_OK;
    case 0x05: {
        uint8_t *c = d + 0x10;
        memset(c, 0, 0xa0);
        uint32_t *u = (uint32_t *)c;
        u[0] = 0x120; u[1] = 0xb; u[2] = 0xa1; u[3] = 1;
        *(uint64_t *)(c + 0x10) = 0x40000;
        u[8] = 2; u[9] = 0x20; u[10] = 0x20000; u[11] = 0x20000; u[12] = 0x1b; u[13] = 0x30000; u[14] = 1;
        u[15] = 0x503; u[16] = 0x503; u[17] = 0x80; u[18] = 0x28;
        *(uint64_t *)(c + 0x50) = 0x55;
        u[22] = 0x902d; u[23] = 0xb197; u[24] = 0xb1c0; u[25] = 0xb06f; u[26] = 0xa140; u[27] = 0xb0b5;
        u[28] = 1; u[30] = 2; u[31] = 1; u[33] = 1; u[34] = 0x21d70;
        memcpy(c + 0x90, "gm20b", 6);
        *(uint64_t *)d = 0xa0;
        return NV_OK;
    }
    case 0x06: w[4] = 3; return NV_OK;
    case 0x14: w[0] = 7; w[1] = 1; return NV_OK;
    case 0x1c: {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        *(uint64_t *)d = (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
        return NV_OK;
    }
    }
    bad_ioctl(DEV_CTRL_GPU, cmd);
    return 0;
}

static void gpu_map(uint64_t va, uint32_t handle, uint64_t off, uint64_t size) {
    NvMapHandle *m = &nvmap[handle];
    if (!size) size = m->size - off;
    gpu_maps[ngpu_maps++] = (GpuMapping){va, m->addr + off, size};
}

static uint32_t ioctl_as(uint32_t cmd, uint8_t *d) {
    uint32_t *w = (uint32_t *)d;
    switch (cmd & 0xff) {
    case 0x01: case 0x03: case 0x05: case 0x09: return NV_OK;
    case 0x02: {
        uint64_t size = (uint64_t)w[0] * w[1];
        if (!(w[2] & 1)) *(uint64_t *)(d + 0x10) = gpu_va_alloc(size, w[1]);
        return NV_OK;
    }
    case 0x06: {
        uint32_t flags = w[0], handle = w[2];
        uint64_t off = *(uint64_t *)(d + 0x10), size = *(uint64_t *)(d + 0x18);
        uint64_t *va = (uint64_t *)(d + 0x20);
        if (flags & 0x100) return NV_OK;
        if (!(flags & 1)) *va = gpu_va_alloc(size ? size : nvmap[handle].size, w[3]);
        gpu_map(*va, handle, off, size);
        return NV_OK;
    }
    case 0x08: {
        uint8_t *r = d + 0x10;
        memset(r, 0, 0x30);
        *(uint64_t *)r = 0x4000000; *(uint32_t *)(r + 8) = 0x1000; *(uint64_t *)(r + 0x10) = 0x3fbfff;
        *(uint64_t *)(r + 0x18) = 0x400000000ull; *(uint32_t *)(r + 0x20) = 0x20000; *(uint64_t *)(r + 0x28) = 0x1bffff;
        return NV_OK;
    }
    case 0x14: {
        uint32_t n = (cmd >> 16 & 0x3fff) / 0x14;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t *e = (uint32_t *)(d + i * 0x14);
            uint32_t handle = e[1];
            if (!handle) continue;
            uint64_t va = (uint64_t)e[3] << 16, off = (uint64_t)e[2] << 16, size = (uint64_t)e[4] << 16;
            gpu_map(va, handle, off, size);
        }
        return NV_OK;
    }
    }
    bad_ioctl(DEV_AS, cmd);
    return 0;
}

static uint32_t ioctl_gpu(uint32_t cmd, uint8_t *d, NvFd *f, Buf extra) {
    uint32_t *w = (uint32_t *)d;
    switch (cmd & 0xff) {
    case 0x01: case 0x03: case 0x0b: case 0x0c: case 0x0d: case 0x12: case 0x14: case 0x1d: return NV_OK;
    case 0x15: *(uint64_t *)d = 0; return NV_OK;
    case 0x16: memset(d, 0, cmd >> 16 & 0x3fff); return NV_OK;
    case 0x08: case 0x1b: {
        uint32_t n = w[2], flags = w[3];
        uint64_t *entries = extra.addr ? (uint64_t *)extra.addr : (uint64_t *)(d + 0x18);
        gpu_submit(f->syncpt, entries, n);
        uint32_t v = __atomic_add_fetch(&syncpt_max[f->syncpt], 1, __ATOMIC_SEQ_CST);
        if (flags & 2) { w[4] = (uint32_t)f->syncpt; w[5] = v; }
        return NV_OK;
    }
    case 0x09: *(uint64_t *)(d + 8) = w[0]; return NV_OK;
    case 0x1a:
        w[6] = (uint32_t)f->syncpt;
        w[7] = syncpt_value(f->syncpt);
        return NV_OK;
    }
    bad_ioctl(DEV_GPU, cmd);
    return 0;
}

static uint32_t mm_iova_next = 0x10000000;

static uint32_t ioctl_mm_channel(uint32_t cmd, uint8_t *d, NvFd *f, Buf extra) {
    uint32_t *w = (uint32_t *)d;
    switch (cmd & 0xff) {
    case 0x01: {
        uint32_t nbuf = w[0], nreloc = w[1], nsync = w[2], nfence = w[3];
        uint8_t *p = d + 0x10 + nbuf * 0xc + nreloc * 0x10 + nreloc * 4;
        uint32_t *incr = (uint32_t *)p;
        uint32_t *fences = (uint32_t *)(p + nsync * 0x14);
        uint32_t total = 0;
        for (uint32_t i = 0; i < nsync; i++) total += incr[i * 5 + 1];
        uint32_t v = __atomic_add_fetch(&syncpt_max[f->syncpt], total ? total : 1, __ATOMIC_SEQ_CST);
        for (uint32_t i = 0; i < nfence; i++) { fences[i * 2] = (uint32_t)f->syncpt; fences[i * 2 + 1] = v; }
        (void)extra;
        return NV_OK;
    }
    case 0x02: w[1] = (uint32_t)f->syncpt; return NV_OK;
    case 0x03: w[1] = 0; return NV_OK;
    case 0x07: return NV_OK;
    case 0x09: {
        uint32_t n = w[0];
        for (uint32_t i = 0; i < n; i++) {
            uint32_t *e = &w[3 + i * 2];
            NvMapHandle *m = &nvmap[e[0] % 4096];
            uint32_t iova = mm_iova_next;
            mm_iova_next += (uint32_t)((m->size + 0xffff) & ~0xffffull);
            gpu_maps[ngpu_maps++] = (GpuMapping){iova, m->addr, m->size};
            e[1] = iova;
        }
        return NV_OK;
    }
    case 0x0a: return NV_OK;
    }
    if ((cmd & 0xff00) == 0x4800) return ioctl_gpu(cmd, d, f, extra);
    bad_ioctl(DEV_OTHER_CHANNEL, cmd);
    return 0;
}

static uint32_t do_ioctl(int fd, uint32_t cmd, Buf in, Buf out, Buf extra, Buf extra_out) {
    NvFd *f = &fds[fd];
    uint32_t size = cmd >> 16 & 0x3fff;
    uint8_t d[0x4000];
    memset(d, 0, sizeof d);
    if (cmd >> 30 & 1 && in.addr) memcpy(d, (void *)in.addr, in.size < size ? in.size : size);
    if (in.addr && in.size > size && in.size <= sizeof d) memcpy(d, (void *)in.addr, in.size);
    uint32_t r;
    if (getenv("NV_TRACE")) fprintf(stderr, "[nv] fd %d dev %d ioctl %#x\n", fd, f->type, cmd);
    switch (f->type) {
    case DEV_NVMAP: r = ioctl_nvmap(cmd, d); break;
    case DEV_CTRL: r = ioctl_ctrl(cmd, d, f); break;
    case DEV_CTRL_GPU: r = ioctl_ctrl_gpu(cmd, d); break;
    case DEV_AS: r = ioctl_as(cmd, d); break;
    case DEV_GPU: r = ioctl_gpu(cmd, d, f, extra); break;
    case DEV_OTHER_CHANNEL: r = ioctl_mm_channel(cmd, d, f, extra); break;
    default: r = NV_NOT_SUPPORTED;
    }
    if (cmd >> 31 && out.addr) memcpy((void *)out.addr, d, out.size < size ? out.size : size);
    if (extra_out.addr && f->type == DEV_CTRL_GPU) {
        if ((cmd & 0xff) == 0x05) memcpy((void *)extra_out.addr, d + 0x10, extra_out.size < 0xa0 ? extra_out.size : 0xa0);
        if ((cmd & 0xff) == 0x06) { uint32_t m = 3; memcpy((void *)extra_out.addr, &m, extra_out.size < 4 ? extra_out.size : 4); }
    }
    return r;
}


SVC(nvdrv, "nvdrv") {
    (void)s; (void)st;
    switch (rq->cmd) {
    case 0: {
        Buf p = buf_in(rq, 0);
        char path[64] = {0};
        memcpy(path, (void *)p.addr, p.size < 63 ? p.size : 63);
        int type = DEV_NONE;
        for (unsigned i = 0; i < sizeof devices / sizeof *devices; i++)
            if (!strcmp(path, devices[i].path)) type = devices[i].type;
        if (type == DEV_NONE) { fprintf(stderr, "[nv] dispositivo desconhecido %s\n", path); abort(); }
        int fd = 1;
        while (fds[fd].type) fd++;
        fds[fd] = (NvFd){.type = type, .syncpt = type == DEV_GPU || type == DEV_OTHER_CHANNEL ? syncpt_next++ : 0};
        if (getenv("NV_TRACE")) fprintf(stderr, "[nv] open %s -> %d\n", path, fd);
        OUT(uint32_t, fd);
        OUT(uint32_t, 0);
        return;
    }
    case 1: case 11: case 12: {
        int fd = (int)IN(uint32_t, 0);
        uint32_t cmd = IN(uint32_t, 4);
        Buf extra = rq->cmd == 11 ? buf_in(rq, 1) : (Buf){0};
        Buf extra_out = rq->cmd == 12 ? buf_out(rq, 1) : (Buf){0};
        OUT(uint32_t, do_ioctl(fd, cmd, buf_in(rq, 0), buf_out(rq, 0), extra, extra_out));
        return;
    }
    case 2: {
        int fd = (int)IN(uint32_t, 0);
        fds[fd].type = DEV_NONE;
        OUT(uint32_t, 0);
        return;
    }
    case 3: case 8: case 13: OUT(uint32_t, 0); return;
    case 4: {
        int fd = (int)IN(uint32_t, 0);
        uint32_t id = IN(uint32_t, 4);
        KObject **slot = fds[fd].type == DEV_CTRL ? &ctrl_events[id & 63] : &fds[fd].events[id & 7];
        if (!*slot) { *slot = event_new(); (*slot)->signaled = fds[fd].type == DEV_CTRL; }
        rs_copy(rs, *slot);
        OUT(uint32_t, 0);
        return;
    }
    }
    UNHANDLED();
}
const Service nvdrv_a = {"nvdrv:a", nvdrv_h}, nvdrv_s = {"nvdrv:s", nvdrv_h}, nvdrv_t = {"nvdrv:t", nvdrv_h};
