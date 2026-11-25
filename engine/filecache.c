#include "filecache.h"
#include "snap.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"

#define FC_CHUNK 4096u

typedef struct fc {
    uint64_t dev, ino;
    memfd_t *m;  /* the cache; no reference of the entry's own (see fc_on_free) */
    int rfd;     /* a dup of the host fd that created the entry: pages are filled through it; -1 only if unset */
    int wfd;     /* a dup of a host fd open for writing, for write-back; -1 if none */
    struct fc *next;
} fc_t;

static fc_t *files;
static int registered;

static void fc_unlink(fc_t *e)
{
    for (fc_t **pp = &files; *pp; pp = &(*pp)->next) {
        if (*pp == e) {
            *pp = e->next;
            return;
        }
    }
}

static fc_t *fc_find(uint64_t dev, uint64_t ino)
{
    for (fc_t *e = files; e; e = e->next)
        if (e->dev == dev && e->ino == ino)
            return e;
    return NULL;
}

/* Fills page idx of the cache from the host file. The page is zero to begin with, so the bytes past the
 * file's end (and any the host returns short) stay zero. */
static void fc_fill(memfd_t *m, uint64_t idx, uint8_t *page, void *ctx)
{
    fc_t *e = ctx;
    uint64_t off = idx * FC_CHUNK;
    uint64_t size = memfd_size(m);
    if (off >= size || e->rfd < 0)
        return;
    size_t n = size - off < FC_CHUNK ? (size_t)(size - off) : FC_CHUNK;
    (void)host_pread(e->rfd, page, n, off);
}

/* Writes the cache's bytes that differ from the host file, then sets the host file's size to the cache's.
 * Only the pages a view or a write has materialised can differ from the host (a page is filled from the
 * host when it is first used), so only those are compared. */
static void fc_write_back(fc_t *e)
{
    if (e->wfd < 0 || !e->m)
        return;
    uint64_t size = memfd_size(e->m);
    uint8_t a[FC_CHUNK], b[FC_CHUNK];
    for (uint64_t off = 0; off < size; off += FC_CHUNK) {
        if (!memfd_materialized(e->m, off / FC_CHUNK))
            continue;
        size_t n = size - off < FC_CHUNK ? (size_t)(size - off) : FC_CHUNK;
        memfd_pread(e->m, a, n, off);
        int64_t r = host_pread(e->wfd, b, n, off);
        if (r < 0)
            r = 0;
        if ((size_t)r < n)
            memset(b + r, 0, n - (size_t)r);
        if (memcmp(a, b, n) != 0)
            host_pwrite(e->wfd, a, n, off);
    }
    host_stat st;
    if (host_stat_fd(e->wfd, &st) == 0 && st.size != size)
        host_ftruncate(e->wfd, size);
}

/* The last view of the cache is gone (or the engine is exiting): the file takes the cache's bytes, then the
 * entry goes. Runs before the memfd's pages are freed. */
static void fc_on_free(memfd_t *m, void *ctx)
{
    (void)m;
    fc_t *e = ctx;
    fc_write_back(e);
    if (e->wfd >= 0)
        host_close(e->wfd);
    if (e->rfd >= 0)
        host_close(e->rfd);
    e->wfd = -1;
    e->rfd = -1;
    e->m = NULL;
    fc_unlink(e);
    free(e);
}

static void fc_register(void)
{
    if (!registered) {
        registered = 1;
        snap_atexit(fc_flush_all);
    }
}

int fc_map_shared(uint64_t va, uint64_t len, unsigned prot, uint64_t off, const host_stat *st, int hfd,
                  int can_write)
{
    fc_t *e = fc_find(st->dev, st->ino);
    int created = 0;
    if (!e) {
        e = calloc(1, sizeof *e);
        if (!e)
            return -1;
        e->dev = st->dev;
        e->ino = st->ino;
        e->rfd = -1;
        e->wfd = -1;
        e->m = memfd_new("xiaoji-file");
        if (!e->m) {
            free(e);
            return -1;
        }
        memfd_set_file(e->m, fc_on_free, e);
        memfd_set_fill(e->m, fc_fill);
        e->rfd = host_dup(hfd);
        if (e->rfd < 0 || memfd_truncate(e->m, st->size) != 0) {
            memfd_unref(e->m); /* frees e through fc_on_free */
            return -1;
        }
        e->next = files;
        files = e;
        created = 1;
        fc_register();
    }
    if (can_write && e->wfd < 0) {
        e->wfd = host_dup(hfd);
        if (e->wfd < 0) {
            if (created)
                memfd_unref(e->m);
            return -1;
        }
    }
    memfd_t *m = e->m;
    int rc = mem_map_shared(va, len, prot, m, off);
    if (rc == 0)
        mem_shm_resize(m, memfd_size(m));
    if (created)
        memfd_unref(m); /* the creator's reference: the views hold the cache now */
    return rc;
}

memfd_t *fc_lookup(uint64_t dev, uint64_t ino)
{
    fc_t *e = fc_find(dev, ino);
    return e ? e->m : NULL;
}

void fc_patch(uint64_t dev, uint64_t ino, const void *buf, size_t n, uint64_t off)
{
    fc_t *e = fc_find(dev, ino);
    if (!e || n == 0)
        return;
    uint64_t before = memfd_size(e->m);
    if (memfd_pwrite(e->m, buf, n, off) < 0)
        return;
    if (memfd_size(e->m) != before)
        mem_shm_resize(e->m, memfd_size(e->m));
}

void fc_truncate(uint64_t dev, uint64_t ino, uint64_t len)
{
    fc_t *e = fc_find(dev, ino);
    if (!e)
        return;
    memfd_truncate(e->m, len);
    mem_shm_resize(e->m, len);
}

void fc_flush_file(uint64_t dev, uint64_t ino)
{
    fc_t *e = fc_find(dev, ino);
    if (e)
        fc_write_back(e);
}

void fc_sync_va(uint64_t va)
{
    uint64_t start, off;
    memfd_t *m = mem_region_memfd(va, &start, &off);
    if (!m || !memfd_is_file(m))
        return;
    for (fc_t *e = files; e; e = e->next)
        if (e->m == m) {
            fc_write_back(e);
            return;
        }
}

void fc_flush_all(void)
{
    for (fc_t *e = files; e; e = e->next)
        fc_write_back(e);
}
