#ifndef XIAOJI_SNAPLAZY_H
#define XIAOJI_SNAPLAZY_H

/* Lazy snapshot pages (docs/snapshot.md, S5). After a restore, host pages of the page pool (guest and memfd pages) can
 * be "pending": in the snapshot, not in memory yet. A bit per host page says so. Every place that reads or writes the
 * bytes of a pool page calls snap_lazy_need first (mem.c's page_lookup, which fills the TLBs, and memfd.c); a page that
 * is freed while pending is dropped (its bytes are dead). Without pending pages snap_lazy_bits is NULL and the check is
 * one load and a branch, on the TLB-miss path only. */
#include <stdint.h>

extern uint8_t *snap_lazy_bits; /* one bit per host page in [snap_lazy_lo, snap_lazy_hi); NULL: nothing pending */
extern uintptr_t snap_lazy_lo, snap_lazy_hi;

void snap_lazy_fill(const void *page); /* brings the pending page in (blocking); the run ends if it cannot */
void snap_lazy_drop_slow(uintptr_t a);
void snap_lazy_poll(void);   /* the scheduler loop: pages that arrived in the background are copied in */
void snap_lazy_finish(void); /* every pending page in (before a snapshot is written) */
/* A restore: the pages the snapshot left pending (the page on the web; natively and in Node, test_mode is
 * XIAOJI_SNAP_LAZY, read before the image replaced the environment). */
void snap_lazy_begin(const char *path, uint64_t data_off, uint64_t ext_off, uint64_t nextents, uint64_t reg_off,
                     uint64_t nregions, int test_mode);

static inline int snap_lazy_pending(uintptr_t a)
{
    if (a < snap_lazy_lo || a >= snap_lazy_hi)
        return 0;
    uintptr_t i = (a - snap_lazy_lo) >> 12;
    return (snap_lazy_bits[i >> 3] >> (i & 7)) & 1;
}

/* Before the bytes of host page p are read or written. */
static inline void snap_lazy_need(const void *p)
{
    if (__builtin_expect(snap_lazy_bits != 0, 0) && snap_lazy_pending((uintptr_t)p))
        snap_lazy_fill(p);
}

/* Host page p goes back to the pool: if it is pending, its bytes are never needed. */
static inline void snap_lazy_drop(const void *p)
{
    if (__builtin_expect(snap_lazy_bits != 0, 0) && snap_lazy_pending((uintptr_t)p))
        snap_lazy_drop_slow((uintptr_t)p);
}

#endif
