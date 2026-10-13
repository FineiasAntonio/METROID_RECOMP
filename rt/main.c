#include <stdio.h>
#include "kernel.h"
#include "runtime.h"
#include "render.h"
#include <pthread.h>
#include <stdlib.h>

static void *game_thread(void *p) {
    (void)p;
    kernel_run_main(modules[0].base, 0x100000, 44, 0);
    exit(0);
}

int main(int argc, char **argv) {
    kernel_init();
    load_modules(argc > 1 ? argv[1] : "extracted/nso", 0);
    fprintf(stderr, "[boot] módulos carregados, entrando no rtld em %#lx\n", modules[0].base);
    if (getenv("HEADLESS")) {
        kernel_run_main(modules[0].base, 0x100000, 44, 0);
        return 0;
    }
    pthread_t t;
    pthread_create(&t, NULL, game_thread, NULL);
    render_loop();
    return 0;
}
