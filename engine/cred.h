/* Process credentials: the set*id and get*id calls (cred.c). */
#ifndef XIAOJI_CRED_H
#define XIAOJI_CRED_H

#include <stdint.h>

/* The credential syscall nr (143 to 152, 158, 159, 174 to 177) with its arguments a. Returns the
 * Linux result (or -errno). */
int64_t cred_syscall(uint64_t nr, const uint64_t a[6]);

#endif
