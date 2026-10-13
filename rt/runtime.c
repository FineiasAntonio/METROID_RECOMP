#define _GNU_SOURCE
#include <elf.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include "runtime.h"
#include "kernel.h"
#include "table.h"
#include "../build/lifted/layout.h"

extern const FuncEntry rtld_funcs[], main_funcs[], subsdk0_funcs[], sdk_funcs[];
extern const unsigned long rtld_nfuncs, main_nfuncs, subsdk0_nfuncs, sdk_nfuncs;

Module modules[NMOD] = {
    {"rtld", BASE_RTLD, rtld_funcs, &rtld_nfuncs},
    {"main", BASE_MAIN, main_funcs, &main_nfuncs},
    {"subsdk0", BASE_SUBSDK0, subsdk0_funcs, &subsdk0_nfuncs},
    {"sdk", BASE_SDK, sdk_funcs, &sdk_nfuncs},
};

void guest_backtrace(CPU *c) {
    uint64_t fp = c->x[29];
    fprintf(stderr, "  backtrace: lr=%#lx", c->x[30]);
    for (int i = 0; i < 24 && fp && !(fp & 7); i++) {
        uint64_t lr = M64(fp + 8);
        fprintf(stderr, " <- %#lx", lr);
        uint64_t nfp = M64(fp);
        if (nfp <= fp) break;
        fp = nfp;
    }
    fputc('\n', stderr);
}

static CPU *die_cpu;
static void die(const char *fmt, uint64_t a, uint64_t b) {
    if (die_cpu) guest_backtrace(die_cpu);
    fprintf(stderr, fmt, a, b);
    fputc('\n', stderr);
    abort();
}

guest_fn lookup(uint64_t addr) {
    for (int m = 0; m < NMOD; m++) {
        const FuncEntry *t = modules[m].funcs;
        unsigned long lo = 0, hi = *modules[m].nfuncs;
        if (addr < modules[m].base || addr >= modules[m].end) continue;
        while (lo < hi) {
            unsigned long mid = (lo + hi) / 2;
            if (t[mid].addr < addr) lo = mid + 1;
            else hi = mid;
        }
        if (lo < *modules[m].nfuncs && t[lo].addr == addr) return t[lo].fn;
    }
    return NULL;
}

void dispatch(CPU *c, uint64_t target) {
    static uint64_t trace_lr = 1;
    if (trace_lr == 1) trace_lr = getenv("TRACE_LR") ? strtoull(getenv("TRACE_LR"), NULL, 16) : 0;
    if (trace_lr && c->x[30] == trace_lr) fprintf(stderr, "[dispatch] -> %#lx (x16=%#lx x0=%#lx x1=%#lx x2=%#lx x3=%#lx)\n", target, c->x[16], c->x[0], c->x[1], c->x[2], c->x[3]);
    guest_fn f = lookup(target);
    if (!f) { die_cpu = c; die("dispatch: sem função em %#lx (lr=%#lx)", target, c->x[30]); }
    f(c);
}

void trace_enter(CPU *c, uint64_t f) {
    fprintf(stderr, "[trace] -> %#lx(x0=%#lx x1=%#lx x2=%#lx x3=%#lx x4=%#lx) lr=%#lx\n", f, c->x[0], c->x[1], c->x[2], c->x[3], c->x[4], c->x[30]);
    if (getenv("TRACE_DEREF") && c->x[0] > 0x1000000) {
        uint64_t off = strtoull(getenv("TRACE_DEREF"), NULL, 0);
        fprintf(stderr, "[trace]    [x0+%#lx] = %#lx\n", off, M64(c->x[0] + off));
    }
}
void trace_exit(CPU *c, uint64_t f) { fprintf(stderr, "[trace] <- %#lx = %#lx\n", f, c->x[0]); }

void unimpl(CPU *c, uint64_t pc, uint32_t word) { (void)c; die("instrução não implementada em %#lx: %#lx", pc, word); }
void guest_brk(CPU *c, uint64_t pc) { (void)c; die("brk em %#lx%.0lu", pc, 0); }

uint64_t sysreg_read(CPU *c, uint32_t key) {
    struct timespec ts;
    switch (key) {
    case 0x5e83: return c->tpidrro;
    case 0x5e82: return c->tpidr;
    case 0x5f00: return 19200000;
    case 0x5f01:
    case 0x5f02:
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 19200000 + (uint64_t)ts.tv_nsec * 192 / 10000;
    case 0x5a20: return c->fpcr;
    case 0x5a21: return c->fpsr;
    case 0x5807: return 4;
    case 0x5801: return 0x8444c004;
    }
    die("mrs sysreg %#lx%.0lu", key, 0);
    return 0;
}

void sysreg_write(CPU *c, uint32_t key, uint64_t v) {
    switch (key) {
    case 0x5e82: c->tpidr = v; return;
    case 0x5a20: c->fpcr = (uint32_t)v; return;
    case 0x5a21: c->fpsr = (uint32_t)v; return;
    }
    die("msr sysreg %#lx = %#lx", key, v);
}

static void *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) die("não abriu nso%.0lu%.0lu", 0, 0);
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    rewind(f);
    void *p = malloc(*n);
    if (fread(p, 1, *n, f) != *n) die("leitura curta%.0lu%.0lu", 0, 0);
    fclose(f);
    return p;
}

static Elf64_Dyn *dynamic(Module *m) {
    uint64_t mod0 = m->base + M32(m->base + 4);
    return (Elf64_Dyn *)(mod0 + (int32_t)M32(mod0 + 4));
}

static uint64_t dyn_get(Module *m, int64_t tag) {
    for (Elf64_Dyn *d = dynamic(m); d->d_tag != DT_NULL; d++)
        if (d->d_tag == tag) return d->d_un.d_val;
    return 0;
}

static uint64_t resolve(const char *name) {
    for (int i = 0; i < NMOD; i++) {
        Module *m = &modules[i];
        Elf64_Sym *sym = (Elf64_Sym *)(m->base + dyn_get(m, DT_SYMTAB));
        const char *str = (const char *)(m->base + dyn_get(m, DT_STRTAB));
        uint32_t *hash = (uint32_t *)(m->base + dyn_get(m, DT_HASH));
        for (uint32_t s = 0; s < hash[1]; s++)
            if (sym[s].st_shndx != SHN_UNDEF && !strcmp(str + sym[s].st_name, name))
                return m->base + sym[s].st_value;
    }
    return 0;
}

static int relocate_table(Module *m, uint64_t off, uint64_t size) {
    int missing = 0;
    Elf64_Sym *sym = (Elf64_Sym *)(m->base + dyn_get(m, DT_SYMTAB));
    const char *str = (const char *)(m->base + dyn_get(m, DT_STRTAB));
    for (Elf64_Rela *r = (Elf64_Rela *)(m->base + off); (uint64_t)r < m->base + off + size; r++) {
        uint64_t *at = (uint64_t *)(m->base + r->r_offset);
        uint32_t type = ELF64_R_TYPE(r->r_info), s = ELF64_R_SYM(r->r_info);
        if (type == R_AARCH64_RELATIVE) { *at = m->base + r->r_addend; continue; }
        if (type != R_AARCH64_GLOB_DAT && type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_ABS64)
            die("reloc tipo %lu em %#lx", type, r->r_offset);
        uint64_t v = sym[s].st_shndx != SHN_UNDEF ? m->base + sym[s].st_value : resolve(str + sym[s].st_name);
        if (!v) missing++;
        *at = v + r->r_addend;
    }
    return missing;
}

int load_modules(const char *dir, int relocate) {
    int missing = 0;
    for (int i = 0; i < NMOD; i++) {
        Module *m = &modules[i];
        char path[512];
        size_t n;
        snprintf(path, sizeof path, "%s/%s.raw", dir, m->name);
        uint8_t *nso = read_file(path, &n);
        uint32_t *seg = (uint32_t *)(nso + 0x10);
        uint32_t mod0 = *(uint32_t *)(nso + seg[0] + 4);
        uint64_t bss_end = mod0 + (int32_t)*(uint32_t *)(nso + seg[0] + mod0 + 12);
        uint64_t size = (bss_end + 0xfff) & ~0xfffull;
        if (mmap((void *)m->base, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0) != (void *)m->base)
            die("mmap falhou em %#lx (%#lx bytes)", m->base, size);
        for (int s = 0; s < 3; s++)
            memcpy((void *)(m->base + seg[s * 4 + 1]), nso + seg[s * 4], seg[s * 4 + 2]);
        region_set(m->base, seg[5], 3, 5);
        region_set(m->base + seg[5], seg[9] - seg[5], 3, 1);
        region_set(m->base + seg[9], size - seg[9], 4, 3);
        m->end = m->base + size;
        free(nso);
    }
    for (int i = 0; relocate && i < NMOD; i++) {
        Module *m = &modules[i];
        missing += relocate_table(m, dyn_get(m, DT_RELA), dyn_get(m, DT_RELASZ));
        missing += relocate_table(m, dyn_get(m, DT_JMPREL), dyn_get(m, DT_PLTRELSZ));
    }
    return missing;
}

uint64_t symbol(const char *name) { return resolve(name); }

void verify_relocs(void) {
    int bad = 0, lazy = 0, ok = 0;
    for (int i = 0; i < NMOD; i++) {
        Module *m = &modules[i];
        Elf64_Sym *sym = (Elf64_Sym *)(m->base + dyn_get(m, DT_SYMTAB));
        const char *str = (const char *)(m->base + dyn_get(m, DT_STRTAB));
        uint64_t tabs[2][2] = {{dyn_get(m, DT_RELA), dyn_get(m, DT_RELASZ)}, {dyn_get(m, DT_JMPREL), dyn_get(m, DT_PLTRELSZ)}};
        for (int t = 0; t < 2; t++)
            for (Elf64_Rela *r = (Elf64_Rela *)(m->base + tabs[t][0]); (uint64_t)r < m->base + tabs[t][0] + tabs[t][1]; r++) {
                uint32_t type = ELF64_R_TYPE(r->r_info), s = ELF64_R_SYM(r->r_info);
                if (type == R_AARCH64_RELATIVE || !s) continue;
                const char *name = str + sym[s].st_name;
                uint64_t want = (sym[s].st_shndx != SHN_UNDEF ? m->base + sym[s].st_value : resolve(name)) + r->r_addend;
                uint64_t got = *(uint64_t *)(m->base + r->r_offset);
                if (got == want) ok++;
                else if (type == R_AARCH64_JUMP_SLOT && got >= m->base && got < m->end && !(want >= m->base && want < m->end)) lazy++;
                else if (want && bad++ < 20) fprintf(stderr, "[reloc] %s: %s em +%#lx = %#lx, esperado %#lx\n", m->name, name, r->r_offset, got, want);
            }
    }
    fprintf(stderr, "[reloc] ok=%d errados=%d lazy=%d | main localeconv slot = %#lx\n", ok, bad, lazy, M64(modules[1].base + 0x1c620a8));
}

CPU *new_cpu(size_t stack) {
    CPU *c = calloc(1, sizeof *c);
    uint8_t *s = mmap(NULL, stack + 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    c->sp = (uint64_t)(s + stack) & ~15ull;
    c->tpidrro = (uint64_t)(s + stack);
    return c;
}

uint64_t call(CPU *c, uint64_t fn, int n, const uint64_t *args) {
    guest_fn f = lookup(fn);
    if (!f) die("call: %#lx não é função%.0lu", fn, 0);
    for (int i = 0; i < n; i++) c->x[i] = args[i];
    c->x[30] = 0;
    f(c);
    return c->x[0];
}
