/* Guest memory: two-level software page table with a direct-mapped TLB.
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
 * Tables store the host pointer as a uint64 so the layout is the same on
 * 32-bit wasm and 64-bit hosts. Host pages are read and written with memcpy,
 * which assumes a little-endian host (arm64, x86-64 and wasm all qualify).
 */
#include "mem.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PTE_R 1u
#define PTE_W 2u
#define PTE_X 4u
#define PTE_MAPPED 8u
#define PTE_FLAGS 0xFu

#define VA_MASK 0x00FFFFFFFFFFFFFFull /* top byte is a tag */
#define VPN_LIMIT (1ull << 36)        /* 48-bit address space in 4 KiB pages */
#define L1_COUNT (1u << 18)
#define L2_COUNT (1u << 18)
#define TLB_SIZE 256u
#define MMAP_TOP 0x700000000000ull

typedef struct {
    uint64_t tag; /* vpn + 1; 0 means empty */
    uint8_t *page;
} tlb_ent;

static uint64_t **l1;
static tlb_ent tlb_rd[TLB_SIZE];
static tlb_ent tlb_wr[TLB_SIZE];
static uint64_t fetch_tag;
static uint8_t *fetch_page;
static uint64_t cur_pc;

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
}

static uint64_t *pte_slot(uint64_t vpn, int create)
{
    if (vpn >= VPN_LIMIT)
        return NULL;
    if (!l1) {
        if (!create)
            return NULL;
        l1 = calloc(L1_COUNT, sizeof *l1);
        if (!l1)
            die_oom();
    }
    uint64_t **l2 = &l1[vpn >> 18];
    if (!*l2) {
        if (!create)
            return NULL;
        *l2 = calloc(L2_COUNT, sizeof(uint64_t));
        if (!*l2)
            die_oom();
    }
    return &(*l2)[vpn & (L2_COUNT - 1)];
}

static void mem_segv(uint64_t va) __attribute__((noreturn));
static void mem_segv(uint64_t va)
{
    fflush(stdout);
    fprintf(stderr, "xiaoji: segfault at 0x%llx (pc 0x%llx)\n", (unsigned long long)va,
            (unsigned long long)cur_pc);
    exit(139);
}

/* Host page for vpn with the given permission, allocating backing if needed.
 * Returns NULL if the page is unmapped or lacks the permission. */
static uint8_t *page_lookup(uint64_t vpn, unsigned need)
{
    if (vpn >= VPN_LIMIT)
        return NULL;
    uint64_t *slot = pte_slot(vpn, 0);
    if (!slot)
        return NULL;
    uint64_t e = *slot;
    if (!(e & PTE_MAPPED) || !(e & need))
        return NULL;
    uint8_t *p = (uint8_t *)(uintptr_t)(e & ~(uint64_t)PTE_FLAGS);
    if (!p) {
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

int mem_map(uint64_t va, uint64_t len, unsigned prot)
{
    if (va & (MEM_PAGE_SIZE - 1) || len & (MEM_PAGE_SIZE - 1))
        return -1;
    if (va >= MEM_VA_LIMIT || len > MEM_VA_LIMIT - va)
        return -1;
    uint64_t first = va >> 12;
    uint64_t n = len >> 12;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t *slot = pte_slot(first + i, 1);
        uint64_t e = *slot;
        *slot = (e & ~(uint64_t)PTE_FLAGS) | PTE_MAPPED | (prot & PTE_FLAGS & 7u);
    }
    tlb_flush();
    return 0;
}

void mem_unmap(uint64_t va, uint64_t len)
{
    uint64_t first = va >> 12;
    uint64_t n = len >> 12;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t *slot = pte_slot(first + i, 0);
        if (!slot)
            continue;
        uint64_t e = *slot;
        uint8_t *p = (uint8_t *)(uintptr_t)(e & ~(uint64_t)PTE_FLAGS);
        free(p);
        *slot = 0;
    }
    tlb_flush();
}

int mem_is_mapped(uint64_t va, uint64_t len)
{
    uint64_t first = va >> 12;
    uint64_t n = len >> 12;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t *slot = pte_slot(first + i, 0);
        if (!slot || !(*slot & PTE_MAPPED))
            return 0;
    }
    return 1;
}

int mem_range_free(uint64_t va, uint64_t len)
{
    uint64_t first = va >> 12;
    uint64_t n = len >> 12;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t *slot = pte_slot(first + i, 0);
        if (slot && (*slot & PTE_MAPPED))
            return 0;
    }
    return 1;
}

int mem_protect(uint64_t va, uint64_t len, unsigned prot)
{
    if (!mem_is_mapped(va, len))
        return -1;
    uint64_t first = va >> 12;
    uint64_t n = len >> 12;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t *slot = pte_slot(first + i, 0);
        *slot = (*slot & ~(uint64_t)PTE_FLAGS) | PTE_MAPPED | (prot & 7u);
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
    uint64_t top = MMAP_TOP;
    for (;;) {
        if (top < len)
            return 0;
        uint64_t start = top - len;
        /* Find the highest mapped page inside [start, top). */
        uint64_t conflict = 0;
        for (uint64_t p = top - MEM_PAGE_SIZE;; p -= MEM_PAGE_SIZE) {
            if (!mem_range_free(p, MEM_PAGE_SIZE)) {
                conflict = p;
                break;
            }
            if (p == start)
                break;
        }
        if (!conflict)
            return start;
        top = conflict;
    }
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
