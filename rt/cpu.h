#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef union {
    uint8_t b[16]; uint16_t h[8]; uint32_t s[4]; uint64_t d[2];
    int8_t sb[16]; int16_t sh[8]; int32_t ss[4]; int64_t sd[2];
    float f[4]; double df[2]; _Float16 hf[8];
} V128;

typedef struct CPU {
    uint64_t x[31];
    uint64_t sp;
    V128 v[32];
    uint8_t nf, zf, cf, vf;
    uint32_t fpcr, fpsr;
    uint64_t tpidr, tpidrro;
    uint64_t excl_addr, excl_val, excl_val2;
} CPU;

typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;
typedef V128 __attribute__((aligned(1), may_alias)) V128u;

#define M8(a) (*(uint8_t *)(uintptr_t)(a))
#define M16(a) (*(u16u *)(uintptr_t)(a))
#define M32(a) (*(u32u *)(uintptr_t)(a))
#define M64(a) (*(u64u *)(uintptr_t)(a))
#define MV(a) (*(V128u *)(uintptr_t)(a))

void dispatch(CPU *c, uint64_t target);
void unimpl(CPU *c, uint64_t pc, uint32_t word);
void svc(CPU *c, uint32_t imm);
void guest_brk(CPU *c, uint64_t pc);
uint64_t sysreg_read(CPU *c, uint32_t key);
void sysreg_write(CPU *c, uint32_t key, uint64_t v);
void trace_enter(CPU *c, uint64_t f);
void trace_exit(CPU *c, uint64_t f);

static inline uint64_t ror64(uint64_t v, unsigned r) { r &= 63; return r ? (v >> r) | (v << (64 - r)) : v; }
static inline uint32_t ror32(uint32_t v, unsigned r) { r &= 31; return r ? (v >> r) | (v << (32 - r)) : v; }

static inline uint64_t adds64(CPU *c, uint64_t a, uint64_t b, uint64_t ci) {
    unsigned __int128 r = (unsigned __int128)a + b + ci;
    uint64_t x = (uint64_t)r;
    c->nf = x >> 63; c->zf = x == 0; c->cf = (uint8_t)(r >> 64);
    c->vf = ((~(a ^ b)) & (a ^ x)) >> 63;
    return x;
}
static inline uint32_t adds32(CPU *c, uint32_t a, uint32_t b, uint32_t ci) {
    uint64_t r = (uint64_t)a + b + ci;
    uint32_t x = (uint32_t)r;
    c->nf = x >> 31; c->zf = x == 0; c->cf = (uint8_t)(r >> 32);
    c->vf = ((~(a ^ b)) & (a ^ x)) >> 31;
    return x;
}
static inline uint64_t logic64(CPU *c, uint64_t x) { c->nf = x >> 63; c->zf = x == 0; c->cf = 0; c->vf = 0; return x; }
static inline uint32_t logic32(CPU *c, uint32_t x) { c->nf = x >> 31; c->zf = x == 0; c->cf = 0; c->vf = 0; return x; }
static inline void setnzcv(CPU *c, unsigned f) { c->nf = f >> 3 & 1; c->zf = f >> 2 & 1; c->cf = f >> 1 & 1; c->vf = f & 1; }

static inline uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
static inline uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static inline uint64_t sdiv64(int64_t a, int64_t b) { return b == 0 ? 0 : (b == -1 ? (uint64_t)0 - (uint64_t)a : (uint64_t)(a / b)); }
static inline uint32_t sdiv32(int32_t a, int32_t b) { return b == 0 ? 0 : (b == -1 ? (uint32_t)0 - (uint32_t)a : (uint32_t)(a / b)); }

static inline uint64_t clz64(uint64_t v) { return v ? __builtin_clzll(v) : 64; }
static inline uint32_t clz32(uint32_t v) { return v ? __builtin_clz(v) : 32; }
static inline uint64_t cls64(uint64_t v) { return clz64((v ^ (uint64_t)((int64_t)v >> 1)) & 0x7fffffffffffffffull) - 1; }
static inline uint32_t cls32(uint32_t v) { return clz32((v ^ (uint32_t)((int32_t)v >> 1)) & 0x7fffffffu) - 1; }
static inline uint64_t rbit64(uint64_t v) {
    v = ((v >> 1) & 0x5555555555555555ull) | ((v & 0x5555555555555555ull) << 1);
    v = ((v >> 2) & 0x3333333333333333ull) | ((v & 0x3333333333333333ull) << 2);
    v = ((v >> 4) & 0x0f0f0f0f0f0f0f0full) | ((v & 0x0f0f0f0f0f0f0f0full) << 4);
    return __builtin_bswap64(v);
}
static inline uint32_t rbit32(uint32_t v) { return (uint32_t)(rbit64(v) >> 32); }
static inline uint64_t rev16_64(uint64_t v) { return ((v >> 8) & 0x00ff00ff00ff00ffull) | ((v & 0x00ff00ff00ff00ffull) << 8); }
static inline uint64_t rev32_64(uint64_t v) { uint64_t b = __builtin_bswap64(v); return (b >> 32) | (b << 32); }

static inline uint64_t ldx64(CPU *c, uint64_t a) { uint64_t v = __atomic_load_n((uint64_t *)a, __ATOMIC_SEQ_CST); c->excl_addr = a; c->excl_val = v; return v; }
static inline uint32_t ldx32(CPU *c, uint64_t a) { uint32_t v = __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST); c->excl_addr = a; c->excl_val = v; return v; }
static inline uint16_t ldx16(CPU *c, uint64_t a) { uint16_t v = __atomic_load_n((uint16_t *)a, __ATOMIC_SEQ_CST); c->excl_addr = a; c->excl_val = v; return v; }
static inline uint8_t ldx8(CPU *c, uint64_t a) { uint8_t v = __atomic_load_n((uint8_t *)a, __ATOMIC_SEQ_CST); c->excl_addr = a; c->excl_val = v; return v; }
#define STX(N, T) static inline uint32_t stx##N(CPU *c, uint64_t a, T v) { \
    if (c->excl_addr != a) return 1; T e = (T)c->excl_val; c->excl_addr = ~0ull; \
    return !__atomic_compare_exchange_n((T *)a, &e, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
STX(64, uint64_t) STX(32, uint32_t) STX(16, uint16_t) STX(8, uint8_t)
static inline void ldxp64(CPU *c, uint64_t a, uint64_t *lo, uint64_t *hi) {
    unsigned __int128 v = __atomic_load_n((unsigned __int128 *)a, __ATOMIC_SEQ_CST);
    *lo = (uint64_t)v; *hi = (uint64_t)(v >> 64); c->excl_addr = a; c->excl_val = *lo; c->excl_val2 = *hi;
}
static inline uint32_t stxp64(CPU *c, uint64_t a, uint64_t lo, uint64_t hi) {
    if (c->excl_addr != a) return 1;
    unsigned __int128 e = (unsigned __int128)c->excl_val2 << 64 | c->excl_val, n = (unsigned __int128)hi << 64 | lo;
    c->excl_addr = ~0ull;
    return !__atomic_compare_exchange_n((unsigned __int128 *)a, &e, n, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static inline void wv64(CPU *c, int d, uint64_t v) { V128 t = {0}; t.d[0] = v; c->v[d] = t; }
static inline void wvS(CPU *c, int d, float v) { V128 t = {0}; t.f[0] = v; c->v[d] = t; }
static inline void wvD(CPU *c, int d, double v) { V128 t = {0}; t.df[0] = v; c->v[d] = t; }
static inline void wvH(CPU *c, int d, _Float16 v) { V128 t = {0}; t.hf[0] = v; c->v[d] = t; }

static inline void fcmp(CPU *c, double a, double b) {
    if (a != a || b != b) setnzcv(c, 3);
    else if (a == b) setnzcv(c, 6);
    else if (a < b) setnzcv(c, 8);
    else setnzcv(c, 2);
}
static inline double fround(double v, int mode) {
    switch (mode) {
    case 0: return nearbyint(v);
    case 1: return ceil(v);
    case 2: return floor(v);
    case 3: return trunc(v);
    default: return round(v);
    }
}
static inline int64_t fcvts64(double v, int mode) {
    if (v != v) return 0; v = fround(v, mode);
    if (v >= 9223372036854775808.0) return INT64_MAX;
    if (v < -9223372036854775808.0) return INT64_MIN;
    return (int64_t)v;
}
static inline uint64_t fcvtu64(double v, int mode) {
    if (v != v) return 0; v = fround(v, mode);
    if (v >= 18446744073709551616.0) return UINT64_MAX;
    if (v <= 0) return 0;
    return (uint64_t)v;
}
static inline int32_t fcvts32(double v, int mode) {
    if (v != v) return 0; v = fround(v, mode);
    if (v >= 2147483648.0) return INT32_MAX;
    if (v < -2147483648.0) return INT32_MIN;
    return (int32_t)v;
}
static inline uint32_t fcvtu32(double v, int mode) {
    if (v != v) return 0; v = fround(v, mode);
    if (v >= 4294967296.0) return UINT32_MAX;
    if (v <= 0) return 0;
    return (uint32_t)v;
}
static inline double fmaxa(double a, double b) { if (a != a || b != b) return a + b; if (a == 0 && b == 0) return signbit(a) ? b : a; return a > b ? a : b; }
static inline double fmina(double a, double b) { if (a != a || b != b) return a + b; if (a == 0 && b == 0) return signbit(a) ? a : b; return a < b ? a : b; }
static inline double fmaxnm(double a, double b) { if (a != a) return b; if (b != b) return a; return fmaxa(a, b); }
static inline double fminnm(double a, double b) { if (a != a) return b; if (b != b) return a; return fmina(a, b); }

#define CRC(NAME, POLY, N) static inline uint32_t NAME##_##N(uint32_t acc, uint64_t v) { \
    for (int i = 0; i < N; i++) { acc ^= (v >> i) & 1; acc = (acc >> 1) ^ (POLY & -(acc & 1)); } return acc; }
CRC(crc32, 0xedb88320u, 8) CRC(crc32, 0xedb88320u, 16) CRC(crc32, 0xedb88320u, 32) CRC(crc32, 0xedb88320u, 64)
CRC(crc32c, 0x82f63b78u, 8) CRC(crc32c, 0x82f63b78u, 16) CRC(crc32c, 0x82f63b78u, 32) CRC(crc32c, 0x82f63b78u, 64)

#include <wmmintrin.h>
#define AESFN __attribute__((target("aes,sse2"))) static inline
AESFN V128 aese(V128 d, V128 k) { V128 r; __m128i x = _mm_loadu_si128((__m128i *)&d) ^ _mm_loadu_si128((__m128i *)&k); _mm_storeu_si128((__m128i *)&r, _mm_aesenclast_si128(x, _mm_setzero_si128())); return r; }
AESFN V128 aesd(V128 d, V128 k) { V128 r; __m128i x = _mm_loadu_si128((__m128i *)&d) ^ _mm_loadu_si128((__m128i *)&k); _mm_storeu_si128((__m128i *)&r, _mm_aesdeclast_si128(x, _mm_setzero_si128())); return r; }
AESFN V128 aesmc(V128 d) { V128 r; __m128i z = _mm_setzero_si128(); _mm_storeu_si128((__m128i *)&r, _mm_aesenc_si128(_mm_aesdeclast_si128(_mm_loadu_si128((__m128i *)&d), z), z)); return r; }
AESFN V128 aesimc(V128 d) { V128 r; _mm_storeu_si128((__m128i *)&r, _mm_aesimc_si128(_mm_loadu_si128((__m128i *)&d))); return r; }

static inline uint32_t recip_est(uint32_t a) { a = a * 2 + 1; uint32_t b = (1u << 19) / a; return (b + 1) / 2; }
static inline uint32_t rsqrt_est(uint32_t a) {
    if (a < 256) a = a * 2 + 1;
    else { a = (a >> 1) << 1; a = (a + 1) * 2; }
    uint64_t b = 512;
    while ((uint64_t)a * (b + 1) * (b + 1) < (1ull << 28)) b++;
    return (uint32_t)((b + 1) / 2);
}
static inline uint64_t frecpe_bits(uint64_t op, int N) {
    int E = N == 32 ? 8 : 11, F = N - 1 - E, bias = (1 << (E - 1)) - 1;
    uint64_t sign = op >> (N - 1) & 1, fracmask = (1ull << F) - 1, expmask = (1ull << E) - 1;
    int64_t exp = (int64_t)(op >> F & expmask);
    uint64_t frac = (op & fracmask) << (52 - F);
    if (exp == (int64_t)expmask) {
        if (op & fracmask) return op | 1ull << (F - 1);
        return sign << (N - 1);
    }
    if (exp == 0 && !frac) return sign << (N - 1) | expmask << F;
    if (exp == 0 && frac < (1ull << 50)) return sign << (N - 1) | expmask << F;
    if (exp == 0) {
        if (!(frac >> 51 & 1)) { exp = -1; frac = (frac << 2) & ((1ull << 52) - 1); }
        else frac = (frac << 1) & ((1ull << 52) - 1);
    }
    uint32_t scaled = (uint32_t)(0x100 | (frac >> 44 & 0xff));
    int64_t rexp = 2 * bias - 1 - exp;
    uint64_t est = recip_est(scaled);
    uint64_t f = (est & 0xff) << 44;
    if (rexp == 0) f = 1ull << 51 | f >> 1;
    else if (rexp == -1) { f = 1ull << 50 | f >> 2; rexp = 0; }
    return sign << (N - 1) | ((uint64_t)rexp & expmask) << F | f >> (52 - F);
}
static inline uint64_t frsqrte_bits(uint64_t op, int N) {
    int E = N == 32 ? 8 : 11, F = N - 1 - E, bias = (1 << (E - 1)) - 1;
    uint64_t sign = op >> (N - 1) & 1, fracmask = (1ull << F) - 1, expmask = (1ull << E) - 1;
    int64_t exp = (int64_t)(op >> F & expmask);
    uint64_t frac = (op & fracmask) << (52 - F);
    if (exp == (int64_t)expmask && (op & fracmask)) return op | 1ull << (F - 1);
    if (exp == 0 && !frac) return sign << (N - 1) | expmask << F;
    if (sign) return expmask << F | 1ull << (F - 1);
    if (exp == (int64_t)expmask) return 0;
    if (exp == 0) {
        while (!(frac >> 51 & 1)) { frac = (frac << 1) & ((1ull << 52) - 1); exp--; }
        frac = (frac << 1) & ((1ull << 52) - 1);
    }
    uint32_t scaled = (exp & 1) ? (uint32_t)(0x80 | (frac >> 45 & 0x7f)) : (uint32_t)(0x100 | (frac >> 44 & 0xff));
    int64_t rexp = (3 * bias - 1 - exp) / 2;
    uint64_t est = rsqrt_est(scaled);
    return ((uint64_t)rexp & expmask) << F | (est & 0xff) << (F - 8);
}
static inline float frecpe32(float x) { uint32_t u; memcpy(&u, &x, 4); u = (uint32_t)frecpe_bits(u, 32); memcpy(&x, &u, 4); return x; }
static inline float frsqrte32(float x) { uint32_t u; memcpy(&u, &x, 4); u = (uint32_t)frsqrte_bits(u, 32); memcpy(&x, &u, 4); return x; }
static inline double frecpe64(double x) { uint64_t u; memcpy(&u, &x, 8); u = frecpe_bits(u, 64); memcpy(&x, &u, 8); return x; }
static inline double frsqrte64(double x) { uint64_t u; memcpy(&u, &x, 8); u = frsqrte_bits(u, 64); memcpy(&x, &u, 8); return x; }
static inline double frecps(double a, double b) { if ((isinf(a) && b == 0) || (a == 0 && isinf(b))) return 2.0; return fma(-a, b, 2.0); }
static inline double frsqrts(double a, double b) { if ((isinf(a) && b == 0) || (a == 0 && isinf(b))) return 1.5; return fma(-a, b, 3.0) / 2; }
static inline float frecpsf(float a, float b) { if ((isinf(a) && b == 0) || (a == 0 && isinf(b))) return 2.0f; return fmaf(-a, b, 2.0f); }
static inline float frsqrtsf(float a, float b) { if ((isinf(a) && b == 0) || (a == 0 && isinf(b))) return 1.5f; return fmaf(-a, b, 3.0f) / 2; }
