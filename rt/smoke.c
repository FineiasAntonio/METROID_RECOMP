#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "runtime.h"

static CPU *c;
static int fails;

static uint64_t g(const char *name, int n, ...) {
    uint64_t args[8];
    __builtin_va_list ap;
    __builtin_va_start(ap, n);
    for (int i = 0; i < n; i++) args[i] = __builtin_va_arg(ap, uint64_t);
    __builtin_va_end(ap);
    uint64_t f = symbol(name);
    assert(f);
    return call(c, f, n, args);
}

static float gf(const char *name, float a, float b) {
    c->v[0].f[0] = a;
    c->v[1].f[0] = b;
    call(c, symbol(name), 0, NULL);
    return c->v[0].f[0];
}

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FALHOU: " __VA_ARGS__); puts(""); } else printf("ok  " #cond "\n"); } while (0)

int main(int argc, char **argv) {
    int missing = load_modules(argc > 1 ? argv[1] : "extracted/nso", 1);
    printf("módulos carregados, %d símbolos importados não resolvidos\n", missing);
    c = new_cpu(1 << 20);

    const char *s = "Metroid Dread";
    CHECK(g("strlen", 1, (uint64_t)s) == strlen(s), "strlen");
    CHECK((int)g("strcmp", 2, (uint64_t)"samus", (uint64_t)"samus") == 0, "strcmp eq");
    CHECK((int)g("strcmp", 2, (uint64_t)"emmi", (uint64_t)"samus") < 0, "strcmp lt");
    CHECK((int)g("strncmp", 3, (uint64_t)"kraid1", (uint64_t)"kraid2", 5ull) == 0, "strncmp");
    CHECK((int)g("memcmp", 3, (uint64_t)"abcz", (uint64_t)"abcd", 4ull) > 0, "memcmp");
    CHECK(g("strchr", 2, (uint64_t)s, (uint64_t)'D') == (uint64_t)(s + 8), "strchr");
    CHECK((int)g("atoi", 1, (uint64_t)"  -1234x") == -1234, "atoi");

    static char buf[300];
    memset(buf, 'x', sizeof buf);
    g("memset", 3, (uint64_t)(buf + 3), 0x41ull, 257ull);
    int okset = buf[2] == 'x' && buf[3] == 'A' && buf[259] == 'A' && buf[260] == 'x';
    CHECK(okset, "memset");

    char dst[32];
    g("strcpy", 2, (uint64_t)dst, (uint64_t)s);
    CHECK(!strcmp(dst, s), "strcpy");

    float xs[] = {0.0f, 0.5f, 1.0f, 2.5f, -3.0f, 10.0f};
    for (unsigned i = 0; i < sizeof xs / sizeof *xs; i++) {
        float x = xs[i];
        CHECK(fabsf(gf("sinf", x, 0) - sinf(x)) <= 1e-6f, "sinf(%g)", x);
        CHECK(fabsf(gf("cosf", x, 0) - cosf(x)) <= 1e-6f, "cosf(%g)", x);
        CHECK(fabsf(gf("expf", x, 0) - expf(x)) <= 1e-6f * expf(x), "expf(%g)", x);
        CHECK(fabsf(gf("atan2f", x, 1.5f) - atan2f(x, 1.5f)) <= 1e-6f, "atan2f(%g)", x);
    }
    CHECK(fabsf(gf("logf", 7.0f, 0) - logf(7.0f)) <= 1e-6f, "logf");
    CHECK(fabsf(gf("powf", 2.0f, 10.0f) - 1024.0f) <= 1e-3f, "powf");
    CHECK(gf("sqrtf", 2.0f, 0) == sqrtf(2.0f), "sqrtf");

    char out[64] = {0};
    g("snprintf", 6, (uint64_t)out, (uint64_t)sizeof out, (uint64_t)"%s %d %x", (uint64_t)"Dread", 2021ull, 255ull);
    CHECK(!strcmp(out, "Dread 2021 ff"), "snprintf -> '%s'", out);

    printf(fails ? "%d falhas\n" : "todos os testes passaram\n", fails);
    return fails != 0;
}
