/* Guest memory: two-level software page table with a direct-mapped TLB, and a
 * sorted list of mapped regions.
 *
 * A guest address is first masked to 56 bits (the top byte is an ignored tag).
 * Bits 48..55 must then be zero, otherwise the access is a segfault.
 *
 * L1 is indexed by va >> 30 (2^18 entries). Each entry points to a lazily
 * allocated L2 table indexed by (va >> 12) & 0x3FFFF. An L2 entry holds the
 * host page pointer (4 KiB aligned, so the low 12 bits are free) ORed with the
 * flags R=1, W=2, X=4, MAPPED=8. A MAPPED page with a NULL pointer has no
 * backing yet; it gets a zeroed host page on first permitted access.
 *
 * The region list is the source of truth for occupancy (mem_find_free is a walk
 * over regions, not pages) and records file backing per range. Regions are
 * disjoint and sorted by start. Adjacent anonymous regions are merged.
 *
 * Tables store the host pointer as a uint64 so the layout is the same on
 * 32-bit wasm and 64-bit hosts. Host pages are read and written with memcpy,
 * which assumes a little-endian host (arm64, x86-64 and wasm all qualify).
 */
#include "mem.h"
#include "host.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include "jit.h"
#endif

/* A host page is about to change (written through any mapping, freed, or moved):
 * drop the translated code on it. Pages without code are not affected. */
static inline void host_page_changed(uint8_t *p)
{
#ifdef __EMSCRIPTEN__
    if (p)
        jit_page_changed(p);
#else
    (void)p;
#endif
}

#define PTE_R 1u
#define PTE_W 2u
#define PTE_X 4u
#define PTE_MAPPED 8u
#define PTE_SHARED 0x10u /* the host page belongs to a memfd; this entry does not own it */
#define PTE_COW 0x20u    /* a private host page shared with another address space (fork): the
                          * first write copies it. The entry keeps PTE_W, the logical permission. */
#define PTE_FLAGS 0xFFu  /* host pages are 4 KiB aligned, so the low 12 bits hold flags */

#define VA_MASK 0x00FFFFFFFFFFFFFFull /* top byte is a tag */
#define VPN_LIMIT (1ull << 36)        /* 48-bit address space in 4 KiB pages */
#define L1_COUNT (1u << 18)
#define L2_COUNT (1u << 18)
#define TLB_SIZE 256u
#define MMAP_TOP 0x700000000000ull
#define MMAP_LOW 0x10000ull

typedef struct {
    uint64_t tag; /* vpn + 1; 0 means empty */
    uint8_t *page;
} tlb_ent;

typedef struct {
    uint64_t start, end; /* [start, end), page aligned */
    char *path;          /* host file backing, or NULL for anonymous memory */
    uint64_t off;        /* file offset of start */
    memfd_t *shm;        /* memfd backing (MAP_SHARED), or NULL; path is then display-only */
    mfile_t *lazy;       /* file backing filled on first access (mem_map_lazy), or NULL */
} region_t;

/* A host file behind lazy mappings. The fd is owned here; the files are on one list. */
struct mfile {
    int hfd;
    int refs;
    uint64_t dev, ino;
    mfile_t *next;
};
static mfile_t *files;

/* Bytes filled per lazy fault (a power of two, a whole aligned window). */
static size_t lazy_window = 65536;
/* 0 fills a file mapping at mmap time (mem_set_lazy_mmap). */
static int lazy_map_on = 1;
/* Counts of the file reads a lazy fill made (host_pread calls that returned data) and their bytes. */
static uint64_t lazy_fill_calls, lazy_fill_bytes;
/* Set when a faulting access hit a lazy page past the end of its file: the fault is then
 * SIGBUS, not SIGSEGV. Holds vpn + 1 of that page; 0 when none. */
static uint64_t bus_vpn1;
static uint8_t *lazy_buf;
static size_t lazy_buf_cap;

/* A region with no file or memfd behind it. Only these merge with neighbours. */
static int reg_anon(const region_t *r) { return !r->path && !r->shm && !r->lazy; }

/* One address space: its page table and its region list. The TLBs and the fetch
 * cache are not part of it: they stay at fixed addresses (the translator bakes them
 * in) and are flushed on every switch. */
struct mm {
    uint64_t **l1;
    region_t *regs;
    size_t nregs, cregs;
    int refs; /* processes using this address space (vfork shares one) */
};

static mm_t *cur_mm;
static tlb_ent tlb_rd[TLB_SIZE];
static tlb_ent tlb_wr[TLB_SIZE];
static uint64_t fetch_tag;
static uint8_t *fetch_page;
static uint64_t cur_pc;

static void tlb_flush(void);

mm_t *mem_mm_new(void)
{
    mm_t *m = calloc(1, sizeof(mm_t));
    if (m)
        m->refs = 1;
    return m;
}

/* Makes m the running address space. The first call installs it without a flush. */
void mem_mm_switch(mm_t *m)
{
    if (cur_mm == m)
        return;
    int first = cur_mm == NULL;
    cur_mm = m;
    if (!first)
        tlb_flush();
}

static void die_oom(void)
{
    fputs("xiaoji: out of host memory\n", stderr);
    exit(1);
}

static void tlb_flush(void)
{
    memset(tlb_rd, 0, sizeof tlb_rd);
    memset(tlb_wr, 0, sizeof tlb_wr);
    fetch_tag = 0;
    fetch_page = NULL;
#ifdef __EMSCRIPTEN__
    /* Every change of the mapping calls this (shared pages do not call
     * jit_page_changed when unmapped), so the translator's chain cache goes too. */
    jit_chain_flush();
#endif
}

static uint64_t *pte_slot(uint64_t vpn, int create)
{
    if (vpn >= VPN_LIMIT)
        return NULL;
    if (!cur_mm->l1) {
        if (!create)
            return NULL;
        cur_mm->l1 = calloc(L1_COUNT, sizeof *cur_mm->l1);
        if (!cur_mm->l1)
            die_oom();
    }
    uint64_t **l2 = &cur_mm->l1[vpn >> 18];
    if (!*l2) {
        if (!create)
            return NULL;
        *l2 = calloc(L2_COUNT, sizeof(uint64_t));
        if (!*l2)
            die_oom();
    }
    return &(*l2)[vpn & (L2_COUNT - 1)];
}

static jmp_buf *fault_jb;
static uint64_t fault_va;
static int fault_code;

jmp_buf *mem_set_fault_jmp(jmp_buf *jb)
{
    jmp_buf *old = fault_jb;
    fault_jb = jb;
    return old;
}

void mem_fault_info(uint64_t *addr, int *code)
{
    *addr = fault_va;
    *code = fault_code;
}

uint64_t mem_cur_pc(void) { return cur_pc; }

uint64_t mem_futex_key(uint64_t va, uint64_t *obj)
{
    uint64_t a = va & VA_MASK, start, off;
    memfd_t *m = mem_region_memfd(a, &start, &off);
    if (m) {
        *obj = (uint64_t)(uintptr_t)m;
        return off + (a - start);
    }
    *obj = (uint64_t)(uintptr_t)cur_mm;
    return a;
}

/* A guest access failed. The CPU loop gets the fault (and the faulting pc) by
 * longjmp, so it can deliver SIGSEGV; without a loop, the process exits. */
static void mem_segv(uint64_t va) __attribute__((noreturn));
static void mem_segv(uint64_t va)
{
    int bus = bus_vpn1 && bus_vpn1 == (((va & VA_MASK) >> 12) + 1);
    if (bus)
        bus_vpn1 = 0;
    if (!fault_jb) {
        fflush(stdout);
        if (bus) {
            fprintf(stderr, "xiaoji: bus error at 0x%llx (pc 0x%llx)\n", (unsigned long long)va,
                    (unsigned long long)cur_pc);
            exit(135);
        }
        fprintf(stderr, "xiaoji: segfault at 0x%llx (pc 0x%llx)\n", (unsigned long long)va,
                (unsigned long long)cur_pc);
        exit(139);
    }
    fault_va = va;
    fault_code = bus ? MEM_FAULT_BUS : mem_is_mapped(va & VA_MASK, 1) ? MEM_FAULT_ACCERR : MEM_FAULT_MAPERR;
    longjmp(*fault_jb, 1);
}

/* ---------- region list ---------- */

/* Index of the first region with end > va. */
static size_t reg_lower(uint64_t va)
{
    size_t lo = 0, hi = cur_mm->nregs;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cur_mm->regs[mid].end <= va)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static char *dup_path(const char *p)
{
    if (!p)
        return NULL;
    size_t n = strlen(p) + 1;
    char *d = malloc(n);
    if (!d)
        die_oom();
    memcpy(d, p, n);
    return d;
}

/* A new reference to a region's backing (and a copy of its path). */
static region_t reg_copy(const region_t *r)
{
    region_t c = *r;
    c.path = dup_path(r->path);
    memfd_ref(c.shm);
    if (c.lazy)
        c.lazy->refs++;
    return c;
}

/* Drops a region's references (its path, memfd and file). */
static void reg_release(region_t *r)
{
    free(r->path);
    memfd_unref(r->shm);
    mem_file_unref(r->lazy);
}

/* Replaces regions[i..j) with n entries from src (n <= 2 is typical, but any n works). */
static void reg_splice(size_t i, size_t j, const region_t *src, size_t n)
{
    size_t removed = j - i;
    size_t want = cur_mm->nregs - removed + n;
    if (want > cur_mm->cregs) {
        size_t nc = cur_mm->cregs ? cur_mm->cregs : 64;
        while (nc < want)
            nc *= 2;
        region_t *nr = realloc(cur_mm->regs, nc * sizeof *cur_mm->regs);
        if (!nr)
            die_oom();
        cur_mm->regs = nr;
        cur_mm->cregs = nc;
    }
    memmove(&cur_mm->regs[i + n], &cur_mm->regs[j], (cur_mm->nregs - j) * sizeof *cur_mm->regs);
    for (size_t k = 0; k < n; k++)
        cur_mm->regs[i + k] = src[k];
    cur_mm->nregs = want;
}

/* Removes [a, b) from the region list, keeping the parts outside it. */
static void reg_clear(uint64_t a, uint64_t b)
{
    size_t i = reg_lower(a), j = i;
    while (j < cur_mm->nregs && cur_mm->regs[j].start < b)
        j++;
    if (i == j)
        return;
    region_t keep[2];
    size_t nk = 0;
    region_t first = cur_mm->regs[i], last = cur_mm->regs[j - 1];
    if (first.start < a) {
        keep[nk] = reg_copy(&first);
        keep[nk].start = first.start;
        keep[nk].end = a;
        nk++;
    }
    if (last.end > b) {
        keep[nk] = reg_copy(&last);
        keep[nk].start = b;
        keep[nk].end = last.end;
        keep[nk].off = last.off + (b - last.start);
        nk++;
    }
    for (size_t k = i; k < j; k++)
        reg_release(&cur_mm->regs[k]);
    reg_splice(i, j, keep, nk);
}

/* Inserts [a, b) with the given backing, replacing whatever was there. shm
 * (may be NULL) takes a reference for the new region. */
static void reg_set(uint64_t a, uint64_t b, const char *path, uint64_t off, memfd_t *shm, mfile_t *lazy)
{
    reg_clear(a, b);
    size_t i = reg_lower(a);
    if (!path && !shm && !lazy) {
        /* Merge with anonymous neighbours so long-lived ranges stay one entry. */
        if (i > 0 && cur_mm->regs[i - 1].end == a && reg_anon(&cur_mm->regs[i - 1])) {
            cur_mm->regs[i - 1].end = b;
            if (i < cur_mm->nregs && cur_mm->regs[i].start == b && reg_anon(&cur_mm->regs[i])) {
                cur_mm->regs[i - 1].end = cur_mm->regs[i].end;
                free(cur_mm->regs[i].path);
                reg_splice(i, i + 1, NULL, 0);
            }
            return;
        }
        if (i < cur_mm->nregs && cur_mm->regs[i].start == b && reg_anon(&cur_mm->regs[i])) {
            cur_mm->regs[i].start = a;
            return;
        }
    }
    region_t r = {a, b, dup_path(path), off, shm, lazy};
    memfd_ref(shm);
    if (lazy)
        lazy->refs++;
    reg_splice(i, i, &r, 1);
}

memfd_t *mem_region_memfd(uint64_t va, uint64_t *start, uint64_t *off)
{
    size_t i = reg_lower(va);
    if (i >= cur_mm->nregs || cur_mm->regs[i].start > va || !cur_mm->regs[i].shm)
        return NULL;
    *start = cur_mm->regs[i].start;
    *off = cur_mm->regs[i].off;
    return cur_mm->regs[i].shm;
}

mfile_t *mem_region_file(uint64_t va, uint64_t *start, uint64_t *off)
{
    size_t i = reg_lower(va);
    if (i >= cur_mm->nregs || cur_mm->regs[i].start > va || !cur_mm->regs[i].lazy)
        return NULL;
    *start = cur_mm->regs[i].start;
    *off = cur_mm->regs[i].off;
    return cur_mm->regs[i].lazy;
}

unsigned mem_prot_at(uint64_t va)
{
    uint64_t *slot = pte_slot((va & VA_MASK) >> 12, 0);
    if (!slot || !(*slot & PTE_MAPPED))
        return 0;
    return (unsigned)(*slot & 7u);
}

int mem_region_next(uint64_t va, uint64_t *start, uint64_t *end, const char **path, uint64_t *off)
{
    size_t i = reg_lower(va);
    if (i >= cur_mm->nregs)
        return 0;
    *start = cur_mm->regs[i].start;
    *end = cur_mm->regs[i].end;
    *path = cur_mm->regs[i].path;
    *off = cur_mm->regs[i].off;
    return 1;
}

/* ---------- copy-on-write ---------- */

/* Host pages with more than one owner. A page owned once is not in the table; an entry
 * holds the number of owners beyond the first. Open addressing with tombstones. */
typedef struct {
    uint8_t *page; /* NULL: empty; SHARE_TOMB: deleted */
    uint32_t extra;
} share_ent;
#define SHARE_TOMB ((uint8_t *)1)
static share_ent *share_tab;
static size_t share_cap, share_used; /* share_used counts live entries and tombstones */

static size_t share_slot(const uint8_t *p, int *found)
{
    uint64_t h = (uint64_t)(uintptr_t)p >> 12;
    h *= 0x9E3779B97F4A7C15ull;
    size_t i = (size_t)(h >> 20) & (share_cap - 1), tomb = (size_t)-1;
    *found = 0;
    for (;;) {
        share_ent *e = &share_tab[i];
        if (e->page == p) {
            *found = 1;
            return i;
        }
        if (e->page == SHARE_TOMB && tomb == (size_t)-1)
            tomb = i;
        if (!e->page)
            return tomb != (size_t)-1 ? tomb : i;
        i = (i + 1) & (share_cap - 1);
    }
}

static void share_grow(void)
{
    size_t oldcap = share_cap;
    share_ent *old = share_tab;
    share_cap = share_cap ? share_cap * 2 : 256;
    share_tab = calloc(share_cap, sizeof *share_tab);
    if (!share_tab)
        die_oom();
    share_used = 0;
    for (size_t i = 0; i < oldcap; i++) {
        if (old[i].page && old[i].page != SHARE_TOMB) {
            int found;
            size_t j = share_slot(old[i].page, &found);
            share_tab[j] = old[i];
            share_used++;
        }
    }
    free(old);
}

static uint32_t share_extra(const uint8_t *p)
{
    if (!share_cap || !p)
        return 0;
    int found;
    size_t i = share_slot(p, &found);
    return found ? share_tab[i].extra : 0;
}

/* One more owner of p. */
static void share_add(uint8_t *p)
{
    if ((share_used + 1) * 4 >= share_cap * 3)
        share_grow();
    int found;
    size_t i = share_slot(p, &found);
    if (found) {
        share_tab[i].extra++;
        return;
    }
    if (share_tab[i].page == NULL)
        share_used++;
    share_tab[i].page = p;
    share_tab[i].extra = 1;
}

/* One owner of p goes away. */
static void share_drop(uint8_t *p)
{
    int found;
    size_t i = share_slot(p, &found);
    if (!found)
        return;
    if (--share_tab[i].extra == 0)
        share_tab[i].page = SHARE_TOMB;
}

/* This address space gives up its owner of a private host page: the last owner frees it. */
static void page_release(uint8_t *p)
{
    if (!p)
        return;
    if (share_extra(p)) {
        share_drop(p);
        return;
    }
    host_page_changed(p);
    free(p);
}

/* Drops the cached translations of vpn in the TLBs and the fetch cache. */
static void tlb_forget(uint64_t vpn)
{
    tlb_ent *r = &tlb_rd[vpn % TLB_SIZE];
    if (r->tag == vpn + 1) {
        r->tag = 0;
        r->page = NULL;
    }
    tlb_ent *w = &tlb_wr[vpn % TLB_SIZE];
    if (w->tag == vpn + 1) {
        w->tag = 0;
        w->page = NULL;
    }
    if (fetch_tag == vpn + 1) {
        fetch_tag = 0;
        fetch_page = NULL;
    }
}

/* A write to a copy-on-write page. If other address spaces still share the host page,
 * this one takes a copy; the last owner keeps the page. The translations of the old page
 * are dropped first (the other owners see only a retranslation). */
static void cow_break(uint64_t vpn, uint64_t *slot)
{
    uint64_t e = *slot;
    uint8_t *old = (uint8_t *)(uintptr_t)(e & ~(uint64_t)PTE_FLAGS);
    uint8_t *p = old;
    host_page_changed(old);
    if (share_extra(old)) {
        p = aligned_alloc(MEM_PAGE_SIZE, MEM_PAGE_SIZE);
        if (!p)
            die_oom();
        memcpy(p, old, MEM_PAGE_SIZE);
        share_drop(old);
    }
    *slot = (uint64_t)(uintptr_t)p | (e & (uint64_t)PTE_FLAGS & ~(uint64_t)PTE_COW);
    tlb_forget(vpn);
}

/* ---------- page access ---------- */

/* Fills the lazy pages of the window that holds vpn, from the region's file. Only pages with
 * no host page yet are filled (a page that was written or holds code keeps its bytes). A page
 * past the end of the file reads as zero up to the end and is left unfilled when it is wholly
 * past it. *is_lazy is 0 if vpn is not in a file-backed region. Returns the host page of vpn,
 * or NULL if it is past the end of the file (or the read failed): that access is SIGBUS. */
static uint8_t *lazy_fill(uint64_t vpn, int *is_lazy)
{
    uint64_t va = vpn << 12;
    *is_lazy = 0;
    bus_vpn1 = 0;
    size_t i = reg_lower(va);
    if (i >= cur_mm->nregs || cur_mm->regs[i].start > va || !cur_mm->regs[i].lazy)
        return NULL;
    *is_lazy = 1;
    region_t *r = &cur_mm->regs[i];
    int hfd = r->lazy->hfd;
    uint64_t rstart = r->start, roff = r->off;
    /* The window is aligned in the file, not in the guest: a window then never crosses a
     * boundary of the file's chunks, and two mappings of one file fill the same windows. */
    uint64_t back = (roff + (va - rstart)) & ((uint64_t)lazy_window - 1);
    uint64_t wa = (va - rstart > back) ? va - back : rstart;
    uint64_t we = va + ((uint64_t)lazy_window - back);
    if (we > r->end)
        we = r->end;
    size_t want_len = (size_t)(we - wa);
    /* One reusable buffer: a fill is not reentrant (host_pread does not call back), and a
     * malloc and free per fault costs more than the copy. */
    if (want_len > lazy_buf_cap) {
        free(lazy_buf);
        lazy_buf = malloc(want_len);
        if (!lazy_buf) {
            lazy_buf_cap = 0;
            die_oom();
        }
        lazy_buf_cap = want_len;
    }
    uint8_t *buf = lazy_buf;
    size_t got = 0;
    while (got < want_len) {
        int64_t k = host_pread(hfd, buf + got, want_len - got, roff + (wa - rstart) + got);
        if (k <= 0)
            break; /* end of file, or an error: the bytes read so far are the file */
        got += (size_t)k;
    }
    lazy_fill_calls++;
    lazy_fill_bytes += got;
    uint8_t *result = NULL;
    for (uint64_t pa = wa; pa < we; pa += MEM_PAGE_SIZE) {
        uint64_t pv = pa >> 12;
        uint64_t *ps = pte_slot(pv, 0);
        if (!ps || !(*ps & PTE_MAPPED) || (*ps & PTE_SHARED) || (*ps & ~(uint64_t)PTE_FLAGS))
            continue;
        size_t at = (size_t)(pa - wa);
        size_t have = at < got ? got - at : 0;
        if (have == 0) {
            if (pv == vpn)
                bus_vpn1 = vpn + 1;
            continue;
        }
        uint8_t *pg = aligned_alloc(MEM_PAGE_SIZE, MEM_PAGE_SIZE);
        if (!pg)
            die_oom();
        size_t cp = have < MEM_PAGE_SIZE ? have : MEM_PAGE_SIZE;
        memcpy(pg, buf + at, cp);
        memset(pg + cp, 0, MEM_PAGE_SIZE - cp);
        host_page_changed(pg); /* a new page holds no translated code; kept for the rule */
        *ps = (uint64_t)(uintptr_t)pg | (*ps & (uint64_t)PTE_FLAGS);
        if (pv == vpn)
            result = pg;
    }
    if (!result)
        bus_vpn1 = vpn + 1;
    return result;
}

/* Host page for vpn with the given permission, allocating backing if needed.
 * Returns NULL if the page is unmapped or lacks the permission. */
static uint8_t *page_lookup(uint64_t vpn, unsigned need)
{
    bus_vpn1 = 0; /* a SIGBUS is only for the access that filled the lazy page */
    if (vpn >= VPN_LIMIT)
        return NULL;
    uint64_t *slot = pte_slot(vpn, 0);
    if (!slot)
        return NULL;
    uint64_t e = *slot;
    if (!(e & PTE_MAPPED) || !(e & need))
        return NULL;
    if ((need & PTE_W) && (e & PTE_COW)) {
        cow_break(vpn, slot);
        e = *slot;
    }
    uint8_t *p = (uint8_t *)(uintptr_t)(e & ~(uint64_t)PTE_FLAGS);
    if (!p && (e & PTE_SHARED)) {
        /* First touch of a shared view: take the page from the memfd at this offset. */
        uint64_t start, off;
        memfd_t *m = mem_region_memfd(vpn << 12, &start, &off);
        p = m ? memfd_page(m, ((vpn << 12) - start + off) >> 12) : NULL;
        if (!p)
            die_oom();
        *slot = (uint64_t)(uintptr_t)p | (e & PTE_FLAGS);
        return p;
    }
    if (!p) {
        int is_lazy;
        p = lazy_fill(vpn, &is_lazy);
        if (is_lazy)
            return p; /* a file-backed page: filled, or NULL past the end of the file */
        p = aligned_alloc(MEM_PAGE_SIZE, MEM_PAGE_SIZE);
        if (!p)
            die_oom();
        memset(p, 0, MEM_PAGE_SIZE);
        *slot = (uint64_t)(uintptr_t)p | (e & PTE_FLAGS);
    }
    return p;
}

static inline uint8_t *tlb_page(uint64_t vpn, unsigned need)
{
    tlb_ent *t = (need == PTE_W) ? &tlb_wr[vpn % TLB_SIZE] : &tlb_rd[vpn % TLB_SIZE];
    if (t->tag == vpn + 1)
        return t->page;
    uint8_t *p = page_lookup(vpn, need);
    if (p) {
#ifdef __EMSCRIPTEN__
        if (need == PTE_W && jit_page_is_code(p)) {
            /* The write changes translated code: drop it. The page is no longer code,
             * so the next write may be cached; it is not cached for this one. */
            jit_page_changed(p);
            return p;
        }
#endif
        t->tag = vpn + 1;
        t->page = p;
    }
    return p;
}

static uint64_t read_bytes(uint64_t va, unsigned size)
{
    uint64_t a = va & VA_MASK;
    uint64_t off = a & (MEM_PAGE_SIZE - 1);
    uint64_t v = 0;
    if (off + size <= MEM_PAGE_SIZE) {
        uint8_t *p = tlb_page(a >> 12, PTE_R);
        if (!p)
            mem_segv(va);
        memcpy(&v, p + off, size);
        return v;
    }
    for (unsigned i = 0; i < size; i++)
        v |= (uint64_t)read_bytes(va + i, 1) << (8 * i);
    return v;
}

static void write_bytes(uint64_t va, uint64_t v, unsigned size)
{
    uint64_t a = va & VA_MASK;
    uint64_t off = a & (MEM_PAGE_SIZE - 1);
    if (off + size <= MEM_PAGE_SIZE) {
        uint8_t *p = tlb_page(a >> 12, PTE_W);
        if (!p)
            mem_segv(va);
        memcpy(p + off, &v, size);
        return;
    }
    for (unsigned i = 0; i < size; i++)
        write_bytes(va + i, v >> (8 * i), 1);
}

uint8_t mem_read8(uint64_t va) { return (uint8_t)read_bytes(va, 1); }
uint16_t mem_read16(uint64_t va) { return (uint16_t)read_bytes(va, 2); }
uint32_t mem_read32(uint64_t va) { return (uint32_t)read_bytes(va, 4); }
uint64_t mem_read64(uint64_t va) { return read_bytes(va, 8); }

void mem_read128(uint64_t va, uint64_t out[2])
{
    out[0] = read_bytes(va, 8);
    out[1] = read_bytes(va + 8, 8);
}

void mem_write8(uint64_t va, uint8_t v) { write_bytes(va, v, 1); }
void mem_write16(uint64_t va, uint16_t v) { write_bytes(va, v, 2); }
void mem_write32(uint64_t va, uint32_t v) { write_bytes(va, v, 4); }
void mem_write64(uint64_t va, uint64_t v) { write_bytes(va, v, 8); }

void mem_write128(uint64_t va, const uint64_t in[2])
{
    write_bytes(va, in[0], 8);
    write_bytes(va + 8, in[1], 8);
}

void mem_set_pc(uint64_t pc) { cur_pc = pc; }

static uint8_t exec_byte(uint64_t va)
{
    uint64_t a = va & VA_MASK;
    uint8_t *p = page_lookup(a >> 12, PTE_X);
    if (!p)
        mem_segv(va);
    return p[a & (MEM_PAGE_SIZE - 1)];
}

uint32_t mem_fetch32(uint64_t va)
{
    uint64_t a = va & VA_MASK;
    uint64_t off = a & (MEM_PAGE_SIZE - 1);
    if (off + 4 <= MEM_PAGE_SIZE) {
        uint64_t vpn = a >> 12;
        if (fetch_tag != vpn + 1) {
            uint8_t *p = page_lookup(vpn, PTE_X);
            if (!p)
                mem_segv(va);
            fetch_tag = vpn + 1;
            fetch_page = p;
        }
        uint32_t v;
        memcpy(&v, fetch_page + off, 4);
        return v;
    }
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; i++)
        v |= (uint32_t)exec_byte(va + i) << (8 * i);
    return v;
}

#ifdef __EMSCRIPTEN__
/* Host page of the instruction at va, with execute permission, or NULL. For the
 * translator, which keys blocks by host address. */
uint8_t *mem_exec_page(uint64_t va)
{
    uint64_t vpn = (va & VA_MASK) >> 12;
    return page_lookup(vpn, PTE_X);
}

/* Layout of the TLBs for the translator's inline probe: the read and write arrays,
 * the entry size, and the offset of the host page pointer in an entry (the tag is at 0). */
void mem_jit_tlb(uintptr_t *rd, uintptr_t *wr, uint32_t *stride, uint32_t *page_off)
{
    *rd = (uintptr_t)tlb_rd;
    *wr = (uintptr_t)tlb_wr;
    *stride = (uint32_t)sizeof(tlb_ent);
    *page_off = (uint32_t)offsetof(tlb_ent, page);
}

/* The translator's inline stores must miss, so it drops the write TLB whenever it
 * starts treating a host page as code. */
void mem_flush_write_tlb(void) { memset(tlb_wr, 0, sizeof tlb_wr); }
#endif /* __EMSCRIPTEN__ */

/* ---------- mapping ---------- */

static int range_ok(uint64_t va, uint64_t len)
{
    if (va & (MEM_PAGE_SIZE - 1) || len & (MEM_PAGE_SIZE - 1))
        return 0;
    return va < MEM_VA_LIMIT && len <= MEM_VA_LIMIT - va;
}

int mem_map_ex(uint64_t va, uint64_t len, unsigned prot, const char *path, uint64_t off)
{
    if (!range_ok(va, len))
        return -1;
    uint64_t first = va >> 12;
    uint64_t n = len >> 12;
    reg_set(va, va + len, path, off, NULL, NULL);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t *slot = pte_slot(first + i, 1);
        uint64_t e = *slot;
        if (e & PTE_SHARED)
            e = 0; /* a private mapping replaces a shared view; it does not keep its pages */
        *slot = (e & ~(uint64_t)PTE_FLAGS) | PTE_MAPPED | (prot & PTE_FLAGS & 7u) | (e & PTE_COW);
    }
    tlb_flush();
    return 0;
}

int mem_map(uint64_t va, uint64_t len, unsigned prot) { return mem_map_ex(va, len, prot, NULL, 0); }

mfile_t *mem_file_get(int hfd, uint64_t dev, uint64_t ino)
{
    for (mfile_t *f = files; f; f = f->next) {
        if (f->dev == dev && f->ino == ino) {
            f->refs++;
            return f;
        }
    }
    int dfd = host_dup(hfd);
    if (dfd < 0)
        return NULL;
    mfile_t *f = calloc(1, sizeof *f);
    if (!f) {
        host_close(dfd);
        return NULL;
    }
    f->hfd = dfd;
    f->refs = 1;
    f->dev = dev;
    f->ino = ino;
    f->next = files;
    files = f;
    return f;
}

void mem_file_unref(mfile_t *f)
{
    if (!f || --f->refs > 0)
        return;
    for (mfile_t **p = &files; *p; p = &(*p)->next) {
        if (*p == f) {
            *p = f->next;
            break;
        }
    }
    host_close(f->hfd);
    free(f);
}

void mem_set_lazy_window(size_t bytes)
{
    if (bytes >= MEM_PAGE_SIZE && bytes <= (1u << 20) && !(bytes & (bytes - 1)))
        lazy_window = bytes;
}

void mem_set_lazy_mmap(int on)
{
    lazy_map_on = on ? 1 : 0;
}

void mem_lazy_counts(uint64_t *calls, uint64_t *bytes)
{
    *calls = lazy_fill_calls;
    *bytes = lazy_fill_bytes;
}

int mem_map_lazy(uint64_t va, uint64_t len, unsigned prot, const char *path, mfile_t *f, uint64_t off)
{
    if (!range_ok(va, len) || !f)
        return -1;
    if (!mem_range_free(va, len))
        mem_unmap(va, len); /* a new file mapping starts with no pages of its own */
    reg_set(va, va + len, path, off, NULL, f);
    uint64_t first = va >> 12;
    uint64_t n = len >> 12;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t *slot = pte_slot(first + i, 1);
        *slot = PTE_MAPPED | (prot & 7u); /* no host page: lazy_fill supplies it on first access */
    }
    tlb_flush();
    if (!lazy_map_on) {
        /* The eager baseline: every page is filled now, window by window, as the old mmap did. */
        for (uint64_t pv = first; pv < first + n; pv++) {
            uint64_t *ps = pte_slot(pv, 0);
            if (!ps || (*ps & ~(uint64_t)PTE_FLAGS))
                continue; /* already has a host page (filled with its window) */
            int is_lazy;
            lazy_fill(pv, &is_lazy);
        }
        bus_vpn1 = 0;
    }
    return 0;
}

int mem_map_shared(uint64_t va, uint64_t len, unsigned prot, memfd_t *m, uint64_t off)
{
    if (!range_ok(va, len) || !m)
        return -1;
    mem_unmap(va, len);
    char path[96];
    snprintf(path, sizeof path, "/memfd:%s (deleted)", memfd_name(m));
    reg_set(va, va + len, path, off, m, NULL);
    uint64_t first = va >> 12;
    uint64_t n = len >> 12;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t *slot = pte_slot(first + i, 1);
        /* No host page yet: page_lookup takes it from the memfd on first touch. */
        *slot = PTE_SHARED | PTE_MAPPED | (prot & 7u);
    }
    tlb_flush();
    return 0;
}

void mem_unmap(uint64_t va, uint64_t len)
{
    if (!len)
        return;
    uint64_t first = va >> 12;
    uint64_t n = len >> 12;
    reg_clear(va, va + len);
    for (uint64_t i = 0; i < n; i++) {
        uint64_t *slot = pte_slot(first + i, 0);
        if (!slot)
            continue;
        uint64_t e = *slot;
        if (!(e & PTE_SHARED)) /* shared pages stay with the memfd */
            page_release((uint8_t *)(uintptr_t)(e & ~(uint64_t)PTE_FLAGS));
        *slot = 0;
    }
    tlb_flush();
}

int mem_is_mapped(uint64_t va, uint64_t len)
{
    uint64_t pos = va, end = va + len;
    while (pos < end) {
        size_t i = reg_lower(pos);
        if (i >= cur_mm->nregs || cur_mm->regs[i].start > pos)
            return 0;
        pos = cur_mm->regs[i].end;
    }
    return 1;
}

int mem_range_free(uint64_t va, uint64_t len)
{
    size_t i = reg_lower(va);
    return !(i < cur_mm->nregs && cur_mm->regs[i].start < va + len);
}

int mem_protect(uint64_t va, uint64_t len, unsigned prot)
{
    if (!mem_is_mapped(va, len))
        return -1;
    uint64_t first = va >> 12;
    uint64_t n = len >> 12;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t *slot = pte_slot(first + i, 0);
        host_page_changed((uint8_t *)(uintptr_t)(*slot & ~(uint64_t)PTE_FLAGS));
        *slot = (*slot & ~(uint64_t)PTE_FLAGS) | (*slot & (PTE_SHARED | PTE_COW)) | PTE_MAPPED | (prot & 7u);
    }
    tlb_flush();
    return 0;
}

uint64_t mem_find_free(uint64_t len, uint64_t hint)
{
    if (len == 0 || (len & (MEM_PAGE_SIZE - 1)) || len > MMAP_TOP)
        return 0;
    if (hint && !(hint & (MEM_PAGE_SIZE - 1)) && hint < MEM_VA_LIMIT && len <= MEM_VA_LIMIT - hint &&
        mem_range_free(hint, len))
        return hint;
    /* Walk the gaps between regions, from the top down. */
    uint64_t top = MMAP_TOP;
    for (size_t i = cur_mm->nregs; i > 0; i--) {
        region_t *r = &cur_mm->regs[i - 1];
        if (r->start >= top)
            continue;
        if (r->end < top && top - r->end >= len)
            return top - len;
        top = r->start;
    }
    if (top >= MMAP_LOW + len)
        return top - len;
    return 0;
}

int mem_move(uint64_t from, uint64_t to, uint64_t len)
{
    if (!range_ok(from, len) || !range_ok(to, len) || !mem_range_free(to, len))
        return -1;
    /* Region pieces inside the source range, clipped to it. */
    size_t i = reg_lower(from), j = i;
    while (j < cur_mm->nregs && cur_mm->regs[j].start < from + len)
        j++;
    size_t np = j - i;
    region_t *pieces = NULL;
    if (np) {
        pieces = calloc(np, sizeof *pieces);
        if (!pieces)
            die_oom();
        for (size_t k = 0; k < np; k++) {
            region_t *r = &cur_mm->regs[i + k];
            uint64_t s = r->start < from ? from : r->start;
            uint64_t e = r->end > from + len ? from + len : r->end;
            pieces[k] = reg_copy(r);
            pieces[k].start = s - from + to;
            pieces[k].end = e - from + to;
            pieces[k].off = r->off + (s - r->start);
        }
    }
    uint64_t first_src = from >> 12, first_dst = to >> 12, n = len >> 12;
    for (uint64_t p = 0; p < n; p++) {
        uint64_t *src = pte_slot(first_src + p, 0);
        if (!src || !(*src & PTE_MAPPED))
            continue;
        uint64_t *dst = pte_slot(first_dst + p, 1);
        host_page_changed((uint8_t *)(uintptr_t)(*src & ~(uint64_t)PTE_FLAGS));
        *dst = *src;
        *src = 0;
    }
    reg_clear(from, from + len);
    for (size_t k = 0; k < np; k++) {
        reg_set(pieces[k].start, pieces[k].end, pieces[k].path, pieces[k].off, pieces[k].shm, pieces[k].lazy);
        reg_release(&pieces[k]);
    }
    free(pieces);
    tlb_flush();
    return 0;
}

void mem_drop(uint64_t va, uint64_t len)
{
    uint64_t first = va >> 12, n = len >> 12;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t *slot = pte_slot(first + i, 0);
        /* Shared pages belong to the memfd: dropping a view keeps its contents. */
        if (!slot || !(*slot & PTE_MAPPED) || (*slot & PTE_SHARED))
            continue;
        uint64_t e = *slot;
        page_release((uint8_t *)(uintptr_t)(e & ~(uint64_t)PTE_FLAGS));
        *slot = e & PTE_FLAGS & ~(uint64_t)PTE_COW;
    }
    tlb_flush();
}

/* ---------- non-faulting copies ---------- */

/* Host page for vpn, allocating backing, ignoring protection. NULL if unmapped. */
static uint8_t *page_any(uint64_t vpn)
{
    uint64_t *slot = pte_slot(vpn, 0);
    if (!slot || !(*slot & PTE_MAPPED))
        return NULL;
    if (*slot & PTE_COW)
        cow_break(vpn, slot); /* a poke writes, whatever the protection */
    return page_lookup(vpn, PTE_MAPPED);
}

int mem_poke(uint64_t va, const void *src, size_t len)
{
    const uint8_t *s = src;
    while (len) {
        uint64_t a = va & VA_MASK;
        size_t off = (size_t)(a & (MEM_PAGE_SIZE - 1));
        size_t chunk = MEM_PAGE_SIZE - off;
        if (chunk > len)
            chunk = len;
        uint8_t *p = page_any(a >> 12);
        if (!p)
            return -1;
        host_page_changed(p);
        memcpy(p + off, s, chunk);
        s += chunk;
        va += chunk;
        len -= chunk;
    }
    return 0;
}

int mem_copy_in(uint64_t va, const void *src, size_t len)
{
    const uint8_t *s = src;
    while (len) {
        uint64_t a = va & VA_MASK;
        size_t off = (size_t)(a & (MEM_PAGE_SIZE - 1));
        size_t chunk = MEM_PAGE_SIZE - off;
        if (chunk > len)
            chunk = len;
        uint8_t *p = tlb_page(a >> 12, PTE_W);
        if (!p)
            return -1;
        memcpy(p + off, s, chunk);
        s += chunk;
        va += chunk;
        len -= chunk;
    }
    return 0;
}

int mem_copy_out(void *dst, uint64_t va, size_t len)
{
    uint8_t *d = dst;
    while (len) {
        uint64_t a = va & VA_MASK;
        size_t off = (size_t)(a & (MEM_PAGE_SIZE - 1));
        size_t chunk = MEM_PAGE_SIZE - off;
        if (chunk > len)
            chunk = len;
        uint8_t *p = tlb_page(a >> 12, PTE_R);
        if (!p)
            return -1;
        memcpy(d, p + off, chunk);
        d += chunk;
        va += chunk;
        len -= chunk;
    }
    return 0;
}

int mem_set(uint64_t va, uint8_t byte, size_t len)
{
    while (len) {
        uint64_t a = va & VA_MASK;
        size_t off = (size_t)(a & (MEM_PAGE_SIZE - 1));
        size_t chunk = MEM_PAGE_SIZE - off;
        if (chunk > len)
            chunk = len;
        uint8_t *p = tlb_page(a >> 12, PTE_W);
        if (!p)
            return -1;
        memset(p + off, byte, chunk);
        va += chunk;
        len -= chunk;
    }
    return 0;
}

int64_t mem_strlen(uint64_t va)
{
    int64_t n = 0;
    for (;;) {
        uint64_t a = (va + (uint64_t)n) & VA_MASK;
        size_t off = (size_t)(a & (MEM_PAGE_SIZE - 1));
        uint8_t *p = tlb_page(a >> 12, PTE_R);
        if (!p)
            return -1;
        for (; off < MEM_PAGE_SIZE; off++, n++)
            if (p[off] == 0)
                return n;
    }
}

/* ---------- address spaces: fork, sharing and release ---------- */

void mem_mm_ref(mm_t *m) { m->refs++; }

/* Clones src (the running address space) for fork. Every private page with a host page
 * becomes copy-on-write in both spaces, and its share count goes up; a write copies it.
 * Shared (memfd) views stay shared. Pages never touched stay lazily zero in both. The
 * regions are copied with their backing; the TLBs are flushed, since the write
 * entries of src are no longer valid. Returns NULL if out of memory. */
mm_t *mem_mm_fork(mm_t *src)
{
    mm_t *dst = mem_mm_new();
    if (!dst)
        return NULL;
    if (src->nregs) {
        dst->regs = malloc(src->nregs * sizeof *dst->regs);
        if (!dst->regs) {
            free(dst);
            return NULL;
        }
        for (size_t i = 0; i < src->nregs; i++) {
            dst->regs[i] = reg_copy(&src->regs[i]);
        }
        dst->nregs = dst->cregs = src->nregs;
    }
    if (src->l1) {
        dst->l1 = calloc(L1_COUNT, sizeof *dst->l1);
        if (!dst->l1)
            die_oom();
        for (uint32_t i = 0; i < L1_COUNT; i++) {
            uint64_t *s2 = src->l1[i];
            if (!s2)
                continue;
            uint64_t *d2 = calloc(L2_COUNT, sizeof(uint64_t));
            if (!d2)
                die_oom();
            dst->l1[i] = d2;
            for (uint32_t j = 0; j < L2_COUNT; j++) {
                uint64_t e = s2[j];
                if (!(e & PTE_MAPPED))
                    continue;
                if (!(e & PTE_SHARED) && (e & ~(uint64_t)PTE_FLAGS)) {
                    share_add((uint8_t *)(uintptr_t)(e & ~(uint64_t)PTE_FLAGS));
                    e |= PTE_COW;
                    s2[j] = e;
                }
                d2[j] = e;
            }
        }
    }
    tlb_flush();
    return dst;
}

/* Resident set of m in KiB: the pages with a host page (shared views included). */
uint64_t mem_mm_rss_kib(mm_t *m)
{
    uint64_t n = 0;
    if (!m || !m->l1)
        return 0;
    for (uint32_t i = 0; i < L1_COUNT; i++) {
        uint64_t *l2 = m->l1[i];
        if (!l2)
            continue;
        for (uint32_t j = 0; j < L2_COUNT; j++) {
            uint64_t e = l2[j];
            if ((e & PTE_MAPPED) && (e & ~(uint64_t)PTE_FLAGS))
                n++;
        }
    }
    return n * (MEM_PAGE_SIZE / 1024);
}

/* Frees an address space when its last process lets go of it. If it is the running
 * one, the running pointer is cleared and the TLBs flushed (the next switch does not
 * flush). */
void mem_mm_unref(mm_t *m)
{
    if (!m || --m->refs > 0)
        return;
    if (m->l1) {
        for (uint32_t i = 0; i < L1_COUNT; i++) {
            uint64_t *l2 = m->l1[i];
            if (!l2)
                continue;
            for (uint32_t j = 0; j < L2_COUNT; j++) {
                uint64_t e = l2[j];
                if ((e & PTE_MAPPED) && !(e & PTE_SHARED))
                    page_release((uint8_t *)(uintptr_t)(e & ~(uint64_t)PTE_FLAGS));
            }
            free(l2);
        }
        free(m->l1);
    }
    for (size_t i = 0; i < m->nregs; i++)
        reg_release(&m->regs[i]);
    free(m->regs);
    if (cur_mm == m) {
        cur_mm = NULL;
        tlb_flush();
    }
    free(m);
}
