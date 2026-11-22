#include "pipe.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "linux.h"
#include "sched.h"

struct pipe {
    uint8_t *buf; /* PIPE_CAP bytes */
    size_t head;  /* index of the first byte */
    size_t count; /* bytes in the ring */
    int readers, writers;
};

/* The wait key of a pipe is its address: every thread blocked on it is woken on any
 * change, and each re-checks its own condition. */
static void changed(pipe_t *p) { sched_wake_key((uint64_t)(uintptr_t)p); }

pipe_t *pipe_new(void)
{
    pipe_t *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->buf = malloc(PIPE_CAP);
    if (!p->buf) {
        free(p);
        return NULL;
    }
    return p;
}

void pipe_open(pipe_t *p, int write)
{
    if (write)
        p->writers++;
    else
        p->readers++;
}

void pipe_close(pipe_t *p, int write)
{
    if (write)
        p->writers--;
    else
        p->readers--;
    changed(p);
    if (p->readers == 0 && p->writers == 0) {
        free(p->buf);
        free(p);
    }
}

int64_t pipe_read(pipe_t *p, void *buf, size_t n)
{
    if (n == 0)
        return 0;
    if (p->count == 0)
        return p->writers ? -LE_AGAIN : 0;
    size_t k = n < p->count ? n : p->count;
    size_t first = PIPE_CAP - p->head;
    if (first > k)
        first = k;
    memcpy(buf, p->buf + p->head, first);
    memcpy((uint8_t *)buf + first, p->buf, k - first);
    p->head = (p->head + k) % PIPE_CAP;
    p->count -= k;
    changed(p);
    return (int64_t)k;
}

int64_t pipe_write(pipe_t *p, const void *buf, size_t n)
{
    if (p->readers == 0)
        return -LE_PIPE;
    if (n == 0)
        return 0;
    size_t space = PIPE_CAP - p->count;
    size_t need = n <= PIPE_BUF ? n : 1;
    if (space < need)
        return -LE_AGAIN;
    size_t k = n < space ? n : space;
    size_t tail = (p->head + p->count) % PIPE_CAP;
    size_t first = PIPE_CAP - tail;
    if (first > k)
        first = k;
    memcpy(p->buf + tail, buf, first);
    memcpy(p->buf, (const uint8_t *)buf + first, k - first);
    p->count += k;
    changed(p);
    return (int64_t)k;
}
