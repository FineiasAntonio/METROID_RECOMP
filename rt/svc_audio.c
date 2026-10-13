#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "ipc.h"

#define SVC(name, id) static void name##_h(Session *s, void *st, Request *rq, Response *rs); \
    const Service name = {id, name##_h}; \
    static void name##_h(Session *s, void *st, Request *rq, Response *rs)
#define UNUSED (void)s; (void)st; (void)rs

#define RATE 48000
#define CHANNELS 2

typedef struct {
    uint64_t tag, addr, size;
    int64_t due;
} OutBuf;

typedef struct {
    pthread_mutex_t lock;
    OutBuf q[64];
    int n;
    int started;
    int64_t clock;
    uint64_t played;
    float volume;
    KObject *event;
} AudioOut;

static int64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void *audio_clock(void *p) {
    AudioOut *a = p;
    for (;;) {
        usleep(2000);
        pthread_mutex_lock(&a->lock);
        int due = a->n && a->q[0].due <= now_ns();
        pthread_mutex_unlock(&a->lock);
        if (due) kobj_signal(a->event);
    }
    return NULL;
}

SVC(audio_out, "audout:IAudioOut") {
    UNUSED;
    AudioOut *a = st;
    pthread_mutex_lock(&a->lock);
    switch (rq->cmd) {
    case 0: OUT(uint32_t, a->started ? 0 : 1); break;
    case 1: a->started = 1; a->clock = now_ns(); break;
    case 2: a->started = 0; break;
    case 3: case 7: {
        uint64_t tag = IN(uint64_t, 0);
        Buf b = buf_in(rq, 0);
        uint64_t *desc = (uint64_t *)b.addr;
        if (a->n == 64) { rs->result = 0x1099; break; }
        int64_t now = now_ns();
        if (a->clock < now) a->clock = now;
        a->clock += (int64_t)(desc[3] / (2 * CHANNELS)) * 1000000000 / RATE;
        a->q[a->n++] = (OutBuf){tag, desc[1], desc[3], a->clock};
        break;
    }
    case 4: rs_copy(rs, a->event); break;
    case 5: case 8: {
        Buf b = buf_out(rq, 0);
        uint64_t *tags = (uint64_t *)b.addr;
        uint32_t max = (uint32_t)(b.size / 8), k = 0;
        int64_t now = now_ns();
        while (k < max && a->n && a->q[0].due <= now) {
            tags[k++] = a->q[0].tag;
            a->played += a->q[0].size / (2 * CHANNELS);
            memmove(a->q, a->q + 1, --a->n * sizeof *a->q);
        }
        if (k < max) tags[k] = 0;
        a->event->signaled = 0;
        OUT(uint32_t, k);
        break;
    }
    case 6: {
        uint64_t tag = IN(uint64_t, 0);
        int found = 0;
        for (int i = 0; i < a->n; i++) found |= a->q[i].tag == tag;
        OUT(uint8_t, found);
        break;
    }
    case 9: OUT(uint32_t, (uint32_t)a->n); break;
    case 10: OUT(uint64_t, a->played); break;
    case 11: OUT(uint8_t, 1); break;
    case 12: a->volume = IN(float, 0); break;
    case 13: OUT(float, a->volume); break;
    default:
        pthread_mutex_unlock(&a->lock);
        UNHANDLED();
        return;
    }
    pthread_mutex_unlock(&a->lock);
}

SVC(audout, "audout:u") {
    UNUSED;
    static const char name[0x100] = "DeviceOut";
    switch (rq->cmd) {
    case 0: case 2: {
        Buf b = buf_out(rq, 0);
        rs_buf(b, name, sizeof name);
        OUT(uint32_t, 1);
        return;
    }
    case 1: case 3: {
        AudioOut *a = calloc(1, sizeof *a);
        pthread_mutex_init(&a->lock, NULL);
        a->event = event_new();
        a->volume = 1.0f;
        pthread_t t;
        pthread_create(&t, NULL, audio_clock, a);
        Buf b = buf_out(rq, 0);
        if (b.addr) rs_buf(b, name, sizeof name);
        rs_object(s, rs, &audio_out, a);
        OUT(uint32_t, RATE);
        OUT(uint32_t, CHANNELS);
        OUT(uint32_t, 2);
        OUT(uint32_t, 1);
        return;
    }
    }
    UNHANDLED();
}

typedef struct {
    uint32_t params[13];
    KObject *event;
    uint64_t frames;
    int started;
} Renderer;

static void *renderer_clock(void *p) {
    Renderer *r = p;
    for (;;) {
        usleep(5000);
        if (r->started) kobj_signal(r->event);
    }
    return NULL;
}

static void renderer_update(Renderer *r, Buf in, Buf out) {
    const uint32_t *h = (const uint32_t *)in.addr;
    if (getenv("AUDIO_TRACE"))
        fprintf(stderr, "[audren] update rev=%.4s behav=%#x pools=%#x voices=%#x vres=%#x fx=%#x mix=%#x sinks=%#x perf=%#x total=%#x\n",
                (const char *)h, h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[15]);
    uint32_t pools = h[2] / 0x20, voices = h[3] / 0x170, effects = h[5] / 0xc0, sinks = h[7] / 0x140;
    uint8_t *o = (uint8_t *)out.addr;
    memset(o, 0, out.size);
    uint32_t *oh = (uint32_t *)o;
    oh[0] = h[0];
    oh[1] = 0xb0;
    oh[2] = pools * 0x10;
    oh[3] = voices * 0x10;
    oh[5] = effects * 0x10;
    oh[7] = sinks * 0x20;
    oh[8] = 0x10;
    oh[10] = 0x10;
    uint8_t *p = o + 0x40;
    const uint8_t *ip = (const uint8_t *)in.addr + 0x40 + h[1];
    for (uint32_t i = 0; i < pools; i++) {
        uint32_t state = *(const uint32_t *)(ip + i * 0x20 + 0x10);
        *(uint32_t *)(p + i * 0x10) = state == 4 ? 5 : state == 2 ? 3 : state;
    }
    p = o + 0x40 + oh[2];
    p += oh[3] + oh[5] + oh[7];
    p += oh[8];
    p += oh[1];
    *(uint64_t *)p = r->frames++;
    p += oh[10];
    oh[15] = (uint32_t)(p - o);
}

SVC(renderer, "audren:IAudioRenderer") {
    UNUSED;
    Renderer *r = st;
    switch (rq->cmd) {
    case 0: OUT(uint32_t, r->params[0]); return;
    case 1: OUT(uint32_t, r->params[1]); return;
    case 2: OUT(uint32_t, r->params[2]); return;
    case 3: OUT(uint32_t, r->started ? 0 : 1); return;
    case 4: case 10: renderer_update(r, buf_in(rq, 0), buf_out(rq, 0)); return;
    case 5: r->started = 1; return;
    case 6: r->started = 0; return;
    case 7: rs_copy(rs, r->event); return;
    case 8: return;
    case 9: OUT(uint32_t, 100); return;
    }
    UNHANDLED();
}

SVC(audio_device, "audren:IAudioDevice") {
    UNUSED;
    static const char dev[0x100] = "AudioTvOutput";
    switch (rq->cmd) {
    case 0: case 6: rs_buf(buf_out(rq, 0), dev, sizeof dev); OUT(uint32_t, 1); return;
    case 1: case 7: return;
    case 2: case 8: OUT(float, 1.0f); return;
    case 3: case 9: rs_buf(buf_out(rq, 0), dev, sizeof dev); return;
    case 4: case 5: case 11: case 12: rs_copy(rs, event_new()); return;
    case 10: OUT(uint32_t, 2); return;
    case 13: OUT(uint32_t, 0); return;
    }
    UNHANDLED();
}

SVC(audren, "audren:u") {
    UNUSED;
    switch (rq->cmd) {
    case 0: {
        Renderer *r = calloc(1, sizeof *r);
        memcpy(r->params, rq_in(rq, 0), 0x34);
        r->event = event_new();
        pthread_t t;
        pthread_create(&t, NULL, renderer_clock, r);
        if (getenv("AUDIO_TRACE")) fprintf(stderr, "[audren] open rate=%u samples=%u mix=%u voices=%u sinks=%u fx=%u rev=%.4s\n",
                                           r->params[0], r->params[1], r->params[2], r->params[4], r->params[5], r->params[6], (char *)&r->params[12]);
        rs_object(s, rs, &renderer, r);
        return;
    }
    case 1: OUT(uint64_t, 0x80000); return;
    case 2: case 4: rs_object(s, rs, &audio_device, NULL); return;
    }
    UNHANDLED();
}
