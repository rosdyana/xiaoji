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
/* The top byte of a guest address is an ignored tag; Linux reports addresses without it. */
#define MEM_UNTAG_MASK 0x00FFFFFFFFFFFFFFull

/* A fault in a guest access longjmps to the buffer set here (NULL: no buffer, so
 * the process exits). Returns the previous buffer. */
/* Address spaces. mem_mm_switch installs m (flushing the TLBs and translation caches). */
typedef struct mm mm_t;
mm_t *mem_mm_new(void);
void mem_mm_switch(mm_t *m);

jmp_buf *mem_set_fault_jmp(jmp_buf *jb);
void mem_fault_info(uint64_t *addr, int *code);
uint64_t mem_cur_pc(void);

/* Mapping. Addresses and lengths must be page aligned. Pages that are already
 * mapped keep their contents and get the new protection. Return 0 or -1. */
int mem_map(uint64_t va, uint64_t len, unsigned prot);
/* As mem_map, and records the range as backed by host file path from offset
 * off (path NULL means anonymous). The path is copied. */
int mem_map_ex(uint64_t va, uint64_t len, unsigned prot, const char *path, uint64_t off);
/* Maps len bytes at va as a MAP_SHARED view of m from file offset off. The pages
 * are m's own pages, so every view of the same memfd sees the same bytes. Any
 * existing mapping in the range is replaced. Returns 0 or -1. */
int mem_map_shared(uint64_t va, uint64_t len, unsigned prot, memfd_t *m, uint64_t off);
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
/* Releases the host backing of mapped pages in the range; they read as zero
 * (or are refilled by the caller for file-backed pages). */
void mem_drop(uint64_t va, uint64_t len);
/* Protection bits (MEM_PROT_*) of the page at va, or 0 if it is unmapped. */
unsigned mem_prot_at(uint64_t va);
/* First region with end > va. Fills the region bounds, its file path (NULL if
 * anonymous; valid until the next mapping change) and its file offset at start.
 * Returns 0 if there is none. */
int mem_region_next(uint64_t va, uint64_t *start, uint64_t *end, const char **path, uint64_t *off);

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
