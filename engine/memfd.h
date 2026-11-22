/* memfd: an anonymous in-engine file. Its pages are host pages that guest
 * mappings can share (MAP_SHARED): several page-table entries point at the same
 * host page, and writes through one view are visible through the others.
 *
 * Pages are materialized on first use. A memfd is refcounted: one reference per
 * open guest fd and one per mapped region that names it. The pages are freed
 * when the last reference goes, so a memfd outlives close() while mappings of it
 * are still live. */
#ifndef XIAOJI_MEMFD_H
#define XIAOJI_MEMFD_H

#include <stddef.h>
#include <stdint.h>

typedef struct memfd memfd_t;

/* Returns a new memfd with one reference, size 0, or NULL if out of memory. */
memfd_t *memfd_new(const char *name);
void memfd_ref(memfd_t *m);
void memfd_unref(memfd_t *m);
const char *memfd_name(const memfd_t *m);
uint64_t memfd_size(const memfd_t *m);

/* Sets the size. Shrinking zeroes the discarded bytes. Returns 0 or -1 (out of memory). */
int memfd_truncate(memfd_t *m, uint64_t size);

/* Host page for file page idx, allocated and zeroed if needed. The page stays
 * valid until the memfd is freed. NULL if out of memory. */
uint8_t *memfd_page(memfd_t *m, uint64_t idx);

/* Byte-range access at file offset off. Reads clip to the size and return the
 * byte count. Writes extend the size as needed and return the byte count or -1. */
int64_t memfd_pread(memfd_t *m, void *buf, size_t n, uint64_t off);
int64_t memfd_pwrite(memfd_t *m, const void *buf, size_t n, uint64_t off);

#endif
