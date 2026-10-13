#include <SDL2/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "gl.h"
#include "render.h"

#define X(type, name) type name;
GL_FUNCS(X)
#undef X

typedef struct Work {
    void (*fn)(void *);
    void *arg;
    int done;
    PresentInfo present;
    int is_present;
    struct Work *next;
} Work;

static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qcond = PTHREAD_COND_INITIALIZER, dcond = PTHREAD_COND_INITIALIZER;
static Work *head, *tail;
static int active;
static SDL_Window *window;
static GLuint present_tex, present_fbo;

int render_active(void) { return active; }

static void push(Work *w) {
    pthread_mutex_lock(&qlock);
    w->next = NULL;
    if (tail) tail->next = w;
    else head = w;
    tail = w;
    pthread_cond_signal(&qcond);
    pthread_mutex_unlock(&qlock);
}

void render_run(void (*fn)(void *), void *arg) {
    if (!active) { fn(arg); return; }
    Work w = {.fn = fn, .arg = arg};
    push(&w);
    pthread_mutex_lock(&qlock);
    while (!w.done) pthread_cond_wait(&dcond, &qlock);
    pthread_mutex_unlock(&qlock);
}

void render_present(const PresentInfo *p) {
    if (!active) return;
    Work *w = calloc(1, sizeof *w);
    w->is_present = 1;
    w->present = *p;
    push(w);
}

uint64_t deswizzle_offset(uint32_t x, uint32_t y, uint32_t width_bytes, uint32_t bh) {
    uint32_t block_rows = 8u << bh, gobs_x = (width_bytes + 63) / 64;
    uint64_t block = (uint64_t)(y / block_rows) * gobs_x + x / 64;
    uint32_t gy = y % block_rows;
    return block * (512u << bh) + (gy / 8) * 512 + ((x % 64) / 32) * 256 + ((gy % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (gy % 2) * 16 + (x % 16);
}

extern int surface_lookup(uint64_t cpu_addr, uint32_t w, uint32_t h, unsigned *tex, unsigned *fbo);

static void do_present(const PresentInfo *p) {
    int ww, wh;
    SDL_GL_GetDrawableSize(window, &ww, &wh);
    GLuint fbo = 0, tex = 0;
    if (!surface_lookup(p->cpu_addr, p->width, p->height, &tex, &fbo)) {
        static uint8_t *pix;
        static size_t cap;
        size_t need = (size_t)p->width * p->height * 4;
        if (need > cap) { pix = realloc(pix, need); cap = need; }
        const uint8_t *src = (const uint8_t *)(uintptr_t)p->cpu_addr;
        uint32_t wb = p->stride * 4;
        for (uint32_t y = 0; y < p->height; y++)
            for (uint32_t x = 0; x < p->width; x++)
                memcpy(pix + ((size_t)y * p->width + x) * 4, src + deswizzle_offset(x * 4, y, wb, p->block_height_log2), 4);
        glBindTexture(GL_TEXTURE_2D, present_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)p->width, (GLsizei)p->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pix);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, present_fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, present_tex, 0);
        fbo = present_fbo;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer(0, 0, (GLint)p->width, (GLint)p->height, 0, wh, ww, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    static uint64_t frame;
    frame++;
    const char *shots = getenv("SHOT");
    if (shots) {
        char key[32];
        snprintf(key, sizeof key, ",%lu,", frame);
        char list[512];
        snprintf(list, sizeof list, ",%s,", shots);
        if (strstr(list, key)) {
            uint8_t *px = malloc((size_t)ww * wh * 3);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            glReadBuffer(GL_BACK);
            glReadPixels(0, 0, ww, wh, GL_RGB, GL_UNSIGNED_BYTE, px);
            char path[64];
            snprintf(path, sizeof path, "build/shot_%05lu.ppm", frame);
            FILE *f = fopen(path, "wb");
            fprintf(f, "P6 %d %d 255\n", ww, wh);
            for (int y = wh - 1; y >= 0; y--) fwrite(px + (size_t)y * ww * 3, 1, (size_t)ww * 3, f);
            fclose(f);
            free(px);
            fprintf(stderr, "[render] captura %s\n", path);
            extern void surfaces_dump(uint64_t);
            surfaces_dump(frame);
        }
    }
    SDL_GL_SwapWindow(window);
}

static void load_gl(void) {
#define X(type, name) name = (type)SDL_GL_GetProcAddress(#name); if (!name) { fprintf(stderr, "[gl] faltando %s\n", #name); exit(1); }
    GL_FUNCS(X)
#undef X
}

void render_loop(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) { fprintf(stderr, "[render] SDL_Init: %s\n", SDL_GetError()); exit(1); }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    window = SDL_CreateWindow("Metroid Dread (recomp)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) { fprintf(stderr, "[render] janela: %s\n", SDL_GetError()); exit(1); }
    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    if (!ctx) { fprintf(stderr, "[render] contexto GL: %s\n", SDL_GetError()); exit(1); }
    SDL_GL_SetSwapInterval(0);
    load_gl();
    fprintf(stderr, "[render] OpenGL %s em %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    glGenTextures(1, &present_tex);
    glGenFramebuffers(1, &present_fbo);
    active = 1;
    for (;;) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_QUIT) exit(0);
        pthread_mutex_lock(&qlock);
        if (!head) {
            struct timespec dl;
            clock_gettime(CLOCK_REALTIME, &dl);
            dl.tv_nsec += 4000000;
            if (dl.tv_nsec >= 1000000000) { dl.tv_nsec -= 1000000000; dl.tv_sec++; }
            pthread_cond_timedwait(&qcond, &qlock, &dl);
        }
        Work *w = head;
        if (w) { head = w->next; if (!head) tail = NULL; }
        pthread_mutex_unlock(&qlock);
        if (!w) continue;
        if (w->is_present) {
            do_present(&w->present);
            free(w);
            continue;
        }
        w->fn(w->arg);
        pthread_mutex_lock(&qlock);
        w->done = 1;
        pthread_cond_broadcast(&dcond);
        pthread_mutex_unlock(&qlock);
    }
}
