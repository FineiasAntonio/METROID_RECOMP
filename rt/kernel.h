#pragma once
#include <pthread.h>
#include "cpu.h"

#define MAX_HANDLES 0x8000

enum { KO_THREAD = 1, KO_EVENT, KO_SESSION, KO_TMEM, KO_SHMEM };

typedef struct KObject {
    int type, refs;
    int signaled;
} KObject;

typedef struct Thread {
    KObject obj;
    CPU *c;
    pthread_t host;
    pthread_cond_t cv;
    uint64_t entry, id;
    uint32_t handle;
    int prio, core, woken, cancel;
    uint64_t mutex_wait, cv_wait, cv_mutex, addr_wait;
} Thread;

typedef struct {
    KObject obj;
    uint64_t addr, size;
} TransferMem;

typedef struct {
    KObject obj;
    int fd;
    uint64_t size;
    void *host;
} SharedMem;

typedef struct Session Session;
struct Session {
    KObject obj;
    const struct Service *svc;
    void *state;
    int is_domain;
    void *objects[256];
    const struct Service *object_svcs[256];
};

void kernel_init(void);
void kernel_run_main(uint64_t entry, uint64_t stack_size, int prio, int core);
void region_set(uint64_t addr, uint64_t size, uint32_t state, uint32_t perm);
uint32_t handle_add(KObject *o);
KObject *handle_get(uint32_t h);
void kobj_signal(KObject *o);
void k_exit_thread(void);

Session *session_open_port(const char *name);
uint32_t ipc_dispatch(Session *s, uint64_t tls);
