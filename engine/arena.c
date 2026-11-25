/* The native engine's heap (docs/snapshot.md). Every allocation of the engine (malloc and its family, which this file
 * defines for the executable) comes from one arena reserved at a fixed address, so that a snapshot of the heap, the
 * engine's data and bss sections, taken by a run with ASLR off, can be read back into another such run at the same
 * addresses (snap.c).
 *
 * The arena has two parts:
 * - the small-block heap [ARENA_BASE, arena_brk): dlmalloc's MORECORE (third_party/dlmalloc), grown upward;
 * - the large-block region [BIG_BASE, big_top): dlmalloc's MMAP, for blocks of DEFAULT_MMAP_THRESHOLD and more (the
 *   page tables, the page pool's slabs, big buffers). A freed block's pages are replaced by fresh zero pages, so
 *   they cost no memory and read as zero (calloc does not clear them, as with mmap).
 * dlmalloc's own state (its malloc_state and mparams) is in this file's bss, so it is in the snapshot with the arena.
 *
 * On macOS the executable's malloc, free and the rest bind to these definitions; the C library's own allocations
 * (opendir, fopen) stay in the system allocator and must not outlive a snapshot point (vfs.c holds no DIR across
 * calls). On Linux these definitions replace the C library's malloc for the whole process. The wasm builds use
 * Emscripten's malloc in linear memory, and this file compiles to nothing there. */
#ifndef __EMSCRIPTEN__

#if defined(__linux__)
#define _GNU_SOURCE
#endif

#include "arena.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

#define ARENA_BASE 0x200000000000ull
#define ARENA_SIZE (96ull << 30) /* the small-block heap's reservation */
#define BIG_BASE (ARENA_BASE + ARENA_SIZE)
#define BIG_SIZE (160ull << 30) /* the large-block region's reservation */
#define ARENA_PAGE 16384ull     /* the largest host page size (Apple silicon) */

static int arena_state;     /* 0: not set up; 1: at the fixed address; 2: elsewhere (no snapshots) */
static uintptr_t heap_base; /* the small-block heap's start and break */
static uintptr_t arena_brk;
static uintptr_t big_base, big_top; /* the large-block region and its high-water mark */

/* The free runs of the large-block region, sorted by address, merged with their neighbours. */
#define NRUNS 32768
typedef struct {
    uintptr_t a, n;
} run_t;
static run_t runs[NRUNS];
static int nruns;

static void arena_fail(const char *what)
{
    static const char pre[] = "xiaoji: arena: ";
    (void)!write(2, pre, sizeof pre - 1);
    (void)!write(2, what, strlen(what));
    (void)!write(2, "\n", 1);
    _exit(70);
}

static void *reserve(uintptr_t want, size_t len)
{
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
#if defined(MAP_FIXED_NOREPLACE)
    void *p = mmap((void *)want, len, PROT_READ | PROT_WRITE, flags | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != MAP_FAILED)
        return p;
#endif
    /* A hint: the kernel places the mapping there when the range is free (macOS, and Linux without NOREPLACE). */
    void *q = mmap((void *)want, len, PROT_READ | PROT_WRITE, flags, -1, 0);
    return q == MAP_FAILED ? NULL : q;
}

static void arena_setup(void)
{
    if (arena_state)
        return;
    void *h = reserve(ARENA_BASE, ARENA_SIZE);
    void *b = h ? reserve(BIG_BASE, BIG_SIZE) : NULL;
    if (!h || !b)
        arena_fail("cannot reserve the heap");
    arena_state = ((uintptr_t)h == ARENA_BASE && (uintptr_t)b == BIG_BASE) ? 1 : 2;
    heap_base = arena_brk = (uintptr_t)h;
    big_base = big_top = (uintptr_t)b;
}

/* dlmalloc's MORECORE: grows the small-block heap (it never shrinks it: MORECORE_CANNOT_TRIM). */
static void *xj_morecore(intptr_t inc)
{
    arena_setup();
    uintptr_t old = arena_brk;
    if (inc < 0)
        return (void *)~(uintptr_t)0;
    if ((uintptr_t)inc > heap_base + ARENA_SIZE - arena_brk) {
        errno = ENOMEM;
        return (void *)~(uintptr_t)0;
    }
    arena_brk += (uintptr_t)inc;
    return (void *)old;
}

static uintptr_t page_up(uintptr_t n) { return (n + ARENA_PAGE - 1) & ~(uintptr_t)(ARENA_PAGE - 1); }

/* Fresh zero pages over [a, a + n): the old pages go back to the host. */
static int fresh_pages(uintptr_t a, uintptr_t n)
{
    void *p = mmap((void *)a, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    return p == MAP_FAILED ? -1 : 0;
}

/* dlmalloc's MMAP: a block of the large-block region (first fit, else above the high-water mark). */
static void *xj_bigmap(size_t len)
{
    arena_setup();
    uintptr_t n = page_up(len);
    for (int i = 0; i < nruns; i++) {
        if (runs[i].n < n)
            continue;
        uintptr_t a = runs[i].a;
        runs[i].a += n;
        runs[i].n -= n;
        if (!runs[i].n) {
            memmove(&runs[i], &runs[i + 1], (size_t)(nruns - i - 1) * sizeof runs[0]);
            nruns--;
        }
        return (void *)a;
    }
    if (n > big_base + BIG_SIZE - big_top)
        return (void *)~(uintptr_t)0;
    uintptr_t a = big_top;
    big_top += n;
    return (void *)a;
}

/* dlmalloc's MUNMAP: the block's pages become fresh zero pages, and the block a free run. */
static int xj_bigunmap(void *ptr, size_t len)
{
    uintptr_t a = (uintptr_t)ptr, n = page_up(len);
    if (fresh_pages(a, n) < 0)
        return -1;
    if (a + n == big_top) {
        big_top = a;
        /* A free run that now ends at the mark goes back to the mark too. */
        if (nruns && runs[nruns - 1].a + runs[nruns - 1].n == big_top) {
            big_top = runs[nruns - 1].a;
            nruns--;
        }
        return 0;
    }
    int i = 0;
    while (i < nruns && runs[i].a < a)
        i++;
    int merge_prev = i > 0 && runs[i - 1].a + runs[i - 1].n == a;
    int merge_next = i < nruns && a + n == runs[i].a;
    if (merge_prev && merge_next) {
        runs[i - 1].n += n + runs[i].n;
        memmove(&runs[i], &runs[i + 1], (size_t)(nruns - i - 1) * sizeof runs[0]);
        nruns--;
    } else if (merge_prev) {
        runs[i - 1].n += n;
    } else if (merge_next) {
        runs[i].a = a;
        runs[i].n += n;
    } else {
        if (nruns == NRUNS)
            return 0; /* the table is full: the block's pages stay zero and unused (leaked address space) */
        memmove(&runs[i + 1], &runs[i], (size_t)(nruns - i) * sizeof runs[0]);
        runs[i].a = a;
        runs[i].n = n;
        nruns++;
    }
    return 0;
}

int arena_ranges(arena_range_t *out, int max)
{
    arena_setup();
    if (arena_state != 1)
        return -1;
    int n = 0;
    if (n < max)
        out[n++] = (arena_range_t){heap_base, arena_brk};
    if (n < max)
        out[n++] = (arena_range_t){big_base, big_top};
    return n;
}

int arena_zero(uintptr_t lo, uintptr_t hi)
{
    /* No arena_setup here: a restore calls this while the image is being read, and arena_state may be the image's
     * zeroed bytes for a moment. The fixed reservation exists whenever an image is read (snap.c checks it first). */
    if (lo < ARENA_BASE || hi > BIG_BASE + BIG_SIZE || lo > hi)
        return -1;
    lo &= ~(uintptr_t)(ARENA_PAGE - 1);
    hi = page_up(hi);
    return hi > lo ? fresh_pages(lo, hi - lo) : 0;
}

/* ---------- dlmalloc ---------- */

#define USE_DL_PREFIX 1
#define USE_LOCKS 0
#define HAVE_MORECORE 1
#define MORECORE xj_morecore
#define MORECORE_CONTIGUOUS 1
#define MORECORE_CANNOT_TRIM 1
#define HAVE_MMAP 1
#define HAVE_MREMAP 0
#define MMAP(s) xj_bigmap(s)
#define DIRECT_MMAP(s) xj_bigmap(s)
#define MUNMAP(a, s) xj_bigunmap((a), (s))
#define MMAP_CLEARS 1
#define DEFAULT_GRANULARITY (1024 * 1024)
#define DEFAULT_MMAP_THRESHOLD (256 * 1024)
#define MALLOC_ALIGNMENT 16
#define NO_MALLOC_STATS 1
#define LACKS_SCHED_H 1
#define MALLOC_INSPECT_ALL 1

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wnull-pointer-arithmetic"
#pragma clang diagnostic ignored "-Wexpansion-to-defined"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wexpansion-to-defined"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
/* malloc.c defines these three for its own declarations; glibc's headers have them already. */
#undef __THROW
#undef __attribute_malloc__
#undef __wur
#include "../third_party/dlmalloc/malloc.c"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* ---------- snapshots ---------- */

static void zero_free_chunk(void *start, void *end, size_t used, void *arg)
{
    if (used == 0 && end > start) {
        memset(start, 0, (size_t)((char *)end - (char *)start));
        *(uint64_t *)arg += (uint64_t)((char *)end - (char *)start);
    }
}

uint64_t arena_zero_free(void)
{
    uint64_t n = 0;
    dlmalloc_inspect_all(zero_free_chunk, &n);
    return n;
}

void arena_heap_stats(uint64_t *used, uint64_t *avail)
{
    struct mallinfo mi = dlmallinfo();
    *used = (uint64_t)mi.uordblks + (uint64_t)mi.hblkhd;
    *avail = (uint64_t)mi.fordblks;
}

/* ---------- the malloc family of the executable ---------- */

void *malloc(size_t n) { return dlmalloc(n); }
void free(void *p) { dlfree(p); }
void *calloc(size_t n, size_t sz) { return dlcalloc(n, sz); }
void *realloc(void *p, size_t n) { return dlrealloc(p, n); }
void *aligned_alloc(size_t align, size_t n) { return dlmemalign(align, n); }
int posix_memalign(void **out, size_t align, size_t n) { return dlposix_memalign(out, align, n); }
void *valloc(size_t n) { return dlvalloc(n); }
char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = dlmalloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}
char *strndup(const char *s, size_t max)
{
    size_t n = strnlen(s, max);
    char *d = dlmalloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = 0;
    }
    return d;
}
#if defined(__linux__)
/* The rest of glibc's replaceable set (its own code calls them by these names). */
void *memalign(size_t align, size_t n) { return dlmemalign(align, n); }
void *pvalloc(size_t n) { return dlpvalloc(n); }
size_t malloc_usable_size(void *p) { return dlmalloc_usable_size(p); }
#endif

#else
/* The wasm builds: Emscripten's malloc. Its free chunks keep their old bytes (no walk of them is compiled in). */
#include "arena.h"
#include <malloc.h>

int arena_ranges(arena_range_t *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}

int arena_zero(uintptr_t lo, uintptr_t hi)
{
    (void)lo;
    (void)hi;
    return -1;
}

uint64_t arena_zero_free(void) { return 0; }

void arena_heap_stats(uint64_t *used, uint64_t *avail)
{
    struct mallinfo mi = mallinfo();
    *used = (uint64_t)(uint32_t)mi.uordblks;
    *avail = (uint64_t)(uint32_t)mi.fordblks;
}
#endif
