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

/* Marks m as the cache of a host file (filecache.c). on_free runs when the last
 * reference goes, before the pages are freed, so the file can be written back. */
void memfd_set_file(memfd_t *m, void (*on_free)(memfd_t *m, void *ctx), void *ctx);
int memfd_is_file(const memfd_t *m);
int memfd_refs(const memfd_t *m);

/* A file cache's pages are filled on first use (filecache.c): fill runs when a page is materialised (memfd_page,
 * and so memfd_pread and memfd_pwrite on it), with the page zeroed; it writes the file's bytes into the page. The
 * ctx is the one memfd_set_file was given. Without a fill, a page that was never materialised reads as zeros. */
void memfd_set_fill(memfd_t *m, void (*fill)(memfd_t *m, uint64_t idx, uint8_t *page, void *ctx));
/* Whether page idx has been materialised (a host page exists for it). */
int memfd_materialized(const memfd_t *m, uint64_t idx);

/* Host page for file page idx, allocated and zeroed if needed. The page stays
 * valid until the memfd is freed. NULL if out of memory. */
uint8_t *memfd_page(memfd_t *m, uint64_t idx);

/* Byte-range access at file offset off. Reads clip to the size and return the
 * byte count. Writes extend the size as needed and return the byte count or -1. */
int64_t memfd_pread(memfd_t *m, void *buf, size_t n, uint64_t off);
int64_t memfd_pwrite(memfd_t *m, const void *buf, size_t n, uint64_t off);

#endif
