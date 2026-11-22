#include "memfd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include "jit.h"
#endif

#define MFD_PAGE 4096u

struct memfd {
    int refs;
    uint64_t size;
    uint8_t **pages; /* host page per file page; NULL until materialized */
    size_t cap;      /* slots in pages */
    char name[64];
};

memfd_t *memfd_new(const char *name)
{
    memfd_t *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->refs = 1;
    snprintf(m->name, sizeof m->name, "%s", name ? name : "");
    return m;
}

void memfd_ref(memfd_t *m)
{
    if (m)
        m->refs++;
}

void memfd_unref(memfd_t *m)
{
    if (!m || --m->refs > 0)
        return;
    for (size_t i = 0; i < m->cap; i++) {
#ifdef __EMSCRIPTEN__
        if (m->pages[i])
            jit_page_changed(m->pages[i]);
#endif
        free(m->pages[i]);
    }
    free(m->pages);
    free(m);
}

const char *memfd_name(const memfd_t *m) { return m->name; }
uint64_t memfd_size(const memfd_t *m) { return m->size; }

static int grow(memfd_t *m, uint64_t idx)
{
    if (idx < m->cap)
        return 0;
    size_t nc = m->cap ? m->cap : 64;
    while (nc <= idx)
        nc *= 2;
    uint8_t **np = realloc(m->pages, nc * sizeof *np);
    if (!np)
        return -1;
    memset(np + m->cap, 0, (nc - m->cap) * sizeof *np);
    m->pages = np;
    m->cap = nc;
    return 0;
}

uint8_t *memfd_page(memfd_t *m, uint64_t idx)
{
    if (grow(m, idx) < 0)
        return NULL;
    if (!m->pages[idx]) {
        uint8_t *p = aligned_alloc(MFD_PAGE, MFD_PAGE);
        if (!p)
            return NULL;
        memset(p, 0, MFD_PAGE);
        m->pages[idx] = p;
    }
    return m->pages[idx];
}

int memfd_truncate(memfd_t *m, uint64_t size)
{
    if (size < m->size) {
        /* Pages past the new end may still be mapped, so zero them rather than free them. */
        for (uint64_t off = size; off < m->size;) {
            uint64_t idx = off / MFD_PAGE;
            uint64_t in = off % MFD_PAGE;
            uint64_t end = (idx + 1) * MFD_PAGE;
            if (end > m->size)
                end = m->size;
            if (idx < m->cap && m->pages[idx]) {
#ifdef __EMSCRIPTEN__
                jit_page_changed(m->pages[idx]);
#endif
                memset(m->pages[idx] + in, 0, (size_t)(end - off));
            }
            off = end;
        }
    }
    m->size = size;
    return 0;
}

int64_t memfd_pread(memfd_t *m, void *buf, size_t n, uint64_t off)
{
    if (off >= m->size)
        return 0;
    if (n > m->size - off)
        n = (size_t)(m->size - off);
    uint8_t *d = buf;
    size_t done = 0;
    while (done < n) {
        uint64_t pos = off + done;
        uint64_t idx = pos / MFD_PAGE;
        size_t in = (size_t)(pos % MFD_PAGE);
        size_t chunk = MFD_PAGE - in;
        if (chunk > n - done)
            chunk = n - done;
        if (idx < m->cap && m->pages[idx])
            memcpy(d + done, m->pages[idx] + in, chunk);
        else
            memset(d + done, 0, chunk);
        done += chunk;
    }
    return (int64_t)n;
}

int64_t memfd_pwrite(memfd_t *m, const void *buf, size_t n, uint64_t off)
{
    if (off + n < off)
        return -1;
    if (off + n > m->size)
        m->size = off + n;
    const uint8_t *s = buf;
    size_t done = 0;
    while (done < n) {
        uint64_t pos = off + done;
        uint64_t idx = pos / MFD_PAGE;
        size_t in = (size_t)(pos % MFD_PAGE);
        size_t chunk = MFD_PAGE - in;
        if (chunk > n - done)
            chunk = n - done;
        uint8_t *p = memfd_page(m, idx);
        if (!p)
            return -1;
#ifdef __EMSCRIPTEN__
        jit_page_changed(p);
#endif
        memcpy(p + in, s + done, chunk);
        done += chunk;
    }
    return (int64_t)n;
}
