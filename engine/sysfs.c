/* Linux file and directory syscalls on top of the guest VFS. Guest structures
 * are written field by field in the arm64 layouts (stat, statx, statfs,
 * dirent64); host structs are never copied to the guest. */
#include "sysfs.h"

#include <stdlib.h>
#include <string.h>

#include "binder.h"
#include "fdobj.h"
#include "poll.h"
#include "host.h"
#include "linux.h"
#include "mem.h"
#include "pipe.h"
#include "proc.h"
#include "sched.h"
#include "signals.h"
#include "syscall.h"
#include "props.h"
#include "selinuxfs.h"
#include "sock.h"
#include "sockcall.h"
#include "vfs.h"

#define SYS_GETCWD 17
#define SYS_IOCTL 29
#define SYS_EPOLL_CREATE1 20
#define SYS_EPOLL_CTL 21
#define SYS_EPOLL_PWAIT 22
#define SYS_EVENTFD2 19
#define SYS_PPOLL 73
#define SYS_TIMERFD_CREATE 85
#define SYS_TIMERFD_SETTIME 86
#define SYS_TIMERFD_GETTIME 87
#define SYS_FTRUNCATE 46
#define SYS_DUP 23
#define SYS_DUP3 24
#define SYS_PIPE2 59
#define SYS_FCNTL 25
#define SYS_MKDIRAT 34
#define SYS_UNLINKAT 35
#define SYS_FACCESSAT 48
#define SYS_FCHMOD 52
#define SYS_FCHMODAT 53
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
static int64_t copy_statfs(uint64_t va, const host_statfs *s, uint64_t ftype)
{
    uint8_t b[120];
    memset(b, 0, sizeof b);
    put_u64(b + 0, ftype);
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

/* A pipe read or write that would block ends the thread's wait (the call restarts
 * when the pipe changes); vfs_pipe_block does the waiting. The returned value is then
 * not used. */
static int64_t fs_read(int fd, uint64_t buf, uint64_t n)
{
    vfs_fd *sf = vfs_get(fd);
    if (sf && sf->kind == VFS_SOCK)
        return sockcall_read(fd, buf, n);
    size_t want = n < IO_CHUNK ? (size_t)n : IO_CHUNK;
    int64_t r = vfs_read(fd, iobuf, want);
    if (r == -LE_AGAIN && vfs_pipe_block(fd))
        return 0;
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

#define SIGPIPE_N 13

/* Pipe writes. A blocked call records how many bytes of its segments are done (in
 * the thread's io_done) and restarts there. The segments are read from guest memory
 * again on each attempt, so the restart sends the same bytes. A blocking write waits
 * until all of its bytes are in the pipe. */
static uint64_t seg_va[IOV_MAX], seg_len[IOV_MAX];

static int64_t pipe_write_segs(int fd, uint64_t cnt)
{
    cpu_t *c = sched_cur_cpu();
    uint64_t base = 0, cur = c->io_done; /* base: offset of segment i; cur: bytes done */
    int64_t res = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        uint64_t len = seg_len[i];
        if (cur >= base + len) {
            base += len;
            continue;
        }
        uint64_t off = cur - base;
        while (off < len) {
            size_t chunk = len - off < IO_CHUNK ? (size_t)(len - off) : IO_CHUNK;
            if (mem_copy_out(iobuf, seg_va[i] + off, chunk) < 0) {
                res = cur ? (int64_t)cur : -LE_FAULT;
                goto out;
            }
            int64_t w = vfs_write(fd, iobuf, chunk);
            if (w == -LE_AGAIN && vfs_pipe_block(fd)) {
                c->io_done = base + off;
                return 0;
            }
            if (w == -LE_PIPE)
                sig_raise_current(SIGPIPE_N);
            if (w < 0) {
                res = base + off ? (int64_t)(base + off) : w;
                goto out;
            }
            off += (uint64_t)w;
            cur = base + off;
        }
        base += len;
    }
    res = (int64_t)base;
out:
    c->io_done = 0;
    return res;
}

static int64_t fs_write(int fd, uint64_t buf, uint64_t n)
{
    vfs_fd *f = vfs_get(fd);
    if (f && f->kind == VFS_PIPE) {
        seg_va[0] = buf;
        seg_len[0] = n;
        return pipe_write_segs(fd, 1);
    }
    if (f && f->kind == VFS_SOCK)
        return sockcall_write(fd, buf, n);
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

/* readv on a pipe: once some bytes are read, an empty pipe ends the call with the count
 * (no blocking, so the call never restarts after data was taken). */
static int64_t pipe_readv(int fd, uint64_t iov, uint64_t cnt)
{
    int64_t total = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        uint64_t ent[2];
        if (mem_copy_out(ent, iov + i * 16, 16) < 0)
            return total ? total : -LE_FAULT;
        if (ent[1] == 0)
            continue;
        size_t want = ent[1] < IO_CHUNK ? (size_t)ent[1] : IO_CHUNK;
        int64_t r = vfs_read(fd, iobuf, want);
        if (r == -LE_AGAIN) {
            if (total)
                return total;
            if (vfs_pipe_block(fd))
                return 0;
            return r;
        }
        if (r < 0)
            return total ? total : r;
        if (r == 0)
            break;
        if (mem_copy_in(ent[0], iobuf, (size_t)r) < 0)
            return total ? total : -LE_FAULT;
        total += r;
    }
    return total;
}

/* writev. On a pipe the segment list is saved in the static arrays for this call. */
static int64_t pipe_writev(int fd, uint64_t iov, uint64_t cnt)
{
    for (uint64_t i = 0; i < cnt; i++) {
        uint64_t ent[2];
        if (mem_copy_out(ent, iov + i * 16, 16) < 0)
            return -LE_FAULT;
        seg_va[i] = ent[0];
        seg_len[i] = ent[1];
    }
    return pipe_write_segs(fd, cnt);
}

/* pipe2(int fds[2], flags): the read end in fds[0], the write end in fds[1]. */
static int64_t sys_pipe2(uint64_t fds_va, uint64_t flags)
{
    if (flags & ~(uint64_t)(LO_CLOEXEC | LO_NONBLOCK))
        return -LE_INVAL;
    int fds[2];
    int rc = vfs_pipe2(fds, (uint32_t)flags);
    if (rc)
        return rc;
    uint8_t b[8];
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 4; k++)
            b[4 * i + k] = (uint8_t)((uint32_t)fds[i] >> (8 * k));
    if (mem_copy_in(fds_va, b, sizeof b) < 0) {
        vfs_close(fds[0]);
        vfs_close(fds[1]);
        return -LE_FAULT;
    }
    return 0;
}

static int64_t fs_readv(int fd, uint64_t iov, uint64_t cnt)
{
    if (cnt > IOV_MAX)
        return -LE_INVAL;
    vfs_fd *pf = vfs_get(fd);
    if (pf && pf->kind == VFS_PIPE)
        return pipe_readv(fd, iov, cnt);
    if (pf && pf->kind == VFS_SOCK)
        return sockcall_readv(fd, iov, cnt);
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
    vfs_fd *pf = vfs_get(fd);
    if (pf && pf->kind == VFS_PIPE)
        return pipe_writev(fd, iov, cnt);
    if (pf && pf->kind == VFS_SOCK)
        return sockcall_writev(fd, iov, cnt);
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
    if (!strcmp(path, PROPS_AREA_PATH)) { /* the property area is a regular file */
        vfs_stat_props(&st);
        return copy_stat(a2, &st);
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

#define LAT_REMOVEDIR 0x200

static int64_t sys_mkdirat(uint64_t a0, uint64_t a1, uint64_t mode)
{
    static char path[VFS_PATH_MAX];
    static vfs_res r;
    int rc = get_path(a1, path, sizeof path);
    if (rc)
        return rc;
    rc = vfs_resolve(dirfd_of(a0), path, 0, &r);
    if (rc)
        return rc;
    if (r.exists || r.is_link)
        return -LE_EXIST;
    if (r.syn != SYN_NONE)
        return -LE_PERM;
    return host_mkdir(r.host, (uint32_t)mode & ~proc_cur()->umask);
}

static int64_t sys_unlinkat(uint64_t a0, uint64_t a1, uint64_t flags)
{
    static char path[VFS_PATH_MAX];
    static vfs_res r;
    int rc = get_path(a1, path, sizeof path);
    if (rc)
        return rc;
    if (flags & ~(uint64_t)LAT_REMOVEDIR)
        return -LE_INVAL;
    rc = vfs_resolve(dirfd_of(a0), path, 0, &r);
    if (rc)
        return rc;
    if (!r.exists && !r.is_link)
        return -LE_NOENT;
    if (r.syn != SYN_NONE)
        return -LE_PERM;
    if (flags & LAT_REMOVEDIR)
        return host_rmdir(r.host);
    int urc = host_unlink(r.host);
    if (urc == 0)
        sock_fsnode_del(r.guest); /* a socket node goes with its file */
    return urc;
}

/* Ownership and capabilities are not modelled. stat reports uid and gid 0 for every file, so a
 * chown names an existing file (or descriptor) and changes nothing. A process is root, so
 * capget reports the full set (the version-3 layout); capset is accepted and ignored. */
static int64_t sys_fchownat(uint64_t a0, uint64_t a1, uint64_t flags)
{
    static char path[VFS_PATH_MAX];
    static vfs_res r;
    int rc = get_path(a1, path, sizeof path);
    if (rc)
        return rc;
    rc = vfs_resolve(dirfd_of(a0), path, (flags & 0x100) == 0, &r);
    if (rc)
        return rc;
    return r.exists || r.is_link ? 0 : -LE_NOENT;
}

/* unshare(flags). Namespaces are not modelled: the guest has one mount table, one network and
 * one set of ids. CLONE_NEWNS succeeds and changes nothing (zygote asks for a private mount
 * namespace before it specializes an app; the synthetic root is shared by every process). Other
 * namespace flags are EINVAL, and so is CLONE_FILES, which would need a copy of the descriptors. */
/* mount(source, target, type, flags, data). The guest has one mount table that the engine
 * serves (the synthetic /dev, /proc and /sys, and the root). Only a change of propagation (MS_SHARED,
 * MS_SLAVE, MS_PRIVATE, MS_UNBINDABLE) is accepted, on an existing target: propagation between
 * mount namespaces is not modelled, so it changes nothing. Any other mount is EPERM. */
static int64_t sys_mount(uint64_t target_va, uint64_t flags)
{
    const uint64_t MS_PROPAGATION = (1ull << 17) | (1ull << 18) | (1ull << 19) | (1ull << 20);
    if (!(flags & MS_PROPAGATION))
        return -LE_PERM;
    static char path[VFS_PATH_MAX];
    static vfs_res r;
    int rc = get_path(target_va, path, sizeof path);
    if (rc)
        return rc;
    rc = vfs_resolve(LAT_FDCWD, path, 1, &r);
    if (rc)
        return rc;
    return r.exists ? 0 : -LE_NOENT;
}

/* membarrier(cmd, flags): guest threads run one at a time on the host thread, so every memory
 * barrier already holds. QUERY reports the global and the private expedited commands (and their
 * registration); the commands themselves succeed. */
static int64_t sys_membarrier(uint64_t cmd, uint64_t flags)
{
    const int64_t supported = (1 << 0) | (1 << 3) | (1 << 4);
    if (flags)
        return -LE_INVAL;
    if (cmd == 0)
        return supported;
    if (cmd == 1 || cmd == 8 || cmd == 16)
        return 0;
    return -LE_INVAL;
}

/* process_vm_readv(pid, local, liovcnt, remote, riovcnt, flags): reads the calling process's own
 * memory (ART's SafeCopy does, through getpid()). Another pid is ESRCH (no such process) or EPERM.
 * A remote range that cannot be read (unmapped, or PROT_NONE) ends the copy: the count so far is
 * returned, and EFAULT when nothing was copied. */
static int64_t sys_process_vm_readv(uint64_t pid, uint64_t liov, uint64_t liovcnt, uint64_t riov,
                                    uint64_t riovcnt, uint64_t flags)
{
    if (flags || liovcnt > IOV_MAX || riovcnt > IOV_MAX)
        return -LE_INVAL;
    if ((int64_t)pid != proc_cur()->pid)
        return proc_find((int)pid) ? -LE_PERM : -LE_SRCH;
    int64_t total = 0;
    uint64_t li = 0, lbase = 0, llen = 0, ri = 0, rbase = 0, rlen = 0;
    int lok = 0, rok = 0;
    for (;;) {
        if (!lok || llen == 0) {
            if (li >= liovcnt)
                break;
            uint8_t e[16];
            if (mem_copy_out(e, liov + li * 16, 16) < 0)
                return total ? total : -LE_FAULT;
            lbase = 0;
            llen = 0;
            for (int k = 0; k < 8; k++) {
                lbase |= (uint64_t)e[k] << (8 * k);
                llen |= (uint64_t)e[8 + k] << (8 * k);
            }
            li++;
            lok = 1;
            continue;
        }
        if (!rok || rlen == 0) {
            if (ri >= riovcnt)
                break;
            uint8_t e[16];
            if (mem_copy_out(e, riov + ri * 16, 16) < 0)
                return total ? total : -LE_FAULT;
            rbase = 0;
            rlen = 0;
            for (int k = 0; k < 8; k++) {
                rbase |= (uint64_t)e[k] << (8 * k);
                rlen |= (uint64_t)e[8 + k] << (8 * k);
            }
            ri++;
            rok = 1;
            continue;
        }
        size_t n = llen < rlen ? (size_t)llen : (size_t)rlen;
        if (n > IO_CHUNK)
            n = IO_CHUNK;
        if (mem_copy_out(iobuf, rbase, n) < 0)
            return total ? total : -LE_FAULT;
        if (mem_copy_in(lbase, iobuf, n) < 0)
            return total ? total : -LE_FAULT;
        lbase += n;
        llen -= n;
        rbase += n;
        rlen -= n;
        total += (int64_t)n;
    }
    return total;
}

static int64_t sys_unshare(uint64_t flags)
{
    const uint64_t CLONE_NEWNS_FLAG = 0x00020000ull;
    if (flags & ~CLONE_NEWNS_FLAG)
        return -LE_INVAL;
    return 0;
}

static int64_t sys_capget(uint64_t hdr_va, uint64_t data_va)
{
    uint8_t h[8];
    if (mem_copy_out(h, hdr_va, sizeof h) < 0)
        return -LE_FAULT;
    const uint32_t cap_v3 = 0x20080522u;
    uint32_t version = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24;
    if (version != cap_v3) {
        for (int i = 0; i < 4; i++)
            h[i] = (uint8_t)(cap_v3 >> (8 * i));
        mem_copy_in(hdr_va, h, 4);
        return data_va ? -LE_INVAL : 0;
    }
    if (!data_va)
        return 0;
    uint8_t d[24];
    for (int i = 0; i < 24; i++)
        d[i] = (i % 12) < 8 ? 0xff : 0; /* effective and permitted full, inheritable empty */
    return mem_copy_in(data_va, d, sizeof d) < 0 ? -LE_FAULT : 0;
}

static int64_t sys_fchmodat(uint64_t a0, uint64_t a1, uint64_t mode)
{
    static char path[VFS_PATH_MAX];
    static vfs_res r;
    int rc = get_path(a1, path, sizeof path);
    if (rc)
        return rc;
    rc = vfs_resolve(dirfd_of(a0), path, 1, &r);
    if (rc)
        return rc;
    if (!r.exists)
        return -LE_NOENT;
    if (r.syn != SYN_NONE)
        return -LE_PERM;
    return host_chmod(r.host, (uint32_t)mode);
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
    /* The selinux mount reports its own magic (libselinux checks it before using the mount). */
    return copy_statfs(a1, &sf, selinuxfs_kind(r.guest) != SELFS_NONE ? SELINUX_MAGIC : STATFS_EXT4_MAGIC);
}

static int64_t sys_fstatfs(uint64_t fd, uint64_t buf)
{
    host_statfs sf;
    int rc = vfs_statfs_fd((int)(int32_t)fd, &sf);
    if (rc)
        return rc;
    return copy_statfs(buf, &sf, STATFS_EXT4_MAGIC);
}

static int64_t sys_getdents64(uint64_t fd, uint64_t buf, uint64_t cnt)
{
    size_t want = cnt < IO_CHUNK ? (size_t)cnt : IO_CHUNK;
    int64_t r = vfs_getdents((int)(int32_t)fd, iobuf, want);
    if (r > 0 && mem_copy_in(buf, iobuf, (size_t)r) < 0)
        return -LE_FAULT;
    return r;
}

/* Pipe capacity (F_SETPIPE_SZ, F_GETPIPE_SZ). The ring is fixed at PIPE_CAP, so a set reports
 * that capacity back: the guest's debuggerd sets a size on its crash pipe and then checks it. */
#define LF_SETPIPE_SZ 1031
#define LF_GETPIPE_SZ 1032

static int64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg)
{
    int ifd = (int)(int32_t)fd;
    vfs_fd *f = vfs_get(ifd);
    if (!f)
        return -LE_BADF;
    switch ((int)cmd) {
    case LF_SETPIPE_SZ:
    case LF_GETPIPE_SZ:
        if (f->kind != VFS_PIPE)
            return -LE_BADF;
        if (cmd == LF_SETPIPE_SZ && (arg == 0 || arg > (1u << 20)))
            return -LE_INVAL;
        return (int64_t)PIPE_CAP;
    case LF_DUPFD:
        return vfs_dup(ifd, (int)arg, 0, 0);
    case LF_DUPFD_CLOEXEC:
        return vfs_dup(ifd, (int)arg, 0, 1);
    case LF_GETFD:
        return vfs_cloexec_get(ifd) ? LFD_CLOEXEC : 0;
    case LF_SETFD:
        return vfs_cloexec_set(ifd, (arg & LFD_CLOEXEC) != 0);
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

/* eventfd2(initval, flags). */
static int64_t sys_eventfd2(uint64_t init, uint64_t flags)
{
    if (flags & ~(uint64_t)(EFD_SEMAPHORE | LO_NONBLOCK | LO_CLOEXEC))
        return -LE_INVAL;
    void *o = efd_new((uint32_t)init, (flags & EFD_SEMAPHORE) != 0);
    if (!o)
        return -LE_NOMEM;
    return vfs_obj_new(VFS_EVENTFD, o, (flags & LO_CLOEXEC) != 0,
                       LO_RDWR | (uint32_t)(flags & LO_NONBLOCK));
}

/* timerfd_create(clockid, flags). The clocks are CLOCK_MONOTONIC and CLOCK_BOOTTIME; both
 * are the host's monotonic clock in the engine. */
static int64_t sys_timerfd_create(uint64_t clockid, uint64_t flags)
{
    if (clockid != 1 && clockid != 7)
        return -LE_INVAL;
    if (flags & ~(uint64_t)(LO_NONBLOCK | LO_CLOEXEC))
        return -LE_INVAL;
    void *o = tfd_new((int)clockid);
    if (!o)
        return -LE_NOMEM;
    return vfs_obj_new(VFS_TIMERFD, o, (flags & LO_CLOEXEC) != 0,
                       LO_RDONLY | (uint32_t)(flags & LO_NONBLOCK));
}

/* A struct itimerspec in guest memory: interval (sec, nsec), then value (sec, nsec). */
static void put_itimer(uint8_t *b, uint64_t interval_ns, uint64_t value_ns)
{
    uint64_t v[4] = {interval_ns / 1000000000ull, interval_ns % 1000000000ull,
                     value_ns / 1000000000ull, value_ns % 1000000000ull};
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 8; k++)
            b[8 * i + k] = (uint8_t)(v[i] >> (8 * k));
}

/* Reads a timespec (sec, nsec) into ns. Negative or out-of-range fields are EINVAL. */
static int get_timespec_ns(const uint8_t *b, uint64_t *ns)
{
    int64_t s = 0, n = 0;
    for (int k = 7; k >= 0; k--) {
        s = (int64_t)(((uint64_t)s << 8) | b[k]);
        n = (int64_t)(((uint64_t)n << 8) | b[8 + k]);
    }
    if (s < 0 || n < 0 || n >= 1000000000)
        return -LE_INVAL;
    if (s > 1000000000)
        s = 1000000000;
    *ns = (uint64_t)s * 1000000000ull + (uint64_t)n;
    return 0;
}

/* timerfd_settime(fd, flags, new, old). */
static int64_t sys_timerfd_settime(uint64_t fd, uint64_t flags, uint64_t new_va, uint64_t old_va)
{
    vfs_fd *f = vfs_get((int)(int32_t)fd);
    if (!f)
        return -LE_BADF;
    if (f->kind != VFS_TIMERFD)
        return -LE_INVAL;
    if (flags & ~(uint64_t)TFD_TIMER_ABSTIME)
        return -LE_INVAL;
    uint8_t it[32];
    if (mem_copy_out(it, new_va, sizeof it) < 0)
        return -LE_FAULT;
    uint64_t interval_ns = 0, value_ns = 0;
    int rc = get_timespec_ns(it, &interval_ns);
    if (rc)
        return rc;
    rc = get_timespec_ns(it + 16, &value_ns);
    if (rc)
        return rc;
    if (old_va) {
        uint64_t remain = 0, iv = 0;
        tfd_gettime(f->obj, &remain, &iv);
        uint8_t old[32];
        put_itimer(old, iv, remain);
        if (mem_copy_in(old_va, old, sizeof old) < 0)
            return -LE_FAULT;
    }
    tfd_settime(f->obj, (flags & TFD_TIMER_ABSTIME) != 0, value_ns, interval_ns);
    return 0;
}

/* timerfd_gettime(fd, cur). */
static int64_t sys_timerfd_gettime(uint64_t fd, uint64_t cur_va)
{
    vfs_fd *f = vfs_get((int)(int32_t)fd);
    if (!f)
        return -LE_BADF;
    if (f->kind != VFS_TIMERFD)
        return -LE_INVAL;
    uint64_t remain = 0, iv = 0;
    tfd_gettime(f->obj, &remain, &iv);
    uint8_t cur[32];
    put_itimer(cur, iv, remain);
    return mem_copy_in(cur_va, cur, sizeof cur) < 0 ? -LE_FAULT : 0;
}

/* epoll_create1(flags): only EPOLL_CLOEXEC is valid. */
static int64_t sys_epoll_create1(uint64_t flags)
{
    if (flags & ~(uint64_t)LO_CLOEXEC)
        return -LE_INVAL;
    void *ep = poll_epoll_new();
    if (!ep)
        return -LE_NOMEM;
    return vfs_obj_new(VFS_EPOLL, ep, (flags & LO_CLOEXEC) != 0, LO_RDONLY);
}

/* ioctl. Only binder fds take ioctls; any other fd is ENOTTY. */
static int64_t sys_ioctl(uint64_t fd, uint64_t cmd, uint64_t arg)
{
    vfs_fd *f = vfs_get((int)(int32_t)fd);
    if (!f)
        return -LE_BADF;
    if (f->kind != VFS_BINDER)
        return -LE_NOTTY;
    return binder_ioctl(f->binder, (uint32_t)cmd, arg, (f->lflags & LO_NONBLOCK) != 0);
}

int sysfs_call(uint64_t nr, const uint64_t a[6], int64_t *ret)
{
    switch (nr) {
    case SYS_IOCTL: *ret = sys_ioctl(a[0], a[1], a[2]); return 1;
    case SYS_EVENTFD2: *ret = sys_eventfd2(a[0], a[1]); return 1;
    case SYS_EPOLL_CREATE1: *ret = sys_epoll_create1(a[0]); return 1;
    case SYS_EPOLL_CTL: *ret = poll_epoll_ctl((int)(int32_t)a[0], (int)(int32_t)a[1], (int)(int32_t)a[2], a[3]); return 1;
    case SYS_EPOLL_PWAIT: *ret = poll_epoll_wait((int)(int32_t)a[0], a[1], (int32_t)a[2], (int32_t)a[3]); return 1;
    case SYS_PPOLL: *ret = poll_ppoll(a[0], a[1], a[2], a[3], a[4]); return 1;
    case SYS_TIMERFD_CREATE: *ret = sys_timerfd_create(a[0], a[1]); return 1;
    case SYS_TIMERFD_SETTIME: *ret = sys_timerfd_settime(a[0], a[1], a[2], a[3]); return 1;
    case SYS_TIMERFD_GETTIME: *ret = sys_timerfd_gettime(a[0], a[1]); return 1;
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
    case SYS_MKDIRAT: *ret = sys_mkdirat(a[0], a[1], a[2]); return 1;
    case SYS_UNLINKAT: *ret = sys_unlinkat(a[0], a[1], a[2]); return 1;
    case SYS_FCHMODAT: *ret = sys_fchmodat(a[0], a[1], a[2]); return 1;
    case SYS_FCHMOD: *ret = vfs_fchmod((int)(int32_t)a[0], (uint32_t)a[1]); return 1;
    case 54: /* fchownat */ *ret = sys_fchownat(a[0], a[1], a[4]); return 1;
    case 55: /* fchown */ *ret = vfs_get((int)(int32_t)a[0]) ? 0 : -LE_BADF; return 1;
    case 40: /* mount */ *ret = sys_mount(a[1], a[3]); return 1;
    case 283: /* membarrier */ *ret = sys_membarrier(a[0], a[1]); return 1;
    case 97: /* unshare */ *ret = sys_unshare(a[0]); return 1;
    case 270: /* process_vm_readv */ *ret = sys_process_vm_readv(a[0], a[1], a[2], a[3], a[4], a[5]); return 1;
    case 90: /* capget */ *ret = sys_capget(a[0], a[1]); return 1;
    case 91: /* capset */ *ret = 0; return 1;
    case SYS_FACCESSAT2: *ret = sys_faccessat(a[0], a[1], a[2], a[3]); return 1;
    case SYS_GETDENTS64: *ret = sys_getdents64(a[0], a[1], a[2]); return 1;
    case SYS_FSTATFS: *ret = sys_fstatfs(a[0], a[1]); return 1;
    case SYS_STATFS: *ret = sys_statfs(a[0], a[1]); return 1;
    case SYS_FCNTL: *ret = sys_fcntl(a[0], a[1], a[2]); return 1;
    case SYS_DUP: *ret = vfs_dup((int)(int32_t)a[0], 0, 0, 0); return 1;
    case SYS_DUP3: *ret = sys_dup3(a[0], a[1], a[2]); return 1;
    case SYS_PIPE2: *ret = sys_pipe2(a[0], a[1]); return 1;
    case 198: case 199: case 200: case 201: case 202: case 203: case 204: case 205:
    case 206: case 207: case 208: case 209: case 210: case 211: case 212: case 242:
        *ret = sockcall_run(nr, a);
        return 1;
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
