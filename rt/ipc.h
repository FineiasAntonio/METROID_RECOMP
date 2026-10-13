#pragma once
#include "kernel.h"

typedef struct {
    uint64_t addr, size;
} Buf;

typedef struct {
    uint32_t type, cmd;
    uint64_t pid;
    int ncopy, nmove;
    uint32_t copy[8], move[8];
    int nx, na, nb, nw, nc;
    Buf x[16], a[16], b[16], c[16];
    uint8_t *data;
    uint32_t data_size;
    uint32_t domain_cmd, domain_obj;
} Request;

typedef struct {
    uint32_t result;
    uint8_t data[0x100];
    uint32_t data_size;
    int ncopy, nmove;
    uint32_t copy[8], move[8];
    int nobjs;
    uint32_t objs[8];
} Response;

typedef struct Service {
    const char *name;
    void (*handle)(Session *s, void *state, Request *rq, Response *rs);
} Service;

void *rq_in(Request *rq, uint32_t off);
#define IN(T, off) (*(T *)rq_in(rq, off))
void *rs_out(Response *rs, uint32_t size);
#define OUT(T, v) (*(T *)rs_out(rs, sizeof(T)) = (v))
void rs_object(Session *parent, Response *rs, const Service *svc, void *state);
Session *session_new(const Service *svc, void *state);
uint32_t rs_handle_session(Response *rs, Session *s);
const Service *service_find(const char *name);
void ipc_unhandled(const char *name, Request *rq);
#define UNHANDLED() ipc_unhandled(__func__, rq)

KObject *event_new(void);
SharedMem *shmem_new(uint64_t size);
void rs_copy(Response *rs, KObject *o);
void rs_buf(Buf b, const void *src, uint64_t n);
#define CASE_OK(n) case n: return
Session *stub_session(const char *name);
static inline Buf buf_in(Request *rq, int i) { return rq->na > i && rq->a[i].addr ? rq->a[i] : rq->nx > i ? rq->x[i] : (Buf){0}; }
static inline Buf buf_out(Request *rq, int i) { return rq->nb > i && rq->b[i].addr ? rq->b[i] : rq->nc > i ? rq->c[i] : (Buf){0}; }
