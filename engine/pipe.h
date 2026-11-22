/* Pipes: a bounded byte ring with a read end and a write end, each end counted
 * per open file description. This module never blocks. It reports "would block"
 * (-LE_AGAIN) and wakes the threads waiting on the pipe; the caller restarts the
 * call (see vfs_pipe_block). */
#ifndef XIAOJI_PIPE_H
#define XIAOJI_PIPE_H

#include <stddef.h>
#include <stdint.h>

#define PIPE_CAP 65536u /* bytes in the ring (the Linux default) */
#define PIPE_BUF 4096u  /* writes of at most this size are atomic */

typedef struct pipe pipe_t;

pipe_t *pipe_new(void); /* NULL when out of host memory */
/* An open file description for one end was created (write 1: the write end). */
void pipe_open(pipe_t *p, int write);
/* Readiness for poll: readable (data is queued), writable (room for a byte), and the
 * number of readers and writers. */
void pipe_state(const pipe_t *p, int *readable, int *writable, int *readers, int *writers);
/* A description for one end was released. Frees the pipe when both ends are gone. */
void pipe_close(pipe_t *p, int write);
/* n > 0 bytes read; 0 at end of file (no writers left and the ring is empty);
 * -LE_AGAIN if the ring is empty and a writer exists. */
int64_t pipe_read(pipe_t *p, void *buf, size_t n);
/* Bytes accepted (at least 1 unless n is 0); -LE_PIPE if no reader is left;
 * -LE_AGAIN if the ring cannot take the write yet (writes of PIPE_BUF bytes or
 * less wait until they fit whole). */
int64_t pipe_write(pipe_t *p, const void *buf, size_t n);

#endif
