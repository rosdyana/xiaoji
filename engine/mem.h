/* Guest memory: a software page table over host pages, plus a sorted list of
 * mapped regions (occupancy and file backing). */
#ifndef XIAOJI_MEM_H
#define XIAOJI_MEM_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

#include "memfd.h"

#define MEM_PAGE_SIZE 4096u
#define MEM_PROT_R 1u
#define MEM_PROT_W 2u
#define MEM_PROT_X 4u
#define MEM_VA_LIMIT (1ull << 48)

/* Fault codes, the Linux si_code values for SIGSEGV. */
#define MEM_FAULT_MAPERR 1 /* SEGV_MAPERR: no mapping at the address */
#define MEM_FAULT_ACCERR 2 /* SEGV_ACCERR: mapped, but the access is not allowed */
#define MEM_FAULT_BUS 3    /* SIGBUS BUS_ADRERR: a file-backed page wholly past the end of its file */
/* The top byte of a guest address is an ignored tag; Linux reports addresses without it. */
#define MEM_UNTAG_MASK 0x00FFFFFFFFFFFFFFull

/* A fault in a guest access longjmps to the buffer set here (NULL: no buffer, so
 * the process exits). Returns the previous buffer. */
/* Address spaces. mem_mm_switch installs m (flushing the TLBs and translation caches). */
typedef struct mm mm_t;
mm_t *mem_mm_new(void);
void mem_mm_switch(mm_t *m);
/* Empties the TLBs and the fetch cache (a restore: they belong to the run that wrote the image). */
void mem_tlb_reset(void);
/* A new address space for fork: the private pages of m become copy-on-write in both. */
mm_t *mem_mm_fork(mm_t *m);
/* The resident set of m in KiB (getrusage's maxrss). */
uint64_t mem_mm_rss_kib(mm_t *m);
/* A process starts (or stops) using m. unref frees m with its last user. */
void mem_mm_ref(mm_t *m);
void mem_mm_unref(mm_t *m);

jmp_buf *mem_set_fault_jmp(jmp_buf *jb);
void mem_fault_info(uint64_t *addr, int *code);
uint64_t mem_cur_pc(void);

/* The host page pool (mem.c): every host page of guest memory and of memfds (4 KiB, aligned, not zeroed). */
uint8_t *mem_page_alloc(void);
void mem_page_free(uint8_t *p);
/* The pool's pages (in slabs) and how many of them are free. */
void mem_pool_counts(uint64_t *pages, uint64_t *free_pages);
/* Snapshot (snap.c): zeroes every free page, so the image carries none of their old bytes. */
void mem_pool_zero_free(void);
/* Snapshot: drops the pages of private file mappings that still hold the file's bytes (they are read again on their
 * next touch), and counts them (*clean) and the file pages that differ from the file (*dirty). */
void mem_snap_drop_clean(uint64_t *clean, uint64_t *dirty);
/* The census: the pool's pages that are not all zero. */
uint64_t mem_pool_nonzero(void);
/* 1 if p is in one of the pool's slabs (a guest or memfd page, used or free). */
int mem_pool_contains(const void *p);
size_t mem_pool_slabs(uint8_t *const **slabs, size_t *slab_bytes);

/* Mapping. Addresses and lengths must be page aligned. Pages that are already
 * mapped keep their contents and get the new protection. Return 0 or -1. */
int mem_map(uint64_t va, uint64_t len, unsigned prot);
/* As mem_map, and records path (copied; NULL means anonymous) and offset off for display
 * only: the pages are zero, not read from the file. File mappings use mem_map_lazy. */
int mem_map_ex(uint64_t va, uint64_t len, unsigned prot, const char *path, uint64_t off);
/* A host file that lazily backs mappings. Each lazy mapping holds a reference; the host
 * fd is closed when the last one goes. */
typedef struct mfile mfile_t;
/* The file with identity (dev, ino), opened once: the first caller's hfd is dup'ed and
 * later callers share that copy, since a process maps one file many times and the browser
 * has a small fd table. Returns a reference, or NULL. */
mfile_t *mem_file_get(int hfd, uint64_t dev, uint64_t ino);
void mem_file_unref(mfile_t *f);
/* Maps len bytes at va as a file-backed range: page i of the range reads the file at
 * off + (page address - va), on first access, in windows of mem_lazy_window bytes.
 * Bytes past the end of the file read as zero in the last page; a page wholly past the
 * end raises SIGBUS (BUS_ADRERR) on access, and the non-faulting copies return -1.
 * path is display-only (NULL is allowed). Any existing mapping in the range is replaced.
 * Returns 0 or -1. */
int mem_map_lazy(uint64_t va, uint64_t len, unsigned prot, const char *path, mfile_t *f, uint64_t off);
/* The lazy fill window in bytes (a power of two between one page and 1 MiB). */
void mem_set_lazy_window(size_t bytes);
/* 0: mem_map_lazy fills the whole range at once, as the eager mmap did (the --no-lazy-mmap baseline for
 * the A/B runs). The default is 1: pages are filled on first access. */
void mem_set_lazy_mmap(int on);
/* The file reads made by lazy fills so far (calls that returned data, and their bytes). */
void mem_lazy_counts(uint64_t *calls, uint64_t *bytes);
/* Maps len bytes at va as a MAP_SHARED view of m from file offset off. The pages
 * are m's own pages, so every view of the same memfd sees the same bytes. Any
 * existing mapping in the range is replaced. Returns 0 or -1. */
int mem_map_shared(uint64_t va, uint64_t len, unsigned prot, memfd_t *m, uint64_t off);
/* The file (mem_map_lazy) backing the region that contains va, or NULL. *start and *off are
 * the region start and its file offset at that start. The file is borrowed: take a
 * reference (mem_map_lazy does) to keep it. */
mfile_t *mem_region_file(uint64_t va, uint64_t *start, uint64_t *off);
/* Sets the size of a file cache memfd (filecache.c) in every address space: the pages of
 * its views wholly past size stop being mapped (an access raises SIGBUS), and pages that
 * come back into the size are mapped again. Called after the memfd's size changes. */
void mem_shm_resize(memfd_t *m, uint64_t size);
/* The memfd backing the region that contains va, or NULL. *start and *off are
 * the region start and its file offset at that start. */
memfd_t *mem_region_memfd(uint64_t va, uint64_t *start, uint64_t *off);
void mem_unmap(uint64_t va, uint64_t len);
/* Returns -1 (and changes nothing) if any page in the range is unmapped. */
int mem_protect(uint64_t va, uint64_t len, unsigned prot);
/* 1 if every page in the range is mapped, 0 otherwise. */
int mem_is_mapped(uint64_t va, uint64_t len);
/* 1 if no page in the range is mapped, 0 otherwise. */
int mem_range_free(uint64_t va, uint64_t len);
/* Returns a free, page-aligned range of len bytes, preferring hint if it is free.
 * Searches downward from 0x7000_0000_0000. Returns 0 if nothing fits. */
uint64_t mem_find_free(uint64_t len, uint64_t hint);
/* Moves mapped pages (host pages and region records) from one free range to
 * another without copying. The source range becomes unmapped. Returns 0 or -1. */
int mem_move(uint64_t from, uint64_t to, uint64_t len);
/* Releases the host backing of mapped pages in the range. They read as zero, or, in a
 * lazy file mapping (mem_map_lazy), are read from the file again on the next access. */
void mem_drop(uint64_t va, uint64_t len);
/* Protection bits (MEM_PROT_*) of the page at va, or 0 if it is unmapped. */
unsigned mem_prot_at(uint64_t va);
/* First region with end > va. Fills the region bounds, its file path (NULL if
 * anonymous; valid until the next mapping change) and its file offset at start.
 * Returns 0 if there is none. */
int mem_region_next(uint64_t va, uint64_t *start, uint64_t *end, const char **path, uint64_t *off);

/* The identity of the futex word at va: *obj is the object the word belongs to (the
 * address space for a private word, the memfd for a word in a shared mapping), and the
 * return value is the word's position in that object (its address, or the offset in
 * the memfd). Two words are the same futex when both match. */
uint64_t mem_futex_key(uint64_t va, uint64_t *obj);

/* Guest-side accesses. These never return on a fault: they print the segfault
 * message and exit with status 139. Little-endian, unaligned and page-crossing
 * accesses work. */
uint8_t mem_read8(uint64_t va);
uint16_t mem_read16(uint64_t va);
uint32_t mem_read32(uint64_t va);
uint64_t mem_read64(uint64_t va);
void mem_read128(uint64_t va, uint64_t out[2]); /* out[0] = low 64 bits */
void mem_write8(uint64_t va, uint8_t v);
void mem_write16(uint64_t va, uint16_t v);
void mem_write32(uint64_t va, uint32_t v);
void mem_write64(uint64_t va, uint64_t v);
void mem_write128(uint64_t va, const uint64_t in[2]);
/* Instruction fetch: requires execute permission. */
uint32_t mem_fetch32(uint64_t va);
/* The CPU records the current pc here so faults can report it. */
void mem_set_pc(uint64_t pc);
#ifdef __EMSCRIPTEN__
/* The wasm build's block translator (engine/jit.c) uses these; see mem.c. */
/* Host page of the page holding va, if it is mapped executable (NULL otherwise). */
uint8_t *mem_exec_page(uint64_t va);
/* Layout of the TLBs, for the translator's inline probe. */
void mem_jit_tlb(uintptr_t *rd, uintptr_t *wr, uint32_t *stride, uint32_t *page_off);
/* Empties the write TLB, so every later write goes through the slow path. */
void mem_flush_write_tlb(void);
#endif

/* Non-faulting accessors for syscalls and the loader. They return 0 on
 * success and -1 if a page is unmapped or lacks the permission. */
int mem_copy_in(uint64_t va, const void *src, size_t len);
int mem_copy_out(void *dst, uint64_t va, size_t len);
int mem_set(uint64_t va, uint8_t byte, size_t len);
/* Writes ignoring page protection (the pages must be mapped). The loader and
 * file-backed mappings use it before the final protection is applied. */
int mem_poke(uint64_t va, const void *src, size_t len);
/* Length of the NUL-terminated string at va, or -1 on a fault. */
int64_t mem_strlen(uint64_t va);

#endif
