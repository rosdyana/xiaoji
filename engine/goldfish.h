/* The goldfish character devices of the emulator (docs/graphics.md, section 1.4), which the guest's graphics
 * libraries open:
 *   /dev/goldfish_pipe (alias /dev/qemu_pipe): the byte-stream pipe to the GL host (goldfish.c, pipe).
 *   /dev/goldfish_address_space: the address-space device; the HostMemoryAllocator subdevice gives the ranchu
 *     allocator its buffers (ioctls, and mmap of a block).
 *   /dev/goldfish_sync: fences as sync-file fds (QUEUE_WORK returns an fd that is already signalled).
 * Every object starts with its kind, so the fd layer can dispatch on it. The guest code is
 * device/generic/goldfish-opengl (shared/qemupipe, shared/GoldfishAddressSpace). */
#ifndef XIAOJI_GOLDFISH_H
#define XIAOJI_GOLDFISH_H

#include <stddef.h>
#include <stdint.h>

#include "memfd.h"

typedef enum { GF_NONE = 0, GF_PIPE = 1, GF_ADDRESS_SPACE = 2, GF_SYNC = 3 } gf_kind;

/* The device a guest path names, or GF_NONE. */
gf_kind goldfish_device(const char *path);

/* A new object of kind (an unnamed pipe, or an address space with no subdevice yet). NULL when out of memory. */
void *goldfish_open(gf_kind kind);
void goldfish_free(void *o);

/* Pipes: read returns -LE_AGAIN when no reply is pending (the caller blocks on goldfish_key, see
 * vfs_pipe_block). write returns the bytes taken, or -LE_INVAL for a bad service name or a malformed stream.
 * An address space has no read or write (-LE_INVAL). */
int64_t goldfish_read(void *o, void *buf, size_t n);
int64_t goldfish_write(void *o, const void *buf, size_t n);
int goldfish_readable(void *o);
int goldfish_deadline(void *o, uint64_t *dl);
/* The wait key of an object: a blocked reader sleeps on it and a write wakes it. */
uint64_t goldfish_key(void *o);

/* ioctl on an address space (the guest's ioctl_* calls): the result, or -LE_NOTTY for a command it lacks. */
int64_t goldfish_ioctl(void *o, uint32_t cmd, uint64_t arg);

/* mmap of an address space: the memfd that backs its blocks (one reference for the caller's description;
 * the caller maps it at the block's offset). Every open shares it (the device's one window). */
memfd_t *goldfish_as_memfd(void *o);

/* The host's access to the window by guest-physical address (rcUpdateColorBufferDMA and rcReadColorBufferDMA, gl_host.c):
 * copies n bytes at phys out of the window (write 0) or into it (write 1). 0, or -1 when the range is outside it. */
int goldfish_as_dma(uint64_t phys, void *buf, size_t n, int write);

#endif
