#ifndef XIAOJI_LOCKS_H
#define XIAOJI_LOCKS_H

/* POSIX record locks (fcntl F_GETLK, F_SETLK, F_SETLKW) and flock(2), as Linux 6.12 fs/locks.c
 * implements them (locks.c has the details). The lock owner of a POSIX lock is the process's fd table
 * (Linux's files_struct): threads share its locks, and a fork gets a new table, so it has none. The
 * owner of a flock lock is the open file description. */

#include <stdint.h>

#include "vfs.h"

/* Lock types, with Linux's values for F_RDLCK, F_WRLCK and F_UNLCK. */
#define LK_RD 0
#define LK_WR 1
#define LK_UN 2
#define LK_EOF INT64_MAX /* OFFSET_MAX: a lock that runs to the end of the file */

/* A blocking call returned this (a positive value): the thread is parked, and the syscall runs again
 * when it is woken (sched_block_restart). The caller returns 0 and leaves x0 alone. */
#define LK_BLOCKED 1

/* The first lock that conflicts with a request (F_GETLK). pid is the process that holds it. */
typedef struct {
    int type, pid;
    int64_t start, end;
} lk_conflict_t;

/* F_GETLK: returns 1 and fills out when a lock of another owner conflicts, else 0. -LE_INVAL when f
 * cannot hold POSIX locks (only regular files, memfds and pipes can). */
int lk_posix_test(void *owner, vfs_fd *f, int type, int64_t start, int64_t end, lk_conflict_t *out);

/* F_SETLK (wait 0) and F_SETLKW (wait 1). Returns 0, -LE_AGAIN (conflict, no wait), -LE_DEADLK,
 * -LE_INTR (a signal that interrupts the wait is pending), LK_BLOCKED, or -LE_INVAL. */
int lk_posix_set(void *owner, int pid, vfs_fd *f, int type, int64_t start, int64_t end, int wait);

/* A close of any descriptor of owner for f's file: the owner's POSIX locks on that file go. */
void lk_posix_close(void *owner, vfs_fd *f);

/* The owner's process ends: every POSIX lock of owner goes, and its waiters are forgotten. */
void lk_posix_owner_release(void *owner);

/* flock(2): type LK_RD (LOCK_SH), LK_WR (LOCK_EX) or LK_UN. Returns 0, -LE_AGAIN (wait 0 and a
 * conflict), -LE_INTR, or LK_BLOCKED. */
int lk_flock(vfs_fd *f, int type, int wait);

/* The last reference to an open file description is gone: its flock lock goes. */
void lk_flock_release(vfs_fd *f);

#endif
