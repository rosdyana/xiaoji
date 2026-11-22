/* Host services. host.c is the only file with host-specific code. */
#ifndef XIAOJI_HOST_H
#define XIAOJI_HOST_H

#include <stddef.h>
#include <stdint.h>

uint64_t host_monotonic_ns(void);
uint64_t host_realtime_ns(void);
void host_random(void *buf, size_t len);
/* Writes to host fd 1 or 2. Returns len, or -1 on failure. */
int64_t host_write(int fd, const void *buf, size_t len);
/* Reads a whole host file into a malloc'd buffer. Returns 0 or -1. */
int host_read_file(const char *path, uint8_t **data, size_t *len);

#endif
