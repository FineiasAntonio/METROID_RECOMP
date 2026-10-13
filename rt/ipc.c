#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "ipc.h"

KObject *event_new(void) {
    KObject *o = calloc(1, sizeof *o);
    o->type = KO_EVENT;
    return o;
}

SharedMem *shmem_new(uint64_t size) {
    SharedMem *m = calloc(1, sizeof *m);
    m->obj.type = KO_SHMEM;
    m->size = size;
    m->fd = memfd_create("shmem", 0);
    if (m->fd < 0 || ftruncate(m->fd, (off_t)size)) abort();
    m->host = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, m->fd, 0);
    return m;
}

void rs_copy(Response *rs, KObject *o) { rs->copy[rs->ncopy++] = handle_add(o); }

void rs_buf(Buf b, const void *src, uint64_t n) { memcpy((void *)b.addr, src, n < b.size ? n : b.size); }


extern const Service *const services[];

const Service *service_find(const char *name) {
    for (int i = 0; services[i]; i++)
        if (!strcmp(services[i]->name, name)) return services[i];
    return NULL;
}

Session *session_new(const Service *svc, void *state) {
    Session *s = calloc(1, sizeof *s);
    s->obj.type = KO_SESSION;
    s->svc = svc;
    s->state = state;
    return s;
}

Session *session_open_port(const char *name) {
    const Service *svc = service_find(name);
    if (!svc) {
        fprintf(stderr, "[ipc] porta desconhecida '%s'\n", name);
        return NULL;
    }

    return session_new(svc, NULL);
}

void *rq_in(Request *rq, uint32_t off) {
    if (off + 1 > rq->data_size) {
        fprintf(stderr, "[ipc] leitura fora do payload (%u >= %u)\n", off, rq->data_size);
        abort();
    }
    return rq->data + off;
}

void *rs_out(Response *rs, uint32_t size) {
    void *p = rs->data + rs->data_size;
    rs->data_size += (size + 3) & ~3u;
    return p;
}

uint32_t rs_handle_session(Response *rs, Session *s) {
    uint32_t h = handle_add(&s->obj);
    rs->move[rs->nmove++] = h;
    return h;
}

void rs_object(Session *parent, Response *rs, const Service *svc, void *state) {
    if (parent->is_domain) {
        for (int i = 1; i < 256; i++)
            if (!parent->object_svcs[i]) {
                parent->object_svcs[i] = svc;
                parent->objects[i] = state;
                rs->objs[rs->nobjs++] = i;
                return;
            }
        fprintf(stderr, "[ipc] domínio cheio\n");
        abort();
    }
    rs_handle_session(rs, session_new(svc, state));
}

static int stub_mode(void) { return getenv("STUB_UNKNOWN") != NULL; }

void ipc_unhandled(const char *name, Request *rq) {
    if (stub_mode()) {
        fprintf(stderr, "[stub] %s cmd %u\n", name, rq->cmd);
        return;
    }
    fprintf(stderr, "[ipc] %s: comando %u não implementado (tipo %u, %u bytes, x=%d a=%d b=%d c=%d)\n",
            name, rq->cmd, rq->type, rq->data_size, rq->nx, rq->na, rq->nb, rq->nc);
    abort();
}

static void control(Session *s, Request *rq, Response *rs) {
    switch (rq->cmd) {
    case 0:
        s->is_domain = 1;
        s->object_svcs[1] = s->svc;
        s->objects[1] = s->state;
        OUT(uint32_t, 1);
        return;
    case 2:
    case 4:
        rs->move[rs->nmove++] = handle_add(&s->obj);
        return;
    case 3:
        OUT(uint16_t, 0x8000);
        return;
    }
    fprintf(stderr, "[ipc] controle %u não implementado\n", rq->cmd);
    abort();
}

uint32_t ipc_dispatch(Session *s, uint64_t tls) {
    uint32_t *m = (uint32_t *)tls;
    Request rq = {0};
    Response rs = {0};
    if (getenv("IPC_DUMP")) { fprintf(stderr, "[ipc] msg:"); for (int i = 0; i < 16; i++) fprintf(stderr, " %08x", m[i]); fputc('\n', stderr); }
    rq.type = m[0] & 0xffff;
    rq.nx = m[0] >> 16 & 0xf;
    rq.na = m[0] >> 20 & 0xf;
    rq.nb = m[0] >> 24 & 0xf;
    rq.nw = m[0] >> 28 & 0xf;
    uint32_t raw_words = m[1] & 0x3ff, cflags = m[1] >> 10 & 0xf;
    uint32_t *p = m + 2;
    if (m[1] >> 31) {
        uint32_t hd = *p++;
        if (hd & 1) { rq.pid = *(uint64_t *)p; p += 2; }
        rq.ncopy = hd >> 1 & 0xf;
        rq.nmove = hd >> 5 & 0xf;
        for (int i = 0; i < rq.ncopy; i++) rq.copy[i] = *p++;
        for (int i = 0; i < rq.nmove; i++) rq.move[i] = *p++;
    }
    for (int i = 0; i < rq.nx; i++, p += 2) {
        rq.x[i].addr = p[1] | (uint64_t)(p[0] >> 12 & 0xf) << 32 | (uint64_t)(p[0] >> 6 & 7) << 36;
        rq.x[i].size = p[0] >> 16;
    }
    Buf *abw[3] = {rq.a, rq.b, NULL};
    int nabw[3] = {rq.na, rq.nb, rq.nw};
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < nabw[k]; i++, p += 3) {
            Buf b = {p[1] | (uint64_t)(p[2] >> 28 & 0xf) << 32 | (uint64_t)(p[2] >> 2 & 7) << 36,
                     p[0] | (uint64_t)(p[2] >> 24 & 0xf) << 32};
            if (abw[k]) abw[k][i] = b;
        }
    uint32_t raw_off = (uint32_t)((uint8_t *)p - (uint8_t *)m);
    uint32_t *raw = (uint32_t *)((uint8_t *)m + ((raw_off + 15) & ~15u));
    uint32_t *cdesc = m + ((uint8_t *)p - (uint8_t *)m) / 4 + raw_words;
    if (cflags >= 2) {
        rq.nc = cflags == 2 ? 1 : (int)cflags - 2;
        for (int i = 0; i < rq.nc && i < 16; i++) {
            rq.c[i].addr = cdesc[i * 2] | (uint64_t)(cdesc[i * 2 + 1] & 0xffff) << 32;
            rq.c[i].size = cdesc[i * 2 + 1] >> 16;
        }
    }
    if (rq.type == 2) return 0xf601;
    const Service *svc = s->svc;
    void *state = s->state;
    int domain = s->is_domain && (rq.type == 4 || rq.type == 6);
    uint32_t *hdr = raw;
    if (domain) {
        rq.domain_cmd = raw[0] & 0xff;
        rq.domain_obj = raw[1];
        uint32_t nin = raw[0] >> 8 & 0xff;
        (void)nin;
        hdr = raw + 4;
        if (rq.domain_cmd == 2) {
            s->object_svcs[rq.domain_obj] = NULL;
            s->objects[rq.domain_obj] = NULL;
            goto respond;
        }
        if (rq.domain_obj) {
            svc = s->object_svcs[rq.domain_obj];
            state = s->objects[rq.domain_obj];
            if (!svc) { fprintf(stderr, "[ipc] objeto de domínio %u inválido\n", rq.domain_obj); abort(); }
        }
    }
    rq.cmd = hdr[2];
    rq.data = (uint8_t *)(hdr + 4);
    uint8_t *raw_end = (uint8_t *)m + raw_off + raw_words * 4;
    rq.data_size = raw_end > rq.data ? (uint32_t)(raw_end - rq.data) : 0;
    if (rq.type == 5 || rq.type == 7) control(s, &rq, &rs);
    else {
        if (getenv("IPC_TRACE")) fprintf(stderr, "[ipc] %s cmd %u\n", svc->name, rq.cmd);
        svc->handle(s, state, &rq, &rs);
    }
respond:;
    uint32_t *o = m;
    memset(m, 0, 0x100);
    int hd = rs.ncopy || rs.nmove;
    o[0] = 0;
    o += 2;
    if (hd) {
        *o++ = (uint32_t)rs.ncopy << 1 | (uint32_t)rs.nmove << 5;
        for (int i = 0; i < rs.ncopy; i++) *o++ = rs.copy[i];
        for (int i = 0; i < rs.nmove; i++) *o++ = rs.move[i];
    }
    uint32_t pre = (uint32_t)((uint8_t *)o - (uint8_t *)m);
    uint32_t *r = (uint32_t *)((uint8_t *)m + ((pre + 15) & ~15u));
    uint32_t *body = r;
    if (domain) {
        r[0] = rs.nobjs;
        body = r + 4;
    }
    body[0] = 0x4f434653;
    body[1] = 0;
    body[2] = rs.result;
    body[3] = 0;
    memcpy(body + 4, rs.data, rs.data_size);
    uint32_t *end = body + 4 + rs.data_size / 4;
    if (domain)
        for (int i = 0; i < rs.nobjs; i++) *end++ = rs.objs[i];
    uint32_t words = (uint32_t)(end - o) + 4;
    m[1] = words | (hd ? 1u << 31 : 0);
    return 0;
}

static void stub_h(Session *s, void *st, Request *rq, Response *rs) {
    (void)st;
    fprintf(stderr, "[stub] %s cmd %u\n", (const char *)s->state, rq->cmd);
    memset(rs_out(rs, 0x20), 0, 0x20);
}
static const Service stub_svc = {"stub", stub_h};

Session *stub_session(const char *name) {
    if (!stub_mode()) return NULL;
    fprintf(stderr, "[stub] serviço %s\n", name);
    return session_new(&stub_svc, strdup(name));
}
