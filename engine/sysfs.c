/* Linux file and directory syscalls on top of the guest VFS. Guest structures
 * are written field by field in the arm64 layouts (stat, statx, statfs,
 * dirent64); host structs are never copied to the guest. */
#include "sysfs.h"

#include <stdlib.h>
#include <string.h>

#include "binder.h"
#include "cgroupfs.h"
#include "filecache.h"
#include "cred.h"
#include "inotify.h"
#include "goldfish.h"
#include "fdobj.h"
#include "poll.h"
#include "host.h"
#include "linux.h"
#include "meta.h"
#include "mem.h"
#include "mntns.h"
#include "pipe.h"
#include "proc.h"
#include "sched.h"
#include "signals.h"
#include "syscall.h"
#include "props.h"
#include "ashmem.h"
#include "locks.h"
#include "xattr.h"
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
#define SYS_INOTIFY_INIT1 26
#define SYS_INOTIFY_ADD_WATCH 27
#define SYS_INOTIFY_RM_WATCH 28
#define SYS_TIMERFD_SETTIME 86
#define SYS_TIMERFD_GETTIME 87
#define SYS_FTRUNCATE 46
#define SYS_DUP 23
#define SYS_DUP3 24
#define SYS_PIPE2 59
#define SYS_FCNTL 25
#define SYS_MKDIRAT 34
#define SYS_UNLINKAT 35
#define SYS_RENAMEAT 38
#define SYS_RENAMEAT2 276
#define SYS_FSYNC 82
#define SYS_FDATASYNC 83
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
#define SYS_PWRITE64 68
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

/* pwrite64(fd, buf, n, off): the chunks of fs_write, at off. */
static int64_t fs_pwrite(int fd, uint64_t buf, uint64_t n, uint64_t off)
{
    uint64_t done = 0;
    do {
        size_t chunk = n - done < IO_CHUNK ? (size_t)(n - done) : IO_CHUNK;
        if (mem_copy_out(iobuf, buf + done, chunk) < 0)
            return done ? (int64_t)done : -LE_FAULT;
        int64_t w = vfs_pwrite(fd, iobuf, chunk, off + done);
        if (w < 0)
            return done ? (int64_t)done : w;
        done += (uint64_t)w;
        if ((size_t)w < chunk)
            break;
    } while (done < n);
    return (int64_t)done;
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
    if (a2 & ~7ull)
        return -LE_INVAL;
    if ((a2 & 2) && r.ro)
        return -LE_ROFS;
    if (!a2)
        return 0;
    host_stat st;
    int srv = host_stat_path(r.host, !(a3 & LAT_SYMLINK_NOFOLLOW), &st);
    if (srv)
        return srv;
    return meta_access(&st, (uint32_t)a2); /* the guest's owner and mode (meta.c), not the host's */
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
    if (r.ro)
        return -LE_ROFS;
    if (cgroupfs_under(r.guest)) /* a cgroup: mkdir makes the cgroup's directory (B11) */
        return cgroupfs_mkdir(r.guest);
    if (r.exists || r.is_link)
        return -LE_EXIST;
    if (r.syn != SYN_NONE)
        return -LE_PERM;
    rc = host_mkdir(r.host, 0755); /* the host mode is fixed; the guest mode is recorded (meta.c) */
    if (rc)
        return rc;
    return meta_new(r.host, 1, (uint32_t)mode, proc_cur()->umask);
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
    if (r.ro)
        return -LE_ROFS;
    if (cgroupfs_under(r.guest)) /* rmdir of a cgroup; a cgroup file cannot be unlinked (B11) */
        return (flags & LAT_REMOVEDIR) ? cgroupfs_rmdir(r.guest) : -LE_PERM;
    if (r.syn != SYN_NONE)
        return -LE_PERM;
    if (flags & LAT_REMOVEDIR)
        return host_rmdir(r.host);
    int urc = host_unlink(r.host);
    if (urc == 0)
        sock_fsnode_del(r.guest); /* a socket node goes with its file */
    return urc;
}

/* renameat(olddirfd, old, newdirfd, new), and renameat2 with its flags. Both names are resolved
 * without following a last link (a rename moves the link itself), and the move is a host rename.
 * The RENAME_* flags are not modelled (EINVAL for any flag). Renaming across mounts is EXDEV on
 * Linux; that check is not made (a gap, docs/boot.md, B4c part 2). */
static int64_t sys_renameat(uint64_t odfd, uint64_t a1, uint64_t ndfd, uint64_t a3, uint64_t flags)
{
    static char from[VFS_PATH_MAX], to[VFS_PATH_MAX];
    static vfs_res rf, rt;
    if (flags)
        return -LE_INVAL;
    int rc = get_path(a1, from, sizeof from);
    if (rc)
        return rc;
    rc = get_path(a3, to, sizeof to);
    if (rc)
        return rc;
    rc = vfs_resolve(dirfd_of(odfd), from, 0, &rf);
    if (rc)
        return rc;
    rc = vfs_resolve(dirfd_of(ndfd), to, 0, &rt);
    if (rc)
        return rc;
    if (!rf.exists && !rf.is_link)
        return -LE_NOENT;
    if (rf.ro || rt.ro)
        return -LE_ROFS;
    if (rf.syn != SYN_NONE || rt.syn != SYN_NONE)
        return -LE_PERM;
    return host_rename(rf.host, rt.host);
}

/* linkat (37): a hard link to a file that is not a directory. The new name must not exist. The record of the
 * file (meta.c) is the same for every name, since it is keyed by the host inode. AT_SYMLINK_FOLLOW (0x400) follows
 * a final symlink of the old name; without it the link itself is linked. */
static int64_t sys_linkat(uint64_t odfd, uint64_t a1, uint64_t ndfd, uint64_t a3, uint64_t flags)
{
    static char from[VFS_PATH_MAX], to[VFS_PATH_MAX];
    static vfs_res rf, rt;
    if (flags & ~(uint64_t)0x400)
        return -LE_INVAL;
    int rc = get_path(a1, from, sizeof from);
    if (rc)
        return rc;
    rc = get_path(a3, to, sizeof to);
    if (rc)
        return rc;
    rc = vfs_resolve(dirfd_of(odfd), from, (flags & 0x400) != 0, &rf);
    if (rc)
        return rc;
    rc = vfs_resolve(dirfd_of(ndfd), to, 0, &rt);
    if (rc)
        return rc;
    if (!rf.exists && !rf.is_link)
        return -LE_NOENT;
    if (rt.exists || rt.is_link)
        return -LE_EXIST;
    if (rf.ro || rt.ro)
        return -LE_ROFS;
    if (rf.syn != SYN_NONE || rt.syn != SYN_NONE)
        return -LE_PERM;
    host_stat st;
    if (host_stat_path(rf.host, 0, &st) == 0 && (st.mode & LS_IFMT) == LS_IFDIR)
        return -LE_PERM;
    return host_link(rf.host, rt.host);
}

/* symlinkat(target, newdirfd, linkpath): a host symbolic link with the target as the guest wrote it. The engine
 * resolves an absolute target inside the guest root when a path walks through the link. No owner record is kept
 * for the link (sys_linkat keeps none either). */
static int64_t sys_symlinkat(uint64_t a0, uint64_t ndfd, uint64_t a2)
{
    static char target[VFS_PATH_MAX], to[VFS_PATH_MAX];
    static vfs_res rt;
    int rc = get_path(a0, target, sizeof target);
    if (rc)
        return rc;
    rc = get_path(a2, to, sizeof to);
    if (rc)
        return rc;
    rc = vfs_resolve(dirfd_of(ndfd), to, 0, &rt);
    if (rc)
        return rc;
    if (rt.exists || rt.is_link)
        return -LE_EXIST;
    if (rt.ro)
        return -LE_ROFS;
    if (rt.syn != SYN_NONE)
        return -LE_PERM;
    return host_symlink(target, rt.host);
}

/* fsync and fdatasync: a host file is synced; a memfd or a directory has nothing to sync (0); other
 * descriptors (pipes, sockets, eventfds, epoll) are EINVAL, as on Linux. */
static int64_t sys_fsync(uint64_t fd)
{
    vfs_fd *f = vfs_get((int)(int32_t)fd);
    if (!f)
        return -LE_BADF;
    if (f->kind == VFS_FILE && f->hfd >= 0) {
        if (f->has_id)
            fc_flush_file(f->hdev, f->hino);
        return host_fsync(f->hfd);
    }
    if (f->kind == VFS_MEMFD || f->kind == VFS_DIR)
        return 0;
    return -LE_INVAL;
}

/* fchownat (54): the owner and group go to the guest metadata (meta.c), with the Linux permission rules. -1 for an
 * id changes nothing. AT_SYMLINK_NOFOLLOW (0x100) changes a final link itself. A synthetic file takes no record and
 * the call is accepted. (capget and capset are in cred.c.) */
/* utimensat(dirfd, path, times, flags) (88), and futimens (path NULL). The checks come in Linux's order
 * (fs/utimes.c): the flags, then both OMIT (a no-op that does not look at the path), then the path (ENOENT), then
 * the times (EINVAL), the read-only mount (EROFS) and the permission (meta_utime_check). Synthetic files, pipes and
 * memfds keep no times, so the call succeeds for them. */
#define UT_NOW ((int64_t)((1ll << 30) - 1))
#define UT_OMIT ((int64_t)((1ll << 30) - 2))
#define AT_SYMLINK_NOFOLLOW_ 0x100
#define AT_EMPTY_PATH_ 0x1000

static int64_t sys_utimensat(uint64_t a0, uint64_t a1, uint64_t times, uint64_t flags)
{
    static char path[VFS_PATH_MAX];
    static vfs_res r;
    if (flags & ~(uint64_t)(AT_SYMLINK_NOFOLLOW_ | AT_EMPTY_PATH_))
        return -LE_INVAL;
    host_time_t t[2] = { { 0, 0, 1, 0 }, { 0, 0, 1, 0 } }; /* NULL times: both now */
    int explicit_times = 0, bad_nsec = 0;
    if (times) {
        int64_t tv[4];
        if (mem_copy_out(tv, times, sizeof tv) < 0)
            return -LE_FAULT;
        for (int i = 0; i < 2; i++) {
            int64_t ns = tv[2 * i + 1];
            t[i].now = t[i].omit = 0;
            if (ns == UT_OMIT) {
                t[i].omit = 1;
            } else if (ns == UT_NOW) {
                t[i].now = 1;
            } else if (ns < 0 || ns >= 1000000000ll) {
                bad_nsec = 1;
            } else {
                t[i].sec = tv[2 * i];
                t[i].nsec = ns;
                explicit_times = 1;
            }
        }
        if (t[0].omit && t[1].omit)
            return 0;
        if (t[0].now && t[1].now) { /* vfs_utimes: both now is the NULL form */
            t[0].now = t[1].now = 1;
        } else {
            explicit_times = 1; /* ATTR_TIMES_SET for any non-NULL time pair (OMIT and NOW included) */
        }
    }
    int follow = (flags & AT_SYMLINK_NOFOLLOW_) == 0;
    int rc, fdform = a1 == 0;
    if (!fdform) {
        rc = get_path(a1, path, sizeof path);
        if (rc)
            return rc;
        fdform = path[0] == 0 && (flags & AT_EMPTY_PATH_) != 0;
    }
    if (fdform) { /* futimens, or an empty path with AT_EMPTY_PATH: the descriptor dirfd */
        if (a1 == 0 && dirfd_of(a0) == -100) /* AT_FDCWD */
            return -LE_FAULT;
        if (!follow)
            return -LE_INVAL;
        vfs_fd *f = vfs_get(dirfd_of(a0));
        if (!f)
            return -LE_BADF;
        if (bad_nsec)
            return -LE_INVAL;
        if (f->kind != VFS_FILE || !f->has_id)
            return 0;
        host_stat st;
        rc = vfs_stat_fd(dirfd_of(a0), &st);
        if (rc)
            return rc;
        rc = meta_utime_check(&st, explicit_times);
        if (rc)
            return rc;
        return host_utimens(NULL, f->hfd, 1, t);
    }
    rc = vfs_resolve(dirfd_of(a0), path, follow, &r);
    if (rc)
        return rc;
    if (!r.exists && !r.is_link)
        return -LE_NOENT;
    if (bad_nsec)
        return -LE_INVAL;
    if (r.ro)
        return -LE_ROFS;
    if (r.syn != SYN_NONE)
        return 0;
    host_stat st;
    rc = host_stat_path(r.host, follow, &st);
    if (rc)
        return rc;
    rc = meta_utime_check(&st, explicit_times);
    if (rc)
        return rc;
    return host_utimens(r.host, -1, follow, t);
}

static int64_t sys_fchownat(uint64_t a0, uint64_t a1, uint64_t uid, uint64_t gid, uint64_t flags)
{
    static char path[VFS_PATH_MAX];
    static vfs_res r;
    int rc = get_path(a1, path, sizeof path);
    if (rc)
        return rc;
    rc = vfs_resolve(dirfd_of(a0), path, (flags & 0x100) == 0, &r);
    if (rc)
        return rc;
    if (!r.exists && !r.is_link)
        return -LE_NOENT;
    if (r.syn != SYN_NONE)
        return 0;
    if (r.ro)
        return -LE_ROFS;
    host_stat st;
    rc = host_stat_path(r.host, 0, &st);
    if (rc)
        return rc;
    return meta_chown(&st, (uint32_t)uid, (uint32_t)gid);
}

/* mount(source, target, type, flags, data) and umount2(target, flags), in the process's mount
 * namespace (mntns.c). Mounting needs an effective uid of 0, as Linux's CAP_SYS_ADMIN check does
 * for a root process. The data argument is not used. A string argument that is NULL is absent. */
static int mount_str(uint64_t va, char *buf, size_t cap, int *present)
{
    *present = va != 0;
    if (!va)
        return 0;
    return get_path(va, buf, cap);
}

static int64_t sys_mount(uint64_t src_va, uint64_t tgt_va, uint64_t type_va, uint64_t flags)
{
    static char src[VFS_PATH_MAX], tgt[VFS_PATH_MAX], type[64];
    if (!cred_has_cap(proc_cur(), LCAP_SYS_ADMIN))
        return -LE_PERM;
    int hsrc, htype, rc;
    rc = mount_str(src_va, src, sizeof src, &hsrc);
    if (rc)
        return rc;
    rc = mount_str(type_va, type, sizeof type, &htype);
    if (rc)
        return rc;
    if (!tgt_va)
        return -LE_FAULT;
    rc = get_path(tgt_va, tgt, sizeof tgt);
    if (rc)
        return rc;
    return mntns_mount(vfs_ns(), hsrc ? src : NULL, tgt, htype ? type : NULL, flags);
}

static int64_t sys_umount2(uint64_t tgt_va, uint64_t flags)
{
    static char tgt[VFS_PATH_MAX];
    if (!cred_has_cap(proc_cur(), LCAP_SYS_ADMIN))
        return -LE_PERM;
    int rc = get_path(tgt_va, tgt, sizeof tgt);
    if (rc)
        return rc;
    return mntns_umount(vfs_ns(), tgt, flags);
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

/* unshare(flags): CLONE_NEWNS gives the process a copy of its mount namespace (the mounts made
 * after this are its own). Other namespace flags, and CLONE_FILES (which would need a copy of the
 * descriptors), are EINVAL. Needs an effective uid of 0. */
static int64_t sys_unshare(uint64_t flags)
{
    const uint64_t CLONE_NEWNS_FLAG = 0x00020000ull;
    if (flags & ~CLONE_NEWNS_FLAG)
        return -LE_INVAL;
    if (!(flags & CLONE_NEWNS_FLAG))
        return 0;
    if (!cred_has_cap(proc_cur(), LCAP_SYS_ADMIN))
        return -LE_PERM;
    vfs_fdtab_copy_ns(proc_cur()->fds);
    return 0;
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
    if (cgroupfs_under(r.guest)) /* cgroup modes are not kept: the call succeeds (B11) */
        return 0;
    if (r.syn != SYN_NONE)
        return -LE_PERM;
    if (r.ro)
        return -LE_ROFS;
    host_stat st;
    rc = host_stat_path(r.host, 1, &st);
    if (rc)
        return rc;
    return meta_chmod(&st, (uint32_t)mode);
}

/* Extended attributes (xattr.h, B7). A path form names the file by its resolved guest path; an fd form by the
 * descriptor's guest path. Synthetic files take no attributes: a set or remove is EPERM, a get is ENODATA. A
 * read-only mount refuses a set or remove with EROFS. Syscalls 5 to 16 (arm64). */
enum { XA_SET, XA_GET, XA_LIST, XA_REMOVE };

static int64_t xattr_do(const char *file, int ro, int syn, int op, uint64_t name_va, uint64_t buf_va,
                        uint64_t size, int flags)
{
    static char name[256];
    if (op != XA_LIST) {
        int rc = get_path(name_va, name, sizeof name);
        if (rc)
            return rc;
    }
    if ((op == XA_SET || op == XA_REMOVE) && syn)
        return -LE_PERM;
    if ((op == XA_SET || op == XA_REMOVE) && ro)
        return -LE_ROFS;
    if (op == XA_GET && syn)
        return -LE_NODATA;
    if (op == XA_LIST && syn)
        return 0;
    switch (op) {
    case XA_SET: {
        if (size > XATTR_VALUE_MAX)
            return -LE_2BIG;
        uint8_t *v = malloc(size ? (size_t)size : 1);
        if (!v)
            return -LE_NOMEM;
        int64_t rc = size && mem_copy_out(v, buf_va, (size_t)size) < 0 ? -LE_FAULT : xattr_set(file, name, v, (size_t)size, flags);
        free(v);
        return rc;
    }
    case XA_GET: {
        if (size == 0)
            return xattr_get(file, name, NULL, 0);
        uint8_t *v = malloc((size_t)size);
        if (!v)
            return -LE_NOMEM;
        int64_t n = xattr_get(file, name, v, (size_t)size);
        if (n > 0 && mem_copy_in(buf_va, v, (size_t)n) < 0)
            n = -LE_FAULT;
        free(v);
        return n;
    }
    case XA_LIST: {
        if (size == 0)
            return xattr_list(file, NULL, 0);
        uint8_t *v = malloc((size_t)size);
        if (!v)
            return -LE_NOMEM;
        int64_t n = xattr_list(file, v, (size_t)size);
        if (n > 0 && mem_copy_in(buf_va, v, (size_t)n) < 0)
            n = -LE_FAULT;
        free(v);
        return n;
    }
    default:
        return xattr_remove(file, name);
    }
}

static int64_t xattr_path_call(uint64_t path_va, int follow, int op, uint64_t name_va, uint64_t buf_va,
                               uint64_t size, int flags)
{
    static char path[VFS_PATH_MAX];
    static vfs_res r;
    int rc = get_path(path_va, path, sizeof path);
    if (rc)
        return rc;
    rc = vfs_resolve(LAT_FDCWD, path, follow, &r);
    if (rc)
        return rc;
    if (!r.exists)
        return -LE_NOENT;
    return xattr_do(r.guest, r.ro, r.syn != SYN_NONE, op, name_va, buf_va, size, flags);
}

static int64_t xattr_fd_call(uint64_t fd, int op, uint64_t name_va, uint64_t buf_va, uint64_t size, int flags)
{
    vfs_fd *f = vfs_get((int)(int32_t)fd);
    if (!f)
        return -LE_BADF;
    return xattr_do(f->guest ? f->guest : "", 0, f->kind == VFS_SYN, op, name_va, buf_va, size, flags);
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

/* fcntl record locks (F_GETLK 5, F_SETLK 6, F_SETLKW 7) and flock(2) (LOCK_*), on the guest's
 * struct flock (arm64: 32 bytes, l_start and l_len at 8 and 16, l_pid at 24). locks.c does the work. */
#define LF_GETLK 5
#define LF_SETLK 6
#define LF_SETLKW 7
#define LF_RDLCK 0
#define LF_WRLCK 1
#define LF_UNLCK 2
#define LF_OFFSET_MAX INT64_MAX

typedef struct {
    int16_t l_type, l_whence;
    int64_t l_start, l_len;
    int32_t l_pid;
} guest_flock_t;
_Static_assert(sizeof(guest_flock_t) == 32, "struct flock is 32 bytes on arm64");

static int fd_readable(const vfs_fd *f)
{
    return !(f->lflags & LO_PATH) && (f->lflags & LO_ACCMODE) != LO_WRONLY;
}

static int fd_writable(const vfs_fd *f)
{
    return !(f->lflags & LO_PATH) && (f->lflags & LO_ACCMODE) != LO_RDONLY;
}

/* flock_to_posix_lock: the range of a struct flock, from its whence (SEEK_SET, SEEK_CUR, SEEK_END) and
 * its start and length (a negative length counts backwards, 0 runs to the end of the file). */
static int lk_range(int ifd, const guest_flock_t *l, int64_t *start, int64_t *end)
{
    int64_t base;
    switch (l->l_whence) {
    case 0: /* SEEK_SET */
        base = 0;
        break;
    case 1: { /* SEEK_CUR: a pipe has no offset */
        int64_t p = vfs_lseek(ifd, 0, 1);
        base = p < 0 ? 0 : p;
        break;
    }
    case 2: { /* SEEK_END */
        host_stat st;
        if (vfs_stat_fd(ifd, &st) < 0)
            return -LE_BADF;
        base = (int64_t)st.size;
        break;
    }
    default:
        return -LE_INVAL;
    }
    if (l->l_start > LF_OFFSET_MAX - base)
        return -LE_OVERFLOW;
    int64_t s = base + l->l_start;
    if (s < 0)
        return -LE_INVAL;
    if (l->l_len > 0) {
        if (l->l_len - 1 > LF_OFFSET_MAX - s)
            return -LE_OVERFLOW;
        *end = s + (l->l_len - 1);
    } else if (l->l_len < 0) {
        if (s + l->l_len < 0)
            return -LE_INVAL;
        *end = s - 1;
        s += l->l_len;
    } else {
        *end = LF_OFFSET_MAX;
    }
    *start = s;
    return 0;
}

static int64_t sys_fcntl_lock(int ifd, int cmd, uint64_t arg)
{
    vfs_fd *f = vfs_get(ifd);
    guest_flock_t l;
    if (mem_copy_out(&l, arg, sizeof l) < 0)
        return -LE_FAULT;
    int type = l.l_type;
    /* F_GETLK accepts only the two lock types, and checks them before the range (fcntl_getlk). */
    if (cmd == LF_GETLK && type != LF_RDLCK && type != LF_WRLCK)
        return -LE_INVAL;
    int64_t start = 0, end = 0;
    int rc = lk_range(ifd, &l, &start, &end);
    if (rc < 0)
        return rc;
    if (type != LF_RDLCK && type != LF_WRLCK && type != LF_UNLCK)
        return -LE_INVAL;
    if (cmd != LF_GETLK) /* check_fmode_for_setlk */
        if ((type == LF_RDLCK && !fd_readable(f)) || (type == LF_WRLCK && !fd_writable(f)))
            return -LE_BADF;

    void *owner = vfs_owner();
    if (cmd == LF_GETLK) {
        lk_conflict_t cf;
        rc = lk_posix_test(owner, f, type, start, end, &cf);
        if (rc < 0)
            return rc;
        if (rc == 1) { /* posix_lock_to_flock: the conflicting lock's range and its holder's pid */
            l.l_type = (int16_t)cf.type;
            l.l_whence = 0;
            l.l_start = cf.start;
            l.l_len = cf.end == LF_OFFSET_MAX ? 0 : cf.end - cf.start + 1;
            l.l_pid = cf.pid;
        } else {
            l.l_type = LF_UNLCK; /* the other fields stay as the caller gave them */
        }
        if (mem_copy_in(arg, &l, sizeof l) < 0)
            return -LE_FAULT;
        return 0;
    }
    rc = lk_posix_set(owner, proc_cur()->pid, f, type, start, end, cmd == LF_SETLKW);
    if (rc == LK_BLOCKED)
        return 0;
    return rc;
}

/* flock(2): LOCK_SH, LOCK_EX, LOCK_UN, and LOCK_NB (no wait). LOCK_MAND is ignored, as on Linux. */
#define LF_LOCK_SH 1
#define LF_LOCK_EX 2
#define LF_LOCK_NB 4
#define LF_LOCK_UN 8
#define LF_LOCK_MAND 32

int64_t sysfs_flock(uint64_t fd, uint64_t cmd)
{
    if (cmd & LF_LOCK_MAND)
        return 0;
    int type;
    switch (cmd & ~(uint64_t)LF_LOCK_NB) {
    case LF_LOCK_SH: type = LK_RD; break;
    case LF_LOCK_EX: type = LK_WR; break;
    case LF_LOCK_UN: type = LK_UN; break;
    default: return -LE_INVAL;
    }
    vfs_fd *f = vfs_get((int)(int32_t)fd);
    if (!f)
        return -LE_BADF;
    if (type != LK_UN && !fd_readable(f) && !fd_writable(f))
        return -LE_BADF;
    int rc = lk_flock(f, type, !(cmd & LF_LOCK_NB));
    if (rc == LK_BLOCKED)
        return 0;
    return rc;
}

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
    case LF_GETLK:
    case LF_SETLK:
    case LF_SETLKW:
        return sys_fcntl_lock(ifd, (int)cmd, arg);
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

/* ftruncate(fd, len) on a memfd, or on a host regular file opened for writing (SQLite truncates its
 * journal this way). Other kinds are not resized (EINVAL), as is a read-only descriptor. */
static int64_t sys_ftruncate(uint64_t fd, uint64_t len)
{
    vfs_fd *f = vfs_get((int)(int32_t)fd);
    if (!f)
        return -LE_BADF;
    if (f->kind == VFS_FILE && f->hfd >= 0) {
        if ((f->lflags & LO_ACCMODE) == LO_RDONLY || len > (1ull << 40))
            return -LE_INVAL;
        int64_t rc = host_ftruncate(f->hfd, len);
        if (rc == 0 && f->has_id)
            fc_truncate(f->hdev, f->hino, len);
        return rc;
    }
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

/* inotify (B9, inotify.c): the instance and its watches. The instance is read-only; no event is queued. */
static int64_t sys_inotify_init1(uint64_t flags)
{
    if (flags & ~(uint64_t)(LO_NONBLOCK | LO_CLOEXEC))
        return -LE_INVAL;
    void *o = ino_new();
    if (!o)
        return -LE_NOMEM;
    return vfs_obj_new(VFS_INOTIFY, o, (flags & LO_CLOEXEC) != 0, LO_RDONLY | (uint32_t)(flags & LO_NONBLOCK));
}

/* inotify_add_watch(fd, path, mask): the path must exist (ENOENT), and the mask must name an event (EINVAL). */
static int64_t sys_inotify_add_watch(uint64_t fd, uint64_t path_va, uint64_t mask)
{
    vfs_fd *f = vfs_get((int)(int32_t)fd);
    if (!f)
        return -LE_BADF;
    if (f->kind != VFS_INOTIFY)
        return -LE_INVAL;
    char path[VFS_PATH_MAX];
    int rc = get_path(path_va, path, sizeof path);
    if (rc)
        return rc;
    if ((mask & 0xfffu) == 0)
        return -LE_INVAL;
    vfs_res r;
    rc = vfs_resolve(LAT_FDCWD, path, 1, &r);
    if (rc < 0)
        return rc;
    if (!r.exists)
        return -LE_NOENT;
    int wd = ino_add(f->obj, r.guest, (uint32_t)mask);
    return wd < 0 ? -LE_NOSPC : wd;
}

static int64_t sys_inotify_rm_watch(uint64_t fd, uint64_t wd)
{
    vfs_fd *f = vfs_get((int)(int32_t)fd);
    if (!f)
        return -LE_BADF;
    if (f->kind != VFS_INOTIFY)
        return -LE_INVAL;
    return ino_rm(f->obj, (int)(int32_t)wd) < 0 ? -LE_INVAL : 0;
}

/* timerfd_create(clockid, flags). The clocks are CLOCK_REALTIME (0), CLOCK_MONOTONIC (1), CLOCK_BOOTTIME (7) and
 * the two alarm clocks (8 and 9, which need CAP_WAKE_ALARM: EPERM without it, as Linux). The engine does not
 * suspend, so the alarm clocks are the realtime and boottime clocks (fdobj.c converts the realtime deadlines). */
static int64_t sys_timerfd_create(uint64_t clockid, uint64_t flags)
{
    if (clockid != 0 && clockid != 1 && clockid != 7 && clockid != 8 && clockid != 9)
        return -LE_INVAL;
    if ((clockid == 8 || clockid == 9) && !cred_has_cap(proc_cur(), LCAP_WAKE_ALARM))
        return -LE_PERM;
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
    /* TFD_TIMER_CANCEL_ON_SET (2): Linux allows it with ABSTIME on CLOCK_REALTIME only. The engine never sets
     * the realtime clock (no settimeofday), so the cancel is never due (B9). */
    if (flags & ~(uint64_t)(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET))
        return -LE_INVAL;
    if ((flags & TFD_TIMER_CANCEL_ON_SET) && (!(flags & TFD_TIMER_ABSTIME) || tfd_clockid(f->obj) != 0))
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
    if (f->kind == VFS_GOLDFISH)
        return goldfish_ioctl(f->obj, (uint32_t)cmd, arg); /* the address space (goldfish.c) */
    if (f->kind == VFS_MEMFD && f->ashmem)
        return ashmem_ioctl(f, (uint32_t)cmd, arg); /* /dev/ashmem (ashmem.c) */
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
    case SYS_INOTIFY_INIT1: *ret = sys_inotify_init1(a[0]); return 1;
    case SYS_INOTIFY_ADD_WATCH: *ret = sys_inotify_add_watch(a[0], a[1], a[2]); return 1;
    case SYS_INOTIFY_RM_WATCH: *ret = sys_inotify_rm_watch(a[0], a[1]); return 1;
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
    case SYS_PWRITE64: *ret = fs_pwrite((int)(int32_t)a[0], a[1], a[2], a[3]); return 1;
    case SYS_LSEEK: *ret = vfs_lseek((int)(int32_t)a[0], (int64_t)a[1], (int)(int32_t)a[2]); return 1;
    case SYS_FSTAT: *ret = sys_fstat(a[0], a[1]); return 1;
    case SYS_NEWFSTATAT: *ret = sys_newfstatat(a[0], a[1], a[2], a[3]); return 1;
    case SYS_STATX: *ret = sys_statx(a[0], a[1], a[2], a[3], a[4]); return 1;
    case SYS_READLINKAT: *ret = sys_readlinkat(a[0], a[1], a[2], a[3]); return 1;
    case SYS_FACCESSAT: *ret = sys_faccessat(a[0], a[1], a[2], 0); return 1;
    case SYS_MKDIRAT: *ret = sys_mkdirat(a[0], a[1], a[2]); return 1;
    case SYS_UNLINKAT: *ret = sys_unlinkat(a[0], a[1], a[2]); return 1;
    case SYS_RENAMEAT: *ret = sys_renameat(a[0], a[1], a[2], a[3], 0); return 1;
    case 37: /* linkat */ *ret = sys_linkat(a[0], a[1], a[2], a[3], a[4]); return 1;
    case 36: /* symlinkat */ *ret = sys_symlinkat(a[0], a[1], a[2]); return 1;
    case SYS_RENAMEAT2: *ret = sys_renameat(a[0], a[1], a[2], a[3], a[4]); return 1;
    case SYS_FSYNC:
    case SYS_FDATASYNC: *ret = sys_fsync(a[0]); return 1;
    case SYS_FCHMODAT: *ret = sys_fchmodat(a[0], a[1], a[2]); return 1;
    case 88: /* utimensat, and futimens (path NULL) */ *ret = sys_utimensat(a[0], a[1], a[2], a[3]); return 1;
    case SYS_FCHMOD: *ret = vfs_fchmod((int)(int32_t)a[0], (uint32_t)a[1]); return 1;
    /* Extended attributes (xattr_do above): the l forms do not follow a final link. */
    case 5: /* setxattr */ *ret = xattr_path_call(a[0], 1, XA_SET, a[1], a[2], a[3], (int)a[4]); return 1;
    case 6: /* lsetxattr */ *ret = xattr_path_call(a[0], 0, XA_SET, a[1], a[2], a[3], (int)a[4]); return 1;
    case 7: /* fsetxattr */ *ret = xattr_fd_call(a[0], XA_SET, a[1], a[2], a[3], (int)a[4]); return 1;
    case 8: /* getxattr */ *ret = xattr_path_call(a[0], 1, XA_GET, a[1], a[2], a[3], 0); return 1;
    case 9: /* lgetxattr */ *ret = xattr_path_call(a[0], 0, XA_GET, a[1], a[2], a[3], 0); return 1;
    case 10: /* fgetxattr */ *ret = xattr_fd_call(a[0], XA_GET, a[1], a[2], a[3], 0); return 1;
    case 11: /* listxattr */ *ret = xattr_path_call(a[0], 1, XA_LIST, 0, a[1], a[2], 0); return 1;
    case 12: /* llistxattr */ *ret = xattr_path_call(a[0], 0, XA_LIST, 0, a[1], a[2], 0); return 1;
    case 13: /* flistxattr */ *ret = xattr_fd_call(a[0], XA_LIST, 0, a[1], a[2], 0); return 1;
    case 14: /* removexattr */ *ret = xattr_path_call(a[0], 1, XA_REMOVE, a[1], 0, 0, 0); return 1;
    case 15: /* lremovexattr */ *ret = xattr_path_call(a[0], 0, XA_REMOVE, a[1], 0, 0, 0); return 1;
    case 16: /* fremovexattr */ *ret = xattr_fd_call(a[0], XA_REMOVE, a[1], 0, 0, 0); return 1;
    case 54: /* fchownat */ *ret = sys_fchownat(a[0], a[1], a[2], a[3], a[4]); return 1;
    case 55: /* fchown */ *ret = vfs_fchown((int)(int32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]); return 1;
    case 40: /* mount */ *ret = sys_mount(a[0], a[1], a[2], a[3]); return 1;
    case 39: /* umount2 */ *ret = sys_umount2(a[0], a[1]); return 1;
    case 283: /* membarrier */ *ret = sys_membarrier(a[0], a[1]); return 1;
    case 97: /* unshare */ *ret = sys_unshare(a[0]); return 1;
    case 270: /* process_vm_readv */ *ret = sys_process_vm_readv(a[0], a[1], a[2], a[3], a[4], a[5]); return 1;
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
