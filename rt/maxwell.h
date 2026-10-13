#pragma once
#include <stdint.h>

void maxwell_clear(const uint32_t *regs, uint32_t value, void *(*gpu_ptr)(uint64_t));
int surface_lookup(uint64_t cpu_addr, uint32_t w, uint32_t h, unsigned *tex, unsigned *fbo);
typedef struct { uint64_t addr; uint32_t size; } CbBind;
void maxwell_draw(const uint32_t *regs, void *(*gpu_ptr)(uint64_t), const CbBind (*cb)[18], int indexed);
