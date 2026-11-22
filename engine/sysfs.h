/* Linux file and directory syscalls, implemented on the guest VFS. */
#ifndef XIAOJI_SYSFS_H
#define XIAOJI_SYSFS_H

#include <stdint.h>

/* Handles syscall nr if it is a file syscall. Returns 1 and sets *ret if it was
 * handled, 0 otherwise. Arguments are the raw x0 to x5 registers. */
int sysfs_call(uint64_t nr, const uint64_t a[6], int64_t *ret);

#endif
