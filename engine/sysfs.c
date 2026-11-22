/* Linux file and directory syscalls on top of the guest VFS. Guest structures
 * are written field by field in the arm64 layouts (stat, statx, statfs,
 * dirent64); host structs are never copied to the guest. */
#include "sysfs.h"

#include <string.h>

#include "host.h"
#include "linux.h"
#include "mem.h"
#include "proc.h"
#include "syscall.h"
#include "vfs.h"

#define SYS_GETCWD 17
#define SYS_FTRUNCATE 46
#define SYS_DUP 23
#define SYS_DUP3 24
#define SYS_FCNTL 25
#define SYS_FACCESSAT 48
#define SYS_CHDIR 49
#define SYS_FCHDIR 50
#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_GETDENTS64 61
#define SYS_LSEEK 62
#define SYS_READ 63
#define SYS_WRITE 64
#define SYS_READV 65
#define SYS_WRITEV 66
#define SYS_PREAD64 67
#define SYS_READLINKAT 78
#define SYS_NEWFSTATAT 79
#define SYS_FSTAT 80
#define SYS_STATFS 43
#define SYS_FSTATFS 44
#define SYS_UMASK 166
#define SYS_STATX 291
#define SYS_FACCESSAT2 439
#define SYS_MEMFD_CREATE 279

#define IOV_MAX 1024
#define IO_CHUNK 65536
#define STATX_BASIC_STATS 0x7FFu
#define STATFS_EXT4_MAGIC 0xEF53

static uint8_t iobuf[IO_CHUNK];

/* ---------- guest structure writers ---------- */

static void put_u32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* struct stat, 128 bytes (arm64 generic layout). */
static int64_t copy_stat(uint64_t va, const host_stat *s)
{
    uint8_t b[128];
    memset(b, 0, sizeof b);
    put_u64(b + 0, s->dev);
    put_u64(b + 8, s->ino);
    put_u32(b + 16, s->mode);
    put_u32(b + 20, (uint32_t)s->nlink);
    put_u32(b + 24, s->uid);
    put_u32(b + 28, s->gid);
    put_u64(b + 32, s->rdev);
    put_u64(b + 48, s->size);
    put_u32(b + 56, (uint32_t)s->blksize);
    put_u64(b + 64, s->blocks);
    put_u64(b + 72, s->atime_s);
    put_u64(b + 80, s->atime_ns);
    put_u64(b + 88, s->mtime_s);
    put_u64(b + 96, s->mtime_ns);
    put_u64(b + 104, s->ctime_s);
    put_u64(b + 112, s->ctime_ns);
    return mem_copy_in(va, b, sizeof b) < 0 ? -LE_FAULT : 0;
}

/* struct statx, 256 bytes. Each timestamp is {i64 sec, u32 nsec, i32 pad}. */
static int64_t copy_statx(uint64_t va, const host_stat *s)
{
    uint8_t b[256];
    memset(b, 0, sizeof b);
    put_u32(b + 0, STATX_BASIC_STATS);
    put_u32(b + 4, (uint32_t)s->blksize);
    put_u32(b + 16, (uint32_t)s->nlink);
    put_u32(b + 20, s->uid);
    put_u32(b + 24, s->gid);
    put_u16(b + 28, (uint16_t)s->mode);
    put_u64(b + 32, s->ino);
    put_u64(b + 40, s->size);
    put_u64(b + 48, s->blocks);
    put_u64(b + 64, s->atime_s);
    put_u32(b + 72, (uint32_t)s->atime_ns);
    put_u64(b + 96, s->ctime_s);
    put_u32(b + 104, (uint32_t)s->ctime_ns);
    put_u64(b + 112, s->mtime_s);
    put_u32(b + 120, (uint32_t)s->mtime_ns);
    put_u32(b + 128, (uint32_t)((s->rdev >> 8) & 0xFFF)); /* rdev_major */
    put_u32(b + 132, (uint32_t)(s->rdev & 0xFF));         /* rdev_minor */
    put_u32(b + 136, (uint32_t)((s->dev >> 8) & 0xFFF));  /* dev_major */
    put_u32(b + 140, (uint32_t)(s->dev & 0xFF));          /* dev_minor */
    return mem_copy_in(va, b, sizeof b) < 0 ? -LE_FAULT : 0;
}

/* struct statfs, 120 bytes. */
static int64_t copy_statfs(uint64_t va, const host_statfs *s)
{
    uint8_t b[120];
    memset(b, 0, sizeof b);
    put_u64(b + 0, STATFS_EXT4_MAGIC);
    put_u64(b + 8, s->bsize);
    put_u64(b + 16, s->blocks);
    put_u64(b + 24, s->bfree);
    put_u64(b + 32, s->bavail);
    put_u64(b + 40, s->files);
    put_u64(b + 48, s->ffree);
    /* fsid (56..63) stays zero */
    put_u64(b + 64, s->namelen);
    put_u64(b + 72, s->frsize);
    return mem_copy_in(va, b, sizeof b) < 0 ? -LE_FAULT : 0;
}

/* ---------- guest I/O helpers ---------- */

static int64_t fs_read(int fd, uint64_t buf, uint64_t n)
{
    size_t want = n < IO_CHUNK ? (size_t)n : IO_CHUNK;
    int64_t r = vfs_read(fd, iobuf, want);
    if (r > 0 && mem_copy_in(buf, iobuf, (size_t)r) < 0)
        return -LE_FAULT;
    return r;
}

static int64_t fs_pread(int fd, uint64_t buf, uint64_t n, uint64_t off)
{
    size_t want = n < IO_CHUNK ? (size_t)n : IO_CHUNK;
    int64_t r = vfs_pread(fd, iobuf, want, off);
    if (r > 0 && mem_copy_in(buf, iobuf, (size_t)r) < 0)
        return -LE_FAULT;
    return r;
}

static int64_t fs_write(int fd, uint64_t buf, uint64_t n)
{
    uint64_t done = 0;
    do {
        size_t chunk = n - done < IO_CHUNK ? (size_t)(n - done) : IO_CHUNK;
        if (mem_copy_out(iobuf, buf + done, chunk) < 0)
            return done ? (int64_t)done : -LE_FAULT;
        int64_t w = vfs_write(fd, iobuf, chunk);
        if (w < 0)
            return done ? (int64_t)done : w;
        done += (uint64_t)w;
        if ((size_t)w < chunk)
            break;
    } while (done < n);
    return (int64_t)done;
}

static int64_t fs_readv(int fd, uint64_t iov, uint64_t cnt)
{
    if (cnt > IOV_MAX)
        return -LE_INVAL;
    int64_t total = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        uint64_t ent[2];
        if (mem_copy_out(ent, iov + i * 16, 16) < 0)
            return total ? total : -LE_FAULT;
        if (ent[1] == 0)
            continue;
        int64_t r = fs_read(fd, ent[0], ent[1]);
        if (r < 0)
            return total ? total : r;
        total += r;
        if ((uint64_t)r < ent[1])
            break;
    }
    return total;
}

static int64_t fs_writev(int fd, uint64_t iov, uint64_t cnt)
{
    if (cnt > IOV_MAX)
        return -LE_INVAL;
    int64_t total = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        uint64_t ent[2];
        if (mem_copy_out(ent, iov + i * 16, 16) < 0)
            return total ? total : -LE_FAULT;
        if (ent[1] == 0)
            continue;
        int64_t w = fs_write(fd, ent[0], ent[1]);
        if (w < 0)
            return total ? total : w;
        total += w;
        if ((uint64_t)w < ent[1])
            break;
    }
    return total;
}

/* Copies a guest path into buf. Returns 0 or -LE_*. */
static int get_path(uint64_t va, char *buf, size_t cap)
{
    int64_t n = mem_strlen(va);
    if (n < 0)
        return -LE_FAULT;
    if ((size_t)n >= cap)
        return -LE_NAMETOOLONG;
    if (mem_copy_out(buf, va, (size_t)n + 1) < 0)
        return -LE_FAULT;
    return 0;
}

static int dirfd_of(uint64_t a) { return (int)(int32_t)a; }

/* ---------- syscalls ---------- */

static int64_t sys_openat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    static char path[VFS_PATH_MAX];
    int rc = get_path(a1, path, sizeof path);
    if (rc)
        return rc;
    return vfs_open(dirfd_of(a0), path, (uint32_t)a2, (uint32_t)a3);
}

static int64_t sys_fstat(uint64_t fd, uint64_t buf)
{
    host_stat st;
    int rc = vfs_stat_fd((int)(int32_t)fd, &st);
    if (rc)
        return rc;
    return copy_stat(buf, &st);
}

static int64_t sys_newfstatat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    static char path[VFS_PATH_MAX];
    int dirfd = dirfd_of(a0);
    uint32_t flags = (uint32_t)a3;
    host_stat st;
    int rc = get_path(a1, path, sizeof path);
    if (rc)
        return rc;
    if ((flags & LAT_EMPTY_PATH) && path[0] == 0)
        return sys_fstat((uint64_t)(int64_t)dirfd, a2);
    static vfs_res r;
    rc = vfs_resolve(dirfd, path, !(flags & LAT_SYMLINK_NOFOLLOW), &r);
    if (rc)
        return rc;
    if (!r.exists)
        return -LE_NOENT;
    rc = vfs_stat_res(&r, &st);
    if (rc)
        return rc;
    return copy_stat(a2, &st);
}

static int64_t sys_statx(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    static char path[VFS_PATH_MAX];
    (void)a3; /* mask: we always fill the basic statistics */
    int dirfd = dirfd_of(a0);
    uint32_t flags = (uint32_t)a2;
    host_stat st;
    int rc = get_path(a1, path, sizeof path);
    if (rc)
        return rc;
    if ((flags & LAT_EMPTY_PATH) && path[0] == 0) {
        rc = vfs_stat_fd(dirfd, &st);
        if (rc)
            return rc;
        return copy_statx(a4, &st);
    }
    static vfs_res r;
    rc = vfs_resolve(dirfd, path, !(flags & LAT_SYMLINK_NOFOLLOW), &r);
    if (rc)
        return rc;
    if (!r.exists)
        return -LE_NOENT;
    rc = vfs_stat_res(&r, &st);
    if (rc)
        return rc;
    return copy_statx(a4, &st);
}

static int64_t sys_readlinkat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    static char path[VFS_PATH_MAX];
    static vfs_res r;
    int rc = get_path(a1, path, sizeof path);
    if (rc)
        return rc;
    rc = vfs_resolve(dirfd_of(a0), path, 0, &r);
    if (rc)
        return rc;
    if (!r.exists)
        return -LE_NOENT;
    if (!r.is_link)
        return -LE_INVAL;
    if (a3 == 0)
        return -LE_INVAL;
    size_t n = strlen(r.target);
    if (n > a3)
        n = (size_t)a3;
    if (mem_copy_in(a2, r.target, n) < 0)
        return -LE_FAULT;
    return (int64_t)n;
}

static int64_t sys_faccessat(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    static char path[VFS_PATH_MAX];
    static vfs_res r;
    int rc = get_path(a1, path, sizeof path);
    if (rc)
        return rc;
    rc = vfs_resolve(dirfd_of(a0), path, !(a3 & LAT_SYMLINK_NOFOLLOW), &r);
    if (rc)
        return rc;
    if (!r.exists)
        return -LE_NOENT;
    if (r.syn != SYN_NONE)
        return 0;
    return host_access(r.host, (uint32_t)a2);
}

static int64_t sys_statfs(uint64_t a0, uint64_t a1)
{
    static char path[VFS_PATH_MAX];
    static vfs_res r;
    host_statfs sf;
    int rc = get_path(a0, path, sizeof path);
    if (rc)
        return rc;
    rc = vfs_resolve(LAT_FDCWD, path, 1, &r);
    if (rc)
        return rc;
    if (!r.exists)
        return -LE_NOENT;
    if (r.syn != SYN_NONE) {
        memset(&sf, 0, sizeof sf);
        sf.bsize = sf.frsize = 4096;
    } else {
        rc = host_statfs_path(r.host, &sf);
        if (rc)
            return rc;
    }
    return copy_statfs(a1, &sf);
}

static int64_t sys_fstatfs(uint64_t fd, uint64_t buf)
{
    host_statfs sf;
    int rc = vfs_statfs_fd((int)(int32_t)fd, &sf);
    if (rc)
        return rc;
    return copy_statfs(buf, &sf);
}

static int64_t sys_getdents64(uint64_t fd, uint64_t buf, uint64_t cnt)
{
    size_t want = cnt < IO_CHUNK ? (size_t)cnt : IO_CHUNK;
    int64_t r = vfs_getdents((int)(int32_t)fd, iobuf, want);
    if (r > 0 && mem_copy_in(buf, iobuf, (size_t)r) < 0)
        return -LE_FAULT;
    return r;
}

static int64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg)
{
    int ifd = (int)(int32_t)fd;
    vfs_fd *f = vfs_get(ifd);
    if (!f)
        return -LE_BADF;
    switch ((int)cmd) {
    case LF_DUPFD:
        return vfs_dup(ifd, (int)arg, 0, 0);
    case LF_DUPFD_CLOEXEC:
        return vfs_dup(ifd, (int)arg, 0, 1);
    case LF_GETFD:
        return f->cloexec ? LFD_CLOEXEC : 0;
    case LF_SETFD:
        f->cloexec = (arg & LFD_CLOEXEC) != 0;
        return 0;
    case LF_GETFL:
        return (int64_t)f->lflags;
    case LF_SETFL:
        f->lflags = (f->lflags & ~(uint32_t)(LO_APPEND | LO_NONBLOCK)) |
                    ((uint32_t)arg & (LO_APPEND | LO_NONBLOCK));
        return 0;
    default:
        return -LE_INVAL;
    }
}

static int64_t sys_dup3(uint64_t old, uint64_t new_fd, uint64_t flags)
{
    int o = (int)(int32_t)old, n = (int)(int32_t)new_fd;
    if (o == n)
        return -LE_INVAL;
    if (flags & ~(uint64_t)LO_CLOEXEC)
        return -LE_INVAL;
    if (n < 0)
        return -LE_BADF;
    return vfs_dup(o, n, 1, (flags & LO_CLOEXEC) != 0);
}

static int64_t sys_getcwd(uint64_t buf, uint64_t size)
{
    char tmp[VFS_PATH_MAX];
    if (size == 0)
        return -LE_INVAL;
    int n = vfs_getcwd(tmp, sizeof tmp);
    if (n < 0)
        return n;
    if ((uint64_t)n > size)
        return -LE_RANGE;
    if (mem_copy_in(buf, tmp, (size_t)n) < 0)
        return -LE_FAULT;
    return n;
}

static int64_t sys_chdir(uint64_t path)
{
    static char p[VFS_PATH_MAX];
    int rc = get_path(path, p, sizeof p);
    if (rc)
        return rc;
    return vfs_chdir(p);
}

/* memfd_create(name, flags). Only MFD_CLOEXEC (bit 0) is accepted; sealing and
 * hugetlb are not modelled. */
static int64_t sys_memfd_create(uint64_t name_va, uint64_t flags)
{
    static char name[256];
    if (flags & ~(uint64_t)1)
        return -LE_INVAL;
    int64_t n = mem_strlen(name_va);
    if (n < 0)
        return -LE_FAULT;
    if (n > 249)
        return -LE_INVAL;
    if (mem_copy_out(name, name_va, (size_t)n) < 0)
        return -LE_FAULT;
    name[n] = 0;
    return vfs_memfd_create(name, (uint32_t)flags);
}

/* ftruncate(fd, len) on a memfd. Other files are not resized (EINVAL). */
static int64_t sys_ftruncate(uint64_t fd, uint64_t len)
{
    vfs_fd *f = vfs_get((int)(int32_t)fd);
    if (!f)
        return -LE_BADF;
    if (f->kind != VFS_MEMFD)
        return -LE_INVAL;
    if ((f->lflags & LO_ACCMODE) == LO_RDONLY)
        return -LE_INVAL;
    if (len > (1ull << 40))
        return -LE_INVAL;
    if (memfd_truncate(f->mfd, len) < 0)
        return -LE_NOMEM;
    return 0;
}

int sysfs_call(uint64_t nr, const uint64_t a[6], int64_t *ret)
{
    switch (nr) {
    case SYS_FTRUNCATE: *ret = sys_ftruncate(a[0], a[1]); return 1;
    case SYS_MEMFD_CREATE: *ret = sys_memfd_create(a[0], a[1]); return 1;
    case SYS_OPENAT: *ret = sys_openat(a[0], a[1], a[2], a[3]); return 1;
    case SYS_CLOSE: *ret = vfs_close((int)(int32_t)a[0]); return 1;
    case SYS_READ: *ret = fs_read((int)(int32_t)a[0], a[1], a[2]); return 1;
    case SYS_WRITE: *ret = fs_write((int)(int32_t)a[0], a[1], a[2]); return 1;
    case SYS_READV: *ret = fs_readv((int)(int32_t)a[0], a[1], a[2]); return 1;
    case SYS_WRITEV: *ret = fs_writev((int)(int32_t)a[0], a[1], a[2]); return 1;
    case SYS_PREAD64: *ret = fs_pread((int)(int32_t)a[0], a[1], a[2], a[3]); return 1;
    case SYS_LSEEK: *ret = vfs_lseek((int)(int32_t)a[0], (int64_t)a[1], (int)(int32_t)a[2]); return 1;
    case SYS_FSTAT: *ret = sys_fstat(a[0], a[1]); return 1;
    case SYS_NEWFSTATAT: *ret = sys_newfstatat(a[0], a[1], a[2], a[3]); return 1;
    case SYS_STATX: *ret = sys_statx(a[0], a[1], a[2], a[3], a[4]); return 1;
    case SYS_READLINKAT: *ret = sys_readlinkat(a[0], a[1], a[2], a[3]); return 1;
    case SYS_FACCESSAT: *ret = sys_faccessat(a[0], a[1], a[2], 0); return 1;
    case SYS_FACCESSAT2: *ret = sys_faccessat(a[0], a[1], a[2], a[3]); return 1;
    case SYS_GETDENTS64: *ret = sys_getdents64(a[0], a[1], a[2]); return 1;
    case SYS_FSTATFS: *ret = sys_fstatfs(a[0], a[1]); return 1;
    case SYS_STATFS: *ret = sys_statfs(a[0], a[1]); return 1;
    case SYS_FCNTL: *ret = sys_fcntl(a[0], a[1], a[2]); return 1;
    case SYS_DUP: *ret = vfs_dup((int)(int32_t)a[0], 0, 0, 0); return 1;
    case SYS_DUP3: *ret = sys_dup3(a[0], a[1], a[2]); return 1;
    case SYS_GETCWD: *ret = sys_getcwd(a[0], a[1]); return 1;
    case SYS_CHDIR: *ret = sys_chdir(a[0]); return 1;
    case SYS_FCHDIR: *ret = vfs_fchdir((int)(int32_t)a[0]); return 1;
    case SYS_UMASK: {
        uint32_t old = proc_cur()->umask;
        proc_cur()->umask = (uint32_t)a[0] & 0777u;
        *ret = old;
        return 1;
    }
    default:
        return 0;
    }
}
