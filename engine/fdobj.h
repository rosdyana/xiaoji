/* eventfd and timerfd objects (B3). The objects live in an open-file description
 * (vfs_fd.obj); sysfs.c makes them and vfs.c reads and writes them. Blocking is a
 * restart on the object's own key (see vfs_pipe_block). */
#ifndef XIAOJI_FDOBJ_H
#define XIAOJI_FDOBJ_H

#include <stddef.h>
#include <stdint.h>

#define EFD_SEMAPHORE 1u
#define TFD_TIMER_ABSTIME 1u

void *efd_new(uint64_t init, int semaphore);
void efd_free(void *o);
int efd_readable(void *o);
int efd_writable(void *o);
int64_t efd_read(void *o, void *buf, size_t n);
int64_t efd_write(void *o, const void *buf, size_t n);

void *tfd_new(int clockid);
void tfd_free(void *o);
int tfd_readable(void *o);
/* 1 and the next expiry (host monotonic ns) if the timer is armed, else 0. */
int tfd_deadline(void *o, uint64_t *dl);
int64_t tfd_read(void *o, void *buf, size_t n);
/* Arms (value_ns != 0, relative or absolute) or disarms (value_ns == 0) the timer. */
void tfd_settime(void *o, int abs, uint64_t value_ns, uint64_t interval_ns);
/* The time to the next expiry (0 if disarmed) and the interval. */
void tfd_gettime(void *o, uint64_t *remain_ns, uint64_t *interval_ns);
/* The host monotonic clock that timers and deadlines use. */
uint64_t fdobj_now(void);

#endif
