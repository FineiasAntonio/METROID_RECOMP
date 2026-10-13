#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "ipc.h"

#define SVC(name, id) static void name##_h(Session *s, void *st, Request *rq, Response *rs); \
    const Service name = {id, name##_h}; \
    static void name##_h(Session *s, void *st, Request *rq, Response *rs)
#define UNUSED (void)s; (void)st; (void)rs

enum { FS_NOT_FOUND = 0x202, FS_EXISTS = 0x402 };

typedef struct {
    int fd;
    char path[1024];
} HostFile;

typedef struct {
    char root[1024];
} HostFs;

typedef struct {
    DIR *d;
    char path[1024];
    uint32_t filter;
} HostDir;

static const char *romfs_path(void) {
    const char *p = getenv("DREAD_ROMFS");
    return p ? p : "extracted/romfs.bin";
}

static const char *save_root(void) {
    const char *p = getenv("DREAD_SAVE");
    return p ? p : "save";
}

static void mkdirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

static void host_path(HostFs *fs, Request *rq, char *out, size_t n) {
    if (!rq->nx || !rq->x[0].addr) { snprintf(out, n, "%s", fs->root); return; }
    const char *p = (const char *)rq->x[0].addr;
    snprintf(out, n, "%s%s%.*s", fs->root, p[0] == '/' ? "" : "/", (int)strnlen(p, rq->x[0].size), p);
}

static uint32_t errno_result(void) { return errno == ENOENT || errno == ENOTDIR ? FS_NOT_FOUND : errno == EEXIST ? FS_EXISTS : 0x3e802; }

SVC(storage, "fs:IStorage") {
    UNUSED;
    HostFile *f = st;
    switch (rq->cmd) {
    case 0: {
        uint64_t off = IN(uint64_t, 0), size = IN(uint64_t, 8);
        Buf b = rq->b[0];
        if (size > b.size) size = b.size;
        ssize_t r = pread(f->fd, (void *)b.addr, size, (off_t)off);
        if (getenv("FS_TRACE")) fprintf(stderr, "[fs] storage read off=%#lx size=%#lx buf=%#lx/%#lx -> %zd\n", off, IN(uint64_t, 8), b.addr, b.size, r);
        if (r < 0) rs->result = 0x3e802;
        return;
    }
    case 4: {
        struct stat sb;
        fstat(f->fd, &sb);
        OUT(uint64_t, (uint64_t)sb.st_size);
        return;
    }
    case 2: return;
    }
    UNHANDLED();
}

SVC(file, "fs:IFile") {
    UNUSED;
    HostFile *f = st;
    switch (rq->cmd) {
    case 0: {
        uint64_t off = IN(uint64_t, 8), size = IN(uint64_t, 16);
        Buf b = rq->b[0];
        if (size > b.size) size = b.size;
        ssize_t r = pread(f->fd, (void *)b.addr, size, (off_t)off);
        OUT(uint64_t, r < 0 ? 0 : (uint64_t)r);
        return;
    }
    case 1: {
        uint64_t off = IN(uint64_t, 8), size = IN(uint64_t, 16);
        Buf b = rq->a[0];
        if (size > b.size) size = b.size;
        if (pwrite(f->fd, (void *)b.addr, size, (off_t)off) < 0) rs->result = errno_result();
        return;
    }
    case 2: fsync(f->fd); return;
    case 3: if (ftruncate(f->fd, (off_t)IN(uint64_t, 0))) rs->result = errno_result(); return;
    case 4: {
        struct stat sb;
        fstat(f->fd, &sb);
        OUT(uint64_t, (uint64_t)sb.st_size);
        return;
    }
    }
    UNHANDLED();
}

SVC(directory, "fs:IDirectory") {
    UNUSED;
    HostDir *d = st;
    switch (rq->cmd) {
    case 0: {
        Buf b = rq->b[0];
        uint64_t max = b.size / 0x310, n = 0;
        struct dirent *e;
        while (n < max && (e = readdir(d->d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char full[2048];
            struct stat sb;
            snprintf(full, sizeof full, "%s/%s", d->path, e->d_name);
            if (stat(full, &sb)) continue;
            int isdir = S_ISDIR(sb.st_mode);
            if (!(d->filter & (isdir ? 1 : 2))) continue;
            uint8_t *ent = (uint8_t *)b.addr + n * 0x310;
            memset(ent, 0, 0x310);
            snprintf((char *)ent, 0x301, "%s", e->d_name);
            ent[0x304] = isdir ? 0 : 1;
            *(uint64_t *)(ent + 0x308) = isdir ? 0 : (uint64_t)sb.st_size;
            n++;
        }
        OUT(uint64_t, n);
        return;
    }
    case 1: {
        uint64_t n = 0;
        long pos = telldir(d->d);
        rewinddir(d->d);
        struct dirent *e;
        while ((e = readdir(d->d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
        seekdir(d->d, pos);
        OUT(uint64_t, n);
        return;
    }
    }
    UNHANDLED();
}

SVC(filesystem, "fs:IFileSystem") {
    UNUSED;
    HostFs *fs = st;
    char p[1300];
    host_path(fs, rq, p, sizeof p);
    if (getenv("FS_TRACE")) fprintf(stderr, "[fs] cmd %u %s\n", rq->cmd, p);
    switch (rq->cmd) {
    case 0: {
        int fd = open(p, O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd < 0) { rs->result = errno_result(); return; }
        if (ftruncate(fd, (off_t)IN(uint64_t, 8))) rs->result = errno_result();
        close(fd);
        return;
    }
    case 1: if (unlink(p)) rs->result = errno_result(); return;
    case 2: if (mkdir(p, 0755)) rs->result = errno_result(); return;
    case 3: if (rmdir(p)) rs->result = errno_result(); return;
    case 4: case 13: {
        char cmd[1400];
        snprintf(cmd, sizeof cmd, rq->cmd == 4 ? "rm -rf '%s'" : "find '%s' -mindepth 1 -delete", p);
        if (system(cmd)) rs->result = 0x3e802;
        return;
    }
    case 5: case 6: {
        char q[1300];
        const char *dst = (const char *)rq->x[1].addr;
        snprintf(q, sizeof q, "%s%s%s", fs->root, dst[0] == '/' ? "" : "/", dst);
        if (rename(p, q)) rs->result = errno_result();
        return;
    }
    case 7: {
        struct stat sb;
        if (stat(p, &sb)) { rs->result = FS_NOT_FOUND; return; }
        OUT(uint32_t, S_ISDIR(sb.st_mode) ? 0 : 1);
        return;
    }
    case 8: {
        uint32_t mode = IN(uint32_t, 0);
        int fd = open(p, mode & 2 ? O_RDWR : O_RDONLY);
        if (fd < 0) { rs->result = errno_result(); return; }
        HostFile *f = calloc(1, sizeof *f);
        f->fd = fd;
        snprintf(f->path, sizeof f->path, "%s", p);
        rs_object(s, rs, &file, f);
        return;
    }
    case 9: {
        DIR *dd = opendir(p);
        if (!dd) { rs->result = errno_result(); return; }
        HostDir *d = calloc(1, sizeof *d);
        d->d = dd;
        d->filter = IN(uint32_t, 0);
        snprintf(d->path, sizeof d->path, "%s", p);
        rs_object(s, rs, &directory, d);
        return;
    }
    case 10: sync(); return;
    case 11: OUT(uint64_t, 0x100000000ull); return;
    case 12: OUT(uint64_t, 0x100000000ull); return;
    case 14: memset(rs_out(rs, 0x20), 0, 0x20); return;
    }
    UNHANDLED();
}

static HostFs *hostfs(const char *sub) {
    HostFs *fs = calloc(1, sizeof *fs);
    snprintf(fs->root, sizeof fs->root, "%s/%s", save_root(), sub);
    mkdirs(fs->root);
    return fs;
}

SVC(fsp_srv, "fsp-srv") {
    UNUSED;
    switch (rq->cmd) {
    case 1: case 620: case 1003: case 1004: case 1006: case 1016: return;
    case 1005: OUT(uint32_t, 0); return;
    case 1011: OUT(uint32_t, 0); OUT(uint32_t, 1); return;
    case 18: rs_object(s, rs, &filesystem, hostfs("sdcard")); return;
    case 22: case 23: return;
    case 51: case 53: {
        uint64_t uid_lo = IN(uint64_t, 8 + 0x8), uid_hi = IN(uint64_t, 8 + 0x10);
        uint8_t type = IN(uint8_t, 8 + 0x18);
        char sub[256];
        snprintf(sub, sizeof sub, "save/%u_%016lx%016lx", type, uid_hi, uid_lo);
        rs_object(s, rs, &filesystem, hostfs(sub));
        return;
    }
    case 200: {
        int fd = open(romfs_path(), O_RDONLY);
        if (fd < 0) { fprintf(stderr, "[fs] não abriu %s (rode tools/extract_romfs.sh)\n", romfs_path()); abort(); }
        HostFile *f = calloc(1, sizeof *f);
        f->fd = fd;
        rs_object(s, rs, &storage, f);
        return;
    }
    case 202: rs->result = FS_NOT_FOUND; return;
    case 203: rs->result = FS_NOT_FOUND; return;
    }
    UNHANDLED();
}

SVC(ectx_registrar, "ectx:IContextRegistrar") {
    UNUSED;
    switch (rq->cmd) {
    case 0: OUT(uint32_t, 0); return;
    }
    UNHANDLED();
}

SVC(ectx_aw, "ectx:aw") {
    UNUSED;
    switch (rq->cmd) {
    case 0: rs_object(s, rs, &ectx_registrar, NULL); return;
    }
    UNHANDLED();
}
