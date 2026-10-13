#include <stdio.h>
#include <stdlib.h>
#include "ipc.h"

static void sm_handle(Session *s, void *st, Request *rq, Response *rs) {
    (void)s; (void)st;
    switch (rq->cmd) {
    case 0:
        return;
    case 1: {
        char name[9] = {0};
        memcpy(name, rq_in(rq, 0), 8);
        const Service *svc = service_find(name);
        if (getenv("IPC_TRACE")) fprintf(stderr, "[sm] GetServiceHandle(%s)\n", name);
        if (!svc) {
            Session *stub = stub_session(name);
            if (stub) { rs_handle_session(rs, stub); return; }
            fprintf(stderr, "[sm] serviço não implementado: '%s'\n", name);
            abort();
        }
        rs_handle_session(rs, session_new(svc, NULL));
        return;
    }
    }
    UNHANDLED();
}

static const Service sm = {"sm:", sm_handle};

static void logger_handle(Session *s, void *st, Request *rq, Response *rs) {
    (void)s; (void)st; (void)rs;
    switch (rq->cmd) {
    case 0: {
        Buf b = buf_in(rq, 0);
        const uint8_t *p = (const uint8_t *)b.addr;
        if (getenv("LM_TRACE")) {
            for (uint64_t i = 0x18; i + 2 <= b.size;) {
                uint8_t key = p[i], len = p[i + 1];
                if (key == 4 && i + 2 + len <= b.size) fprintf(stderr, "[lm] %.*s\n", len, (const char *)p + i + 2);
                i += 2 + len;
            }
        }
        return;
    }
    case 1: return;
    }
    UNHANDLED();
}
static const Service logger = {"lm:ILogger", logger_handle};

static void lm_handle(Session *s, void *st, Request *rq, Response *rs) {
    (void)st;
    switch (rq->cmd) {
    case 0: rs_object(s, rs, &logger, NULL); return;
    }
    UNHANDLED();
}
static const Service lm = {"lm", lm_handle};

extern const Service apm, set, set_sys, time_svc, time_a, time_s, applet_oe, hid, aoc, fsp_srv, ectx_aw, pctl, pctl_a, pctl_r, pctl_s, nvdrv, nvdrv_a, nvdrv_s, nvdrv_t, acc, audout, audren, vi_m, vi_u, vi_s, mm;
const Service *const services[] = {&sm, &lm, &apm, &set, &set_sys, &time_svc, &time_a, &time_s, &applet_oe, &hid, &aoc, &fsp_srv, &ectx_aw, &pctl, &pctl_a, &pctl_r, &pctl_s, &nvdrv, &nvdrv_a, &nvdrv_s, &nvdrv_t, &acc, &audout, &audren, &vi_m, &vi_u, &vi_s, &mm, NULL};
