#pragma once
#include <stdint.h>

enum { STAGE_VERTEX = 1, STAGE_FRAGMENT = 5 };
#define MAX_TEX 16

typedef struct {
    int stage;
    uint32_t header[20];
    uint32_t cbuf_mask, attr_in, attr_out, attr_int, frag_out;
    int ntex;
    uint32_t tex_handle[MAX_TEX];
    int tex_kind[MAX_TEX];
    int unsupported;
    char *glsl;
} ShaderInfo;

int shader_translate(const uint64_t *prog, ShaderInfo *info);
int sass_decode(uint64_t w);
const char *sass_name(int op);
