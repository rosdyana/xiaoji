/* Host services. host.c is the only file with host-specific code: errno and
 * flag translation, stat conversion, and the clock and random sources. Every
 * host_* call that can fail returns a Linux errno as -LE_* (or 0), never a host
 * errno, so guest-visible numbers never depend on the host. */
#ifndef XIAOJI_HOST_H
#define XIAOJI_HOST_H

#include <stddef.h>
#include <stdint.h>

/* File metadata in Linux terms. mode holds Linux S_IF* type bits and permissions. */
typedef struct {
    uint64_t dev, ino, nlink, rdev, size, blocks, blksize;
    uint64_t atime_s, atime_ns, mtime_s, mtime_ns, ctime_s, ctime_ns;
    uint32_t mode, uid, gid;
} host_stat;

typedef struct {
    uint64_t bsize, frsize, blocks, bfree, bavail, files, ffree, namelen;
} host_statfs;

/* Host errno to Linux errno (returned negative by callers). */
int host_linux_errno(int host_errno);

uint64_t host_monotonic_ns(void);
uint64_t host_realtime_ns(void);
void host_sleep_ns(uint64_t ns);
void host_random(void *buf, size_t len);

/* Files. Flags are Linux open(2) flags. host_open returns a host fd or -LE_*. */
int host_open(const char *path, uint32_t lflags, uint32_t mode);
int host_close(int fd);
int64_t host_read(int fd, void *buf, size_t len);
int64_t host_pread(int fd, void *buf, size_t len, uint64_t off);
int64_t host_write(int fd, const void *buf, size_t len);
int64_t host_lseek(int fd, int64_t off, int whence);
int host_dup(int fd);
/* Linux F_GETFL flags for a host fd (fds 0 to 2), or -LE_*. */
int host_fd_flags(int fd);

/* Metadata. follow=0 is lstat. Returns 0 or -LE_*. */
int host_stat_path(const char *path, int follow, host_stat *out);
int host_stat_fd(int fd, host_stat *out);
int host_statfs_path(const char *path, host_statfs *out);
int host_access(const char *path, uint32_t mode);
/* Filesystem changes; each returns 0 or -linux_errno. */
int host_chmod(const char *path, uint32_t mode);
int host_fchmod(int fd, uint32_t mode);
int host_mkdir(const char *path, uint32_t mode);
int host_unlink(const char *path);
int host_rmdir(const char *path);
int host_rename(const char *from, const char *to);
int host_link(const char *from, const char *to);
int host_fsync(int fd);
/* Returns the link length (truncated to cap) or -LE_*. */
int64_t host_readlink(const char *path, char *buf, size_t cap);

/* Reads a whole host file into a malloc'd buffer. Returns 0 or -1. */
int host_read_file(const char *path, uint8_t **data, size_t *len);

#endif
