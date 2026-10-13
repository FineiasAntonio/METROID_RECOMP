#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sass_ops.h"
#include "shader.h"

#define RZ 255

typedef struct {
    char *s;
    size_t n, cap;
} Str;

static void emit(Str *b, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int k = vsnprintf(b->s + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (b->n + (size_t)k + 1 < b->cap) { b->n += (size_t)k; return; }
        b->cap = b->cap * 2 + 4096;
        b->s = realloc(b->s, b->cap);
    }
}

static uint64_t bf(uint64_t w, int lo, int n) { return (w >> lo) & ((1ull << n) - 1); }

int sass_decode(uint64_t w) {
    for (size_t i = 0; i < sizeof sass_ops / sizeof *sass_ops; i++)
        if ((w & sass_ops[i].mask) == sass_ops[i].val) return sass_ops[i].op;
    return OP_INVALID;
}

const char *sass_name(int op) {
    for (size_t i = 0; i < sizeof sass_ops / sizeof *sass_ops; i++)
        if (sass_ops[i].op == op) return sass_ops[i].name;
    return "???";
}

typedef struct {
    ShaderInfo *info;
    Str *b;
    uint32_t regs_used[8];
    char tmp[32][1024];
    int tmpi;
} Ctx;

static const char *T(Ctx *c, const char *fmt, ...) {
    char *t = c->tmp[c->tmpi++ & 31];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, 1024, fmt, ap);
    va_end(ap);
    return t;
}

static const char *R(Ctx *c, unsigned r) {
    if (r == RZ) return "0u";
    c->regs_used[r / 32] |= 1u << (r % 32);
    return T(c, "r%u", r);
}

static const char *F(Ctx *c, const char *u) { return T(c, "uintBitsToFloat(%s)", u); }

static const char *cbuf(Ctx *c, uint64_t w) {
    unsigned idx = (unsigned)bf(w, 34, 5), off = (unsigned)bf(w, 20, 14) * 4;
    c->info->cbuf_mask |= 1u << idx;
    return T(c, "cb%u.d[%u][%u]", idx, off >> 4, (off >> 2) & 3);
}

static const char *imm19f(Ctx *c, uint64_t w) {
    uint32_t v = (uint32_t)(bf(w, 20, 19) << 12) | (uint32_t)(bf(w, 56, 1) << 31);
    return T(c, "%#xu", v);
}

static const char *imm19i(Ctx *c, uint64_t w) {
    uint32_t v = (uint32_t)bf(w, 20, 19) | (bf(w, 56, 1) ? 0xfff80000u : 0);
    return T(c, "%#xu", v);
}

static const char *opb(Ctx *c, uint64_t w, int kind, int fp) {
    switch (kind) {
    case 0: return R(c, (unsigned)bf(w, 20, 8));
    case 1: return cbuf(c, w);
    default: return fp ? imm19f(c, w) : imm19i(c, w);
    }
}

static const char *fmod_(Ctx *c, const char *f, int neg, int abs_) {
    const char *x = abs_ ? T(c, "abs(%s)", f) : f;
    return neg ? T(c, "(-%s)", x) : x;
}

static const char *sat(Ctx *c, const char *f, int s) { return s ? T(c, "clamp(%s, 0.0, 1.0)", f) : f; }

static void set_reg(Ctx *c, unsigned d, const char *val) {
    if (d == RZ) return;
    emit(c->b, "  %s = %s;\n", R(c, d), val);
}

static void set_f(Ctx *c, unsigned d, const char *f) { set_reg(c, d, T(c, "floatBitsToUint(%s)", f)); }

static const char *pred(Ctx *c, unsigned p, int neg) {
    const char *v = p == 7 ? "true" : T(c, "p%u", p);
    return neg ? T(c, "!(%s)", v) : v;
}

static const char *fcmp(Ctx *c, unsigned op, const char *a, const char *b) {
    static const char *ord[] = {"false", "<", "==", "<=", ">", "!=", ">=", "", "", "<", "==", "<=", ">", "!=", ">=", "true"};
    if (op == 0) return "false";
    if (op == 15) return "true";
    if (op == 7) return T(c, "(!isnan(%s) && !isnan(%s))", a, b);
    if (op == 8) return T(c, "(isnan(%s) || isnan(%s))", a, b);
    if (op < 7) return T(c, "(%s %s %s)", a, ord[op], b);
    return T(c, "(isnan(%s) || isnan(%s) || %s %s %s)", a, b, a, ord[op], b);
}

static const char *bop(Ctx *c, unsigned op, const char *a, const char *b) {
    static const char *o[] = {"&&", "||", "^^", "&&"};
    return T(c, "(%s %s %s)", a, o[op & 3], b);
}

static void emit_attr_store(Ctx *c, uint32_t addr, const char *val) {
    if (addr >= 0x70 && addr < 0x80) {
        emit(c->b, "  gl_Position[%u] = uintBitsToFloat(%s);\n", (addr - 0x70) / 4, val);
        return;
    }
    if (addr >= 0x80 && addr < 0x280) {
        unsigned i = (addr - 0x80) / 16, comp = (addr / 4) & 3;
        c->info->attr_out |= 1u << i;
        emit(c->b, "  out_%u[%u] = uintBitsToFloat(%s);\n", i, comp, val);
        return;
    }
    if (addr == 0x6c) { emit(c->b, "  gl_PointSize = uintBitsToFloat(%s);\n", val); return; }
    emit(c->b, "  /* AST %#x ignorado */\n", addr);
}

static const char *attr_load(Ctx *c, uint32_t addr) {
    if (addr >= 0x80 && addr < 0x280) {
        unsigned i = (addr - 0x80) / 16, comp = (addr / 4) & 3;
        c->info->attr_in |= 1u << i;
        if (c->info->stage == STAGE_VERTEX && c->info->attr_int & (1u << i)) return T(c, "uint(in_%u[%u])", i, comp);
        return T(c, "floatBitsToUint(in_%u[%u])", i, comp);
    }
    if (addr >= 0x70 && addr < 0x80) {
        unsigned comp = (addr - 0x70) / 4;
        if (c->info->stage == STAGE_FRAGMENT) return comp == 3 ? "floatBitsToUint(gl_FragCoord.w)" : T(c, "floatBitsToUint(gl_FragCoord[%u])", comp);
        return T(c, "floatBitsToUint(gl_Position[%u])", comp);
    }
    if (addr == 0x2f8) return "uint(gl_InstanceID)";
    if (addr == 0x2fc) return "uint(gl_VertexID)";
    if (addr == 0x3fc) return "(gl_FrontFacing ? 0xffffffffu : 0u)";
    return T(c, "0u /* ALD %#x */", addr);
}

static void emit_frag_outputs(Ctx *c) {
    const uint32_t *h = c->info->header;
    uint32_t target = h[18], reg = 0;
    for (int rt = 0; rt < 8; rt++) {
        uint32_t m = target >> (rt * 4) & 0xf;
        if (!m) continue;
        c->info->frag_out |= 1u << rt;
        for (int comp = 0; comp < 4; comp++)
            if (m >> comp & 1) emit(c->b, "  frag_%d[%d] = uintBitsToFloat(%s);\n", rt, comp, R(c, reg++));
    }
    if (h[19] >> 1 & 1) emit(c->b, "  gl_FragDepth = uintBitsToFloat(%s);\n", R(c, reg + 1));
}

static void texs(Ctx *c, uint64_t w) {
    unsigned da = (unsigned)bf(w, 0, 8), ra = (unsigned)bf(w, 8, 8), rb = (unsigned)bf(w, 20, 8), db = (unsigned)bf(w, 28, 8);
    unsigned handle = (unsigned)bf(w, 36, 13) * 4, enc = (unsigned)bf(w, 53, 4), swz = (unsigned)bf(w, 50, 3), f16 = !bf(w, 59, 1);
    int slot = -1;
    for (int i = 0; i < c->info->ntex; i++)
        if (c->info->tex_handle[i] == handle) slot = i;
    if (slot < 0 && c->info->ntex < MAX_TEX) { slot = c->info->ntex++; c->info->tex_handle[slot] = handle; c->info->tex_kind[slot] = 0; }
    if (slot < 0) return;
    const char *coord, *extra = "";
    unsigned ra1 = ra == RZ ? RZ : ra + 1;
    switch (enc) {
    case 0: coord = T(c, "vec2(%s, 0.0)", F(c, R(c, ra))); extra = "lod"; break;
    case 1: coord = T(c, "vec2(%s, %s)", F(c, R(c, ra)), F(c, R(c, rb))); break;
    case 2: coord = T(c, "vec2(%s, %s)", F(c, R(c, ra)), F(c, R(c, rb))); extra = "lod"; break;
    case 3: coord = T(c, "vec2(%s, %s)", F(c, R(c, ra)), F(c, R(c, ra1))); extra = T(c, "%s", F(c, R(c, rb))); break;
    default:
        coord = T(c, "vec2(%s, %s)", F(c, R(c, ra)), F(c, R(c, ra1)));
        emit(c->b, "  /* TEXS enc %u não suportado */\n", enc);
    }
    if (!strcmp(extra, "lod")) emit(c->b, "  { vec4 t = textureLod(tex%d, %s, 0.0);\n", slot, coord);
    else if (*extra) emit(c->b, "  { vec4 t = textureLod(tex%d, %s, %s);\n", slot, coord, extra);
    else emit(c->b, "  { vec4 t = texture(tex%d, %s);\n", slot, coord);
    static const unsigned rg_lut[] = {1, 2, 4, 8, 3, 9, 10, 12};
    static const unsigned rgba_lut[] = {7, 11, 13, 14, 15, 15, 15, 15};
    unsigned mask = db == RZ ? rg_lut[swz] : rgba_lut[swz];
    unsigned comps[4], n = 0;
    for (unsigned i = 0; i < 4; i++)
        if (mask >> i & 1) comps[n++] = i;
    if (f16) {
        if (n > 0) set_reg(c, da, n > 1 ? T(c, "packHalf2x16(vec2(t[%u], t[%u]))", comps[0], comps[1]) : T(c, "packHalf2x16(vec2(t[%u], 0.0))", comps[0]));
        if (n > 2) set_reg(c, db, n > 3 ? T(c, "packHalf2x16(vec2(t[%u], t[%u]))", comps[2], comps[3]) : T(c, "packHalf2x16(vec2(t[%u], 0.0))", comps[2]));
    } else {
        unsigned dst[4] = {da, da == RZ ? RZ : da + 1, db, db == RZ ? RZ : db + 1};
        for (unsigned i = 0; i < n; i++) set_reg(c, dst[i], T(c, "floatBitsToUint(t[%u])", comps[i]));
    }
    emit(c->b, "  }\n");
}

static int translate_insn(Ctx *c, uint64_t w, int op, uint32_t pc, uint32_t *targets, int ntargets) {
    unsigned d = (unsigned)bf(w, 0, 8), a = (unsigned)bf(w, 8, 8);
    (void)targets; (void)ntargets;
    switch (op) {
    case OP_NOP: case OP_DEPBAR: case OP_BAR: case OP_MEMBAR: return 1;
    case OP_EXIT:
        emit(c->b, "  pc = 0xffffffffu; break;\n");
        return 1;
    case OP_KIL:
        emit(c->b, "  discard;\n");
        return 1;
    case OP_BRA: {
        int32_t off = (int32_t)(bf(w, 20, 24) << 8) >> 8;
        uint32_t t = (uint32_t)((int32_t)pc + 8 + off);
        emit(c->b, "  pc = %uu; continue;\n", t);
        return 1;
    }
    case OP_MOV_reg: set_reg(c, d, R(c, (unsigned)bf(w, 20, 8))); return 1;
    case OP_MOV_cbuf: set_reg(c, d, cbuf(c, w)); return 1;
    case OP_MOV_imm: set_reg(c, d, imm19i(c, w)); return 1;
    case OP_MOV32I: set_reg(c, d, T(c, "%#xu", (uint32_t)bf(w, 20, 32))); return 1;
    case OP_FFMA_reg: case OP_FFMA_cr: case OP_FFMA_rc: case OP_FFMA_imm: {
        const char *b, *cc;
        if (op == OP_FFMA_reg) { b = R(c, (unsigned)bf(w, 20, 8)); cc = R(c, (unsigned)bf(w, 39, 8)); }
        else if (op == OP_FFMA_cr) { b = cbuf(c, w); cc = R(c, (unsigned)bf(w, 39, 8)); }
        else if (op == OP_FFMA_rc) { b = R(c, (unsigned)bf(w, 39, 8)); cc = cbuf(c, w); }
        else { b = imm19f(c, w); cc = R(c, (unsigned)bf(w, 39, 8)); }
        const char *fa = F(c, R(c, a)), *fb = fmod_(c, F(c, b), (int)bf(w, 48, 1), 0), *fc = fmod_(c, F(c, cc), (int)bf(w, 49, 1), 0);
        set_f(c, d, sat(c, T(c, "fma(%s, %s, %s)", fa, fb, fc), (int)bf(w, 50, 1)));
        return 1;
    }
    case OP_FMUL_reg: case OP_FMUL_cbuf: case OP_FMUL_imm: {
        const char *b = opb(c, w, op == OP_FMUL_reg ? 0 : op == OP_FMUL_cbuf ? 1 : 2, 1);
        static const char *scale[] = {"", " * 0.5", " * 0.25", " * 0.125", " * 8.0", " * 4.0", " * 2.0", ""};
        const char *e = T(c, "(%s * %s%s)", F(c, R(c, a)), fmod_(c, F(c, b), (int)bf(w, 48, 1), 0), scale[bf(w, 41, 3)]);
        set_f(c, d, sat(c, e, (int)bf(w, 50, 1)));
        return 1;
    }
    case OP_FMUL32I: {
        const char *e = T(c, "(%s * uintBitsToFloat(%#xu))", F(c, R(c, a)), (uint32_t)bf(w, 20, 32));
        set_f(c, d, sat(c, e, (int)bf(w, 55, 1)));
        return 1;
    }
    case OP_FADD_reg: case OP_FADD_cbuf: case OP_FADD_imm: {
        const char *b = opb(c, w, op == OP_FADD_reg ? 0 : op == OP_FADD_cbuf ? 1 : 2, 1);
        const char *fa = fmod_(c, F(c, R(c, a)), (int)bf(w, 48, 1), (int)bf(w, 46, 1));
        const char *fb = fmod_(c, F(c, b), (int)bf(w, 45, 1), (int)bf(w, 49, 1));
        set_f(c, d, sat(c, T(c, "(%s + %s)", fa, fb), (int)bf(w, 50, 1)));
        return 1;
    }
    case OP_FADD32I: {
        const char *fa = fmod_(c, F(c, R(c, a)), (int)bf(w, 56, 1), (int)bf(w, 54, 1));
        const char *fb = fmod_(c, T(c, "uintBitsToFloat(%#xu)", (uint32_t)bf(w, 20, 32)), (int)bf(w, 53, 1), (int)bf(w, 57, 1));
        set_f(c, d, T(c, "(%s + %s)", fa, fb));
        return 1;
    }
    case OP_MUFU: {
        const char *x = fmod_(c, F(c, R(c, a)), (int)bf(w, 48, 1), (int)bf(w, 46, 1));
        static const char *fn[] = {"cos(%s)", "sin(%s)", "exp2(%s)", "log2(%s)", "(1.0 / %s)", "inversesqrt(%s)", "(1.0 / %s)", "inversesqrt(%s)", "sqrt(%s)"};
        unsigned o = (unsigned)bf(w, 20, 4);
        if (o > 8) { emit(c->b, "  /* MUFU %u */\n", o); return 0; }
        set_f(c, d, sat(c, T(c, fn[o], x), (int)bf(w, 50, 1)));
        return 1;
    }
    case OP_RRO_reg: case OP_RRO_cbuf: case OP_RRO_imm: {
        const char *b = opb(c, w, op == OP_RRO_reg ? 0 : op == OP_RRO_cbuf ? 1 : 2, 1);
        set_f(c, d, fmod_(c, F(c, b), (int)bf(w, 45, 1), (int)bf(w, 49, 1)));
        return 1;
    }
    case OP_FMNMX_reg: case OP_FMNMX_cbuf: case OP_FMNMX_imm: {
        const char *b = opb(c, w, op == OP_FMNMX_reg ? 0 : op == OP_FMNMX_cbuf ? 1 : 2, 1);
        const char *fa = fmod_(c, F(c, R(c, a)), (int)bf(w, 48, 1), (int)bf(w, 46, 1));
        const char *fb = fmod_(c, F(c, b), (int)bf(w, 45, 1), (int)bf(w, 49, 1));
        const char *p = pred(c, (unsigned)bf(w, 39, 3), (int)bf(w, 42, 1));
        set_f(c, d, T(c, "(%s ? min(%s, %s) : max(%s, %s))", p, fa, fb, fa, fb));
        return 1;
    }
    case OP_FSETP_reg: case OP_FSETP_cbuf: case OP_FSETP_imm: {
        const char *b = opb(c, w, op == OP_FSETP_reg ? 0 : op == OP_FSETP_cbuf ? 1 : 2, 1);
        const char *fa = fmod_(c, F(c, R(c, a)), (int)bf(w, 43, 1), (int)bf(w, 7, 1));
        const char *fb = fmod_(c, F(c, b), (int)bf(w, 6, 1), (int)bf(w, 44, 1));
        const char *cmp = fcmp(c, (unsigned)bf(w, 48, 4), fa, fb);
        const char *cp = pred(c, (unsigned)bf(w, 39, 3), (int)bf(w, 42, 1));
        unsigned pa = (unsigned)bf(w, 3, 3), pb = (unsigned)bf(w, 0, 3), o = (unsigned)bf(w, 45, 2);
        emit(c->b, "  { bool r = %s; bool q = %s;\n", cmp, cp);
        if (pa != 7) emit(c->b, "  p%u = %s;\n", pa, bop(c, o, "r", "q"));
        if (pb != 7) emit(c->b, "  p%u = %s;\n", pb, bop(c, o, "!r", "q"));
        emit(c->b, "  }\n");
        return 1;
    }
    case OP_FSET_reg: case OP_FSET_cbuf: case OP_FSET_imm: {
        const char *b = opb(c, w, op == OP_FSET_reg ? 0 : op == OP_FSET_cbuf ? 1 : 2, 1);
        const char *fa = fmod_(c, F(c, R(c, a)), (int)bf(w, 43, 1), (int)bf(w, 54, 1));
        const char *fb = fmod_(c, F(c, b), (int)bf(w, 53, 1), (int)bf(w, 44, 1));
        const char *r = bop(c, (unsigned)bf(w, 45, 2), fcmp(c, (unsigned)bf(w, 48, 4), fa, fb), pred(c, (unsigned)bf(w, 39, 3), (int)bf(w, 42, 1)));
        set_reg(c, d, T(c, "(%s ? %s : 0u)", r, bf(w, 52, 1) ? "0x3f800000u" : "0xffffffffu"));
        return 1;
    }
    case OP_IPA: {
        uint32_t addr = (uint32_t)bf(w, 30, 8) * 4;
        set_reg(c, d, attr_load(c, addr));
        if (bf(w, 51, 1)) set_f(c, d, T(c, "clamp(%s, 0.0, 1.0)", F(c, R(c, d))));
        return 1;
    }
    case OP_ALD: {
        uint32_t addr = (uint32_t)bf(w, 20, 10), n = (uint32_t)bf(w, 47, 2) + 1;
        for (uint32_t i = 0; i < n; i++) set_reg(c, d + i, attr_load(c, addr + i * 4));
        return 1;
    }
    case OP_AST: {
        uint32_t addr = (uint32_t)bf(w, 20, 10), n = (uint32_t)bf(w, 47, 2) + 1;
        for (uint32_t i = 0; i < n; i++) emit_attr_store(c, addr + i * 4, R(c, d + i));
        return 1;
    }
    case OP_TEXS: texs(c, w); return 1;
    }
    emit(c->b, "  /* %s não suportada */\n", sass_name(op));
    return 0;
}

int shader_translate(const uint64_t *prog, ShaderInfo *info) {
    memcpy(info->header, prog, sizeof info->header);
    const uint64_t *code = prog + 10;
    uint32_t n = 0;
    while (n < 0x4000) {
        uint64_t w = code[n++];
        if ((n - 1) % 4 == 0) continue;
        if ((w >> 48) == 0xe240 && (bf(w, 20, 24) == 0xfffff0 || bf(w, 20, 24) == 0xfffff8)) break;
    }
    uint32_t targets[256];
    int ntargets = 0;
    targets[ntargets++] = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (i % 4 == 0) continue;
        uint64_t w = code[i];
        if (sass_decode(w) == OP_BRA && ntargets < 256 && i + 1 < n) {
            int32_t off = (int32_t)(bf(w, 20, 24) << 8) >> 8;
            targets[ntargets++] = (uint32_t)((int32_t)i * 8 + 8 + off);
        }
    }
    Str body = {0};
    Ctx c = {.info = info, .b = &body};
    int ok = 1;
    info->unsupported = 0;
    emit(&body, "  default:\n");
    if (n) n--;
    for (uint32_t i = 0; i < n; i++) {
        if (i % 4 == 0) continue;
        for (int t = 0; t < ntargets; t++)
            if (targets[t] == i * 8) { emit(&body, "  case %uu:\n", i * 8); break; }
        uint64_t w = code[i];
        int op = sass_decode(w);
        unsigned p = (unsigned)bf(w, 16, 3), pn = (unsigned)bf(w, 19, 1);
        int guarded = !(p == 7 && !pn);
        if (guarded) emit(&body, "  if (%s) {\n", pred(&c, p, (int)pn));
        if (!translate_insn(&c, w, op, i * 8, targets, ntargets)) { ok = 0; info->unsupported++; }
        if (guarded) emit(&body, "  }\n");
    }
    Str tail = {0};
    if (info->stage == STAGE_FRAGMENT) {
        Ctx c2 = c;
        c2.b = &tail;
        emit_frag_outputs(&c2);
        memcpy(c.regs_used, c2.regs_used, sizeof c.regs_used);
    }
    Str out = {0};
    emit(&out, "#version 460\n");
    if (info->stage == STAGE_VERTEX) {
        emit(&out, "out gl_PerVertex { vec4 gl_Position; float gl_PointSize; };\n");
        for (int i = 0; i < 32; i++) {
            if (info->attr_in >> i & 1) emit(&out, "layout(location = %d) in %s in_%d;\n", i, info->attr_int >> i & 1 ? "uvec4" : "vec4", i);
            if (info->attr_out >> i & 1) emit(&out, "layout(location = %d) out vec4 out_%d;\n", i, i);
        }
        emit(&out, "uniform float flip_y;\n");
    } else {
        for (int i = 0; i < 32; i++)
            if (info->attr_in >> i & 1) emit(&out, "layout(location = %d) in vec4 in_%d;\n", i, i);
        for (int i = 0; i < 8; i++)
            if (info->frag_out >> i & 1) emit(&out, "layout(location = %d) out vec4 frag_%d;\n", i, i);
    }
    int ub = info->stage == STAGE_VERTEX ? 0 : 18;
    for (int i = 0; i < 18; i++)
        if (info->cbuf_mask >> i & 1) emit(&out, "layout(std140, binding = %d) uniform CB%d_%d { uvec4 d[4096]; } cb%d;\n", ub + i, info->stage, i, i);
    int tb = info->stage == STAGE_VERTEX ? 0 : 16;
    for (int i = 0; i < info->ntex; i++) emit(&out, "layout(binding = %d) uniform sampler2D tex%d;\n", tb + i, i);
    emit(&out, "void main() {\n");
    for (int r = 0; r < 255; r++)
        if (c.regs_used[r / 32] >> (r % 32) & 1) emit(&out, "  uint r%d = 0u;\n", r);
    emit(&out, "  bool p0 = false, p1 = false, p2 = false, p3 = false, p4 = false, p5 = false, p6 = false;\n");
    if (info->stage == STAGE_VERTEX) emit(&out, "  gl_Position = vec4(0.0, 0.0, 0.0, 1.0);\n");
    emit(&out, "  uint pc = 0u;\n  while (true) {\n  switch (pc) {\n");
    emit(&out, "%s", body.s ? body.s : "");
    emit(&out, "  }\n  break;\n  }\n");
    emit(&out, "%s", tail.s ? tail.s : "");
    free(tail.s);
    if (info->stage == STAGE_VERTEX) emit(&out, "  gl_Position.y *= flip_y;\n");
    emit(&out, "}\n");
    free(body.s);
    info->glsl = out.s;
    return ok;
}
