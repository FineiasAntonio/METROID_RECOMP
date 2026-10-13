#pragma once
#include "cpu.h"
#include "table.h"

#define NMOD 4
typedef void (*guest_fn)(CPU *);
typedef struct {
    const char *name;
    uint64_t base;
    const FuncEntry *funcs;
    const unsigned long *nfuncs;
    uint64_t end;
} Module;

extern Module modules[NMOD];
int load_modules(const char *dir, int relocate);
uint64_t symbol(const char *name);
guest_fn lookup(uint64_t addr);
CPU *new_cpu(size_t stack);
uint64_t call(CPU *c, uint64_t fn, int n, const uint64_t *args);
void guest_backtrace(CPU *c);
void verify_relocs(void);
