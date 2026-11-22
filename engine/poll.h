/* Readiness and waiting: ppoll, epoll (level-triggered), over the engine's objects.
 * Every waiter sleeps on one shared key, and any readiness change wakes them all
 * (poll_notify); each re-checks, so a spurious wake-up is harmless. Timed waits store
 * their absolute deadline per thread, so a rerun does not restart the timeout. */
#ifndef XIAOJI_POLL_H
#define XIAOJI_POLL_H

#include <stdint.h>

#include "vfs.h"

#define PX_IN 0x001u
#define PX_OUT 0x004u
#define PX_ERR 0x008u
#define PX_HUP 0x010u
#define PX_NVAL 0x020u

/* Wakes every poll, ppoll and epoll waiter. Called on any readiness change. */
void poll_notify(void);

/* The POLL_* bits of an open description that are ready, restricted to events and the
 * always-reported ERR, HUP and NVAL. */
uint32_t poll_revents(vfs_fd *f, uint32_t events);

/* The earliest deadline of a timerfd in fds (host monotonic ns); 0 when there is none. */
uint64_t poll_timer_deadline(const vfs_fd *f);

int64_t poll_ppoll(uint64_t fds, uint64_t nfds, uint64_t tmo, uint64_t sigmask, uint64_t sigsize);

void *poll_epoll_new(void);
void poll_epoll_free(void *ep);
int64_t poll_epoll_ctl(int epfd, int op, int fd, uint64_t ev_va);
int64_t poll_epoll_wait(int epfd, uint64_t events_va, int64_t maxevents, int64_t timeout_ms);

#endif
