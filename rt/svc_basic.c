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

SVC(apm_session, "apm:ISession") {
    UNUSED;
    switch (rq->cmd) {
    CASE_OK(0);
    case 1: OUT(uint32_t, 0x00020003); return;
    }
    UNHANDLED();
}

SVC(apm, "apm") {
    UNUSED;
    switch (rq->cmd) {
    case 0: rs_object(s, rs, &apm_session, NULL); return;
    case 1: OUT(uint32_t, 0); return;
    }
    UNHANDLED();
}

SVC(set, "set") {
    UNUSED;
    switch (rq->cmd) {
    case 0: OUT(uint64_t, 0x53552d6e65ull); return;
    case 1: case 5: {
        static const char codes[][8] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
        int n = rq->cmd == 1 ? 15 : 18;
        rs_buf(buf_out(rq, 0), codes, sizeof codes[0] * n);
        OUT(uint32_t, n);
        return;
    }
    case 3: OUT(uint32_t, 15); return;
    case 6: OUT(uint32_t, 18); return;
    case 4: OUT(uint32_t, 1); return;
    case 8: OUT(uint8_t, 0); return;
    }
    UNHANDLED();
}

SVC(set_sys, "set:sys") {
    UNUSED;
    switch (rq->cmd) {
    case 3: case 4: {
        uint8_t fw[0x100] = {11, 0, 0, 0, 3, 0, 0, 0};
        snprintf((char *)fw + 0x8, 0x20, "NintendoSDK Firmware for NX");
        snprintf((char *)fw + 0x28, 0x40, "11.0.0-1.0");
        snprintf((char *)fw + 0x68, 0x18, "11.0.0");
        snprintf((char *)fw + 0x80, 0x80, "NintendoSDK Firmware for NX 11.0.0-1.0");
        rs_buf(buf_out(rq, 0), fw, sizeof fw);
        return;
    }
    case 23: OUT(uint32_t, 0); return;
    }
    UNHANDLED();
}

static uint64_t posix_now(void) { return (uint64_t)time(NULL); }

SVC(system_clock, "time:ISystemClock") {
    UNUSED;
    switch (rq->cmd) {
    case 0: OUT(uint64_t, posix_now()); return;
    case 2: { uint8_t ctx[0x20] = {0}; memcpy(rs_out(rs, sizeof ctx), ctx, sizeof ctx); return; }
    case 3: return;
    }
    UNHANDLED();
}

SVC(steady_clock, "time:ISteadyClock") {
    UNUSED;
    switch (rq->cmd) {
    case 0: {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        OUT(uint64_t, (uint64_t)ts.tv_sec);
        memset(rs_out(rs, 16), 0, 16);
        return;
    }
    }
    UNHANDLED();
}

static void calendar(Response *rs, uint64_t t) {
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    uint8_t *c = rs_out(rs, 8);
    *(uint16_t *)c = (uint16_t)(tm.tm_year + 1900);
    c[2] = (uint8_t)(tm.tm_mon + 1); c[3] = (uint8_t)tm.tm_mday; c[4] = (uint8_t)tm.tm_hour; c[5] = (uint8_t)tm.tm_min; c[6] = (uint8_t)tm.tm_sec;
    uint8_t *a = rs_out(rs, 0x18);
    memset(a, 0, 0x18);
    *(uint32_t *)a = (uint32_t)tm.tm_wday;
    *(uint32_t *)(a + 4) = (uint32_t)tm.tm_yday;
    snprintf((char *)a + 8, 8, "%s", tm.tm_zone ? tm.tm_zone : "UTC");
    *(int32_t *)(a + 0x14) = (int32_t)tm.tm_gmtoff;
}

SVC(timezone_svc, "time:ITimeZoneService") {
    UNUSED;
    switch (rq->cmd) {
    case 0: { char n[0x24] = "America/Sao_Paulo"; memcpy(rs_out(rs, 0x24), n, 0x24); return; }
    case 100: case 101: calendar(rs, IN(uint64_t, 0)); return;
    }
    UNHANDLED();
}

static SharedMem *time_shmem;

static SharedMem *time_shared(void) {
    if (time_shmem) return time_shmem;
    time_shmem = shmem_new(0x1000);
    uint8_t *m = time_shmem->host;
    static const uint8_t uuid[16] = "DREADRECOMPCLOCK";
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t steady = ts.tv_sec, offset = (int64_t)time(NULL) - steady;
    memcpy(m + 0x10, uuid, 16);
    for (int k = 0; k < 2; k++) {
        uint8_t *ctx = m + (k ? 0x88 : 0x40);
        *(int64_t *)ctx = offset;
        *(int64_t *)(ctx + 8) = steady;
        memcpy(ctx + 0x10, uuid, 16);
    }
    m[0xcc] = 1;
    return time_shmem;
}

SVC(time_svc, "time:u") {
    UNUSED;
    switch (rq->cmd) {
    case 0: case 1: case 4: case 5: rs_object(s, rs, &system_clock, NULL); return;
    case 2: rs_object(s, rs, &steady_clock, NULL); return;
    case 3: rs_object(s, rs, &timezone_svc, NULL); return;
    case 20: rs_copy(rs, &time_shared()->obj); return;
    case 100: case 101: OUT(uint8_t, 1); return;
    }
    UNHANDLED();
}
const Service time_a = {"time:a", time_svc_h};
const Service time_s = {"time:s", time_svc_h};

static KObject *applet_msg_event;

SVC(common_state, "am:ICommonStateGetter") {
    UNUSED;
    switch (rq->cmd) {
    case 0:
        if (!applet_msg_event) { applet_msg_event = event_new(); applet_msg_event->signaled = 1; }
        rs_copy(rs, applet_msg_event);
        return;
    case 1: {
        static int sent;
        if (!sent) { sent = 1; OUT(uint32_t, 0xf); return; }
        applet_msg_event->signaled = 0;
        rs->result = 0x680;
        return;
    }
    case 5: OUT(uint8_t, 1); return;
    case 6: OUT(uint32_t, 0); return;
    case 9: OUT(uint8_t, 1); return;
    case 8: OUT(uint8_t, 0); return;
    case 50: OUT(uint8_t, 0); return;
    case 60: OUT(uint32_t, 1280); OUT(uint32_t, 720); return;
    case 61: rs_copy(rs, event_new()); return;
    case 10: case 66: case 67: case 900: return;
    }
    UNHANDLED();
}

SVC(self_ctrl, "am:ISelfController") {
    UNUSED;
    switch (rq->cmd) {
    case 9: { KObject *e = event_new(); e->signaled = 1; rs_copy(rs, e); return; }
    case 91: rs_copy(rs, event_new()); return;
    case 40: OUT(uint64_t, 1); return;
    case 44: OUT(uint64_t, 1); OUT(uint64_t, 2); return;
    case 69: OUT(uint8_t, 0); return;
    case 1: case 2: case 10: case 11: case 12: case 13: case 14: case 16: case 19: case 50: case 62: case 65: case 68: case 100: case 110: return;
    }
    UNHANDLED();
}

SVC(window_ctrl, "am:IWindowController") {
    UNUSED;
    switch (rq->cmd) {
    case 1: OUT(uint64_t, 0x10); return;
    CASE_OK(10);
    }
    UNHANDLED();
}

typedef struct {
    uint64_t size;
    uint8_t data[];
} AmStorage;

static const uint64_t user_uid[2];

SVC(am_storage_accessor, "am:IStorageAccessor") {
    UNUSED;
    AmStorage *m = st;
    switch (rq->cmd) {
    case 0: OUT(uint64_t, m->size); return;
    case 10: {
        uint64_t off = IN(uint64_t, 0);
        Buf b = buf_in(rq, 0);
        memcpy(m->data + off, (void *)b.addr, off + b.size <= m->size ? b.size : m->size - off);
        return;
    }
    case 11: {
        uint64_t off = IN(uint64_t, 0);
        Buf b = buf_out(rq, 0);
        memcpy((void *)b.addr, m->data + off, off + b.size <= m->size ? b.size : m->size - off);
        return;
    }
    }
    UNHANDLED();
}

SVC(am_storage, "am:IStorage") {
    UNUSED;
    switch (rq->cmd) {
    case 0: rs_object(s, rs, &am_storage_accessor, st); return;
    }
    UNHANDLED();
}

SVC(audio_ctrl, "am:IAudioController") { UNUSED; switch (rq->cmd) { CASE_OK(0); case 1: case 2: OUT(float, 1.0f); return; } UNHANDLED(); }
SVC(display_ctrl, "am:IDisplayController") { UNUSED; UNHANDLED(); }
typedef struct {
    uint32_t applet_id, mode;
    KObject *state_event, *out_event;
    AmStorage *out;
} Applet;

static AmStorage *storage_new(uint64_t size) {
    AmStorage *m = calloc(1, sizeof *m + size);
    m->size = size;
    return m;
}

SVC(applet_accessor, "am:ILibraryAppletAccessor") {
    UNUSED;
    Applet *a = st;
    switch (rq->cmd) {
    case 0: rs_copy(rs, a->state_event); return;
    case 1: OUT(uint8_t, a->state_event->signaled); return;
    case 10: {
        if (getenv("IPC_TRACE")) fprintf(stderr, "[applet] iniciando applet %#x (modo %u)\n", a->applet_id, a->mode);
        if (a->applet_id == 0x0c) {
            a->out = storage_new(0xc);
            a->out->data[0] = 1;
        } else a->out = storage_new(0x10);
        kobj_signal(a->out_event);
        kobj_signal(a->state_event);
        return;
    }
    case 20: case 25: case 30: case 100: case 103: case 150: return;
    case 101:
        if (!a->out) { rs->result = 0x480; return; }
        rs_object(s, rs, &am_storage, a->out);
        a->out = NULL;
        a->out_event->signaled = 0;
        return;
    case 105: case 106: rs_copy(rs, a->out_event); return;
    case 120: OUT(uint32_t, a->applet_id); OUT(uint32_t, a->mode); return;
    }
    UNHANDLED();
}

SVC(lib_creator, "am:ILibraryAppletCreator") {
    UNUSED;
    switch (rq->cmd) {
    case 0: {
        Applet *a = calloc(1, sizeof *a);
        a->applet_id = IN(uint32_t, 0);
        a->mode = IN(uint32_t, 4);
        a->state_event = event_new();
        a->out_event = event_new();
        rs_object(s, rs, &applet_accessor, a);
        return;
    }
    case 10: rs_object(s, rs, &am_storage, storage_new(IN(uint64_t, 0))); return;
    }
    UNHANDLED();
}
SVC(debug_funcs, "am:IDebugFunctions") { UNUSED; UNHANDLED(); }

SVC(app_funcs, "am:IApplicationFunctions") {
    UNUSED;
    switch (rq->cmd) {
    case 1: {
        static int popped;
        if (IN(uint32_t, 0) != 2 || popped) { rs->result = 0x480; return; }
        popped = 1;
        AmStorage *m = calloc(1, sizeof *m + 0x88);
        m->size = 0x88;
        *(uint32_t *)m->data = 0xc79497ca;
        *(uint32_t *)(m->data + 4) = 1;
        memcpy(m->data + 8, user_uid, 16);
        rs_object(s, rs, &am_storage, m);
        return;
    }
    case 20: OUT(uint64_t, 0); return;
    case 21: OUT(uint64_t, 0x53552d6e65ull); return;
    case 23: { char v[0x10] = "1.0.0"; memcpy(rs_out(rs, 0x10), v, 0x10); return; }
    case 26: OUT(uint64_t, 0x40000000); OUT(uint64_t, 0x40000000); return;
    case 40: OUT(uint8_t, 1); return;
    case 50: memset(rs_out(rs, 0x10), 0, 0x10); return;
    case 65: OUT(uint8_t, 0); return;
    case 123: OUT(int32_t, -1); return;
    case 130: case 140: case 150: case 160: rs_copy(rs, event_new()); return;
    case 22: case 25: case 30: case 31: case 32: case 33: case 60: case 66: case 67: case 68: case 90: case 100: case 101: case 102: case 124: case 170: return;
    }
    UNHANDLED();
}

SVC(app_proxy, "am:IApplicationProxy") {
    UNUSED;
    const Service *sub = NULL;
    switch (rq->cmd) {
    case 0: sub = &common_state; break;
    case 1: sub = &self_ctrl; break;
    case 2: sub = &window_ctrl; break;
    case 3: sub = &audio_ctrl; break;
    case 4: sub = &display_ctrl; break;
    case 11: sub = &lib_creator; break;
    case 20: sub = &app_funcs; break;
    case 1000: sub = &debug_funcs; break;
    }
    if (!sub) UNHANDLED();
    rs_object(s, rs, sub, NULL);
}

SVC(applet_oe, "appletOE") {
    UNUSED;
    switch (rq->cmd) {
    case 0: rs_object(s, rs, &app_proxy, NULL); return;
    }
    UNHANDLED();
}

static SharedMem *hid_shmem;
uint64_t hid_buttons;
int32_t hid_sticks[4];

#define NPAD(i) ((uint8_t *)hid_shmem->host + 0x9a00 + (i) * 0x5000)

static void lifo_push(uint8_t *lifo, uint64_t sampling, uint64_t buttons) {
    uint64_t *h = (uint64_t *)lifo;
    h[1] = 17;
    uint64_t tail = (h[2] + 1) % 17;
    uint8_t *e = lifo + 0x20 + tail * 0x30;
    memset(e, 0, 0x30);
    *(uint64_t *)e = sampling;
    *(uint64_t *)(e + 8) = sampling;
    *(uint64_t *)(e + 0x10) = buttons;
    memcpy(e + 0x18, hid_sticks, 16);
    *(uint32_t *)(e + 0x28) = 1;
    __atomic_store_n(&h[2], tail, __ATOMIC_RELEASE);
    if (h[3] < 17) h[3]++;
}

static void *hid_thread(void *p) {
    (void)p;
    uint8_t *np = NPAD(0);
    *(uint32_t *)np = 1;
    *(uint32_t *)(np + 4) = 0;
    uint32_t color[3] = {0, 0xff2d2d2d, 0xffe6e6e6};
    memcpy(np + 0x8, color, 12);
    *(uint32_t *)(np + 0x14) = 2;
    *(uint32_t *)(np + 0x4188) = 1;
    *(uint64_t *)(np + 0x4190) = 1u << 11 | 1u << 13 | 1u << 14 | 1u << 15;
    for (int i = 0; i < 3; i++) *(uint32_t *)(np + 0x419c + i * 4) = 4;
    struct { double t; uint64_t mask; } script[64];
    int nscript = 0;
    const char *ap = getenv("AUTO_PRESS");
    while (ap && *ap && nscript < 64) {
        char *end;
        script[nscript].t = strtod(ap, &end);
        script[nscript].mask = strtoull(end + 1, &end, 0);
        nscript++;
        ap = *end ? end + 1 : end;
    }
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (uint64_t n = 1;; n++) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        double el = (double)(now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;
        uint64_t scripted = 0;
        for (int i = 0; i < nscript; i++)
            if (el >= script[i].t && el < script[i].t + 0.15) {
                scripted |= script[i].mask;
                static int last = -1;
                if (last != i) { last = i; fprintf(stderr, "[hid] pressionando %#lx em %.1fs\n", script[i].mask, el); }
            }
        lifo_push(np + 0x28, n, hid_buttons | scripted);
        usleep(5000);
    }
    return NULL;
}

SVC(applet_resource, "hid:IAppletResource") {
    UNUSED;
    switch (rq->cmd) {
    case 0:
        if (!hid_shmem) {
            hid_shmem = shmem_new(0x40000);
            pthread_t t;
            pthread_create(&t, NULL, hid_thread, NULL);
        }
        rs_copy(rs, &hid_shmem->obj);
        return;
    }
    UNHANDLED();
}

SVC(vibration_list, "hid:IActiveVibrationDeviceList") {
    UNUSED;
    switch (rq->cmd) {
    case 0: return;
    }
    UNHANDLED();
}

static KObject *npad_style_event;

SVC(hid, "hid") {
    UNUSED;
    switch (rq->cmd) {
    case 0: rs_object(s, rs, &applet_resource, NULL); return;
    case 1: case 11: case 21: case 31: case 66: case 67: case 69: case 70: case 72: case 79:
    case 100: case 102: case 103: case 104: case 107: case 109: case 120: case 122: case 123: case 124: case 125: case 126: case 127:
    case 128: case 130: case 132: case 134: case 201: case 206: case 209: case 210:
    case 300: case 301: case 302: case 303: case 304: case 305: case 306: case 307: case 308: case 309: case 310:
        return;
    case 68: case 82: case 131: case 211: OUT(uint8_t, 0); return;
    case 101: OUT(uint32_t, 0x1f); return;
    case 106:
        if (!npad_style_event) { npad_style_event = event_new(); npad_style_event->signaled = 1; }
        rs_copy(rs, npad_style_event);
        return;
    case 108: OUT(uint64_t, 0); return;
    case 121: case 129: OUT(uint64_t, 0); return;
    case 133: OUT(uint8_t, 0); OUT(uint32_t, 0); return;
    case 200: OUT(uint32_t, 1); OUT(uint32_t, 0); return;
    case 202: OUT(float, 0); OUT(float, 160); OUT(float, 0); OUT(float, 320); return;
    case 203: rs_object(s, rs, &vibration_list, NULL); return;
    }
    UNHANDLED();
}

SVC(aoc, "aoc:u") {
    UNUSED;
    switch (rq->cmd) {
    case 2: OUT(uint32_t, 0); return;
    case 3: OUT(uint32_t, 0); return;
    case 5: OUT(uint64_t, 0x010093801237d000ull); return;
    case 7: case 9: case 10: return;
    case 8: rs_copy(rs, event_new()); return;
    }
    UNHANDLED();
}

SVC(pctl_service, "pctl:IParentalControlService") {
    UNUSED;
    switch (rq->cmd) {
    case 1: case 1001: case 1002: case 1003: case 1006: case 1013: case 1017: case 1018: case 1046: case 1047: case 1061: return;
    case 1031: case 1032: case 1033: case 1034: case 1035: case 1036: case 1037: case 1038: case 1039: case 1040: case 1043: case 1045:
        OUT(uint64_t, 0); return;
    case 1007: case 1008: case 1009: case 1010: case 1011: case 1012: case 1014: case 1015: case 1016: case 1048: case 1049: return;
    }
    UNHANDLED();
}

SVC(pctl, "pctl") {
    UNUSED;
    switch (rq->cmd) {
    case 0: case 1: rs_object(s, rs, &pctl_service, NULL); return;
    }
    UNHANDLED();
}
const Service pctl_a = {"pctl:a", pctl_h}, pctl_r = {"pctl:r", pctl_h}, pctl_s = {"pctl:s", pctl_h};

static const uint64_t user_uid[2] = {0x0000000000000001ull, 0x46494e4549415321ull};

static void profile_base(Response *rs) {
    uint8_t *p = rs_out(rs, 0x38);
    memset(p, 0, 0x38);
    memcpy(p, user_uid, 16);
    *(uint64_t *)(p + 0x10) = (uint64_t)time(NULL);
    snprintf((char *)p + 0x18, 0x20, "%s", getenv("DREAD_USER") ? getenv("DREAD_USER") : "Fineias");
}

SVC(acc_profile, "acc:IProfile") {
    UNUSED;
    switch (rq->cmd) {
    case 0:
        profile_base(rs);
        { Buf b = buf_out(rq, 0); if (b.addr) memset((void *)b.addr, 0, b.size); }
        return;
    case 1: profile_base(rs); return;
    case 10: OUT(uint32_t, 0); return;
    case 11: OUT(uint32_t, 0); return;
    }
    UNHANDLED();
}

SVC(acc_manager, "acc:IManagerForApplication") {
    UNUSED;
    switch (rq->cmd) {
    case 0: OUT(uint8_t, 0); return;
    case 1: OUT(uint64_t, 0x46494e4549415321ull); return;
    case 3: case 130: rs->result = 0x7c7c; return;
    }
    UNHANDLED();
}

SVC(acc, "acc:u0") {
    UNUSED;
    switch (rq->cmd) {
    case 0: OUT(int32_t, 1); return;
    case 1: OUT(uint8_t, IN(uint64_t, 0) == user_uid[0] && IN(uint64_t, 8) == user_uid[1]); return;
    case 2: case 3: {
        Buf b = buf_out(rq, 0);
        memset((void *)b.addr, 0, b.size);
        rs_buf(b, user_uid, 16);
        return;
    }
    case 4: case 51: memcpy(rs_out(rs, 16), user_uid, 16); return;
    case 5: rs_object(s, rs, &acc_profile, NULL); return;
    case 50: case 150: OUT(uint8_t, 0); return;
    case 100: case 140: case 160: return;
    case 101: rs_object(s, rs, &acc_manager, NULL); return;
    }
    UNHANDLED();
}

SVC(mm, "mm:u") {
    UNUSED;
    switch (rq->cmd) {
    case 0: case 1: case 2: case 5: case 6: return;
    case 3: case 7: OUT(uint32_t, 0); return;
    case 4: OUT(uint32_t, 1); return;
    }
    UNHANDLED();
}
