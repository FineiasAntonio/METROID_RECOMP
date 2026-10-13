#pragma once
#include <stdint.h>

typedef struct {
    uint64_t size, addr;
    uint32_t align, kind, id, iova;
} NvMapHandle;

typedef struct {
    uint64_t gpu_va, cpu_addr, size;
} GpuMapping;

extern NvMapHandle nvmap[4096];
extern GpuMapping gpu_maps[65536];
extern int ngpu_maps;

uint32_t syncpt_value(int id);
void gpu_submit(int channel_syncpt, const uint64_t *entries, uint32_t n);
void gpu_dump_hist(void);
void host1x_cmdbuf(int cls, const uint32_t *w, uint32_t n);
