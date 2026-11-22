/* The Android binder driver (B3). Three synthetic devices (/dev/binder, /dev/hwbinder,
 * /dev/vndbinder), each a separate context with its own context manager and handle
 * space. Every open of a device is one binder proc; its descriptions are the owners of
 * the proc (the last close releases it). See docs/boot.md, B3. */
#ifndef XIAOJI_BINDER_H
#define XIAOJI_BINDER_H

#include <stdint.h>

#include "memfd.h"

typedef struct bproc binder_proc_t;

/* The device index for a guest path (0 binder, 1 hwbinder, 2 vndbinder), or -1. */
int binder_device(const char *path);

/* A new proc on device which, for the running process. NULL if out of memory. */
binder_proc_t *binder_open(int which);

/* Releases a proc (its description's last reference). Frees its nodes and refs, and
 * sends death notifications. */
void binder_release(binder_proc_t *b);

/* ioctl on a binder fd. nonblock is O_NONBLOCK. Returns a value or -LE_*. A call that
 * must wait returns with the thread blocked (sched_block_restart); the call then runs
 * again from its start (the write half is already consumed in guest memory). */
int64_t binder_ioctl(binder_proc_t *b, uint32_t cmd, uint64_t arg, int nonblock);

/* mmap of a binder fd: the receive buffer. len is the mapping length (at most 4 MiB,
 * clamped). Only one mapping per proc (EBUSY). PROT_WRITE is EPERM. A read-only view is
 * always the shared one: libbinder maps MAP_PRIVATE|PROT_READ, and a private read-only
 * view never copies, so it sees the driver's writes as a shared one does. On success *out
 * is a memfd with one reference for the caller's description. The caller maps it, then
 * calls binder_mmap_done. */
int binder_mmap_prepare(binder_proc_t *b, uint64_t len, int prot_write, memfd_t **out);
void binder_mmap_done(binder_proc_t *b, uint64_t va);

/* 1 if the calling thread has work to read (its own, or the proc's when it is waiting for work). */
int binder_readable(binder_proc_t *b);

#endif
