#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gl.h"
#include "maxwell.h"
#include "shader.h"
#include "render.h"

typedef struct {
    uint64_t cpu_addr;
    uint32_t width, height, format, depth;
    GLuint tex, fbo;
} Surface;

static Surface surfaces[1024];
static int nsurfaces;

typedef struct {
    uint32_t fmt;
    GLenum internal;
    uint32_t bpp;
} Fmt;

static const Fmt color_fmts[] = {
    {0xc0, GL_RGBA32F, 16}, {0xc1, GL_RGBA32I, 16}, {0xc2, GL_RGBA32UI, 16}, {0xc3, GL_RGBA32F, 16},
    {0xc6, GL_RGBA16, 8}, {0xc7, GL_RGBA16_SNORM, 8}, {0xc8, GL_RGBA16I, 8}, {0xc9, GL_RGBA16UI, 8}, {0xca, GL_RGBA16F, 8},
    {0xcb, GL_RG32F, 8}, {0xcc, GL_RG32I, 8}, {0xcd, GL_RG32UI, 8}, {0xce, GL_RGBA16F, 8},
    {0xcf, GL_RGBA8, 4}, {0xd0, GL_SRGB8_ALPHA8, 4}, {0xd1, GL_RGB10_A2, 4}, {0xd2, GL_RGB10_A2UI, 4},
    {0xd5, GL_RGBA8, 4}, {0xd6, GL_SRGB8_ALPHA8, 4}, {0xd7, GL_RGBA8_SNORM, 4}, {0xd8, GL_RGBA8I, 4}, {0xd9, GL_RGBA8UI, 4},
    {0xda, GL_RG16, 4}, {0xdb, GL_RG16_SNORM, 4}, {0xdc, GL_RG16I, 4}, {0xdd, GL_RG16UI, 4}, {0xde, GL_RG16F, 4},
    {0xdf, GL_RGB10_A2, 4}, {0xe0, GL_R11F_G11F_B10F, 4}, {0xe3, GL_R32I, 4}, {0xe4, GL_R32UI, 4}, {0xe5, GL_R32F, 4},
    {0xe6, GL_RGBA8, 4}, {0xe7, GL_SRGB8_ALPHA8, 4}, {0xe8, GL_RGB565, 2}, {0xe9, GL_RGB5_A1, 2},
    {0xea, GL_RG8, 2}, {0xeb, GL_RG8_SNORM, 2}, {0xec, GL_RG8I, 2}, {0xed, GL_RG8UI, 2},
    {0xee, GL_R16, 2}, {0xef, GL_R16_SNORM, 2}, {0xf0, GL_R16I, 2}, {0xf1, GL_R16UI, 2}, {0xf2, GL_R16F, 2},
    {0xf3, GL_R8, 1}, {0xf4, GL_R8_SNORM, 1}, {0xf5, GL_R8I, 1}, {0xf6, GL_R8UI, 1},
};

static const Fmt depth_fmts[] = {
    {0x0a, GL_DEPTH_COMPONENT32F, 4}, {0x13, GL_DEPTH_COMPONENT16, 2}, {0x14, GL_DEPTH24_STENCIL8, 4},
    {0x15, GL_DEPTH24_STENCIL8, 4}, {0x16, GL_DEPTH24_STENCIL8, 4}, {0x19, GL_DEPTH32F_STENCIL8, 8},
};

static const Fmt *find_fmt(uint32_t f, int depth) {
    const Fmt *t = depth ? depth_fmts : color_fmts;
    size_t n = depth ? sizeof depth_fmts / sizeof *depth_fmts : sizeof color_fmts / sizeof *color_fmts;
    for (size_t i = 0; i < n; i++)
        if (t[i].fmt == f) return &t[i];
    static uint8_t warned[256];
    if (!warned[f & 0xff]++) fprintf(stderr, "[maxwell] formato %s %#x desconhecido\n", depth ? "zeta" : "cor", f);
    return depth ? &depth_fmts[2] : &color_fmts[13];
}

static Surface *surface_get(uint64_t cpu, uint32_t w, uint32_t h, uint32_t fmt, int depth) {
    for (int i = 0; i < nsurfaces; i++) {
        Surface *s = &surfaces[i];
        if (s->cpu_addr == cpu && s->depth == (uint32_t)depth) {
            if (s->width == w && s->height == h && s->format == fmt) return s;
            glDeleteTextures(1, &s->tex);
            glDeleteFramebuffers(1, &s->fbo);
            *s = surfaces[--nsurfaces];
            break;
        }
    }
    if (nsurfaces == 1024) { glDeleteTextures(1, &surfaces[0].tex); glDeleteFramebuffers(1, &surfaces[0].fbo); surfaces[0] = surfaces[--nsurfaces]; }
    Surface *s = &surfaces[nsurfaces++];
    const Fmt *f = find_fmt(fmt, depth);
    *s = (Surface){cpu, w, h, fmt, (uint32_t)depth, 0, 0};
    glGenTextures(1, &s->tex);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, f->internal, (GLsizei)w, (GLsizei)h);
    glGenFramebuffers(1, &s->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    GLenum att = !depth ? GL_COLOR_ATTACHMENT0 : (f->internal == GL_DEPTH24_STENCIL8 || f->internal == GL_DEPTH32F_STENCIL8) ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
    glFramebufferTexture2D(GL_FRAMEBUFFER, att, GL_TEXTURE_2D, s->tex, 0);
    if (getenv("GPU_TRACE")) fprintf(stderr, "[maxwell] nova superfície %s %ux%u fmt %#x em %#lx\n", depth ? "zeta" : "cor", w, h, fmt, cpu);
    return s;
}

static float f32(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

int surface_lookup(uint64_t cpu_addr, uint32_t w, uint32_t h, unsigned *tex, unsigned *fbo) {
    for (int i = 0; i < nsurfaces; i++)
        if (surfaces[i].cpu_addr == cpu_addr && !surfaces[i].depth && surfaces[i].width >= w && surfaces[i].height >= h) {
            *tex = surfaces[i].tex;
            *fbo = surfaces[i].fbo;
            return 1;
        }
    return 0;
}

void maxwell_clear(const uint32_t *r, uint32_t v, void *(*gpu_ptr)(uint64_t)) {
    static int n;
    if (getenv("CLEAR_TRACE") && n++ < 12) {
        uint32_t rt = v >> 6 & 0xf;
        const uint32_t *t = r + 0x200 + rt * 0x10;
        fprintf(stderr, "[clear] v=%#x rt%u va=%#x%08x %ux%u fmt=%#x tile=%#x ctl=%#x cor=(%g %g %g %g) zeta=%#x%08x en=%u\n", v, rt, t[0], t[1], t[2], t[3], t[4], t[5],
                r[0x487], f32(r[0x360]), f32(r[0x361]), f32(r[0x362]), f32(r[0x363]), r[0x3f8], r[0x3f9], r[0x54e]);
    }
    glDisable(GL_SCISSOR_TEST);
    if (v & 0x3c) {
        uint32_t rt = v >> 6 & 0xf;
        const uint32_t *t = r + 0x200 + rt * 0x10;
        uint64_t va = (uint64_t)(t[0] & 0xff) << 32 | t[1];
        void *cpu = gpu_ptr(va);
        if (cpu && t[2] && t[3]) {
            Surface *s = surface_get((uint64_t)(uintptr_t)cpu, t[2], t[3], t[4], 0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s->fbo);
            glColorMaski(0, v >> 2 & 1, v >> 3 & 1, v >> 4 & 1, v >> 5 & 1);
            float c[4] = {f32(r[0x360]), f32(r[0x361]), f32(r[0x362]), f32(r[0x363])};
            glClearBufferfv(GL_COLOR, 0, c);
            glColorMaski(0, 1, 1, 1, 1);
        }
    }
    if (v & 3 && r[0x54e] & 1) {
        uint64_t va = (uint64_t)(r[0x3f8] & 0xff) << 32 | r[0x3f9];
        void *cpu = gpu_ptr(va);
        if (cpu && r[0x48a] && r[0x48b]) {
            Surface *s = surface_get((uint64_t)(uintptr_t)cpu, r[0x48a], r[0x48b], r[0x3fa], 1);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s->fbo);
            glDepthMask(v & 1);
            glStencilMask(v & 2 ? 0xff : 0);
            glClearBufferfi(GL_DEPTH_STENCIL, 0, f32(r[0x364]), (GLint)r[0x368]);
            glDepthMask(1);
            glStencilMask(0xff);
        }
    }
}

#include <sys/stat.h>

static void shader_dump(const uint32_t *r, void *(*gpu_ptr)(uint64_t)) {
    static uint64_t seen[4096];
    static int nseen;
    if (!getenv("SHADER_DUMP")) return;
    uint64_t base = (uint64_t)(r[0x582] & 0xff) << 32 | r[0x583];
    {
        static uint64_t snaps[64];
        static int nsnap;
        uint64_t key = (uint64_t)r[0x801] << 32 | r[0x851];
        int have = 0;
        for (int i = 0; i < nsnap; i++) have |= snaps[i] == key;
        if (!have && nsnap < 64) {
            snaps[nsnap++] = key;
            mkdir("build/shaders", 0755);
            char path[96];
            snprintf(path, sizeof path, "build/shaders/regs_%02d.bin", nsnap - 1);
            FILE *f = fopen(path, "wb");
            fwrite(r, 4, 0x1000, f);
            uint64_t tic = (uint64_t)(r[0x55d] & 0xff) << 32 | r[0x55e], tsc = (uint64_t)(r[0x557] & 0xff) << 32 | r[0x558];
            uint8_t *tp = gpu_ptr(tic), *sp = gpu_ptr(tsc);
            uint8_t zero[0x2000] = {0};
            fwrite(tp ? tp : zero, 1, 0x2000, f);
            fwrite(sp ? sp : zero, 1, 0x2000, f);
            fclose(f);
        }
    }
    for (int st = 0; st < 6; st++) {
        const uint32_t *c = r + 0x800 + st * 0x10;
        if (!(c[0] & 1) && st != 1) continue;
        uint64_t va = base + c[1];
        int dup = 0;
        for (int i = 0; i < nseen; i++) dup |= seen[i] == (va << 3 | (uint64_t)st);
        if (dup || nseen == 4096) continue;
        seen[nseen++] = va << 3 | (uint64_t)st;
        const uint64_t *p = gpu_ptr(va);
        if (!p) continue;
        size_t n = 10;
        while (n < 0x2000) {
            uint64_t w = p[n++];
            if ((w >> 48) == 0xe240 && (((w >> 20) & 0xffffff) == 0xfffff0 || ((w >> 20) & 0xffffff) == 0xfffff8)) break;
        }
        mkdir("build/shaders", 0755);
        char path[96];
        snprintf(path, sizeof path, "build/shaders/%010lx_s%d.bin", va, st);
        FILE *f = fopen(path, "wb");
        fwrite(p, 8, n, f);
        fclose(f);
    }
}

static void *(*gp)(uint64_t);
static const uint32_t *R;

static void gl_debug(GLenum src, GLenum type, GLuint id, GLenum sev, GLsizei len, const GLchar *msg, const void *u) {
    (void)src; (void)id; (void)len; (void)u;
    static int n;
    if (sev == GL_DEBUG_SEVERITY_NOTIFICATION || n > 200) return;
    if (type == GL_DEBUG_TYPE_ERROR || getenv("GL_DEBUG")) { n++; fprintf(stderr, "[gl] %s\n", msg); }
}

typedef struct {
    uint64_t va, hash;
    int stage;
    GLuint prog;
    ShaderInfo info;
    GLint flip_loc;
} Prog;

static Prog progs[512];
static int nprogs;

static uint64_t fnv(const void *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 1099511628211ull;
    return h;
}

static Prog *get_prog(uint64_t va, int stage) {
    const uint64_t *code = gp(va);
    if (!code) return NULL;
    uint64_t h = fnv(code, 0x400);
    for (int i = 0; i < nprogs; i++)
        if (progs[i].va == va && progs[i].hash == h && progs[i].stage == stage) return &progs[i];
    if (nprogs == 512) nprogs = 0;
    Prog *p = &progs[nprogs++];
    memset(p, 0, sizeof *p);
    p->va = va;
    p->hash = h;
    p->stage = stage;
    p->info.stage = stage;
    shader_translate(code, &p->info);
    const char *src = p->info.glsl;
    p->prog = glCreateShaderProgramv(stage == STAGE_VERTEX ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER, 1, &src);
    GLint ok = 0;
    glGetProgramiv(p->prog, GL_LINK_STATUS, &ok);
    if (!ok || getenv("SHADER_LOG")) {
        char log[4096] = {0};
        glGetProgramInfoLog(p->prog, sizeof log, NULL, log);
        if (!ok) fprintf(stderr, "[shader] falha ao compilar %#lx (estágio %d):\n%s\n", va, stage, log);
        if (getenv("SHADER_LOG")) fprintf(stderr, "[shader] %#lx estágio %d: %d instr. não suportadas, %d texturas\n", va, stage, p->info.unsupported, p->info.ntex);
    }
    p->flip_loc = stage == STAGE_VERTEX ? glGetUniformLocation(p->prog, "flip_y") : -1;
    return p;
}

typedef struct {
    uint8_t tic[32];
    GLuint tex;
    uint64_t cpu;
} Tex;

static Tex texs[2048];
static int ntexs;

static uint32_t gob_off(uint32_t x, uint32_t y, uint32_t width_bytes, uint32_t bh) { return (uint32_t)deswizzle_offset(x, y, width_bytes, bh); }

typedef struct {
    uint32_t fmt;
    GLenum internal, format, type;
    uint32_t bpb, bw, bh;
} TexFmt;

static const TexFmt tex_fmts[] = {
    {0x08, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, 1, 1}, {0x07, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, 1, 1},
    {0x1d, GL_R8, GL_RED, GL_UNSIGNED_BYTE, 1, 1, 1}, {0x18, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, 2, 1, 1},
    {0x1b, GL_R16F, GL_RED, GL_HALF_FLOAT, 2, 1, 1}, {0x0c, GL_RG16F, GL_RG, GL_HALF_FLOAT, 4, 1, 1},
    {0x03, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, 8, 1, 1}, {0x01, GL_RGBA32F, GL_RGBA, GL_FLOAT, 16, 1, 1},
    {0x0f, GL_R32F, GL_RED, GL_FLOAT, 4, 1, 1}, {0x04, GL_RG32F, GL_RG, GL_FLOAT, 8, 1, 1},
    {0x21, GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV, 4, 1, 1}, {0x09, GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, 4, 1, 1},
    {0x15, GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, 2, 1, 1},
    {0x24, 0x83f1, 0, 0, 8, 4, 4}, {0x25, 0x83f2, 0, 0, 16, 4, 4}, {0x26, 0x83f3, 0, 0, 16, 4, 4},
    {0x27, 0x8dbb, 0, 0, 8, 4, 4}, {0x28, 0x8dbd, 0, 0, 16, 4, 4}, {0x17, 0x8e8c, 0, 0, 16, 4, 4},
    {0x10, 0x8e8e, 0, 0, 16, 4, 4}, {0x11, 0x8e8f, 0, 0, 16, 4, 4},
};

static GLuint get_texture(const uint32_t *tic) {
    uint64_t cpu = 0;
    uint64_t va = (uint64_t)(tic[2] & 0xffff) << 32 | tic[1];
    void *p = gp(va);
    if (!p) return 0;
    cpu = (uint64_t)(uintptr_t)p;
    uint32_t w = (tic[4] & 0xffff) + 1, h = (tic[5] & 0xffff) + 1;
    unsigned stex, sfbo;
    if (surface_lookup(cpu, w, h, &stex, &sfbo)) return stex;
    for (int i = 0; i < ntexs; i++)
        if (!memcmp(texs[i].tic, tic, 32)) return texs[i].tex;
    uint32_t fmt = tic[0] & 0x7f, layout = tic[2] >> 21 & 7, bh = tic[3] >> 3 & 7;
    const TexFmt *f = NULL;
    for (size_t i = 0; i < sizeof tex_fmts / sizeof *tex_fmts; i++)
        if (tex_fmts[i].fmt == fmt) f = &tex_fmts[i];
    if (!f) {
        static uint8_t warned[128];
        if (!warned[fmt]++) fprintf(stderr, "[tex] formato %#x não suportado (%ux%u)\n", fmt, w, h);
        return 0;
    }
    int srgb = (tic[4] >> 22 & 1) && fmt == 0x08;
    uint32_t bw = (w + f->bw - 1) / f->bw, bhh = (h + f->bh - 1) / f->bh, row = bw * f->bpb;
    uint8_t *lin = malloc((size_t)row * bhh);
    const uint8_t *src = p;
    if (layout == 3 || layout == 4) {
        for (uint32_t y = 0; y < bhh; y++)
            for (uint32_t x = 0; x < row; x += f->bpb)
                memcpy(lin + (size_t)y * row + x, src + gob_off(x, y, row, bh), f->bpb);
    } else {
        uint32_t pitch = (tic[3] & 0xffff) << 5;
        if (!pitch) pitch = row;
        for (uint32_t y = 0; y < bhh; y++) memcpy(lin + (size_t)y * row, src + (size_t)y * pitch, row);
    }
    if (ntexs == 2048) { glDeleteTextures(1, &texs[0].tex); texs[0] = texs[--ntexs]; }
    Tex *t = &texs[ntexs++];
    memcpy(t->tic, tic, 32);
    t->cpu = cpu;
    glCreateTextures(GL_TEXTURE_2D, 1, &t->tex);
    glTextureStorage2D(t->tex, 1, srgb ? GL_SRGB8_ALPHA8 : f->internal, (GLsizei)w, (GLsizei)h);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (f->bw > 1) glCompressedTextureSubImage2D(t->tex, 0, 0, 0, (GLsizei)w, (GLsizei)h, f->internal, (GLsizei)((size_t)row * bhh), lin);
    else glTextureSubImage2D(t->tex, 0, 0, 0, (GLsizei)w, (GLsizei)h, f->format, f->type, lin);
    free(lin);
    static const GLint src_map[8] = {GL_ZERO, GL_ZERO, GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA, GL_ONE, GL_ONE};
    GLint sw[4] = {src_map[tic[0] >> 19 & 7], src_map[tic[0] >> 22 & 7], src_map[tic[0] >> 25 & 7], src_map[tic[0] >> 28 & 7]};
    glTextureParameteriv(t->tex, GL_TEXTURE_SWIZZLE_RGBA, sw);
    glTextureParameteri(t->tex, GL_TEXTURE_MAX_LEVEL, 0);
    if (getenv("TEX_TRACE")) fprintf(stderr, "[tex] %ux%u fmt %#x layout %u em %#lx\n", w, h, fmt, layout, cpu);
    return t->tex;
}

static GLuint get_sampler(const uint32_t *tsc) {
    static struct { uint32_t tsc[8]; GLuint s; } cache[256];
    static int n;
    for (int i = 0; i < n; i++)
        if (!memcmp(cache[i].tsc, tsc, 32)) return cache[i].s;
    if (n == 256) n = 0;
    GLuint s;
    glCreateSamplers(1, &s);
    static const GLint wrap[8] = {GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_BORDER, GL_CLAMP_TO_EDGE, GL_MIRROR_CLAMP_TO_EDGE, GL_MIRROR_CLAMP_TO_EDGE, GL_MIRROR_CLAMP_TO_EDGE};
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, wrap[tsc[0] & 7]);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, wrap[tsc[0] >> 3 & 7]);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_R, wrap[tsc[0] >> 6 & 7]);
    int mag = tsc[1] & 3, min = tsc[1] >> 4 & 3;
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, mag == 1 ? GL_NEAREST : GL_LINEAR);
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, min == 1 ? GL_NEAREST : GL_LINEAR);
    float border[4];
    memcpy(border, tsc + 4, 16);
    glSamplerParameterfv(s, GL_TEXTURE_BORDER_COLOR, border);
    memcpy(cache[n].tsc, tsc, 32);
    cache[n++].s = s;
    return s;
}

static GLenum blend_factor(uint32_t v) {
    if (v >= 0x4000 || v == 0 || v == 1) {
        if (v == 0x4000) return GL_ZERO;
        if (v == 0x4001) return GL_ONE;
        return v;
    }
    static const GLenum t[] = {0, GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA,
                               GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE, 0, 0, 0, 0, GL_SRC1_COLOR,
                               GL_ONE_MINUS_SRC1_COLOR, GL_SRC1_ALPHA, GL_ONE_MINUS_SRC1_ALPHA, GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR,
                               GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA};
    return v < sizeof t / sizeof *t && t[v] ? t[v] : GL_ONE;
}

static GLenum blend_eq(uint32_t v) {
    if (v >= 0x8000) return v;
    static const GLenum t[] = {GL_FUNC_ADD, GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX};
    return v < 6 ? t[v] : GL_FUNC_ADD;
}

static GLenum cmp_func(uint32_t v) {
    if (v >= 0x200) return v;
    static const GLenum t[] = {GL_NEVER, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    return v < 9 ? t[v] : GL_ALWAYS;
}

static GLuint vao, vbufs[32], ibuf, ubos[2][18], pipeline;

static void attr_format(GLuint idx, uint32_t a) {
    uint32_t size = a >> 21 & 0x3f, type = a >> 27 & 7, off = a >> 7 & 0x3fff;
    int comps = 4;
    GLenum gt = GL_FLOAT;
    switch (size) {
    case 0x01: comps = 4; gt = GL_FLOAT; break;
    case 0x02: comps = 3; gt = GL_FLOAT; break;
    case 0x03: comps = 4; gt = GL_SHORT; break;
    case 0x04: comps = 2; gt = GL_FLOAT; break;
    case 0x05: comps = 3; gt = GL_SHORT; break;
    case 0x0a: comps = 4; gt = GL_BYTE; break;
    case 0x0f: comps = 2; gt = GL_SHORT; break;
    case 0x12: comps = 1; gt = GL_FLOAT; break;
    case 0x13: comps = 3; gt = GL_BYTE; break;
    case 0x18: comps = 2; gt = GL_BYTE; break;
    case 0x1b: comps = 1; gt = GL_SHORT; break;
    case 0x1d: comps = 1; gt = GL_BYTE; break;
    case 0x30: comps = 4; gt = GL_INT_2_10_10_10_REV; break;
    case 0x31: comps = 3; gt = GL_UNSIGNED_INT_10F_11F_11F_REV; break;
    }
    if (gt == GL_SHORT || gt == GL_BYTE || gt == GL_INT_2_10_10_10_REV) {
        int unsig = type == 2 || type == 4 || type == 5;
        if (gt == GL_SHORT) gt = type == 7 ? GL_HALF_FLOAT : unsig ? GL_UNSIGNED_SHORT : GL_SHORT;
        else if (gt == GL_BYTE) gt = unsig ? GL_UNSIGNED_BYTE : GL_BYTE;
        else if (unsig) gt = GL_UNSIGNED_INT_2_10_10_10_REV;
    }
    if (size == 0x01 || size == 0x02 || size == 0x04 || size == 0x12) {
        if (type == 3) gt = GL_INT;
        else if (type == 4) gt = GL_UNSIGNED_INT;
    }
    if (type == 3 || type == 4) glVertexArrayAttribIFormat(vao, idx, comps, gt, off);
    else glVertexArrayAttribFormat(vao, idx, comps, gt, type == 1 || type == 2, off);
    glVertexArrayAttribBinding(vao, idx, a & 31);
}

static uint32_t max_index(const void *ib, uint32_t fmt, uint32_t first, uint32_t count) {
    uint32_t m = 0;
    for (uint32_t i = first; i < first + count; i++) {
        uint32_t v = fmt == 0 ? ((const uint8_t *)ib)[i] : fmt == 1 ? ((const uint16_t *)ib)[i] : ((const uint32_t *)ib)[i];
        if (v > m && !(fmt == 1 && v == 0xffff) && !(fmt == 2 && v == 0xffffffff)) m = v;
    }
    return m;
}

static GLuint bind_targets(const uint32_t *r, uint32_t *out_w, uint32_t *out_h) {
    static struct { GLuint tex[9]; GLuint fbo; } fbos[256];
    static int nf;
    GLuint att[9] = {0};
    uint32_t ctl = r[0x487], count = ctl & 0xf, w = 0, h = 0;
    for (uint32_t i = 0; i < count && i < 8; i++) {
        uint32_t rt = ctl >> (4 + i * 3) & 7;
        const uint32_t *t = r + 0x200 + rt * 0x10;
        void *cpu = gp((uint64_t)(t[0] & 0xff) << 32 | t[1]);
        if (!cpu || !t[2] || !t[3] || !t[4]) continue;
        Surface *s = surface_get((uint64_t)(uintptr_t)cpu, t[2], t[3], t[4], 0);
        att[i] = s->tex;
        w = t[2]; h = t[3];
    }
    if (r[0x54e] & 1) {
        void *cpu = gp((uint64_t)(r[0x3f8] & 0xff) << 32 | r[0x3f9]);
        if (cpu && r[0x48a] && r[0x48b]) {
            Surface *s = surface_get((uint64_t)(uintptr_t)cpu, r[0x48a], r[0x48b], r[0x3fa], 1);
            att[8] = s->tex;
            if (!w) { w = r[0x48a]; h = r[0x48b]; }
        }
    }
    *out_w = w;
    *out_h = h;
    for (int i = 0; i < nf; i++)
        if (!memcmp(fbos[i].tex, att, sizeof att)) return fbos[i].fbo;
    if (nf == 256) { glDeleteFramebuffers(1, &fbos[0].fbo); fbos[0] = fbos[--nf]; }
    GLuint fbo;
    glCreateFramebuffers(1, &fbo);
    GLenum bufs[8];
    for (int i = 0; i < 8; i++) {
        bufs[i] = att[i] ? GL_COLOR_ATTACHMENT0 + i : GL_NONE;
        if (att[i]) glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0 + i, att[i], 0);
    }
    glNamedFramebufferDrawBuffers(fbo, 8, bufs);
    if (att[8]) {
        uint32_t zf = r[0x3fa];
        glNamedFramebufferTexture(fbo, zf == 0x0a || zf == 0x13 ? GL_DEPTH_ATTACHMENT : GL_DEPTH_STENCIL_ATTACHMENT, att[8], 0);
    }
    memcpy(fbos[nf].tex, att, sizeof att);
    fbos[nf++].fbo = fbo;
    return fbo;
}

void maxwell_draw(const uint32_t *r, void *(*gpu_ptr)(uint64_t), const CbBind (*cb)[18], int indexed) {
    shader_dump(r, gpu_ptr);
    gp = gpu_ptr;
    R = r;
    static int init;
    if (!init) {
        init = 1;
        glEnable(GL_DEBUG_OUTPUT);
        glDebugMessageCallback(gl_debug, NULL);
        glCreateVertexArrays(1, &vao);
        glCreateBuffers(32, vbufs);
        glCreateBuffers(1, &ibuf);
        glCreateBuffers(36, &ubos[0][0]);
        glGenProgramPipelines(1, &pipeline);
    }
    uint64_t base = (uint64_t)(r[0x582] & 0xff) << 32 | r[0x583];
    Prog *vs = get_prog(base + r[0x811], STAGE_VERTEX);
    Prog *fs = (r[0x850] & 1) ? get_prog(base + r[0x851], STAGE_FRAGMENT) : NULL;
    if (!vs) return;
    glBindProgramPipeline(pipeline);
    glUseProgramStages(pipeline, GL_VERTEX_SHADER_BIT, vs->prog);
    glUseProgramStages(pipeline, GL_FRAGMENT_SHADER_BIT, fs ? fs->prog : 0);
    float sy = 0;
    memcpy(&sy, &r[0x281], 4);
    glProgramUniform1f(vs->prog, vs->flip_loc, sy < 0 ? -1.0f : 1.0f);
    uint32_t w, h;
    GLuint fbo = bind_targets(r, &w, &h);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    for (int i = 0; i < 1; i++) {
        float sc[3], tr[3];
        memcpy(sc, r + 0x280 + i * 8, 12);
        memcpy(tr, r + 0x283 + i * 8, 12);
        glViewportIndexedf((GLuint)i, tr[0] - fabsf(sc[0]), tr[1] - fabsf(sc[1]), 2 * fabsf(sc[0]), 2 * fabsf(sc[1]));
        glDepthRangeIndexed((GLuint)i, tr[2] - sc[2], tr[2] + sc[2]);
        const uint32_t *sci = r + 0x380 + i * 4;
        if (sci[0] & 1) {
            glEnablei(GL_SCISSOR_TEST, (GLuint)i);
            glScissorIndexed((GLuint)i, (GLint)(sci[1] & 0xffff), (GLint)(sci[2] & 0xffff), (GLsizei)((sci[1] >> 16) - (sci[1] & 0xffff)), (GLsizei)((sci[2] >> 16) - (sci[2] & 0xffff)));
        } else glDisablei(GL_SCISSOR_TEST, (GLuint)i);
    }
    for (GLuint i = 0; i < 8; i++) {
        uint32_t m = r[0x680 + (r[0x4b9] & 1 ? i : 0)];
        glColorMaski(i, m & 1, m >> 4 & 1, m >> 8 & 1, m >> 12 & 1);
        if (r[0x4d8 + i] & 1) {
            glEnablei(GL_BLEND, i);
            const uint32_t *b = r[0x4b9] & 1 ? r + 0x780 + i * 8 : r + 0x4cf;
            int sep = b[0] & 1;
            glBlendEquationSeparatei(i, blend_eq(b[1]), blend_eq(sep ? b[4] : b[1]));
            glBlendFuncSeparatei(i, blend_factor(b[2]), blend_factor(b[3]), blend_factor(sep ? b[5] : b[2]), blend_factor(sep ? b[6] : b[3]));
        } else glDisablei(GL_BLEND, i);
    }
    if (r[0x4b3] & 1) { glEnable(GL_DEPTH_TEST); glDepthFunc(cmp_func(r[0x4c3])); }
    else glDisable(GL_DEPTH_TEST);
    glDepthMask(r[0x4ba] & 1);
    if (r[0x646] & 1) {
        glEnable(GL_CULL_FACE);
        glCullFace(r[0x648] >= 0x404 ? r[0x648] : GL_BACK);
        GLenum ff = r[0x647] == 0x900 || r[0x647] == 1 ? GL_CW : GL_CCW;
        if (sy < 0) ff = ff == GL_CW ? GL_CCW : GL_CW;
        glFrontFace(ff);
    } else glDisable(GL_CULL_FACE);
    uint32_t vfirst = r[0x35d], vcount = r[0x35e];
    uint32_t ifmt = r[0x5f6], ifirst = r[0x5f7], icount = r[0x5f8];
    const void *ib = indexed ? gpu_ptr((uint64_t)(r[0x5f2] & 0xff) << 32 | r[0x5f3]) : NULL;
    uint32_t isz = ifmt == 0 ? 1 : ifmt == 1 ? 2 : 4;
    uint32_t maxv = ib ? max_index(ib, ifmt, ifirst, icount) + 1 : vfirst + vcount;
    glBindVertexArray(vao);
    uint32_t used = vs->info.attr_in;
    for (GLuint i = 0; i < 16; i++) {
        uint32_t a = r[0x458 + i];
        if (!(used >> i & 1) || a >> 6 & 1) { glDisableVertexArrayAttrib(vao, i); continue; }
        glEnableVertexArrayAttrib(vao, i);
        attr_format(i, a);
    }
    for (GLuint sidx = 0; sidx < 16; sidx++) {
        const uint32_t *st = r + 0x700 + sidx * 4;
        if (!(st[0] >> 12 & 1)) continue;
        uint32_t stride = st[0] & 0xfff;
        void *src = gpu_ptr((uint64_t)(st[1] & 0xff) << 32 | st[2]);
        if (!src) continue;
        uint64_t limit = ((uint64_t)(r[0x7c0 + sidx * 2] & 0xff) << 32 | r[0x7c1 + sidx * 2]) - ((uint64_t)(st[1] & 0xff) << 32 | st[2]) + 1;
        uint64_t size = (uint64_t)stride * maxv + 64;
        if (limit && limit < size && limit < (1ull << 30)) size = limit;
        if (size > (64u << 20)) size = 64u << 20;
        glNamedBufferData(vbufs[sidx], (GLsizeiptr)size, src, GL_STREAM_DRAW);
        glVertexArrayVertexBuffer(vao, sidx, vbufs[sidx], 0, (GLsizei)stride);
        glVertexArrayBindingDivisor(vao, sidx, r[0x620 + sidx] & 1 ? st[3] : 0);
    }
    Prog *stages[2] = {vs, fs};
    int cbstage[2] = {0, 4};
    for (int s = 0; s < 2; s++) {
        Prog *p = stages[s];
        if (!p) continue;
        for (int i = 0; i < 18; i++) {
            if (!(p->info.cbuf_mask >> i & 1)) continue;
            const CbBind *b = &cb[cbstage[s]][i];
            void *src = b->addr ? gpu_ptr(b->addr) : NULL;
            static uint8_t zero[65536];
            uint32_t sz = b->size ? b->size : 0x10000;
            if (sz > 0x10000) sz = 0x10000;
            glNamedBufferData(ubos[s][i], 0x10000, zero, GL_STREAM_DRAW);
            if (src) glNamedBufferSubData(ubos[s][i], 0, sz, src);
            glBindBufferRange(GL_UNIFORM_BUFFER, (GLuint)(s * 18 + i), ubos[s][i], 0, 0x10000);
        }
        uint32_t texcb = r[0x982];
        const CbBind *tb = &cb[cbstage[s]][texcb & 0x1f];
        const uint8_t *hcb = tb->addr ? gpu_ptr(tb->addr) : NULL;
        const uint32_t *tic = gpu_ptr((uint64_t)(r[0x55d] & 0xff) << 32 | r[0x55e]);
        const uint32_t *tsc = gpu_ptr((uint64_t)(r[0x557] & 0xff) << 32 | r[0x558]);
        for (int t = 0; t < p->info.ntex; t++) {
            GLuint unit = (GLuint)(s * 16 + t);
            if (!hcb || !tic || !tsc) { glBindTextureUnit(unit, 0); continue; }
            uint32_t handle = *(const uint32_t *)(hcb + p->info.tex_handle[t]);
            uint32_t ti = handle & 0xfffff, si = handle >> 20;
            static int nt;
            if (getenv("TEX_TRACE") && nt++ < 30)
                fprintf(stderr, "[tex] estágio %d slot %d texcb=%u cb=%#lx+%#x handle=%#x tic=%#x%08x tsc=%#x%08x | tic[ti]=%08x %08x %08x\n", s, t, texcb, tb->addr, p->info.tex_handle[t], handle,
                        r[0x55d], r[0x55e], r[0x557], r[0x558], tic[ti * 8], tic[ti * 8 + 1], tic[ti * 8 + 4]);
            glBindTextureUnit(unit, get_texture(tic + ti * 8));
            glBindSampler(unit, get_sampler(tsc + si * 8));
        }
    }
    if (getenv("DRAW_LOG")) {
        static uint64_t ndraw;
        uint64_t lo = strtoull(getenv("DRAW_LOG"), NULL, 0);
        if (ndraw >= lo && ndraw < lo + 40) {
            const uint32_t *t0 = r + 0x200 + (r[0x487] >> 4 & 7) * 0x10;
            fprintf(stderr, "[draw %lu] vs=%#x fs=%#x rt0=%#x%08x %ux%u fmt=%#x n=%u idx=%d cnt=%u blend=%u tex=%d",
                    ndraw, r[0x811], fs ? r[0x851] : 0, t0[0], t0[1], t0[2], t0[3], t0[4], r[0x487] & 0xf, indexed, indexed ? icount : vcount, r[0x4d8] & 1, fs ? fs->info.ntex : 0);
            if (fs) {
                const CbBind *tb = &cb[4][r[0x982] & 0x1f];
                const uint8_t *hcb = tb->addr ? gpu_ptr(tb->addr) : NULL;
                const uint32_t *tic = gpu_ptr((uint64_t)(r[0x55d] & 0xff) << 32 | r[0x55e]);
                for (int t = 0; t < fs->info.ntex && hcb && tic; t++) {
                    const uint32_t *e = tic + (*(const uint32_t *)(hcb + fs->info.tex_handle[t]) & 0xfffff) * 8;
                    uint64_t va = (uint64_t)(e[2] & 0xffff) << 32 | e[1];
                    fprintf(stderr, " t%d=[va %#lx %ux%u fmt %#x]", t, va, (e[4] & 0xffff) + 1, (e[5] & 0xffff) + 1, e[0] & 0x7f);
                }
            }
            fputc('\n', stderr);
        }
        ndraw++;
    }
    GLenum topo[] = {GL_POINTS, GL_LINES, GL_LINE_LOOP, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_TRIANGLES, GL_TRIANGLES, GL_TRIANGLE_FAN};
    uint32_t prim = r[0x586] & 0xffff;
    GLenum mode = prim < 10 ? topo[prim] : GL_TRIANGLES;
    if (ib) {
        glNamedBufferData(ibuf, (GLsizeiptr)(icount + ifirst) * isz, ib, GL_STREAM_DRAW);
        glVertexArrayElementBuffer(vao, ibuf);
        GLenum it = isz == 1 ? GL_UNSIGNED_BYTE : isz == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
        glDrawElementsInstancedBaseVertex(mode, (GLsizei)icount, it, (void *)(uintptr_t)(ifirst * isz), 1, (GLint)r[0x50d]);
    } else glDrawArraysInstanced(mode, (GLint)vfirst, (GLsizei)vcount, 1);
}

void surfaces_dump(uint64_t frame) {
    for (int i = 0; i < nsurfaces; i++) {
        Surface *s = &surfaces[i];
        if (s->depth) continue;
        size_t n = (size_t)s->width * s->height * 3;
        uint8_t *px = malloc(n);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTextureImage(s->tex, 0, GL_RGB, GL_UNSIGNED_BYTE, (GLsizei)n, px);
        char path[96];
        snprintf(path, sizeof path, "build/surf_%05lu_%02d_%ux%u_%x.ppm", frame, i, s->width, s->height, s->format);
        FILE *f = fopen(path, "wb");
        fprintf(f, "P6 %u %u 255\n", s->width, s->height);
        fwrite(px, 1, n, f);
        fclose(f);
        free(px);
    }
}
