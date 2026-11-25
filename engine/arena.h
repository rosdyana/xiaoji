/* The native engine's heap: a fixed-address arena (arena.c, docs/snapshot.md). The wasm builds have no arena: their
 * heap is in linear memory, whose addresses are the same in every run already. */
#ifndef XIAOJI_ARENA_H
#define XIAOJI_ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uintptr_t lo, hi;
} arena_range_t;

/* Native builds: the arena's ranges in use (the small-block heap and the large-block region), at most max. Returns
 * how many, or -1 when the arena could not be placed at its fixed address (a snapshot is then impossible). The wasm
 * builds return 0. */
int arena_ranges(arena_range_t *out, int max);
/* Makes [lo, hi) of the arena fresh zero pages (restore, before the image is read). 0, or -1. */
int arena_zero(uintptr_t lo, uintptr_t hi);

/* Snapshot: zeroes the bytes of every free heap chunk, so the image carries none of their old contents (native only;
 * the wasm builds return 0). Returns the bytes zeroed. */
uint64_t arena_zero_free(void);
/* The heap's bytes in use and free (the census). */
void arena_heap_stats(uint64_t *used, uint64_t *avail);

#endif
