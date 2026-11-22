/* The socket syscalls (198 to 212, and accept4 at 242): the guest side of sock.h. Each call
 * reads guest memory and descriptors, calls sock.c, and parks a blocked call with a restart. */
#ifndef XIAOJI_SOCKCALL_H
#define XIAOJI_SOCKCALL_H

#include <stdint.h>

/* 1 for a socket syscall number. */
int sockcall_handles(uint64_t nr);
/* Runs a socket syscall with its six arguments. Returns the Linux result (or -errno). */
int64_t sockcall_run(uint64_t nr, const uint64_t *a);

/* The plain data paths of a socket fd: read, readv, write and writev (no ancillary data). */
int64_t sockcall_read(int fd, uint64_t buf, uint64_t n);
int64_t sockcall_readv(int fd, uint64_t iov, uint64_t cnt);
int64_t sockcall_write(int fd, uint64_t buf, uint64_t n);
int64_t sockcall_writev(int fd, uint64_t iov, uint64_t cnt);

#endif
