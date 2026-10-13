#pragma once
#include <stdint.h>

typedef struct {
    uint64_t cpu_addr;
    uint32_t width, height, stride, format, block_height_log2;
} PresentInfo;

void render_loop(void);
int render_active(void);
void render_run(void (*fn)(void *), void *arg);
void render_present(const PresentInfo *p);
uint64_t deswizzle_offset(uint32_t x_bytes, uint32_t y, uint32_t width_bytes, uint32_t bh_log2);
