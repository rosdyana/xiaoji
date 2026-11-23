#ifndef XIAOJI_FILECACHE_H
#define XIAOJI_FILECACHE_H

#include <stddef.h>
#include <stdint.h>

#include "host.h"
#include "memfd.h"

/* The shared page cache of host files (SQLite's -shm, any MAP_SHARED of a host file).
 *
 * A host file (dev, ino) with a MAP_SHARED mapping has one cache: a memfd of the file's size whose
 * pages are filled from the host file on first use (a view's fault, a read or a write). Every MAP_SHARED
 * view of the file, in every guest process, is a view of that memfd, so the views are coherent with each other. read, pread, write and pwrite
 * on the file go through the cache too, so they see the views' stores. The cache writes
 * through to the host file on write() and pwrite(); a store through a view reaches the
 * host file only at write-back, which runs on msync, fsync and fdatasync of the file,
 * on the exit of a guest process or of the engine, and when the last view is unmapped
 * (the cache's memfd is freed).
 *
 * The cache holds no reference on its memfd: its lifetime is the lifetime of the views.
 * A write-back writes only the bytes that differ from the host file, and never past its
 * end. ftruncate (and an O_TRUNC open) resize the cache and the views (mem_shm_resize).
 *
 * Not coherent: a read that goes to the host file directly (exec, the loader's whole-file
 * reads, sendfile), which sees the host bytes until the next write-back. */

/* Maps len bytes at va as a MAP_SHARED view of the file at offset off. st and hfd are the
 * file's host identity and an open fd (hfd is read from). can_write says whether hfd is
 * open for writing: write-back then uses a dup of it. Returns 0 or -1 (no memory, or a
 * failed read or dup). Any existing mapping in the range is replaced, as mem_map_shared. */
int fc_map_shared(uint64_t va, uint64_t len, unsigned prot, uint64_t off, const host_stat *st, int hfd,
                  int can_write);

/* The cache of the file (dev, ino), or NULL if no view of it is live. Borrowed. */
memfd_t *fc_lookup(uint64_t dev, uint64_t ino);

/* write() and pwrite() of n bytes at off landed in the host file: patch the cache. */
void fc_patch(uint64_t dev, uint64_t ino, const void *buf, size_t n, uint64_t off);

/* The file's size is now len (after a host truncation): resize the cache and its views. */
void fc_truncate(uint64_t dev, uint64_t ino, uint64_t len);

/* Writes back the cache of the file (dev, ino), if any (fsync, fdatasync). */
void fc_flush_file(uint64_t dev, uint64_t ino);

/* Writes back the cache of the file mapped at va, if any (msync). */
void fc_sync_va(uint64_t va);

/* Writes back every cache (process and engine exit). Registered with atexit on first use. */
void fc_flush_all(void);

#endif
