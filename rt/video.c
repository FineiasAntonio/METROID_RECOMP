#define _GNU_SOURCE
#include <dirent.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "nv.h"
#include "render.h"

extern void *gpu_ptr(uint64_t va);
extern uint32_t nvdec_regs[256][1024];

typedef struct {
    uint64_t key;
    int file, packet, nsub;
} Entry;

static char *files[256];
static int nfiles;
static Entry *table;
static size_t table_cap;
static int indexed;

static struct {
    int file, next_packet;
    AVFormatContext *fmt;
    AVCodecContext *dec;
    int stream;
    AVFrame *frame;
    AVPacket *pkt;
} cur = {.file = -1};

static AVFrame *latest;
static pthread_mutex_t vlock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t tail_key(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    size_t k = n < 24 ? n : 24;
    for (size_t i = n - k; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h | 1;
}

static void insert(uint64_t key, int file, int packet, int nsub) {
    size_t i = key % table_cap;
    while (table[i].key) i = (i + 1) % table_cap;
    table[i] = (Entry){key, file, packet, nsub};
}

static int pending_subs;

static const Entry *lookup(uint64_t key) {
    const Entry *same = NULL, *ahead = NULL, *back = NULL, *other = NULL;
    for (size_t i = key % table_cap; table[i].key; i = (i + 1) % table_cap) {
        const Entry *e = &table[i];
        if (e->key != key) continue;
        if (e->file != cur.file) { if (!other) other = e; continue; }
        if (pending_subs > 0 && e->packet == cur.next_packet - 1) same = e;
        else if (e->packet >= cur.next_packet) { if (!ahead || e->packet < ahead->packet) ahead = e; }
        else if (!back || e->packet < back->packet) back = e;
    }
    if (same) { pending_subs--; return same; }
    const Entry *e = ahead ? ahead : back ? back : other;
    if (e) pending_subs = e->nsub - 1;
    return e;
}

static void scan_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat sb;
        if (stat(path, &sb)) continue;
        if (S_ISDIR(sb.st_mode)) scan_dir(path);
        else if (strstr(e->d_name, ".webm") && nfiles < 256) files[nfiles++] = strdup(path);
    }
    closedir(d);
}

static void index_subframes(const uint8_t *p, int size, int file, int packet) {
    uint8_t marker = p[size - 1];
    if ((marker & 0xe0) == 0xc0) {
        int frames = (marker & 7) + 1, mag = ((marker >> 3) & 3) + 1, isz = 2 + mag * frames;
        if (size >= isz && p[size - isz] == marker) {
            const uint8_t *idx = p + size - isz + 1;
            int off = 0;
            for (int f = 0; f < frames; f++) {
                int fs = 0;
                for (int b = 0; b < mag; b++) fs |= idx[f * mag + b] << (8 * b);
                if (off + fs <= size - isz) insert(tail_key(p + off, (size_t)fs), file, packet, frames);
                off += fs;
            }
            return;
        }
    }
    insert(tail_key(p, (size_t)size), file, packet, 1);
}

static void build_index(void) {
    indexed = 1;
    const char *root = getenv("DREAD_ROMFS_DIR") ? getenv("DREAD_ROMFS_DIR") : "extracted/romfs";
    char dir[512];
    snprintf(dir, sizeof dir, "%s/system/video", root);
    scan_dir(dir);
    table_cap = 1 << 20;
    table = calloc(table_cap, sizeof *table);
    AVPacket *pkt = av_packet_alloc();
    size_t total = 0;
    for (int f = 0; f < nfiles; f++) {
        AVFormatContext *fmt = NULL;
        if (avformat_open_input(&fmt, files[f], NULL, NULL) < 0) continue;
        avformat_find_stream_info(fmt, NULL);
        int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        int n = 0;
        while (av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == vs && pkt->size > 0) { index_subframes(pkt->data, pkt->size, f, n); total++; }
            if (pkt->stream_index == vs) n++;
            av_packet_unref(pkt);
        }
        avformat_close_input(&fmt);
    }
    av_packet_free(&pkt);
    fprintf(stderr, "[video] índice: %d vídeos, %zu pacotes\n", nfiles, total);
}

static int open_file(int f) {
    if (cur.fmt) { avformat_close_input(&cur.fmt); avcodec_free_context(&cur.dec); }
    cur.file = -1;
    if (avformat_open_input(&cur.fmt, files[f], NULL, NULL) < 0) return 0;
    avformat_find_stream_info(cur.fmt, NULL);
    cur.stream = av_find_best_stream(cur.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    const AVCodec *codec = avcodec_find_decoder(cur.fmt->streams[cur.stream]->codecpar->codec_id);
    cur.dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(cur.dec, cur.fmt->streams[cur.stream]->codecpar);
    cur.dec->thread_count = 4;
    if (avcodec_open2(cur.dec, codec, NULL) < 0) return 0;
    if (!cur.frame) cur.frame = av_frame_alloc();
    if (!cur.pkt) cur.pkt = av_packet_alloc();
    cur.file = f;
    cur.next_packet = 0;
    if (getenv("VIDEO_TRACE")) fprintf(stderr, "[video] abrindo %s\n", files[f]);
    return 1;
}

static void decode_until(int packet) {
    while (cur.next_packet <= packet) {
        int r = av_read_frame(cur.fmt, cur.pkt);
        if (r < 0) return;
        if (cur.pkt->stream_index != cur.stream) { av_packet_unref(cur.pkt); continue; }
        cur.next_packet++;
        avcodec_send_packet(cur.dec, cur.pkt);
        av_packet_unref(cur.pkt);
        while (avcodec_receive_frame(cur.dec, cur.frame) == 0) {
            if (!latest) latest = av_frame_alloc();
            av_frame_unref(latest);
            av_frame_ref(latest, cur.frame);
        }
    }
}

void nvdec_execute(void) {
    if (getenv("NO_VIDEO")) return;
    const uint8_t *pic = gpu_ptr((uint64_t)nvdec_regs[0xf0][0x101] << 8);
    const uint8_t *bs = gpu_ptr((uint64_t)nvdec_regs[0xf0][0x102] << 8);
    if (!pic || !bs) return;
    uint32_t size = *(const uint32_t *)(pic + 0x30);
    if (!size || size > (16u << 20) || gpu_ptr(((uint64_t)nvdec_regs[0xf0][0x102] << 8) + size - 1) != bs + size - 1) return;
    pthread_mutex_lock(&vlock);
    if (!indexed) build_index();
    const Entry *e = lookup(tail_key(bs, size));
    if (!e) {
        static int misses;
        if (misses++ < 5) fprintf(stderr, "[video] quadro de %u bytes não encontrado nos vídeos\n", size);
        pthread_mutex_unlock(&vlock);
        return;
    }
    if (getenv("VIDEO_TRACE")) fprintf(stderr, "[video] bitstream %u bytes -> arquivo %d pacote %d (próximo %d)\n", size, e->file, e->packet, cur.next_packet);
    if (cur.file != e->file || e->packet < cur.next_packet - 1) {
        if (cur.file != e->file || e->packet < cur.next_packet - 1) open_file(e->file);
    }
    if (cur.file == e->file) decode_until(e->packet);
    pthread_mutex_unlock(&vlock);
}

void vic_execute(void) {
    const uint64_t *cfg = gpu_ptr((uint64_t)nvdec_regs[0x5d][0x1c2] << 8);
    uint8_t *out = gpu_ptr((uint64_t)nvdec_regs[0x5d][0x1c8] << 8);
    if (!cfg || !out) return;
    uint64_t oc = cfg[4];
    uint32_t fmt = oc & 0x7f, blk_kind = oc >> 11 & 0xf, blk_h = oc >> 15 & 0xf;
    uint32_t w = (uint32_t)(oc >> 32 & 0x3fff) + 1, h = (uint32_t)(oc >> 46 & 0x3fff) + 1;
    if ((fmt != 0x1f && fmt != 0x20 && fmt != 0x23) || w > 4096 || h > 4096) {
        static int warned;
        if (warned++ < 4 && getenv("VIDEO_TRACE")) fprintf(stderr, "[vic] configuração ignorada: fmt %#x %ux%u\n", fmt, w, h);
        return;
    }
    pthread_mutex_lock(&vlock);
    if (!latest) { pthread_mutex_unlock(&vlock); return; }
    static uint8_t *rgba;
    static size_t cap;
    size_t need = (size_t)w * h * 4;
    if (need > cap) { rgba = realloc(rgba, need); cap = need; }
    static struct SwsContext *sws;
    enum AVPixelFormat dst = fmt == 0x20 ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGBA;
    sws = sws_getCachedContext(sws, latest->width, latest->height, latest->format, (int)w, (int)h, dst, SWS_BILINEAR, NULL, NULL, NULL);
    uint8_t *dp[1] = {rgba};
    int ds[1] = {(int)w * 4};
    sws_scale(sws, (const uint8_t *const *)latest->data, latest->linesize, 0, latest->height, dp, ds);
    pthread_mutex_unlock(&vlock);
    if (fmt == 0x23)
        for (size_t i = 3; i < need; i += 4) rgba[i] = 0xff;
    uint32_t wb = w * 4;
    extern uint64_t gpu_avail(uint64_t va);
    uint64_t avail = gpu_avail((uint64_t)nvdec_regs[0x5d][0x1c8] << 8);
    if (blk_kind) {
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < wb; x += 16) {
                uint64_t o = deswizzle_offset(x, y, wb, blk_h);
                if (o + 16 <= avail) memcpy(out + o, rgba + (size_t)y * wb + x, 16);
            }
    } else
        for (uint32_t y = 0; y < h && (uint64_t)(y + 1) * wb <= avail; y++) memcpy(out + (size_t)y * wb, rgba + (size_t)y * wb, wb);
    extern void texture_invalidate(uint64_t cpu, uint64_t size);
    texture_invalidate((uint64_t)(uintptr_t)out, (uint64_t)wb * h);
    if (getenv("VIDEO_TRACE") > (char *)1) fprintf(stderr, "[vic] %ux%u fmt %#x bloco %u/%u out=%p avail=%#lx\n", w, h, fmt, blk_kind, blk_h, (void *)out, avail);
}
