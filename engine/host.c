/* Host services for native builds (macOS, Linux) and Emscripten. */
/* POSIX clock_gettime and getentropy are hidden under strict -std=c11. */
#define _POSIX_C_SOURCE 200809L

#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <sys/random.h>

static uint64_t ns_of(const struct timespec *ts)
{
    return (uint64_t)ts->tv_sec * 1000000000ull + (uint64_t)ts->tv_nsec;
}

uint64_t host_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ns_of(&ts);
}

uint64_t host_realtime_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ns_of(&ts);
}

void host_random(void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len) {
        size_t chunk = len > 256 ? 256 : len; /* getentropy is limited to 256 bytes */
        if (getentropy(p, chunk) != 0) {
            fputs("xiaoji: getentropy failed\n", stderr);
            exit(1);
        }
        p += chunk;
        len -= chunk;
    }
}

int64_t host_write(int fd, const void *buf, size_t len)
{
    FILE *f = fd == 2 ? stderr : stdout;
    if (fwrite(buf, 1, len, f) != len)
        return -1;
    if (fd == 2)
        fflush(stderr);
    return (int64_t)len;
}

int host_read_file(const char *path, uint8_t **data, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t cap = 1 << 16, used = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) {
        fclose(f);
        return -1;
    }
    for (;;) {
        if (used == cap) {
            uint8_t *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                fclose(f);
                return -1;
            }
            buf = nb;
            cap *= 2;
        }
        size_t n = fread(buf + used, 1, cap - used, f);
        used += n;
        if (n == 0)
            break;
    }
    int bad = ferror(f);
    fclose(f);
    if (bad) {
        free(buf);
        return -1;
    }
    *data = buf;
    *len = used;
    return 0;
}
