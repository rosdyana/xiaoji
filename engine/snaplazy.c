/* Lazy snapshot pages (docs/snapshot.md, S5).
 *
 * A restore may start before every page of the image is in memory. The pages that may be late are pool pages (the
 * host pages of guest memory and of memfds, mem.c's page pool): every access to their bytes goes through a few places
 * (mem.c's page_lookup, which fills the TLBs and the fetch cache, and memfd.c), which call snap_lazy_need. Everything
 * else of the image (the engine's own state: page tables, processes, binder, the GL objects) is in memory before main.
 *
 * Who supplies a pending page:
 * - the page (XIAOJI_WEB): web/worker.js and web/lazypages.js. They wrote the hot part of the image before main and
 *   hold the rest: a fault calls xj_lazy_fault, which copies the page's chunk in (from what the background loader has
 *   brought, or with a synchronous request) and clears the bits of the pages it wrote; the scheduler loop calls
 *   xj_lazy_poll to copy in what arrived meanwhile.
 * - natively and in Node, for the tests (XIAOJI_SNAP_LAZY=1: on demand only; 2: also in the background, a few pages
 *   per millisecond; 3: as 2, without the poison, a debugging aid): the image is read as always, then every pool page of it is poisoned (0xA5) and marked pending,
 *   and a fault reads the page from the snapshot file again. A path that reads a pending page without filling it reads
 *   the poison, not zeros, so the snapshot cases fail.
 *
 * Pages leave the pending set by being filled or by being freed (snap_lazy_drop: a freed page's bytes are never read;
 * its next owner writes it first). When none is left the bitmap goes and the checks cost one load. */
#define _POSIX_C_SOURCE 200809L

#include "snaplazy.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "host.h"
#include "mem.h"
#include "snap.h"

#ifdef XIAOJI_WEB
#include <emscripten.h>
#endif

uint8_t *snap_lazy_bits;
uintptr_t snap_lazy_lo, snap_lazy_hi;

static uint64_t lz_pending, lz_total, lz_faults, lz_fault_ns, lz_dropped, lz_polled, lz_t0, lz_next_poll;

#ifdef XIAOJI_WEB
/* The page's side (web/lazypages.js, Module.xjLazy). Addresses cross as doubles, taken >>> 0 where they are pointers. */
EM_JS(double, xj_lazy_lo, (void), { var z = Module['xjLazy']; return z ? z.lo() : 0; });
EM_JS(double, xj_lazy_hi, (void), { var z = Module['xjLazy']; return z ? z.hi() : 0; });
EM_JS(double, xj_lazy_mark, (uint8_t *bits), { return Module['xjLazy'].mark(bits >>> 0); });
EM_JS(double, xj_lazy_fault, (double addr), { return Module['xjLazy'].fault(addr); });
EM_JS(double, xj_lazy_poll, (void), { return Module['xjLazy'].poll(); });
EM_JS(double, xj_lazy_finish, (void), { return Module['xjLazy'].finish(); });
EM_JS(void, xj_lazy_end, (double faults, double fault_ms, double dropped), {
    Module['xjLazy'].end({faults : faults, faultMs : fault_ms, dropped : dropped});
});
#else
static int lz_mode;        /* XIAOJI_SNAP_LAZY: 1 on demand, 2 also in the background */
static char lz_path[1024]; /* the snapshot file */
static uint64_t *lz_ext;   /* per extent: address, length, file offset of its bytes */
static uint64_t lz_next;   /* extents: count; the background's cursor (a page index) */
static uint64_t lz_cursor;
#endif

static void lz_end(void)
{
    double secs = (double)(host_monotonic_ns() - lz_t0) / 1e9;
    fprintf(stderr,
            "xiaoji: restore: lazy pages: all %llu in after %.1f s: %llu faults (%.0f ms blocked), %llu in the background, "
            "%llu freed before they were read\n",
            (unsigned long long)lz_total, secs, (unsigned long long)lz_faults, (double)lz_fault_ns / 1e6,
            (unsigned long long)lz_polled, (unsigned long long)lz_dropped);
#ifdef XIAOJI_WEB
    xj_lazy_end((double)lz_faults, (double)lz_fault_ns / 1e6, (double)lz_dropped);
#else
    free(lz_ext);
    lz_ext = NULL;
    lz_next = 0;
#endif
    free(snap_lazy_bits);
    snap_lazy_bits = NULL;
    snap_lazy_lo = snap_lazy_hi = 0;
}

static void lz_clear(uintptr_t a)
{
    uintptr_t i = (a - snap_lazy_lo) >> 12;
    snap_lazy_bits[i >> 3] &= (uint8_t) ~(1u << (i & 7));
}

static void lz_gone(uint64_t n)
{
    lz_pending = n > lz_pending ? 0 : lz_pending - n;
    if (!lz_pending && snap_lazy_bits)
        lz_end();
}

void snap_lazy_drop_slow(uintptr_t a)
{
    lz_clear(a);
    lz_dropped++;
    lz_gone(1);
}

#ifndef XIAOJI_WEB
/* The file offset of host page a's bytes, or 0 when no extent holds it (cannot happen for a marked page). */
static uint64_t lz_file_off(uintptr_t a)
{
    uint64_t lo = 0, hi = lz_next;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (lz_ext[mid * 3] + lz_ext[mid * 3 + 1] <= a)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo >= lz_next || lz_ext[lo * 3] > a)
        return 0;
    return lz_ext[lo * 3 + 2] + (a - lz_ext[lo * 3]);
}

/* Reads pending pages [a, a + n pages) that are still pending from the file. Returns how many it filled. */
static uint64_t lz_read(uintptr_t a, uint64_t n)
{
    int fd = open(lz_path, O_RDONLY); /* not kept open: a restore puts host descriptors back at fixed numbers */
    if (fd < 0) {
        fprintf(stderr, "xiaoji: restore: lazy pages: cannot open %s again\n", lz_path);
        exit(1);
    }
    uint64_t done = 0;
    for (uint64_t k = 0; k < n; k++) {
        uintptr_t p = a + k * MEM_PAGE_SIZE;
        if (p >= snap_lazy_hi || !snap_lazy_pending(p))
            continue;
        uint64_t off = lz_file_off(p);
        if (!off || pread(fd, (void *)p, MEM_PAGE_SIZE, (off_t)off) != (ssize_t)MEM_PAGE_SIZE) {
            fprintf(stderr, "xiaoji: restore: lazy pages: page 0x%llx could not be read from %s\n",
                    (unsigned long long)p, lz_path);
            exit(1);
        }
        lz_clear(p);
        done++;
    }
    close(fd);
    return done;
}
#endif

void snap_lazy_fill(const void *page)
{
    uintptr_t a = (uintptr_t)page & ~(uintptr_t)(MEM_PAGE_SIZE - 1);
    uint64_t t0 = host_monotonic_ns();
#ifdef XIAOJI_WEB
    uint64_t n = (uint64_t)xj_lazy_fault((double)a);
#else
    uint64_t n = lz_read(a, 1);
#endif
    if (!lz_faults++) /* once: the tests see that the restore did fault (a run may end without its exit handlers) */
        fprintf(stderr, "xiaoji: restore: lazy pages: first fault at 0x%llx\n", (unsigned long long)a);
    lz_fault_ns += host_monotonic_ns() - t0;
    if (snap_lazy_pending(a)) {
        fprintf(stderr, "xiaoji: restore: lazy pages: page 0x%llx did not arrive; the run cannot go on\n",
                (unsigned long long)a);
        exit(1);
    }
    lz_gone(n);
}

void snap_lazy_poll(void)
{
    if (!snap_lazy_bits)
        return;
    uint64_t now = host_monotonic_ns();
    if (now < lz_next_poll)
        return;
    lz_next_poll = now + 2000000ull;
#ifdef XIAOJI_WEB
    uint64_t n = (uint64_t)xj_lazy_poll();
#else
    if (lz_mode < 2)
        return;
    /* The background, emulated: the next 64 pending pages from a cursor, in address order. */
    uint64_t pages = (snap_lazy_hi - snap_lazy_lo) / MEM_PAGE_SIZE, n = 0, seen = 0;
    while (n < 64 && seen < pages) {
        uintptr_t p = snap_lazy_lo + (uintptr_t)(lz_cursor % pages) * MEM_PAGE_SIZE;
        if (snap_lazy_pending(p))
            n += lz_read(p, 1);
        lz_cursor++;
        seen++;
    }
#endif
    lz_polled += n;
    lz_gone(n);
}

void snap_lazy_finish(void)
{
    if (!snap_lazy_bits)
        return;
    uint64_t t0 = host_monotonic_ns();
#ifdef XIAOJI_WEB
    uint64_t n = (uint64_t)xj_lazy_finish();
#else
    uint64_t n = lz_read(snap_lazy_lo, (snap_lazy_hi - snap_lazy_lo) / MEM_PAGE_SIZE);
#endif
    lz_fault_ns += host_monotonic_ns() - t0;
    lz_polled += n;
    lz_gone(n);
    if (snap_lazy_bits) {
        fprintf(stderr, "xiaoji: restore: lazy pages: %llu still pending after the final fill\n",
                (unsigned long long)lz_pending);
        exit(1);
    }
}

#ifndef XIAOJI_WEB
static int cmp_reg(const void *a, const void *b)
{
    uint64_t x = ((const uint64_t *)a)[0], y = ((const uint64_t *)b)[0];
    return x < y ? -1 : x > y;
}

static int read_at(int fd, uint64_t off, void *p, uint64_t len)
{
    uint8_t *b = p;
    while (len) {
        ssize_t k = pread(fd, b, len, (off_t)off);
        if (k <= 0)
            return -1;
        b += k;
        off += (uint64_t)k;
        len -= (uint64_t)k;
    }
    return 0;
}
#endif

/* At the end of a run that still has pages pending: how far the fill got (the tests read this line). */
static void lz_exit_report(void)
{
    if (!snap_lazy_bits)
        return;
    fprintf(stderr, "xiaoji: restore: lazy pages: %llu of %llu in at exit: %llu faults (%.0f ms blocked), %llu in the "
            "background, %llu freed before they were read\n", (unsigned long long)(lz_total - lz_pending),
            (unsigned long long)lz_total, (unsigned long long)lz_faults, (double)lz_fault_ns / 1e6,
            (unsigned long long)lz_polled, (unsigned long long)lz_dropped);
}

void snap_lazy_begin(const char *path, uint64_t data_off, uint64_t ext_off, uint64_t nextents, uint64_t reg_off,
                     uint64_t nregions, int test_mode)
{
    snap_lazy_bits = NULL;
    snap_lazy_lo = snap_lazy_hi = 0;
    lz_pending = lz_total = lz_faults = lz_fault_ns = lz_dropped = lz_polled = lz_next_poll = 0;
#ifdef XIAOJI_WEB
    (void)path, (void)data_off, (void)ext_off, (void)nextents, (void)reg_off, (void)nregions, (void)test_mode;
    uint64_t lo = (uint64_t)xj_lazy_lo(), hi = (uint64_t)xj_lazy_hi();
    if (hi <= lo)
        return;
    uint8_t *bits = calloc((size_t)((hi - lo) / MEM_PAGE_SIZE / 8 + 1), 1);
    if (!bits) {
        fprintf(stderr, "xiaoji: restore: lazy pages: out of memory for the bitmap\n");
        exit(1);
    }
    snap_lazy_bits = bits;
    snap_lazy_lo = (uintptr_t)lo;
    snap_lazy_hi = (uintptr_t)hi;
    lz_total = (uint64_t)xj_lazy_mark(bits);
#else
    lz_mode = test_mode;
    if (lz_mode <= 0)
        return;
    if (!nregions) {
        fprintf(stderr, "xiaoji: restore: lazy pages: the snapshot has no region table; restored eagerly\n");
        return;
    }
    snprintf(lz_path, sizeof lz_path, "%s", path);
    int fd = open(path, O_RDONLY);
    uint64_t *reg = calloc(nregions, 24);
    lz_ext = calloc(nextents ? nextents : 1, 24);
    uint64_t *et = calloc(nextents ? nextents : 1, 16);
    if (fd < 0 || !reg || !lz_ext || !et || read_at(fd, reg_off, reg, nregions * 24) < 0 ||
        read_at(fd, ext_off, et, nextents * 16) < 0) {
        fprintf(stderr, "xiaoji: restore: lazy pages: cannot read the tables of %s\n", path);
        exit(1);
    }
    close(fd);
    uint64_t at = data_off;
    for (uint64_t k = 0; k < nextents; k++) {
        lz_ext[k * 3] = et[k * 2];
        lz_ext[k * 3 + 1] = et[k * 2 + 1];
        lz_ext[k * 3 + 2] = at;
        at += et[k * 2 + 1];
    }
    lz_next = nextents;
    free(et);
    /* The pool's slabs (kind 1), in address order. */
    uint64_t np = 0;
    for (uint64_t r = 0; r < nregions; r++)
        if (reg[r * 3 + 2] == 1) {
            memmove(&reg[np * 3], &reg[r * 3], 24);
            np++;
        }
    qsort(reg, np, 24, cmp_reg);
    if (!np) {
        free(reg);
        return;
    }
    uint64_t lo = reg[0], hi = reg[(np - 1) * 3] + reg[(np - 1) * 3 + 1];
    snap_lazy_bits = calloc((size_t)((hi - lo) / MEM_PAGE_SIZE / 8 + 1), 1);
    if (!snap_lazy_bits) {
        fprintf(stderr, "xiaoji: restore: lazy pages: out of memory for the bitmap\n");
        exit(1);
    }
    snap_lazy_lo = (uintptr_t)lo;
    snap_lazy_hi = (uintptr_t)hi;
    /* Every page of an extent that lies in a slab: poisoned and pending. */
    uint64_t r = 0;
    for (uint64_t k = 0; k < nextents; k++) {
        uint64_t ea = lz_ext[k * 3], ee = ea + lz_ext[k * 3 + 1];
        while (r < np && reg[r * 3] + reg[r * 3 + 1] <= ea)
            r++;
        for (uint64_t q = r; q < np && reg[q * 3] < ee; q++) {
            uint64_t a = ea > reg[q * 3] ? ea : reg[q * 3];
            uint64_t e = ee < reg[q * 3] + reg[q * 3 + 1] ? ee : reg[q * 3] + reg[q * 3 + 1];
            a = (a + MEM_PAGE_SIZE - 1) & ~(uint64_t)(MEM_PAGE_SIZE - 1);
            for (; a + MEM_PAGE_SIZE <= e; a += MEM_PAGE_SIZE) {
                if (lz_mode != 3) /* 3: the bytes are kept (a debugging aid: is a failure a missed fill?) */
                    memset((void *)(uintptr_t)a, 0xA5, MEM_PAGE_SIZE);
                uintptr_t i = ((uintptr_t)a - snap_lazy_lo) >> 12;
                snap_lazy_bits[i >> 3] |= (uint8_t)(1u << (i & 7));
                lz_total++;
            }
        }
    }
    free(reg);
    lz_cursor = 0;
#endif
    lz_pending = lz_total;
    lz_t0 = host_monotonic_ns();
    fprintf(stderr, "xiaoji: restore: lazy pages: %llu pending (%llu MiB)\n", (unsigned long long)lz_total,
            (unsigned long long)(lz_total * MEM_PAGE_SIZE >> 20));
    if (lz_total)
        snap_atexit(lz_exit_report);
    if (!lz_total) {
        free(snap_lazy_bits);
        snap_lazy_bits = NULL;
        snap_lazy_lo = snap_lazy_hi = 0;
#ifndef XIAOJI_WEB
        free(lz_ext);
        lz_ext = NULL;
#endif
    }
}
