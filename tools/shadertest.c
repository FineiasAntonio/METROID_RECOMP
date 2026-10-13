#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shader.h"

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        static uint64_t buf[0x10000];
        size_t n = fread(buf, 8, 0x10000, f);
        fclose(f);
        (void)n;
        ShaderInfo info = {.stage = strstr(argv[i], "_s5") ? STAGE_FRAGMENT : STAGE_VERTEX};
        int ok = shader_translate(buf, &info);
        printf("// %s ok=%d unsupported=%d tex=%d cbuf=%#x in=%#x out=%#x frag=%#x\n%s\n", argv[i], ok, info.unsupported, info.ntex, info.cbuf_mask, info.attr_in, info.attr_out, info.frag_out, info.glsl);
    }
}
