#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "ipc.h"
#include "nv.h"
#include "render.h"

#define SVC(name, id) static void name##_h(Session *s, void *st, Request *rq, Response *rs); \
    const Service name = {id, name##_h}; \
    static void name##_h(Session *s, void *st, Request *rq, Response *rs)
#define UNUSED (void)s; (void)st; (void)rs

#define WIDTH 1280
#define HEIGHT 720
#define SLOTS 8

enum { SLOT_FREE, SLOT_DEQUEUED, SLOT_QUEUED };

typedef struct {
    int state;
    uint8_t buffer[0x200];
    uint32_t buffer_size;
} Slot;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t freed;
    Slot slots[SLOTS];
    int queue[SLOTS], nqueue;
    KObject *buffer_event;
    uint64_t frames;
} bq = {.lock = PTHREAD_MUTEX_INITIALIZER, .freed = PTHREAD_COND_INITIALIZER};

static KObject *vsync_event;

static void *vsync_thread(void *p) {
    (void)p;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_nsec += 16666667;
        if (next.tv_nsec >= 1000000000) { next.tv_nsec -= 1000000000; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        pthread_mutex_lock(&bq.lock);
        if (bq.nqueue) {
            bq.slots[bq.queue[0]].state = SLOT_FREE;
            memmove(bq.queue, bq.queue + 1, --bq.nqueue * sizeof *bq.queue);
            pthread_cond_broadcast(&bq.freed);
            if (bq.buffer_event) kobj_signal(bq.buffer_event);
        }
        pthread_mutex_unlock(&bq.lock);
        kobj_signal(vsync_event);
    }
    return NULL;
}

static KObject *get_vsync(void) {
    if (!vsync_event) {
        vsync_event = event_new();
        pthread_t t;
        pthread_create(&t, NULL, vsync_thread, NULL);
    }
    return vsync_event;
}

typedef struct {
    uint8_t *p;
    uint32_t off, size;
} Parcel;

static uint32_t rd32(Parcel *p) { uint32_t v = 0; if (p->off + 4 <= p->size) memcpy(&v, p->p + p->off, 4); p->off += 4; return v; }

typedef struct {
    uint8_t data[0x800];
    uint32_t n;
} Out;

static void wr32(Out *o, uint32_t v) { memcpy(o->data + o->n, &v, 4); o->n += 4; }
static void wrbytes(Out *o, const void *b, uint32_t n) { memcpy(o->data + o->n, b, n); o->n += (n + 3) & ~3u; }
static void wr_flat(Out *o, const void *b, uint32_t n) { wr32(o, 1); wr32(o, n); wr32(o, 0); wrbytes(o, b, n); }

static uint32_t emit_parcel_objs(Buf dst, Out *o, const uint32_t *objs, uint32_t nobjs) {
    uint32_t hdr[4] = {o->n, 0x10, nobjs * 4, 0x10 + o->n};
    uint8_t tmp[0x900];
    memcpy(tmp, hdr, 16);
    memcpy(tmp + 16, o->data, o->n);
    memcpy(tmp + 16 + o->n, objs, nobjs * 4);
    uint32_t total = 16 + o->n + nobjs * 4;
    rs_buf(dst, tmp, total);
    return total;
}

static uint32_t emit_parcel(Buf dst, Out *o) { return emit_parcel_objs(dst, o, NULL, 0); }

static void queue_output(Out *o) {
    wr32(o, WIDTH);
    wr32(o, HEIGHT);
    wr32(o, 0);
    wr32(o, 0);
}

static void transact(uint32_t code, Buf in, Buf out) {
    const uint32_t *h = (const uint32_t *)in.addr;
    Parcel p = {(uint8_t *)in.addr + h[1], 0, h[0]};
    rd32(&p);
    uint32_t len = rd32(&p);
    p.off += ((len + 1) * 2 + 3) & ~3u;
    Out o = {0};
    int32_t status = 0;
    if (getenv("VI_TRACE")) fprintf(stderr, "[vi] transação %u\n", code);
    pthread_mutex_lock(&bq.lock);
    switch (code) {
    case 1: {
        uint32_t slot = rd32(&p);
        Slot *sl = &bq.slots[slot % SLOTS];
        if (sl->buffer_size) wr_flat(&o, sl->buffer, sl->buffer_size);
        else wr32(&o, 0);
        break;
    }
    case 2: break;
    case 3: {
        int slot = -1;
        get_vsync();
        for (int tries = 0; slot < 0 && tries < 100; tries++) {
            for (int i = 0; i < SLOTS; i++)
                if (bq.slots[i].buffer_size && bq.slots[i].state == SLOT_FREE) { slot = i; break; }
            if (slot < 0) {
                struct timespec dl;
                clock_gettime(CLOCK_REALTIME, &dl);
                dl.tv_nsec += 20000000;
                if (dl.tv_nsec >= 1000000000) { dl.tv_nsec -= 1000000000; dl.tv_sec++; }
                pthread_cond_timedwait(&bq.freed, &bq.lock, &dl);
            }
        }
        if (slot < 0) { status = -11; wr32(&o, 0); break; }
        bq.slots[slot].state = SLOT_DEQUEUED;
        wr32(&o, (uint32_t)slot);
        uint8_t fence[0x24] = {0};
        wr_flat(&o, fence, sizeof fence);
        break;
    }
    case 7: {
        uint32_t slot = rd32(&p);
        {
            const uint8_t *g = bq.slots[slot % SLOTS].buffer;
            uint32_t handle = *(const uint32_t *)(g + 0x74), off = *(const uint32_t *)(g + 0x78);
            PresentInfo pi = {nvmap[handle % 4096].addr + off, *(const uint32_t *)(g + 4), *(const uint32_t *)(g + 8),
                              *(const uint32_t *)(g + 0xc), *(const uint32_t *)(g + 0x10), 4};
            static int shown;
            if (!shown++ && getenv("VI_TRACE")) {
                fprintf(stderr, "[vi] GraphicBuffer:");
                for (int i = 0; i < 0x40; i++) fprintf(stderr, " %08x", ((const uint32_t *)g)[i]);
                fputc('\n', stderr);
            }
            render_present(&pi);
        }
        bq.slots[slot % SLOTS].state = SLOT_QUEUED;
        bq.queue[bq.nqueue++] = (int)(slot % SLOTS);
        bq.frames++;
        if (getenv("VI_TRACE") || !(bq.frames & (bq.frames - 1))) {
            extern uint64_t stat_draws, stat_clears, stat_macros;
            fprintf(stderr, "[vi] quadro %lu apresentado (slot %u) | total: %lu draws, %lu clears, %lu macros\n", bq.frames, slot, stat_draws, stat_clears, stat_macros);
        }
        queue_output(&o);
        break;
    }
    case 8: {
        uint32_t slot = rd32(&p);
        bq.slots[slot % SLOTS].state = SLOT_FREE;
        if (bq.buffer_event) kobj_signal(bq.buffer_event);
        break;
    }
    case 9: {
        uint32_t what = rd32(&p);
        uint32_t v = what == 0 ? WIDTH : what == 1 ? HEIGHT : what == 2 ? 1 : what == 3 ? 1 : 0;
        wr32(&o, v);
        break;
    }
    case 10: queue_output(&o); break;
    case 11: break;
    case 14: {
        uint32_t slot = rd32(&p);
        uint32_t has = rd32(&p);
        Slot *sl = &bq.slots[slot % SLOTS];
        if (has) {
            uint32_t size = rd32(&p);
            rd32(&p);
            if (size > sizeof sl->buffer) size = sizeof sl->buffer;
            memcpy(sl->buffer, p.p + p.off, size);
            sl->buffer_size = size;
        } else sl->buffer_size = 0;
        sl->state = SLOT_FREE;
        pthread_mutex_unlock(&bq.lock);
        emit_parcel(out, &o);
        return;
    }
    default:
        fprintf(stderr, "[vi] transação %u não implementada\n", code);
        abort();
    }
    pthread_mutex_unlock(&bq.lock);
    wr32(&o, (uint32_t)status);
    emit_parcel(out, &o);
}

SVC(binder, "vi:IHOSBinderDriver") {
    UNUSED;
    switch (rq->cmd) {
    case 0: case 3: transact(IN(uint32_t, 4), buf_in(rq, 0), buf_out(rq, 0)); return;
    case 1: return;
    case 2:
        if (!bq.buffer_event) { bq.buffer_event = event_new(); bq.buffer_event->signaled = 1; }
        rs_copy(rs, bq.buffer_event);
        return;
    }
    UNHANDLED();
}

SVC(manager_display, "vi:IManagerDisplayService") {
    UNUSED;
    switch (rq->cmd) {
    case 2010: OUT(uint64_t, 1); return;
    case 2011: case 2012: case 6000: case 6001: case 6002: case 7000: case 8000: return;
    }
    UNHANDLED();
}

SVC(system_display, "vi:ISystemDisplayService") {
    UNUSED;
    switch (rq->cmd) {
    case 1200: case 1202: case 1203: OUT(uint64_t, 0); return;
    case 2205: case 2207: case 2209: return;
    case 2312: OUT(uint64_t, 1); OUT(uint64_t, 0); return;
    case 3200: OUT(uint32_t, WIDTH); OUT(uint32_t, HEIGHT); OUT(float, 60.0f); OUT(uint32_t, 0); return;
    }
    UNHANDLED();
}

static uint32_t native_window(Buf b) {
    Out o = {0};
    uint32_t nw[10] = {2, 0, 1, 0, 0, 0, 0x70736964, 0x00767264, 0, 0};
    wrbytes(&o, nw, sizeof nw);
    static const uint32_t objs[1] = {0};
    return emit_parcel_objs(b, &o, objs, 1);
}

SVC(app_display, "vi:IApplicationDisplayService") {
    UNUSED;
    switch (rq->cmd) {
    case 100: case 103: rs_object(s, rs, &binder, NULL); return;
    case 101: rs_object(s, rs, &system_display, NULL); return;
    case 102: rs_object(s, rs, &manager_display, NULL); return;
    case 1000: {
        uint8_t info[0x60] = "Default";
        *(uint64_t *)(info + 0x48) = 1;
        *(uint64_t *)(info + 0x50) = WIDTH;
        *(uint64_t *)(info + 0x58) = HEIGHT;
        rs_buf(buf_out(rq, 0), info, sizeof info);
        OUT(uint64_t, 1);
        return;
    }
    case 1010: case 1011: OUT(uint64_t, 0); return;
    case 1020: case 1101: case 2021: case 2031: case 2101: return;
    case 1102: OUT(uint64_t, WIDTH); OUT(uint64_t, HEIGHT); return;
    case 2020: OUT(uint64_t, native_window(buf_out(rq, 0))); return;
    case 2030: OUT(uint64_t, 1); OUT(uint64_t, native_window(buf_out(rq, 0))); return;
    case 2102: OUT(uint64_t, IN(uint32_t, 0)); return;
    case 2460: OUT(uint64_t, 0x7e000); OUT(uint64_t, 0x1000); return;
    case 5202: case 5203: rs_copy(rs, get_vsync()); return;
    }
    UNHANDLED();
}

SVC(vi_m, "vi:m") {
    UNUSED;
    switch (rq->cmd) {
    case 0: case 1: case 2: case 3: rs_object(s, rs, &app_display, NULL); return;
    }
    UNHANDLED();
}
const Service vi_u = {"vi:u", vi_m_h}, vi_s = {"vi:s", vi_m_h};
