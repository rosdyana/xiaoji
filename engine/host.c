/* Host services for native builds (macOS, Linux) and Emscripten NODERAWFS.
 * This is the only file with host-specific translation. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#endif
/* POSIX clock_gettime, getentropy, statvfs and fcntl are hidden under strict -std=c11. */
#define _POSIX_C_SOURCE 200809L

#include "host.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <sys/random.h>

#include "linux.h"

/* ---------- errno ---------- */

int host_linux_errno(int e)
{
    switch (e) {
    case EPERM: return LE_PERM;
    case ENOENT: return LE_NOENT;
    case ESRCH: return LE_SRCH;
    case EINTR: return LE_INTR;
    case EIO: return LE_IO;
    case ENXIO: return LE_NXIO;
    case E2BIG: return LE_2BIG;
    case ENOEXEC: return LE_NOEXEC;
    case EBADF: return LE_BADF;
    case ECHILD: return LE_CHILD;
    case EAGAIN: return LE_AGAIN;
    case ENOMEM: return LE_NOMEM;
    case EACCES: return LE_ACCES;
    case EFAULT: return LE_FAULT;
    case EBUSY: return LE_BUSY;
    case EEXIST: return LE_EXIST;
    case EXDEV: return LE_XDEV;
    case ENODEV: return LE_NODEV;
    case ENOTDIR: return LE_NOTDIR;
    case EISDIR: return LE_ISDIR;
    case EINVAL: return LE_INVAL;
    case ENFILE: return LE_NFILE;
    case EMFILE: return LE_MFILE;
    case ENOTTY: return LE_NOTTY;
    case EFBIG: return LE_FBIG;
    case ENOSPC: return LE_NOSPC;
    case ESPIPE: return LE_SPIPE;
    case EROFS: return LE_ROFS;
    case EPIPE: return LE_PIPE;
    case ERANGE: return LE_RANGE;
    case ENAMETOOLONG: return LE_NAMETOOLONG;
    case ENOSYS: return LE_NOSYS;
    case ENOTEMPTY: return LE_NOTEMPTY;
    case ELOOP: return LE_LOOP;
    case EOVERFLOW: return LE_OVERFLOW;
    case ENOTSUP: return LE_NOTSUP;
#if EOPNOTSUPP != ENOTSUP /* the same value on Linux and Emscripten */
    case EOPNOTSUPP: return LE_NOTSUP;
#endif
    case ETIMEDOUT: return LE_TIMEDOUT;
    default: return LE_IO;
    }
}

static int neg_errno(void) { return -host_linux_errno(errno); }

/* ---------- open flags ---------- */

static int host_open_flags(uint32_t l)
{
    int h;
    switch (l & LO_ACCMODE) {
    case LO_WRONLY: h = O_WRONLY; break;
    case LO_RDWR: h = O_RDWR; break;
    default: h = O_RDONLY; break;
    }
    if (l & LO_CREAT) h |= O_CREAT;
    if (l & LO_EXCL) h |= O_EXCL;
    if (l & LO_NOCTTY) h |= O_NOCTTY;
    if (l & LO_TRUNC) h |= O_TRUNC;
    if (l & LO_APPEND) h |= O_APPEND;
    if (l & LO_NONBLOCK) h |= O_NONBLOCK;
    if (l & LO_NOFOLLOW) h |= O_NOFOLLOW;
    if (l & LO_CLOEXEC) h |= O_CLOEXEC;
    return h;
}

/* ---------- stat ---------- */

static uint32_t linux_mode(mode_t m)
{
    uint32_t t;
    if (S_ISREG(m)) t = LS_IFREG;
    else if (S_ISDIR(m)) t = LS_IFDIR;
    else if (S_ISLNK(m)) t = LS_IFLNK;
    else if (S_ISCHR(m)) t = LS_IFCHR;
    else if (S_ISBLK(m)) t = LS_IFBLK;
    else if (S_ISFIFO(m)) t = LS_IFIFO;
    else if (S_ISSOCK(m)) t = LS_IFSOCK;
    else t = 0;
    return t | ((uint32_t)m & 07777u);
}

static void fill_stat(const struct stat *st, host_stat *o)
{
    o->dev = (uint64_t)st->st_dev;
    o->ino = (uint64_t)st->st_ino;
    o->nlink = (uint64_t)st->st_nlink;
    o->rdev = (uint64_t)st->st_rdev;
    o->size = (uint64_t)st->st_size;
    o->blocks = (uint64_t)st->st_blocks;
    o->blksize = (uint64_t)st->st_blksize;
#if defined(__APPLE__)
    o->atime_s = (uint64_t)st->st_atimespec.tv_sec;
    o->atime_ns = (uint64_t)st->st_atimespec.tv_nsec;
    o->mtime_s = (uint64_t)st->st_mtimespec.tv_sec;
    o->mtime_ns = (uint64_t)st->st_mtimespec.tv_nsec;
    o->ctime_s = (uint64_t)st->st_ctimespec.tv_sec;
    o->ctime_ns = (uint64_t)st->st_ctimespec.tv_nsec;
#else
    o->atime_s = (uint64_t)st->st_atim.tv_sec;
    o->atime_ns = (uint64_t)st->st_atim.tv_nsec;
    o->mtime_s = (uint64_t)st->st_mtim.tv_sec;
    o->mtime_ns = (uint64_t)st->st_mtim.tv_nsec;
    o->ctime_s = (uint64_t)st->st_ctim.tv_sec;
    o->ctime_ns = (uint64_t)st->st_ctim.tv_nsec;
#endif
    o->mode = linux_mode(st->st_mode);
    o->uid = (uint32_t)st->st_uid;
    o->gid = (uint32_t)st->st_gid;
}

int host_stat_path(const char *path, int follow, host_stat *out)
{
    struct stat st;
    int rc = follow ? stat(path, &st) : lstat(path, &st);
    if (rc != 0)
        return neg_errno();
    fill_stat(&st, out);
    return 0;
}

int host_stat_fd(int fd, host_stat *out)
{
    struct stat st;
    if (fstat(fd, &st) != 0)
        return neg_errno();
    fill_stat(&st, out);
    return 0;
}

int host_statfs_path(const char *path, host_statfs *out)
{
    struct statvfs sv;
    if (statvfs(path, &sv) != 0)
        return neg_errno();
    out->bsize = (uint64_t)sv.f_bsize;
    out->frsize = (uint64_t)sv.f_frsize;
    out->blocks = (uint64_t)sv.f_blocks;
    out->bfree = (uint64_t)sv.f_bfree;
    out->bavail = (uint64_t)sv.f_bavail;
    out->files = (uint64_t)sv.f_files;
    out->ffree = (uint64_t)sv.f_ffree;
    out->namelen = (uint64_t)sv.f_namemax;
    return 0;
}

/* access(2) mode bits are the same on every host (R_OK=4, W_OK=2, X_OK=1). */
int host_access(const char *path, uint32_t mode)
{
    if (access(path, (int)mode) != 0)
        return neg_errno();
    return 0;
}

int host_chmod(const char *path, uint32_t mode)
{
    return chmod(path, (mode_t)(mode & 07777)) == 0 ? 0 : neg_errno();
}

int host_fchmod(int fd, uint32_t mode)
{
    return fchmod(fd, (mode_t)(mode & 07777)) == 0 ? 0 : neg_errno();
}

int host_mkdir(const char *path, uint32_t mode)
{
    return mkdir(path, (mode_t)(mode & 07777)) == 0 ? 0 : neg_errno();
}

int host_unlink(const char *path)
{
    return unlink(path) == 0 ? 0 : neg_errno();
}

int host_rmdir(const char *path)
{
    return rmdir(path) == 0 ? 0 : neg_errno();
}

int host_rename(const char *from, const char *to)
{
    return rename(from, to) == 0 ? 0 : neg_errno();
}

/* A hard link. The caller resolves from (sysfs.c sys_linkat), so a final symlink is linked as it is unless the caller follows it. */
int host_link(const char *from, const char *to)
{
    return link(from, to) == 0 ? 0 : neg_errno();
}

int host_fsync(int fd)
{
    return fsync(fd) == 0 ? 0 : neg_errno();
}

int64_t host_readlink(const char *path, char *buf, size_t cap)
{
    ssize_t n = readlink(path, buf, cap);
    if (n < 0)
        return neg_errno();
    return (int64_t)n;
}

/* ---------- files ---------- */

int host_open(const char *path, uint32_t lflags, uint32_t mode)
{
    int fd = open(path, host_open_flags(lflags), (mode_t)(mode & 07777));
    if (fd < 0)
        return neg_errno();
    return fd;
}

int host_close(int fd)
{
    if (close(fd) != 0)
        return neg_errno();
    return 0;
}

int64_t host_read(int fd, void *buf, size_t len)
{
    ssize_t n = read(fd, buf, len);
    return n < 0 ? neg_errno() : (int64_t)n;
}

int64_t host_pread(int fd, void *buf, size_t len, uint64_t off)
{
    ssize_t n = pread(fd, buf, len, (off_t)off);
    return n < 0 ? neg_errno() : (int64_t)n;
}

int64_t host_write(int fd, const void *buf, size_t len)
{
    ssize_t n = write(fd, buf, len);
    return n < 0 ? neg_errno() : (int64_t)n;
}

int64_t host_lseek(int fd, int64_t off, int whence)
{
    off_t r = lseek(fd, (off_t)off, whence);
    return r < 0 ? neg_errno() : (int64_t)r;
}

int host_dup(int fd)
{
    int n = dup(fd);
    return n < 0 ? neg_errno() : n;
}

int host_fd_flags(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0)
        return neg_errno();
    uint32_t l = (uint32_t)(fl & O_ACCMODE);
    if (fl & O_APPEND)
        l |= LO_APPEND;
    if (fl & O_NONBLOCK)
        l |= LO_NONBLOCK;
    return (int)l;
}

/* ---------- clocks and randomness ---------- */

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

void host_sleep_ns(uint64_t ns)
{
    struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
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
