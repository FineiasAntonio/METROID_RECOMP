#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <ucontext.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>
#include "kernel.h"
#include "runtime.h"

enum {
    RES_OK = 0, R_TIMEOUT = 0xea01, R_CANCELLED = 0xec01, R_BAD_HANDLE = 0xe401, R_INVALID_STATE = 0xd401,
    R_OUT_OF_RANGE = 0xf401, R_NOT_FOUND = 0xf201, R_INVALID_ENUM = 0xf001, R_INVALID_SIZE = 0xca01,
};
#define WAIT_MASK 0x40000000u
#define CUR_THREAD 0xffff8001u
#define CUR_PROCESS 0xffff8000u

#define ALIAS_BASE 0x1000000000ull
#define ALIAS_SIZE 0x1000000000ull
#define HEAP_BASE 0x2000000000ull
#define HEAP_SIZE 0x180000000ull
#define STACK_BASE 0x2800000000ull
#define STACK_SIZE 0x100000000ull
#define KMISC_BASE 0x3000000000ull
#define ASLR_BASE 0x8000000ull
#define ASLR_END 0x8000000000ull

typedef struct {
    uint64_t addr, size;
    uint32_t state, perm;
} Region;

static Region regions[4096];
static int nregions;
static uint64_t heap_size, kmisc_next = KMISC_BASE;

static pthread_mutex_t klock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ksync;
static pthread_condattr_t mono;
static KObject *handles[MAX_HANDLES];
static Thread *threads[1024];
static int nthreads;
static uint64_t next_tid = 0x50;
static __thread Thread *cur;

static void kdie(const char *msg, uint64_t a, uint64_t b) {
    fprintf(stderr, "[kernel] %s %#lx %#lx\n", msg, a, b);
    abort();
}

void region_set(uint64_t addr, uint64_t size, uint32_t state, uint32_t perm) {
    int w = 0;
    Region tmp[4096];
    uint64_t end = addr + size;
    for (int i = 0; i < nregions; i++) {
        Region r = regions[i];
        uint64_t re = r.addr + r.size;
        if (re <= addr || r.addr >= end) { tmp[w++] = r; continue; }
        if (r.addr < addr) tmp[w++] = (Region){r.addr, addr - r.addr, r.state, r.perm};
        if (re > end) tmp[w++] = (Region){end, re - end, r.state, r.perm};
    }
    if (state) tmp[w++] = (Region){addr, size, state, perm};
    for (int i = 1; i < w; i++)
        for (int j = i; j > 0 && tmp[j - 1].addr > tmp[j].addr; j--) { Region t = tmp[j]; tmp[j] = tmp[j - 1]; tmp[j - 1] = t; }
    memcpy(regions, tmp, w * sizeof *regions);
    nregions = w;
}

static Region query(uint64_t addr) {
    uint64_t lo = 0;
    for (int i = 0; i < nregions; i++) {
        if (addr < regions[i].addr) return (Region){lo > addr ? lo : lo, regions[i].addr - lo, 0, 0};
        if (addr < regions[i].addr + regions[i].size) return regions[i];
        lo = regions[i].addr + regions[i].size;
    }
    return (Region){lo, 0 - lo, 0, 0};
}

static void *reserve(uint64_t addr, uint64_t size) {
    void *p = mmap((void *)addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    if (p != (void *)addr) kdie("reserve falhou", addr, size);
    return p;
}

static void commit(uint64_t addr, uint64_t size) {
    if (mprotect((void *)addr, size, PROT_READ | PROT_WRITE)) kdie("mprotect falhou", addr, size);
}

static uint64_t kalloc(uint64_t size, uint32_t state) {
    size = (size + 0xfff) & ~0xfffull;
    uint64_t a = kmisc_next;
    kmisc_next += size + 0x10000;
    void *p = mmap((void *)a, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    if (p != (void *)a) kdie("kalloc falhou", a, size);
    region_set(a, size, state, 3);
    return a;
}

static uint32_t handle_new(KObject *o) {
    for (uint32_t i = 1; i < MAX_HANDLES; i++)
        if (!handles[i]) {
            handles[i] = o;
            o->refs++;
            return 0x8000 | i;
        }
    kdie("tabela de handles cheia", 0, 0);
    return 0;
}

KObject *handle_get(uint32_t h) {
    if (h == CUR_THREAD) return &cur->obj;
    uint32_t i = h & 0x7fff;
    if ((h & ~0x7fffu) != 0x8000 || !handles[i]) return NULL;
    return handles[i];
}

uint32_t handle_add(KObject *o) {
    pthread_mutex_lock(&klock);
    uint32_t h = handle_new(o);
    pthread_mutex_unlock(&klock);
    return h;
}

void kobj_signal(KObject *o) {
    pthread_mutex_lock(&klock);
    o->signaled = 1;
    pthread_cond_broadcast(&ksync);
    pthread_mutex_unlock(&klock);
}

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static struct timespec deadline(int64_t timeout) {
    int64_t d = now_ns() + timeout;
    return (struct timespec){d / 1000000000, d % 1000000000};
}

static int ksleep(Thread *t, int64_t timeout) {
    struct timespec dl = deadline(timeout);
    t->woken = 0;
    while (!t->woken) {
        if (timeout >= 0) {
            if (timeout == 0 || pthread_cond_timedwait(&t->cv, &klock, &dl)) {
                if (t->woken) break;
                return 1;
            }
        } else pthread_cond_wait(&t->cv, &klock);
    }
    return 0;
}

static void kwake(Thread *t) {
    t->woken = 1;
    pthread_cond_signal(&t->cv);
}

static Thread *best_waiter(int (*match)(Thread *, uint64_t), uint64_t key, int *count) {
    Thread *best = NULL;
    int n = 0;
    for (int i = 0; i < nthreads; i++)
        if (match(threads[i], key)) {
            n++;
            if (!best || threads[i]->prio < best->prio) best = threads[i];
        }
    if (count) *count = n;
    return best;
}

static int m_mutex(Thread *t, uint64_t k) { return t->mutex_wait == k; }
static int m_cv(Thread *t, uint64_t k) { return t->cv_wait == k; }
static int m_addr(Thread *t, uint64_t k) { return t->addr_wait == k; }

static void mutex_release(uint64_t addr) {
    int n;
    Thread *next = best_waiter(m_mutex, addr, &n);
    if (!next) {
        __atomic_store_n((uint32_t *)addr, 0, __ATOMIC_SEQ_CST);
        return;
    }
    next->mutex_wait = 0;
    __atomic_store_n((uint32_t *)addr, next->handle | (n > 1 ? WAIT_MASK : 0), __ATOMIC_SEQ_CST);
    kwake(next);
}

static void mutex_acquire_for(Thread *t) {
    uint32_t *m = (uint32_t *)t->cv_mutex;
    uint32_t v = __atomic_load_n(m, __ATOMIC_SEQ_CST);
    for (;;) {
        if (v == 0) {
            if (__atomic_compare_exchange_n(m, &v, t->handle, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                kwake(t);
                return;
            }
        } else if (__atomic_compare_exchange_n(m, &v, v | WAIT_MASK, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            t->mutex_wait = t->cv_mutex;
            return;
        }
    }
}

static void *thread_main(void *p) {
    Thread *t = p;
    cur = t;
    static __thread uint8_t alt[1 << 16];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt};
    sigaltstack(&ss, NULL);
    guest_fn f = lookup(t->entry);
    if (!f) kdie("thread entry inválida", t->entry, 0);
    t->c->x[30] = 0;
    f(t->c);
    k_exit_thread();
    return NULL;
}

void k_exit_thread(void) {
    pthread_mutex_lock(&klock);
    cur->obj.signaled = 1;
    pthread_cond_broadcast(&ksync);
    pthread_mutex_unlock(&klock);
    pthread_exit(NULL);
}

Thread *thread_new(uint64_t entry, uint64_t arg, uint64_t sp, int prio, int core) {
    Thread *t = calloc(1, sizeof *t);
    t->obj.type = KO_THREAD;
    t->c = calloc(1, sizeof *t->c);
    t->c->x[0] = arg;
    t->c->sp = sp;
    t->c->tpidrro = kalloc(0x1000, 0xc);
    t->entry = entry;
    t->prio = prio;
    t->core = core;
    t->id = next_tid++;
    pthread_cond_init(&t->cv, &mono);
    t->handle = handle_new(&t->obj);
    threads[nthreads++] = t;
    return t;
}

static void thread_start(Thread *t) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64 << 20);
    if (pthread_create(&t->host, &a, thread_main, t)) kdie("pthread_create", 0, 0);
    pthread_attr_destroy(&a);
}

static uint64_t get_info(uint32_t id0, uint32_t h, uint64_t id1, uint32_t *res) {
    *res = RES_OK;
    switch (id0) {
    case 0: return 0xf;
    case 1: return 0xfffffffffffffff0ull;
    case 2: return ALIAS_BASE;
    case 3: return ALIAS_SIZE;
    case 4: return HEAP_BASE;
    case 5: return HEAP_SIZE;
    case 6: case 21: return 0xcd500000ull;
    case 7: case 22: return heap_size + 0x7a000000ull;
    case 8: return 0;
    case 9: return 0;
    case 10: return 0;
    case 11: { uint64_t r; getrandom(&r, 8, 0); return r; }
    case 12: return ASLR_BASE;
    case 13: return ASLR_END - ASLR_BASE;
    case 14: return STACK_BASE;
    case 15: return STACK_SIZE;
    case 16: case 17: return 0;
    case 18: return 0x010093801237c000ull;
    case 20: return 0;
    case 0xf0000002: return 0;
    }
    (void)h; (void)id1;
    *res = R_INVALID_ENUM;
    fprintf(stderr, "[kernel] GetInfo desconhecido %u/%lu\n", id0, id1);
    return 0;
}

static uint32_t wait_sync(CPU *c, uint32_t *hs, int n, int64_t timeout, int *index) {
    KObject *objs[64];
    if (n > 64) return R_OUT_OF_RANGE;
    for (int i = 0; i < n; i++)
        if (!(objs[i] = handle_get(hs[i]))) { fprintf(stderr, "[kernel] WaitSynchronization: handle inválido %#x\n", hs[i]); guest_backtrace(c); return R_BAD_HANDLE; }
    struct timespec dl = deadline(timeout);
    for (;;) {
        if (cur->cancel) { cur->cancel = 0; return R_CANCELLED; }
        for (int i = 0; i < n; i++)
            if (objs[i]->signaled) { *index = i; return RES_OK; }
        if (timeout == 0) return R_TIMEOUT;
        if (timeout > 0) {
            if (pthread_cond_timedwait(&ksync, &klock, &dl)) return R_TIMEOUT;
        } else pthread_cond_wait(&ksync, &klock);
    }
}

static uint32_t wait_for_address(uint64_t addr, uint32_t type, int32_t value, int64_t timeout) {
    int32_t *p = (int32_t *)addr;
    int32_t v = __atomic_load_n(p, __ATOMIC_SEQ_CST);
    if (type == 0 || type == 1) {
        if (v >= value) return R_INVALID_STATE;
        if (type == 1) __atomic_fetch_sub(p, 1, __ATOMIC_SEQ_CST);
    } else if (type == 2) {
        if (v != value) return R_INVALID_STATE;
    } else return R_INVALID_ENUM;
    if (timeout == 0) return R_TIMEOUT;
    cur->addr_wait = addr;
    int to = ksleep(cur, timeout);
    cur->addr_wait = 0;
    return to ? R_TIMEOUT : RES_OK;
}

static void wake_addr(uint64_t addr, int32_t count) {
    for (int k = 0; count <= 0 || k < count; k++) {
        Thread *t = best_waiter(m_addr, addr, NULL);
        if (!t) break;
        t->addr_wait = 0;
        kwake(t);
    }
}

static uint32_t signal_to_address(uint64_t addr, uint32_t type, int32_t value, int32_t count) {
    int32_t *p = (int32_t *)addr;
    if (type == 1) {
        int32_t e = value;
        if (!__atomic_compare_exchange_n(p, &e, value + 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return R_INVALID_STATE;
    } else if (type == 2) {
        int n;
        best_waiter(m_addr, addr, &n);
        int32_t nv;
        if (count <= 0) nv = n ? value - 2 : value + 1;
        else nv = n ? (n <= count ? value - 1 : value) : value + 1;
        int32_t e = value;
        if (!__atomic_compare_exchange_n(p, &e, nv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return R_INVALID_STATE;
    } else if (type != 0) return R_INVALID_ENUM;
    wake_addr(addr, count);
    return RES_OK;
}

static uint64_t map_src[256], map_dst[256], map_size[256];

void svc(CPU *c, uint32_t imm) {
    uint64_t *x = c->x;
    uint32_t r = RES_OK;
    if (getenv("SVC_TRACE")) fprintf(stderr, "[svc %#x t%lu] x0=%#lx x1=%#lx x2=%#lx x3=%#lx lr=%#lx\n", imm, cur->id, x[0], x[1], x[2], x[3], x[30]);
    pthread_mutex_lock(&klock);
    switch (imm) {
    case 0x01: {
        uint64_t size = x[1];
        if (size > HEAP_SIZE || size & 0x1fffff) { r = R_INVALID_SIZE; break; }
        if (size > heap_size) commit(HEAP_BASE + heap_size, size - heap_size);
        else if (size < heap_size) {
            mmap((void *)(HEAP_BASE + size), heap_size - size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
        }
        heap_size = size;
        region_set(HEAP_BASE, HEAP_SIZE, 0, 0);
        if (size) region_set(HEAP_BASE, size, 5, 3);
        x[1] = HEAP_BASE;
        break;
    }
    case 0x02: {
        Region q = query(x[0]);
        if (q.state) region_set(x[0], x[1], q.state, (uint32_t)x[2]);
        break;
    }
    case 0x03: break;
    case 0x04: {
        uint64_t dst = x[0], src = x[1], size = x[2];
        if (mmap((void *)dst, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != (void *)dst) kdie("MapMemory", dst, size);
        memcpy((void *)dst, (void *)src, size);
        region_set(dst, size, 0xb, 3);
        for (int i = 0; i < 256; i++)
            if (!map_size[i]) { map_src[i] = src; map_dst[i] = dst; map_size[i] = size; break; }
        break;
    }
    case 0x05: {
        uint64_t dst = x[0], src = x[1], size = x[2];
        memcpy((void *)src, (void *)dst, size);
        mmap((void *)dst, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
        region_set(dst, size, 0, 0);
        for (int i = 0; i < 256; i++)
            if (map_dst[i] == dst) map_size[i] = 0;
        break;
    }
    case 0x06: {
        Region q = query(x[2]);
        uint32_t *mi = (uint32_t *)x[0];
        memset(mi, 0, 0x28);
        *(uint64_t *)mi = q.addr;
        *(uint64_t *)(mi + 2) = q.size;
        mi[4] = q.state;
        mi[6] = q.perm;
        x[1] = 0;
        break;
    }
    case 0x07:
        fprintf(stderr, "[kernel] ExitProcess\n");
        exit(0);
    case 0x08: {
        Thread *t = thread_new(x[1], x[2], x[3], (int)x[4], (int)(int32_t)x[5]);
        x[1] = t->handle;
        break;
    }
    case 0x09: {
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o || o->type != KO_THREAD) { r = R_BAD_HANDLE; break; }
        thread_start((Thread *)o);
        break;
    }
    case 0x0a:
        pthread_mutex_unlock(&klock);
        k_exit_thread();
        break;
    case 0x0b: {
        int64_t ns = (int64_t)x[0];
        pthread_mutex_unlock(&klock);
        if (ns <= 0) sched_yield();
        else nanosleep(&(struct timespec){ns / 1000000000, ns % 1000000000}, NULL);
        c->x[0] = 0;
        return;
    }
    case 0x0c: {
        KObject *o = handle_get((uint32_t)x[1]);
        if (!o || o->type != KO_THREAD) { r = R_BAD_HANDLE; break; }
        x[1] = ((Thread *)o)->prio;
        break;
    }
    case 0x0d: {
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o || o->type != KO_THREAD) { r = R_BAD_HANDLE; break; }
        ((Thread *)o)->prio = (int)x[1];
        break;
    }
    case 0x0e: {
        KObject *o = handle_get((uint32_t)x[2]);
        if (!o || o->type != KO_THREAD) { r = R_BAD_HANDLE; break; }
        x[1] = ((Thread *)o)->core;
        x[2] = 0xf;
        break;
    }
    case 0x0f: {
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o || o->type != KO_THREAD) { r = R_BAD_HANDLE; break; }
        if ((int32_t)x[1] >= 0) ((Thread *)o)->core = (int)x[1];
        break;
    }
    case 0x10: x[0] = cur->core < 0 ? 0 : cur->core; pthread_mutex_unlock(&klock); return;
    case 0x11: {
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o || o->type != KO_EVENT) { r = R_BAD_HANDLE; break; }
        o->signaled = 1;
        pthread_cond_broadcast(&ksync);
        break;
    }
    case 0x12:
    case 0x17: {
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o) { r = R_BAD_HANDLE; break; }
        if (imm == 0x17 && !o->signaled) r = R_INVALID_STATE;
        o->signaled = 0;
        break;
    }
    case 0x13: {
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o || o->type != KO_SHMEM) { r = R_BAD_HANDLE; break; }
        SharedMem *s = (SharedMem *)o;
        if (mmap((void *)x[1], x[2], PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, s->fd, 0) != (void *)x[1]) kdie("MapSharedMemory", x[1], x[2]);
        region_set(x[1], x[2], 6, (uint32_t)x[3]);
        break;
    }
    case 0x14:
        mmap((void *)x[1], x[2], PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
        region_set(x[1], x[2], 0, 0);
        break;
    case 0x15: {
        KObject *o = calloc(1, sizeof(TransferMem));
        o->type = KO_TMEM;
        ((TransferMem *)o)->addr = x[1];
        ((TransferMem *)o)->size = x[2];
        x[1] = handle_new(o);
        break;
    }
    case 0x16: {
        uint32_t h = (uint32_t)x[0];
        if (h == CUR_THREAD || h == CUR_PROCESS) break;
        KObject *o = handle_get(h);
        if (!o) { r = R_BAD_HANDLE; break; }
        handles[h & 0x7fff] = NULL;
        o->refs--;
        break;
    }
    case 0x18: {
        int idx = -1;
        uint32_t hs[64];
        int n = (int)x[2];
        if (n > 64) { r = R_OUT_OF_RANGE; break; }
        memcpy(hs, (void *)x[1], n * 4);
        r = wait_sync(c, hs, n, (int64_t)x[3], &idx);
        x[1] = (uint32_t)idx;
        break;
    }
    case 0x19: {
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o || o->type != KO_THREAD) { r = R_BAD_HANDLE; break; }
        ((Thread *)o)->cancel = 1;
        pthread_cond_broadcast(&ksync);
        break;
    }
    case 0x1a: {
        uint32_t owner = (uint32_t)x[0], req = (uint32_t)x[2];
        uint64_t addr = x[1];
        if (__atomic_load_n((uint32_t *)addr, __ATOMIC_SEQ_CST) != (owner | WAIT_MASK)) break;
        (void)req;
        cur->mutex_wait = addr;
        ksleep(cur, -1);
        break;
    }
    case 0x1b:
        mutex_release(x[0]);
        break;
    case 0x1c: {
        uint64_t m = x[0], key = x[1];
        int64_t timeout = (int64_t)x[3];
        mutex_release(m);
        cur->cv_wait = key;
        cur->cv_mutex = m;
        __atomic_store_n((uint32_t *)key, 1, __ATOMIC_SEQ_CST);
        if (ksleep(cur, timeout)) {
            if (cur->cv_wait) {
                cur->cv_wait = 0;
                r = R_TIMEOUT;
            } else if (cur->mutex_wait) {
                ksleep(cur, -1);
            }
        }
        break;
    }
    case 0x1d: {
        uint64_t key = x[0];
        int32_t count = (int32_t)x[1];
        int k = 0;
        Thread *t;
        while ((count <= 0 || k < count) && (t = best_waiter(m_cv, key, NULL))) {
            t->cv_wait = 0;
            mutex_acquire_for(t);
            k++;
        }
        if (!best_waiter(m_cv, key, NULL)) __atomic_store_n((uint32_t *)key, 0, __ATOMIC_SEQ_CST);
        break;
    }
    case 0x1e: pthread_mutex_unlock(&klock); x[0] = sysreg_read(c, 0x5f01); return;
    case 0x1f: {
        Session *s = session_open_port((const char *)x[1]);
        if (!s) { r = R_NOT_FOUND; break; }
        x[1] = handle_new(&s->obj);
        break;
    }
    case 0x21: {
        KObject *o = handle_get((uint32_t)x[0]);
        if (!o || o->type != KO_SESSION) { r = R_BAD_HANDLE; break; }
        pthread_mutex_unlock(&klock);
        x[0] = ipc_dispatch((Session *)o, c->tpidrro);
        return;
    }
    case 0x25: {
        KObject *o = handle_get((uint32_t)x[1]);
        if (!o || o->type != KO_THREAD) { r = R_BAD_HANDLE; break; }
        x[1] = ((Thread *)o)->id;
        break;
    }
    case 0x26:
        fprintf(stderr, "[kernel] svcBreak reason=%#lx info=%#lx size=%#lx lr=%#lx thread=%lu\n", x[0], x[1], x[2], x[30], cur->id);
        guest_backtrace(c);
        if (x[2] == 4 && x[1]) fprintf(stderr, "[kernel] result=%#x (módulo %u, descrição %u)\n", *(uint32_t *)x[1], *(uint32_t *)x[1] & 0x1ff, *(uint32_t *)x[1] >> 9 & 0x1fff);
        abort();
    case 0x27:
        fwrite((void *)x[0], 1, x[1], stderr);
        break;
    case 0x29: x[1] = get_info((uint32_t)x[1], (uint32_t)x[2], x[3], &r); break;
    case 0x2c:
        commit(x[0], x[1]);
        region_set(x[0], x[1], 5, 3);
        break;
    case 0x2d:
        mmap((void *)x[0], x[1], PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
        region_set(x[0], x[1], 0, 0);
        break;
    case 0x30: x[1] = 0x10000; break;
    case 0x31: x[1] = 0; break;
    case 0x34: {
        Thread *t = cur;
        uint64_t addr = x[0];
        (void)t;
        r = wait_for_address(addr, (uint32_t)x[1], (int32_t)x[2], (int64_t)x[3]);
        break;
    }
    case 0x35: r = signal_to_address(x[0], (uint32_t)x[1], (int32_t)x[2], (int32_t)x[3]); break;
    case 0x45: {
        KObject *o = calloc(1, sizeof *o);
        o->type = KO_EVENT;
        x[1] = handle_new(o);
        x[2] = handle_new(o);
        break;
    }
    case 0x5f: break;
    default:
        pthread_mutex_unlock(&klock);
        kdie("svc não implementada", imm, x[30]);
    }
    pthread_mutex_unlock(&klock);
    if (r && getenv("SVC_TRACE")) fprintf(stderr, "[svc %#x t%lu] -> erro %#x\n", imm, cur->id, r);
    x[0] = r;
}

uint64_t kernel_heap_base(void) { return HEAP_BASE; }

static void on_fault(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    extern int texture_fault(uint64_t addr);
    if (sig == SIGSEGV && si->si_code == SEGV_ACCERR && texture_fault((uint64_t)(uintptr_t)si->si_addr)) return;
    extern char __executable_start;
    fprintf(stderr, "\n[kernel] sinal %d acessando %p (rip=exe+%#llx) thread=%lu\n", sig, si->si_addr,
            (unsigned long long)(uc->uc_mcontext.gregs[REG_RIP] - (uintptr_t)&__executable_start), cur ? cur->id : 0);
    if (cur) {
        guest_backtrace(cur->c);
        for (int i = 0; i < 31; i++) fprintf(stderr, "x%d=%#lx%s", i, cur->c->x[i], i % 6 == 5 ? "\n" : " ");
        fprintf(stderr, "sp=%#lx\n", cur->c->sp);
    }
    if (getenv("RELOC_CHECK")) verify_relocs();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void on_dump(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    ucontext_t *uc = uc_;
    extern char __executable_start;
    if (!cur) return;
    fprintf(stderr, "[dump] thread %lu rip=exe+%#llx prio=%d\n", cur->id,
            (unsigned long long)(uc->uc_mcontext.gregs[REG_RIP] - (uintptr_t)&__executable_start), cur->prio);
    guest_backtrace(cur->c);
}

static void *dump_thread(void *p) {
    sleep((unsigned)(uintptr_t)p);
    if (getenv("GPU_HIST")) { extern void gpu_dump_hist(void); gpu_dump_hist(); }
    for (int i = 0; i < nthreads; i++)
        if (threads[i]->host && !threads[i]->obj.signaled) { pthread_kill(threads[i]->host, SIGUSR1); usleep(20000); }
    return NULL;
}

void kernel_init(void) {
    struct sigaction du = {.sa_sigaction = on_dump, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART};
    sigaction(SIGUSR1, &du, NULL);
    if (getenv("DUMP_AFTER")) {
        pthread_t t;
        pthread_create(&t, NULL, dump_thread, (void *)(uintptr_t)atoi(getenv("DUMP_AFTER")));
    }
    pthread_condattr_init(&mono);
    pthread_condattr_setclock(&mono, CLOCK_MONOTONIC);
    pthread_cond_init(&ksync, &mono);
    static uint8_t altstack[1 << 16];
    stack_t ss = {.ss_sp = altstack, .ss_size = sizeof altstack};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {.sa_sigaction = on_fault, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    reserve(ALIAS_BASE, ALIAS_SIZE);
    reserve(HEAP_BASE, HEAP_SIZE);
    reserve(STACK_BASE, STACK_SIZE);
}

void kernel_run_main(uint64_t entry, uint64_t stack_size, int prio, int core) {
    uint64_t stack = kalloc(stack_size, 0xb);
    pthread_mutex_lock(&klock);
    Thread *t = thread_new(entry, 0, stack + stack_size, prio, core);
    t->c->x[1] = t->handle;
    thread_start(t);
    pthread_mutex_unlock(&klock);
    pthread_join(t->host, NULL);
}
