#include <stdio.h>
#include <stdlib.h>
#include "nv.h"

void host1x_method(int cls, uint32_t method, uint32_t value);

static uint32_t thi_method0[256];
uint32_t nvdec_regs[256][1024];
extern void *gpu_ptr(uint64_t va);

void host1x_cmdbuf(int cls, const uint32_t *w, uint32_t n) {
    int cur = cls;
    for (uint32_t i = 0; i < n;) {
        uint32_t c = w[i++], op = c >> 28, off = c >> 16 & 0xfff;
        switch (op) {
        case 0:
            cur = (int)(c >> 6 & 0x3ff);
            for (int b = 0; b < 6; b++)
                if (c >> b & 1 && i < n) host1x_method(cur, off + (uint32_t)b, w[i++]);
            break;
        case 1:
            for (uint32_t k = 0; k < (c & 0xffff) && i < n; k++) host1x_method(cur, off + k, w[i++]);
            break;
        case 2:
            for (uint32_t k = 0; k < (c & 0xffff) && i < n; k++) host1x_method(cur, off, w[i++]);
            break;
        case 3:
            for (int b = 0; b < 16; b++)
                if (c >> b & 1 && i < n) host1x_method(cur, off + (uint32_t)b, w[i++]);
            break;
        case 4:
            host1x_method(cur, off, c & 0xffff);
            break;
        default:
            if (getenv("HOST1X_TRACE")) fprintf(stderr, "[host1x] opcode %u ignorado\n", op);
            break;
        }
    }
}

void host1x_method(int cls, uint32_t method, uint32_t value) {
    if (cls == 0x1) return;
    if (method == 0x10) { thi_method0[cls & 0xff] = value; return; }
    if (method == 0x11) {
        uint32_t m = thi_method0[cls & 0xff];
        if (getenv("HOST1X_TRACE")) fprintf(stderr, "[host1x] classe %#x método %#x = %#x\n", cls, m, value);
        nvdec_regs[cls & 0xff][m & 0x3ff] = value;
        if (cls == 0xf0 && m == 0xc0) { extern void nvdec_execute(void); nvdec_execute(); }
        if (cls == 0x5d && m == 0xc0) { extern void vic_execute(void); vic_execute(); }
        if (cls == 0x5d && m == 0xc0 && getenv("NVDEC_DUMP")) {
            static int nv;
            uint64_t *cfg = gpu_ptr((uint64_t)nvdec_regs[0x5d][0x1c2] << 8);
            if (cfg && nv++ < 2) {
                fprintf(stderr, "[vic] 1c1=%#x 1c3=%#x 1c8=%#x 1cc=%#x cfg:", nvdec_regs[0x5d][0x1c1], nvdec_regs[0x5d][0x1c3], nvdec_regs[0x5d][0x1c8], nvdec_regs[0x5d][0x1cc]);
                for (int i = 0; i < 24; i++) fprintf(stderr, " %016lx", cfg[i]);
                fputc('\n', stderr);
            }
        }
        if (cls == 0xf0 && m == 0xc0 && getenv("NVDEC_DUMP")) {
            static int n;
            uint8_t *pic = gpu_ptr((uint64_t)nvdec_regs[0xf0][0x101] << 8);
            uint8_t *bs = gpu_ptr((uint64_t)nvdec_regs[0xf0][0x102] << 8);
            if (pic && bs && n < 3) {
                uint32_t size = *(uint32_t *)(pic + 0x30);
                char path[64];
                snprintf(path, sizeof path, "build/nvdec_%d.bin", n);
                FILE *f = fopen(path, "wb");
                fwrite(bs, 1, size, f);
                fclose(f);
                snprintf(path, sizeof path, "build/nvdec_pic_%d.bin", n);
                f = fopen(path, "wb");
                fwrite(pic, 1, 0x200, f);
                fclose(f);
                fprintf(stderr, "[nvdec] frame %d: %u bytes\n", n, size);
            }
            n++;
        }
        return;
    }
    if (getenv("HOST1X_TRACE")) fprintf(stderr, "[host1x] classe %#x reg %#x = %#x\n", cls, method, value);
}
