/* Linux arm64 syscalls. Numbers are from asm-generic/unistd.h (checked against
 * the builder VM's headers). File and directory calls live in sysfs.c. */
#include "syscall.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "binder.h"
#include "goldfish.h"
#include "exec.h"
#include "host.h"
#include "linux.h"
#include "mem.h"
#include "cred.h"
#include "proc.h"
#include "prof.h"
#include "props.h"
#include "sched.h"
#include "signals.h"
#include "sysfs.h"
#include "vfs.h"

#define SYS_IOCTL 29
#define SYS_EXIT 93
#define SYS_EXIT_GROUP 94
#define SYS_SET_TID_ADDRESS 96
#define SYS_FUTEX 98
#define SYS_SET_ROBUST_LIST 99
#define SYS_NANOSLEEP 101
#define SYS_SCHED_SETSCHEDULER 119
#define SYS_SCHED_GETSCHEDULER 120
#define SYS_SCHED_GETPARAM 121
#define SYS_SCHED_YIELD 124
#define SYS_SCHED_GETAFFINITY 123
#define SYS_KILL 129
#define SYS_TKILL 130
#define SYS_TGKILL 131
#define SYS_SIGALTSTACK 132
#define SYS_RT_SIGACTION 134
#define SYS_RT_SIGPROCMASK 135
#define SYS_RT_SIGTIMEDWAIT 137
#define SYS_RT_SIGQUEUEINFO 138
#define SYS_RT_SIGRETURN 139
#define SYS_RT_TGSIGQUEUEINFO 240
#define SYS_SETPRIORITY 140
#define SYS_GETPRIORITY 141
#define SYS_GETPGID 155
#define SYS_SETPGID 154
#define SYS_GETSID 156
#define SYS_SETSID 157
#define SYS_WAITID 95
#define SYS_WAIT4 260
#define SYS_RT_SIGSUSPEND 133
#define SYS_UNAME 160
#define SYS_PRCTL 167
#define SYS_GETPPID 173
#define SYS_GETPID 172
#define SYS_GETUID 174
#define SYS_GETEUID 175
#define SYS_GETGID 176
#define SYS_GETEGID 177
#define SYS_GETTID 178
#define SYS_CLOCK_GETTIME 113
#define SYS_CLOCK_NANOSLEEP 115
#define SYS_GETTIMEOFDAY 169
#define SYS_CLONE 220
#define SYS_EXECVE 221
#define SYS_EXECVEAT 281
#define SYS_GETRUSAGE 165
#define SYS_BRK 214
#define SYS_MUNMAP 215
#define SYS_MREMAP 216
#define SYS_MMAP 222
#define SYS_MPROTECT 226
#define SYS_MSYNC 227
#define SYS_MLOCK 228
#define SYS_MUNLOCK 229
#define SYS_MADVISE 233
#define SYS_PRLIMIT64 261
#define SYS_GETRANDOM 278
#define SYS_RSEQ 293
#define SYS_SOCKET 198
#define SYS_FLOCK 32
#define SYS_GETRLIMIT 163
#define SYS_SETRLIMIT 164

#define PAGE_MASK 4095ull
#define SIGSET_SIZE 8
#define IO_CHUNK 65536

static uint64_t unimpl_seen[1024 / 64];
static uint8_t fill_buf[IO_CHUNK];

static uint64_t page_up(uint64_t x) { return (x + PAGE_MASK) & ~PAGE_MASK; }

const char *syscall_name(uint64_t nr)
{
    switch (nr) {
    case 17: return "getcwd";
    case 23: return "dup";
    case 24: return "dup3";
    case 25: return "fcntl";
    case 29: return "ioctl";
    case 43: return "statfs";
    case 44: return "fstatfs";
    case 46: return "ftruncate";
    case 48: return "faccessat";
    case 49: return "chdir";
    case 50: return "fchdir";
    case 56: return "openat";
    case 57: return "close";
    case 61: return "getdents64";
    case 62: return "lseek";
    case 63: return "read";
    case 64: return "write";
    case 65: return "readv";
    case 66: return "writev";
    case 198: return "socket";
    case 199: return "socketpair";
    case 200: return "bind";
    case 201: return "listen";
    case 202: return "accept";
    case 203: return "connect";
    case 204: return "getsockname";
    case 205: return "getpeername";
    case 206: return "sendto";
    case 207: return "recvfrom";
    case 208: return "setsockopt";
    case 209: return "getsockopt";
    case 210: return "shutdown";
    case 211: return "sendmsg";
    case 212: return "recvmsg";
    case 242: return "accept4";
    case 67: return "pread64";
    case 68: return "pwrite64";
    case 78: return "readlinkat";
    case 79: return "newfstatat";
    case 80: return "fstat";
    case 93: return "exit";
    case 94: return "exit_group";
    case 96: return "set_tid_address";
    case 98: return "futex";
    case 99: return "set_robust_list";
    case 101: return "nanosleep";
    case 113: return "clock_gettime";
    case 169: return "gettimeofday";
    case 120: return "sched_getscheduler";
    case 123: return "sched_getaffinity";
    case 124: return "sched_yield";
    case 129: return "kill";
    case 130: return "tkill";
    case 131: return "tgkill";
    case 132: return "sigaltstack";
    case 134: return "rt_sigaction";
    case 135: return "rt_sigprocmask";
    case 137: return "rt_sigtimedwait";
    case 138: return "rt_sigqueueinfo";
    case 240: return "rt_tgsigqueueinfo";
    case 139: return "rt_sigreturn";
    case 140: return "setpriority";
    case 141: return "getpriority";
    case 155: return "getpgid";
    case 160: return "uname";
    case 165: return "getrusage";
    case 166: return "umask";
    case 167: return "prctl";
    case 172: return "getpid";
    case 173: return "getppid";
    case 174: return "getuid";
    case 175: return "geteuid";
    case 176: return "getgid";
    case 177: return "getegid";
    case 178: return "gettid";
    case 214: return "brk";
    case 221: return "execve";
    case 281: return "execveat";
    case 215: return "munmap";
    case 216: return "mremap";
    case 222: return "mmap";
    case 226: return "mprotect";
    case 227: return "msync";
    case 228: return "mlock";
    case 229: return "munlock";
    case 233: return "madvise";
    case 261: return "prlimit64";
    case 278: return "getrandom";
    case 279: return "memfd_create";
    case 291: return "statx";
    case 293: return "rseq";
    case 439: return "faccessat2";
    default: return "?";
    }
}

void syscall_init(uint64_t base)
{
    proc_cur()->brk_base = base;
    proc_cur()->brk_cur = base;
}

void syscall_set_comm(const char *argv0)
{
    const char *base = strrchr(argv0, '/');
    sched_set_name(base ? base + 1 : argv0);
}

/* ---------- memory ---------- */

static int64_t sys_brk(uint64_t want)
{
    if (want == 0 || want < proc_cur()->brk_base || want >= MEM_VA_LIMIT)
        return (int64_t)proc_cur()->brk_cur;
    uint64_t cur_top = page_up(proc_cur()->brk_cur), new_top = page_up(want);
    if (new_top > cur_top) {
        if (!mem_range_free(cur_top, new_top - cur_top))
            return (int64_t)proc_cur()->brk_cur;
        if (mem_map(cur_top, new_top - cur_top, MEM_PROT_R | MEM_PROT_W) < 0)
            return (int64_t)proc_cur()->brk_cur;
    } else if (new_top < cur_top) {
        mem_unmap(new_top, cur_top - new_top);
    }
    proc_cur()->brk_cur = want;
    return (int64_t)want;
}

/* Copies n bytes from a host fd into guest memory at va, from file offset off.
 * Bytes past the end of the file stay zero (the pages start zeroed). */
static int fill_from_fd(int hfd, uint64_t va, uint64_t n, uint64_t off)
{
    uint64_t done = 0;
    while (done < n) {
        size_t want = n - done < IO_CHUNK ? (size_t)(n - done) : IO_CHUNK;
        int64_t r = host_pread(hfd, fill_buf, want, off + done);
        if (r < 0)
            return -1;
        if (r == 0)
            break;
        if (mem_poke(va + done, fill_buf, (size_t)r) < 0)
            return -1;
        done += (uint64_t)r;
    }
    return 0;
}

/* Maps len bytes of file path (open as hfd) at base, with the final protection. */
static int map_file(uint64_t base, uint64_t len, unsigned prot, const char *path, int hfd, uint64_t off)
{
    if (mem_map_ex(base, len, prot, path, off) < 0)
        return -1;
    return fill_from_fd(hfd, base, len, off);
}

/* Maps a tail of len bytes after a mapping whose last page is last, with the
 * protection and file continuation of that page. */
static int map_tail(uint64_t va, uint64_t len, uint64_t last)
{
    unsigned prot = mem_prot_at(last);
    uint64_t s, e, off;
    const char *path;
    uint64_t shm_start;
    memfd_t *shm = mem_region_memfd(last, &shm_start, &off);
    if (shm) /* the tail continues the same memfd view */
        return mem_map_shared(va, len, prot, shm, off + (last - shm_start) + MEM_PAGE_SIZE) < 0 ? -1 : 0;
    if (mem_region_next(last, &s, &e, &path, &off) && path && s <= last) {
        uint64_t toff = off + (last - s) + MEM_PAGE_SIZE;
        if (mem_map_ex(va, len, prot, path, toff) < 0)
            return -1;
        int hfd = host_open(path, LO_RDONLY, 0);
        if (hfd < 0)
            return -1;
        int rc = fill_from_fd(hfd, va, len, toff);
        host_close(hfd);
        return rc;
    }
    return mem_map(va, len, prot);
}

/* mmap of a memfd. MAP_SHARED views the memfd's own pages, so views of the same
 * memfd alias each other. MAP_PRIVATE takes a copy of the contents. */
static int64_t mmap_memfd(uint64_t addr, uint64_t len, unsigned p, uint64_t type, uint64_t flags, vfs_fd *f,
                          uint64_t off)
{
    uint64_t base;
    if (flags & LMAP_FIXED) {
        if (addr & PAGE_MASK)
            return -LE_INVAL;
        if (addr >= MEM_VA_LIMIT || len > MEM_VA_LIMIT - addr)
            return -LE_NOMEM;
        mem_unmap(addr, len);
        base = addr;
    } else {
        base = mem_find_free(len, addr);
        if (!base)
            return -LE_NOMEM;
    }
    if (type == LMAP_SHARED)
        return mem_map_shared(base, len, p, f->mfd, off) < 0 ? -LE_NOMEM : (int64_t)base;
    if (mem_map(base, len, p) < 0)
        return -LE_NOMEM;
    static uint8_t chunk[IO_CHUNK];
    for (uint64_t done = 0; done < len;) {
        size_t want = len - done < IO_CHUNK ? (size_t)(len - done) : IO_CHUNK;
        int64_t r = memfd_pread(f->mfd, chunk, want, off + done);
        if (r <= 0)
            break; /* past the end: the rest of the pages stay zero */
        if (mem_poke(base + done, chunk, (size_t)r) < 0) {
            mem_unmap(base, len);
            return -LE_NOMEM;
        }
        done += want;
    }
    return (int64_t)base;
}

static int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t off)
{
    if (len == 0)
        return -LE_INVAL;
    if (len > MEM_VA_LIMIT)
        return -LE_NOMEM;
    len = page_up(len);
    unsigned p = (unsigned)(prot & 7);
    uint64_t type = flags & 0x0F;
    if (type != LMAP_SHARED && type != LMAP_PRIVATE)
        return -LE_INVAL;

    int file = !(flags & LMAP_ANONYMOUS);
    int hfd = -1;
    const char *path = NULL;
    if (file) {
        if (off & PAGE_MASK)
            return -LE_INVAL;
        vfs_fd *f = vfs_get((int)(int32_t)fd);
        if (!f)
            return -LE_BADF;
        if (f->kind == VFS_PATH)
            return -LE_BADF;
        /* The property area is read-only to guests: no shared writable view. */
        if (f->ro_area && type == LMAP_SHARED && (p & 2))
            return -LE_ACCES;
        if (f->kind == VFS_BINDER) {
            /* The receive buffer: a memfd the binder proc owns, mapped read-only. */
            memfd_t *m = NULL;
            int rc = binder_mmap_prepare(f->binder, len, (p & LPROT_WRITE) != 0, &m);
            if (rc)
                return rc;
            f->mfd = m;
            int64_t base = mmap_memfd(addr, len, p, LMAP_SHARED, flags, f, off);
            if (base >= 0)
                binder_mmap_done(f->binder, (uint64_t)base);
            return base;
        }
        if (f->kind == VFS_GOLDFISH) {
            /* An address-space block: the window's memfd, mapped at the block's offset (goldfish.c). */
            if (!f->mfd) {
                f->mfd = goldfish_as_memfd(f->obj);
                if (!f->mfd)
                    return -LE_NODEV;
            }
            return mmap_memfd(addr, len, p, LMAP_SHARED, flags, f, off);
        }
        if (f->kind == VFS_MEMFD)
            return mmap_memfd(addr, len, p, type, flags, f, off);
        if (f->kind == VFS_DIR || f->kind == VFS_SYN || !f->host)
            return -LE_NODEV;
        if (type == LMAP_SHARED && (prot & LPROT_WRITE))
            return -LE_ACCES; /* only read-only MAP_SHARED is supported */
        if ((f->lflags & LO_ACCMODE) == LO_WRONLY)
            return -LE_ACCES;
        hfd = f->hfd;
        path = f->host;
    }

    uint64_t base;
    if (flags & LMAP_FIXED) {
        if (addr & PAGE_MASK)
            return -LE_INVAL;
        if (addr >= MEM_VA_LIMIT || len > MEM_VA_LIMIT - addr)
            return -LE_NOMEM;
        mem_unmap(addr, len);
        base = addr;
    } else {
        base = mem_find_free(len, addr);
        if (!base)
            return -LE_NOMEM;
    }
    if (file) {
        if (map_file(base, len, p, path, hfd, off) < 0) {
            mem_unmap(base, len);
            return -LE_NOMEM;
        }
    } else if (type == LMAP_SHARED) {
        /* A MAP_SHARED anonymous mapping is a view of a hidden memfd, so a forked child
         * shares it with its parent. */
        memfd_t *m = memfd_new("zero");
        if (!m)
            return -LE_NOMEM;
        int rc = memfd_truncate(m, len);
        if (rc == 0)
            rc = mem_map_shared(base, len, p, m, 0);
        memfd_unref(m);
        if (rc < 0)
            return -LE_NOMEM;
    } else if (mem_map(base, len, p) < 0) {
        return -LE_NOMEM;
    }
    return (int64_t)base;
}

static int64_t sys_munmap(uint64_t addr, uint64_t len)
{
    if ((addr & PAGE_MASK) || len == 0)
        return -LE_INVAL;
    if (addr >= MEM_VA_LIMIT || len > MEM_VA_LIMIT - addr)
        return -LE_INVAL;
    mem_unmap(addr, page_up(len));
    return 0;
}

static int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot)
{
    if (addr & PAGE_MASK)
        return -LE_INVAL;
    if (len == 0)
        return 0;
    if (addr >= MEM_VA_LIMIT || page_up(len) > MEM_VA_LIMIT - addr)
        return -LE_NOMEM;
    /* The property area is read-only to guests: no view of it becomes writable. Checked
     * on the first page only; a range that spans other mappings is not split here. */
    uint64_t mstart = 0, moff = 0;
    memfd_t *region = mem_region_memfd(addr, &mstart, &moff);
    if ((prot & 2) && region && region == props_memfd())
        return -LE_ACCES;
    if (mem_protect(addr, page_up(len), (unsigned)(prot & 7)) < 0)
        return -LE_NOMEM;
    return 0;
}

static int64_t sys_mremap(uint64_t old, uint64_t oldlen, uint64_t newlen, uint64_t flags, uint64_t newaddr)
{
    if (old & PAGE_MASK)
        return -LE_INVAL;
    if (flags & ~(uint64_t)(LMREMAP_MAYMOVE | LMREMAP_FIXED))
        return -LE_INVAL;
    if ((flags & LMREMAP_FIXED) && !(flags & LMREMAP_MAYMOVE))
        return -LE_INVAL;
    if (oldlen == 0 || newlen == 0)
        return -LE_INVAL;
    if (oldlen > MEM_VA_LIMIT || newlen > MEM_VA_LIMIT || old >= MEM_VA_LIMIT)
        return -LE_NOMEM;
    oldlen = page_up(oldlen);
    newlen = page_up(newlen);
    if (oldlen > MEM_VA_LIMIT - old)
        return -LE_NOMEM;
    if (!mem_is_mapped(old, oldlen))
        return -LE_FAULT;

    if (!(flags & LMREMAP_FIXED)) {
        if (newlen == oldlen)
            return (int64_t)old;
        if (newlen < oldlen) {
            mem_unmap(old + newlen, oldlen - newlen);
            return (int64_t)old;
        }
        uint64_t delta = newlen - oldlen;
        if (newlen <= MEM_VA_LIMIT - old && mem_range_free(old + oldlen, delta)) {
            if (map_tail(old + oldlen, delta, old + oldlen - MEM_PAGE_SIZE) < 0)
                return -LE_NOMEM;
            return (int64_t)old;
        }
        if (!(flags & LMREMAP_MAYMOVE))
            return -LE_NOMEM;
        uint64_t base = mem_find_free(newlen, 0);
        if (!base)
            return -LE_NOMEM;
        if (mem_move(old, base, oldlen) < 0)
            return -LE_NOMEM;
        if (map_tail(base + oldlen, delta, base + oldlen - MEM_PAGE_SIZE) < 0)
            return -LE_NOMEM;
        return (int64_t)base;
    }

    /* MREMAP_FIXED: the old mapping moves to newaddr, which is replaced. */
    if (newaddr & PAGE_MASK)
        return -LE_INVAL;
    if (newaddr >= MEM_VA_LIMIT || newlen > MEM_VA_LIMIT - newaddr)
        return -LE_NOMEM;
    if (newaddr < old + oldlen && old < newaddr + newlen)
        return -LE_INVAL;
    uint64_t span = oldlen > newlen ? oldlen : newlen;
    mem_unmap(newaddr, span);
    if (mem_move(old, newaddr, oldlen) < 0)
        return -LE_NOMEM;
    if (newlen < oldlen)
        mem_unmap(newaddr + newlen, oldlen - newlen);
    else if (newlen > oldlen && map_tail(newaddr + oldlen, newlen - oldlen, newaddr + oldlen - MEM_PAGE_SIZE) < 0)
        return -LE_NOMEM;
    return (int64_t)newaddr;
}

static int64_t sys_madvise(uint64_t addr, uint64_t len, uint64_t advice)
{
    if (addr & PAGE_MASK)
        return -LE_INVAL;
    len = page_up(len);
    if (len == 0)
        return 0;
    if (addr >= MEM_VA_LIMIT || len > MEM_VA_LIMIT - addr)
        return -LE_NOMEM;
    if (advice != LMADV_DONTNEED)
        return 0;
    if (!mem_is_mapped(addr, len))
        return -LE_NOMEM;
    /* Drop the backing. Anonymous pages read back as zero; file-backed private
     * pages are read again from their file. */
    uint64_t pos = addr, end = addr + len;
    while (pos < end) {
        uint64_t s, e, off;
        const char *path;
        if (!mem_region_next(pos, &s, &e, &path, &off))
            break;
        uint64_t a = s > pos ? s : pos;
        uint64_t b = e < end ? e : end;
        mem_drop(a, b - a);
        /* A memfd view keeps its contents (mem_drop skips shared pages), and its
         * region path is a display name, not a host file, so there is nothing to refill. */
        uint64_t ms, moff;
        if (path && !mem_region_memfd(a, &ms, &moff)) {
            int hfd = host_open(path, LO_RDONLY, 0);
            if (hfd < 0)
                return hfd;
            int rc = fill_from_fd(hfd, a, b - a, off + (a - s));
            host_close(hfd);
            if (rc < 0)
                return -LE_FAULT;
        }
        pos = b;
    }
    return 0;
}

/* ---------- process, time and signals ---------- */

static int64_t sys_uname(uint64_t buf)
{
    /* struct utsname: six fields of 65 bytes. */
    char u[6 * 65];
    memset(u, 0, sizeof u);
    strcpy(u + 0 * 65, "Linux");
    strcpy(u + 1 * 65, "xiaoji");
    strcpy(u + 2 * 65, "6.1.0-xiaoji");
    strcpy(u + 3 * 65, "#1 xiaoji M1");
    strcpy(u + 4 * 65, "aarch64");
    strcpy(u + 5 * 65, "(none)");
    if (mem_copy_in(buf, u, sizeof u) < 0)
        return -LE_FAULT;
    return 0;
}

static int64_t sys_clock_gettime(uint64_t clk, uint64_t tp)
{
    uint64_t ns;
    if (clk == 0 || clk == 5) /* REALTIME, REALTIME_COARSE */
        ns = host_realtime_ns();
    else if (clk == 1 || clk == 6 || clk == 7) /* MONOTONIC, MONOTONIC_COARSE, BOOTTIME (no suspend) */
        ns = host_monotonic_ns();
    else
        return -LE_INVAL;
    uint64_t ts[2] = {ns / 1000000000ull, ns % 1000000000ull};
    if (mem_copy_in(tp, ts, sizeof ts) < 0)
        return -LE_FAULT;
    return 0;
}

/* struct timeval: seconds and microseconds. The timezone argument is ignored
 * (a zeroed struct is returned if asked for). */
static int64_t sys_gettimeofday(uint64_t tv, uint64_t tz)
{
    uint64_t ns = host_realtime_ns();
    if (tv) {
        uint64_t t[2] = {ns / 1000000000ull, (ns % 1000000000ull) / 1000ull};
        if (mem_copy_in(tv, t, sizeof t) < 0)
            return -LE_FAULT;
    }
    if (tz && mem_set(tz, 0, 8) < 0)
        return -LE_FAULT;
    return 0;
}

static int64_t sys_getrandom(uint64_t buf, uint64_t len)
{
    uint8_t tmp[256];
    uint64_t done = 0;
    while (done < len) {
        size_t chunk = len - done > sizeof tmp ? sizeof tmp : (size_t)(len - done);
        host_random(tmp, chunk);
        if (mem_copy_in(buf + done, tmp, chunk) < 0)
            return done ? (int64_t)done : -LE_FAULT;
        done += chunk;
    }
    return (int64_t)len;
}

/* prlimit64 and its getrlimit and setrlimit forms, on the calling process's limits (proc.c has the
 * defaults). The old limit is read before the new one is applied and written after, as Linux does.
 * Raising the hard limit needs an effective uid of 0. pid other than the caller's is ESRCH, or EPERM
 * for a process that exists (the engine does not change another process's limits). */
static int64_t sys_prlimit64(uint64_t resource, uint64_t newv, uint64_t old)
{
    proc_t *p = proc_cur();
    if (resource >= PROC_RLIM_N)
        return -LE_INVAL;
    uint64_t prev[2] = {p->rlim[resource][0], p->rlim[resource][1]};
    if (newv) {
        uint8_t e[16];
        if (mem_copy_out(e, newv, sizeof e) < 0)
            return -LE_FAULT;
        uint64_t nl[2] = {0, 0};
        for (int k = 0; k < 8; k++) {
            nl[0] |= (uint64_t)e[k] << (8 * k);
            nl[1] |= (uint64_t)e[8 + k] << (8 * k);
        }
        if (nl[0] > nl[1])
            return -LE_INVAL;
        if (nl[1] > p->rlim[resource][1] && !cred_has_cap(p, LCAP_SYS_RESOURCE))
            return -LE_PERM;
        p->rlim[resource][0] = nl[0];
        p->rlim[resource][1] = nl[1];
    }
    if (old) {
        uint8_t e[16];
        for (int k = 0; k < 8; k++) {
            e[k] = (uint8_t)(prev[0] >> (8 * k));
            e[8 + k] = (uint8_t)(prev[1] >> (8 * k));
        }
        if (mem_copy_in(old, e, sizeof e) < 0)
            return -LE_FAULT;
    }
    return 0;
}

/* ---------- threads, futexes and sleeps ---------- */

static int read_u32(uint64_t va, uint32_t *out)
{
    uint8_t b[4];
    if (mem_copy_out(b, va, sizeof b) < 0)
        return -1;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 0;
}

/* Reads a struct timespec as nanoseconds. Seconds are capped at 10^9 (about 31
 * years) so the sum cannot overflow. */
static int read_timespec(uint64_t va, uint64_t *ns)
{
    uint64_t ts[2];
    if (mem_copy_out(ts, va, sizeof ts) < 0)
        return -1;
    if (ts[0] > 1000000000ull)
        ts[0] = 1000000000ull;
    *ns = ts[0] * 1000000000ull + ts[1];
    return 0;
}

/* Host monotonic deadline for a wait of ns: relative, or absolute on
 * CLOCK_MONOTONIC or CLOCK_REALTIME. */
static uint64_t deadline_of(uint64_t ns, int absolute, int realtime)
{
    uint64_t now = host_monotonic_ns();
    if (!absolute)
        return now + ns;
    if (!realtime)
        return ns;
    uint64_t rt = host_realtime_ns();
    return ns > rt ? now + (ns - rt) : now;
}

static int64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val, uint64_t timeout, uint64_t uaddr2, uint64_t val3)
{
    unsigned cmd = (unsigned)(op & LFUTEX_OP_MASK);
    uint64_t obj1 = 0, obj2 = 0;
    uint64_t key1 = mem_futex_key(uaddr, &obj1);
    uint64_t key2 = mem_futex_key(uaddr2, &obj2);
    int realtime = (op & LFUTEX_CLOCK_REALTIME) != 0;
    switch (cmd) {
    case LFUTEX_WAIT:
    case LFUTEX_WAIT_BITSET: {
        uint32_t bits = 0xFFFFFFFFu;
        if (cmd == LFUTEX_WAIT_BITSET) {
            bits = (uint32_t)val3;
            if (bits == 0)
                return -LE_INVAL;
        }
        uint32_t cur;
        if (read_u32(uaddr, &cur) < 0)
            return -LE_FAULT;
        if (cur != (uint32_t)val)
            return -LE_AGAIN;
        int has = 0;
        uint64_t deadline = 0;
        if (timeout) {
            uint64_t ns;
            if (read_timespec(timeout, &ns) < 0)
                return -LE_FAULT;
            deadline = deadline_of(ns, cmd == LFUTEX_WAIT_BITSET, realtime);
            has = 1;
        }
        sched_block_futex(key1, obj1, bits, has, deadline); /* x0 is set on wake-up */
        return 0;
    }
    case LFUTEX_WAKE:
    case LFUTEX_WAKE_BITSET: {
        uint32_t bits = 0xFFFFFFFFu;
        if (cmd == LFUTEX_WAKE_BITSET) {
            bits = (uint32_t)val3;
            if (bits == 0)
                return -LE_INVAL;
        }
        return sched_futex_wake(key1, obj1, val, bits);
    }
    case LFUTEX_REQUEUE: /* the timeout argument carries the requeue count */
        return sched_futex_requeue(key1, obj1, key2, obj2, val, timeout);
    case LFUTEX_CMP_REQUEUE: {
        uint32_t cur;
        if (read_u32(uaddr, &cur) < 0)
            return -LE_FAULT;
        if (cur != (uint32_t)val3)
            return -LE_AGAIN;
        return sched_futex_requeue(key1, obj1, key2, obj2, val, timeout);
    }
    default: {
        static int notice;
        if (!notice) {
            notice = 1;
            fprintf(stderr, "xiaoji: unimplemented futex op %u\n", cmd);
        }
        return -LE_NOSYS;
    }
    }
}

/* read_timespec for a sleep: Linux rejects a negative or out-of-range field with EINVAL. */
static int read_sleep_timespec(uint64_t va, uint64_t *ns)
{
    uint64_t ts[2];
    if (mem_copy_out(ts, va, sizeof ts) < 0)
        return -LE_FAULT;
    if ((int64_t)ts[0] < 0 || (int64_t)ts[1] < 0 || ts[1] >= 1000000000ull)
        return -LE_INVAL;
    return read_timespec(va, ns) < 0 ? -LE_FAULT : 0;
}

static int64_t sys_nanosleep(uint64_t req)
{
    uint64_t ns;
    int rc = read_sleep_timespec(req, &ns);
    if (rc)
        return rc;
    sched_sleep_until(deadline_of(ns, 0, 0));
    return 0;
}

static int64_t sys_clock_nanosleep(uint64_t clk, uint64_t flags, uint64_t req)
{
    if (clk != LCLOCK_REALTIME && clk != LCLOCK_MONOTONIC)
        return -LE_INVAL;
    uint64_t ns;
    int rc = read_sleep_timespec(req, &ns);
    if (rc)
        return rc;
    sched_sleep_until(deadline_of(ns, (flags & LTIMER_ABSTIME) != 0, clk == LCLOCK_REALTIME));
    return 0;
}

/* The scheduling policy and priority of a thread (sched.h): pid is a tid, 0 is the calling
 * thread. The policies are Linux's: OTHER 0, FIFO 1, RR 2, BATCH 3, IDLE 5 (SCHED_RESET_ON_FORK,
 * a flag, is accepted). A real-time priority is 1..99, the others 0. */
#define SCHED_RESET_ON_FORK_FLAG 0x40000000
static uint64_t sched_target(uint64_t pid)
{
    return (int32_t)pid == 0 ? sched_tid() : pid;
}

static int64_t sys_sched_setscheduler(uint64_t pid, uint64_t policy_arg, uint64_t param)
{
    uint8_t raw[4];
    if (mem_copy_out(raw, param, sizeof raw) < 0)
        return -LE_FAULT;
    int32_t prio = (int32_t)(raw[0] | (raw[1] << 8) | (raw[2] << 16) | ((uint32_t)raw[3] << 24));
    int policy = (int32_t)policy_arg;
    uint64_t tid = sched_target(pid);
    int cur_pol = 0, cur_pri = 0;
    if (sched_policy_get(tid, &cur_pol, &cur_pri) != 0)
        return -LE_SRCH;
    if (policy == -1) /* sched_setparam: keep the policy */
        policy = cur_pol;
    int base = policy & ~SCHED_RESET_ON_FORK_FLAG;
    int ok;
    switch (base) {
    case 0: case 3: case 5: ok = prio == 0; break;
    case 1: case 2: ok = prio >= 1 && prio <= 99; break;
    default: ok = 0; break;
    }
    if (!ok)
        return -LE_INVAL;
    return sched_policy_set(tid, policy, prio) == 0 ? 0 : -LE_SRCH;
}

static int64_t sys_sched_getscheduler(uint64_t pid)
{
    int policy = 0, prio = 0;
    if (sched_policy_get(sched_target(pid), &policy, &prio) != 0)
        return -LE_SRCH;
    return policy;
}

static int64_t sys_sched_getparam(uint64_t pid, uint64_t param)
{
    int policy = 0, prio = 0;
    if (sched_policy_get(sched_target(pid), &policy, &prio) != 0)
        return -LE_SRCH;
    uint8_t raw[4] = {(uint8_t)prio, (uint8_t)((uint32_t)prio >> 8), (uint8_t)((uint32_t)prio >> 16),
                      (uint8_t)((uint32_t)prio >> 24)};
    if (mem_copy_in(param, raw, sizeof raw) < 0)
        return -LE_FAULT;
    return 0;
}

static int64_t sys_sched_getaffinity(uint64_t len, uint64_t mask)
{
    if (len < SIGSET_SIZE)
        return -LE_INVAL;
    uint64_t bits = 1; /* CPU 0 only */
    if (mem_copy_in(mask, &bits, sizeof bits) < 0)
        return -LE_FAULT;
    return SIGSET_SIZE;
}

/* rt_sigtimedwait: the timeout is relative. A zero timeout only polls. */
static int64_t sys_rt_sigtimedwait(cpu_t *c, uint64_t set, uint64_t info, uint64_t timeout, uint64_t sigsetsize)
{
    if (sigsetsize != SIGSET_SIZE)
        return -LE_INVAL;
    uint64_t wanted;
    if (mem_copy_out(&wanted, set, sizeof wanted) < 0)
        return -LE_FAULT;
    int poll = 0, has = 0;
    uint64_t deadline = 0;
    if (timeout) {
        uint64_t ns;
        if (read_timespec(timeout, &ns) < 0)
            return -LE_FAULT;
        if (ns == 0)
            poll = 1;
        else {
            has = 1;
            deadline = deadline_of(ns, 0, 0);
        }
    }
    return sig_timedwait(c, wanted, info, poll, has, deadline);
}

static int64_t sys_prctl(uint64_t option, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    proc_t *me = proc_cur();
    /* The unused arguments must be zero for these options, as Linux checks them. */
    if (option == LPR_SET_VMA) /* anonymous VMA names are not kept; the oracle kernel returns EINVAL too */
        return -LE_INVAL;
    switch ((int)option) {
    case LPR_GET_DUMPABLE:
        return me->dumpable;
    case LPR_SET_DUMPABLE:
        if (arg2 > 1)
            return -LE_INVAL;
        me->dumpable = (int)arg2;
        return 0;
    case LPR_SET_PDEATHSIG:
        if (arg2 > 64)
            return -LE_INVAL;
        me->pdeathsig = (int)arg2;
        return 0;
    case LPR_GET_PDEATHSIG: {
        int32_t v = me->pdeathsig;
        return mem_copy_in(arg2, &v, sizeof v) < 0 ? -LE_FAULT : 0;
    }
    case LPR_GET_KEEPCAPS:
        return me->keepcaps;
    case LPR_SET_KEEPCAPS:
        if (arg2 > 1)
            return -LE_INVAL;
        me->keepcaps = (int)arg2;
        return 0;
    case LPR_CAPBSET_READ:
        return cred_capbset_read(me, arg2);
    case LPR_CAPBSET_DROP:
        return cred_capbset_drop(me, arg2);
    case LPR_CAP_AMBIENT:
        return cred_ambient(me, arg2, arg3, arg4, arg5);
    case LPR_GET_SECUREBITS:
        return me->securebits;
    case LPR_SET_SECUREBITS:
        if (!cred_has_cap(me, LCAP_SETPCAP))
            return -LE_PERM;
        me->securebits = (int)(uint32_t)arg2;
        return 0;
    case LPR_SET_TIMERSLACK:
        if ((int64_t)arg2 < 0)
            return -LE_INVAL;
        me->timerslack = arg2 == 0 ? 50000 : (int)arg2;
        return 0;
    case LPR_GET_TIMERSLACK:
        return me->timerslack;
    case LPR_SET_CHILD_SUBREAPER:
        me->subreaper = arg2 != 0;
        return 0;
    case LPR_GET_CHILD_SUBREAPER: {
        int32_t v = me->subreaper;
        return mem_copy_in(arg2, &v, sizeof v) < 0 ? -LE_FAULT : 0;
    }
    case LPR_SET_NO_NEW_PRIVS:
        /* Only the setting to 1, with the other arguments zero, is valid (as in Linux). */
        if (arg2 != 1 || arg3 || arg4 || arg5)
            return -LE_INVAL;
        me->no_new_privs = 1;
        return 0;
    case LPR_GET_NO_NEW_PRIVS:
        if (arg2 || arg3 || arg4 || arg5)
            return -LE_INVAL;
        return me->no_new_privs;
    case LPR_GET_TAGGED_ADDR_CTRL:
        return 0; /* no tagged addresses (no MTE, no TBI control) */
    case LPR_SET_TAGGED_ADDR_CTRL:
        return arg2 == 0 && !arg3 && !arg4 && !arg5 ? 0 : -LE_INVAL;
    case LPR_SET_PTRACER:
        /* Accepted with no effect: the oracle kernel has Yama, which takes it (0), and the engine
         * has no ptrace restriction to set. */
        return 0;
    case LPR_SET_NAME: {
        char name[16];
        int64_t n = mem_strlen(arg2);
        if (n < 0)
            return -LE_FAULT;
        if (n > 15)
            n = 15;
        memset(name, 0, sizeof name);
        if (n > 0 && mem_copy_out(name, arg2, (size_t)n) < 0)
            return -LE_FAULT;
        sched_set_name(name);
        return 0;
    }
    case LPR_GET_NAME: {
        uint8_t name[16];
        memcpy(name, sched_name(), sizeof name);
        return mem_copy_in(arg2, name, sizeof name) < 0 ? -LE_FAULT : 0;
    }
    default:
        return -LE_INVAL;
    }
}

/* ---------- processes: wait, process groups and sessions ---------- */

#define LWNOHANG 1
#define LWEXITED 4
#define LWNOWAIT 0x01000000u
#define LWAIT_KNOWN (LWNOHANG | LWEXITED | LWNOWAIT | 2u | 8u | 0x40000000u | 0x80000000u)
#define LP_ALL 0
#define LP_PID 1
#define LP_PGID 2
#define LSIGCHLD 17
#define LCLD_EXITED 1
#define LCLD_KILLED 2

/* Whether child c is chosen by a wait4 selector: >0 that pid, 0 the caller's group,
 * -1 any child, below -1 the group -sel. */
static int child_selected(const proc_t *c, const proc_t *me, int32_t sel)
{
    if (sel > 0)
        return c->pid == sel;
    if (sel == 0)
        return c->pgid == me->pgid;
    if (sel == -1)
        return 1;
    return c->pgid == -(int64_t)sel;
}

/* The wait status of an ended process: the exit status in bits 8..15, or the signal. */
static uint32_t wait_status(const proc_t *c)
{
    return c->term_sig ? (uint32_t)(c->term_sig & 0x7F) : ((uint32_t)(c->exit_code & 0xFF) << 8);
}

/* wait4: reaps a zombie child, or blocks until one ends (the call is restarted then). */
static int64_t sys_wait4(uint64_t pid64, uint64_t wstatus, uint64_t options, uint64_t rusage)
{
    (void)rusage;
    int32_t sel = (int32_t)pid64;
    proc_t *me = proc_cur();
    if (options & ~(uint64_t)(LWNOHANG | 2u | 8u | 0x40000000u | 0x80000000u))
        return -LE_INVAL;
    int any = 0;
    for (proc_t *c = proc_first(); c; c = proc_next(c)) {
        if (c->parent != me || !child_selected(c, me, sel))
            continue;
        any = 1;
        if (!c->zombie)
            continue;
        uint32_t st = wait_status(c);
        if (wstatus && mem_copy_in(wstatus, &st, sizeof st) < 0)
            return -LE_FAULT;
        int64_t pid = c->pid;
        proc_free(c);
        return pid;
    }
    if (!any)
        return -LE_CHILD;
    if (options & LWNOHANG)
        return 0;
    sched_block_restart((uint64_t)(uintptr_t)me); /* woken when a child ends */
    return 0;
}

/* siginfo of a waited-for child (si_signo, si_code, si_pid, si_uid, si_status). */
static int write_child_info(uint64_t va, const proc_t *c)
{
    /* siginfo: si_signo 0, si_errno 4, si_code 8; the SIGCHLD union at 16: si_pid, si_uid
     * (20), si_status (24). */
    uint8_t b[128];
    memset(b, 0, sizeof b);
    uint32_t fields[6];
    fields[0] = LSIGCHLD;
    fields[1] = 0;
    fields[2] = c->term_sig ? LCLD_KILLED : LCLD_EXITED;
    fields[3] = (uint32_t)c->pid;
    fields[4] = 0;
    fields[5] = c->term_sig ? (uint32_t)c->term_sig : (uint32_t)(c->exit_code & 0xFF);
    const int off[6] = {0, 4, 8, 16, 20, 24};
    for (int i = 0; i < 6; i++)
        for (int k = 0; k < 4; k++)
            b[off[i] + k] = (uint8_t)(fields[i] >> (8 * k));
    return mem_copy_in(va, b, sizeof b) < 0 ? -LE_FAULT : 0;
}

/* waitid: like wait4, for children chosen by idtype (P_ALL, P_PID, P_PGID). */
static int64_t sys_waitid(uint64_t idtype, uint64_t id, uint64_t info, uint64_t options, uint64_t rusage)
{
    (void)rusage;
    proc_t *me = proc_cur();
    if (!(options & LWEXITED) || (options & ~(uint64_t)LWAIT_KNOWN))
        return -LE_INVAL;
    if (idtype != LP_ALL && idtype != LP_PID && idtype != LP_PGID)
        return -LE_INVAL;
    int32_t want = (int32_t)id;
    if (idtype == LP_PGID && want == 0)
        want = me->pgid;
    int any = 0;
    for (proc_t *c = proc_first(); c; c = proc_next(c)) {
        if (c->parent != me)
            continue;
        if ((idtype == LP_PID && c->pid != want) || (idtype == LP_PGID && c->pgid != want))
            continue;
        any = 1;
        if (!c->zombie)
            continue;
        if (info) {
            int r = write_child_info(info, c);
            if (r < 0)
                return r;
        }
        if (!(options & LWNOWAIT))
            proc_free(c);
        return 0;
    }
    if (!any)
        return -LE_CHILD;
    if (options & LWNOHANG) {
        if (info) {
            uint8_t zero[128];
            memset(zero, 0, sizeof zero);
            if (mem_copy_in(info, zero, sizeof zero) < 0)
                return -LE_FAULT;
        }
        return 0;
    }
    sched_block_restart((uint64_t)(uintptr_t)me);
    return 0;
}

static proc_t *proc_or_self(int32_t pid)
{
    if (pid == 0)
        return proc_cur();
    proc_t *p = proc_find(pid);
    return p && !p->zombie ? p : NULL;
}

static int64_t sys_getpgid(uint64_t pid64)
{
    proc_t *p = proc_or_self((int32_t)pid64);
    return p ? p->pgid : -LE_SRCH;
}

static int64_t sys_getsid(uint64_t pid64)
{
    proc_t *p = proc_or_self((int32_t)pid64);
    return p ? p->sid : -LE_SRCH;
}

/* setpgid: the caller or one of its children, not a session leader. */
static int64_t sys_setpgid(uint64_t pid64, uint64_t pgid64)
{
    int32_t pid = (int32_t)pid64, pgid = (int32_t)pgid64;
    proc_t *me = proc_cur();
    proc_t *p = pid == 0 ? me : proc_find(pid);
    if (!p || p->zombie)
        return -LE_SRCH;
    if (p != me && p->parent != me)
        return -LE_SRCH;
    if (p->sid == p->pid && p->pid != me->pid)
        return -LE_PERM;
    if (pgid < 0)
        return -LE_INVAL;
    p->pgid = pgid == 0 ? p->pid : pgid;
    return 0;
}

/* setsid: a new session led by the caller, unless it already leads a group. */
static int64_t sys_setsid(void)
{
    proc_t *me = proc_cur();
    if (me->pgid == me->pid)
        return -LE_PERM;
    me->sid = me->pgid = me->pid;
    return me->pid;
}

/* ---------- resource usage ---------- */

#define LRUSAGE_SELF 0
#define LRUSAGE_CHILDREN (-1)
#define LRUSAGE_THREAD 1
#define RUSAGE_SIZE 144 /* struct rusage on arm64: two timevals, then 14 longs */

/* getrusage. The times are derived from the counts so they are deterministic: an
 * instruction is 1 ns of user time, and a syscall is 10 us of system time. maxrss is the
 * peak resident set (in KiB), as seen at exit or here. Children's totals come from the
 * reaped children (proc_free). */
static int64_t sys_getrusage(int64_t who, uint64_t va)
{
    proc_t *me = proc_cur();
    uint64_t icount, nsys, maxrss;
    if (who == LRUSAGE_SELF || who == LRUSAGE_THREAD) {
        icount = sched_icount_of(me);
        if (who == LRUSAGE_THREAD && sched_cur_cpu())
            icount = sched_cur_cpu()->icount;
        nsys = me->nsys;
        uint64_t rss = mem_mm_rss_kib(me->mm);
        if (rss > me->maxrss_kib)
            me->maxrss_kib = rss;
        maxrss = me->maxrss_kib;
    } else if (who == LRUSAGE_CHILDREN) {
        icount = me->cu_icount;
        nsys = me->cu_nsys;
        maxrss = me->cu_maxrss_kib;
    } else {
        return -LE_INVAL;
    }
    uint64_t utime_us = icount / 1000, stime_us = nsys * 10;
    uint64_t out[RUSAGE_SIZE / 8] = {0};
    out[0] = utime_us / 1000000;
    out[1] = utime_us % 1000000;
    out[2] = stime_us / 1000000;
    out[3] = stime_us % 1000000;
    out[4] = maxrss;
    if (mem_copy_in(va, out, sizeof out) < 0)
        return -LE_FAULT;
    return 0;
}

/* ---------- dispatch ---------- */

static void dispatch(cpu_t *c, uint64_t nr, const uint64_t a[6], int64_t *ret)
{
    if (sysfs_call(nr, a, ret))
        return;
    switch (nr) {
    case SYS_EXIT:
        sched_thread_exit(c, a[0]);
        *ret = 0;
        return;
    case SYS_EXIT_GROUP:
        sched_exit_process(a[0] & 0xFF);
        *ret = 0;
        return;
    case SYS_BRK:
        *ret = sys_brk(a[0]);
        return;
    case SYS_MMAP:
        *ret = sys_mmap(a[0], a[1], a[2], a[3], a[4], a[5]);
        return;
    case SYS_MUNMAP:
        *ret = sys_munmap(a[0], a[1]);
        return;
    case SYS_MPROTECT:
        *ret = sys_mprotect(a[0], a[1], a[2]);
        return;
    case SYS_MREMAP:
        *ret = sys_mremap(a[0], a[1], a[2], a[3], a[4]);
        return;
    case SYS_MADVISE:
        *ret = sys_madvise(a[0], a[1], a[2]);
        return;
    case SYS_MSYNC:
    case SYS_MLOCK:
    case SYS_MUNLOCK:
        *ret = 0;
        return;
    case SYS_SET_TID_ADDRESS:
        sched_set_clear_child_tid(a[0]);
        *ret = (int64_t)sched_tid();
        return;
    case SYS_SET_ROBUST_LIST:
        *ret = 0;
        return;
    case SYS_UNAME:
        *ret = sys_uname(a[0]);
        return;
    case SYS_GETPID:
        *ret = proc_cur()->pid;
        return;
    case SYS_GETTID:
        *ret = (int64_t)sched_tid();
        return;
    case SYS_CLONE:
        *ret = sched_clone(c, a[0], a[1], a[2], a[3], a[4]);
        return;
    case SYS_EXECVE:
        *ret = exec_run(c, LAT_FDCWD, a[0], a[1], a[2], 0);
        return;
    case SYS_EXECVEAT:
        *ret = exec_run(c, (int)(int32_t)a[0], a[1], a[2], a[3], a[4]);
        return;
    case SYS_GETRUSAGE:
        *ret = sys_getrusage((int32_t)a[0], a[1]);
        return;
    case SYS_NANOSLEEP:
        *ret = sys_nanosleep(a[0]);
        return;
    case SYS_CLOCK_NANOSLEEP:
        *ret = sys_clock_nanosleep(a[0], a[1], a[2]);
        return;
    case 90: case 91:
    case 143: case 144: case 145: case 146: case 147: case 148: case 149: case 150: case 151:
    case 152: case 158: case 159:
    case SYS_GETUID:
    case SYS_GETEUID:
    case SYS_GETGID:
    case SYS_GETEGID:
        *ret = cred_syscall(nr, a);
        return;
    case SYS_GETTIMEOFDAY:
        *ret = sys_gettimeofday(a[0], a[1]);
        return;
    case SYS_GETPPID:
        *ret = proc_cur()->ppid;
        return;
    case SYS_GETPGID:
        *ret = sys_getpgid(a[0]);
        return;
    case SYS_SETPGID:
        *ret = sys_setpgid(a[0], a[1]);
        return;
    case SYS_GETSID:
        *ret = sys_getsid(a[0]);
        return;
    case SYS_SETSID:
        *ret = sys_setsid();
        return;
    case SYS_WAIT4:
        *ret = sys_wait4(a[0], a[1], a[2], a[3]);
        return;
    case SYS_WAITID:
        *ret = sys_waitid(a[0], a[1], a[2], a[3], a[4]);
        return;
    case SYS_SCHED_YIELD:
        c->stop = 1; /* the rest of the slice goes to the next thread */
        *ret = 0;
        return;
    case SYS_GETPRIORITY:
    case SYS_SETPRIORITY:
        *ret = 0;
        return;
    case SYS_SCHED_SETSCHEDULER:
        *ret = sys_sched_setscheduler(a[0], a[1], a[2]);
        return;
    case SYS_SCHED_GETSCHEDULER:
        *ret = sys_sched_getscheduler(a[0]);
        return;
    case SYS_SCHED_GETPARAM:
        *ret = sys_sched_getparam(a[0], a[1]);
        return;
    case SYS_SCHED_GETAFFINITY:
        *ret = sys_sched_getaffinity(a[1], a[2]);
        return;
    case SYS_FUTEX:
        *ret = sys_futex(a[0], a[1], a[2], a[3], a[4], a[5]);
        return;
    case SYS_SIGALTSTACK:
        *ret = sig_sigaltstack(c, a[0], a[1]);
        return;
    case SYS_RT_SIGACTION:
        *ret = sig_rt_sigaction(a[0], a[1], a[2], a[3]);
        return;
    case SYS_RT_SIGPROCMASK:
        *ret = sig_rt_sigprocmask(c, a[0], a[1], a[2], a[3]);
        return;
    case SYS_RT_SIGTIMEDWAIT:
        *ret = sys_rt_sigtimedwait(c, a[0], a[1], a[2], a[3]);
        return;
    case SYS_RT_SIGSUSPEND:
        *ret = sig_sigsuspend(c, a[0], a[1]);
        return;
    case SYS_RT_SIGRETURN:
        sig_rt_sigreturn(c); /* sets c->sig_restore: x0 comes from the frame */
        *ret = 0;
        return;
    case SYS_KILL:
        *ret = sig_kill(a[0], a[1]);
        return;
    case SYS_TKILL:
        *ret = sig_tkill(a[0], a[1]);
        return;
    case SYS_TGKILL:
        *ret = sig_tgkill(a[0], a[1], a[2]);
        return;
    case SYS_RT_SIGQUEUEINFO:
        *ret = sig_rt_sigqueueinfo(a[0], a[1], a[2]);
        return;
    case SYS_RT_TGSIGQUEUEINFO:
        *ret = sig_rt_tgsigqueueinfo(a[0], a[1], a[2], a[3]);
        return;
    case SYS_PRCTL:
        *ret = sys_prctl(a[0], a[1], a[2], a[3], a[4]);
        return;
    case SYS_GETRANDOM:
        *ret = sys_getrandom(a[0], a[1]);
        return;
    case SYS_CLOCK_GETTIME:
        *ret = sys_clock_gettime(a[0], a[1]);
        return;
    case SYS_PRLIMIT64:
        if (a[0] != 0 && (int64_t)a[0] != proc_cur()->pid)
            *ret = proc_find((int)a[0]) ? -LE_PERM : -LE_SRCH;
        else
            *ret = sys_prlimit64(a[1], a[2], a[3]);
        return;
    case SYS_RSEQ:
        *ret = -LE_NOSYS;
        return;
    case SYS_SOCKET:
        /* No network and no logd: the socket cannot be created (ART copes without logd). */
        *ret = -LE_ACCES;
        return;
    case SYS_FLOCK:
        *ret = sysfs_flock(a[0], a[1]);
        return;
    case SYS_GETRLIMIT:
        *ret = sys_prlimit64(a[0], 0, a[1]);
        return;
    case SYS_SETRLIMIT:
        *ret = sys_prlimit64(a[0], a[1], 0);
        return;
    default:
        if (nr < 1024) {
            uint64_t bit = 1ull << (nr & 63);
            if (unimpl_seen[nr >> 6] & bit) {
                *ret = -LE_NOSYS;
                return;
            }
            unimpl_seen[nr >> 6] |= bit;
        }
        fprintf(stderr, "xiaoji: unimplemented syscall %llu\n", (unsigned long long)nr);
        *ret = -LE_NOSYS;
        return;
    }
}

void syscall_dispatch(cpu_t *c)
{
    uint64_t nr = c->x[8];
    uint64_t a[6] = {c->x[0], c->x[1], c->x[2], c->x[3], c->x[4], c->x[5]};
    int64_t ret = 0;
    proc_cur()->nsys++;
    dispatch(c, nr, a, &ret);
    if (prof_on)
        prof_syscall(c->tid, nr, a, ret, c->blocked);
    if (!c->blocked && !c->sig_restore) /* a blocked call gets its x0 when it wakes */
        c->x[0] = (uint64_t)ret;
    if (!c->blocked)
        c->io_done = 0; /* a finished or failed call has no partial pipe write left */
    c->sig_restore = 0;
    if (c->trace) {
        fprintf(stderr, "xiaoji: %s(%llu) (0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx) = %lld\n",
                syscall_name(nr), (unsigned long long)nr, (unsigned long long)a[0],
                (unsigned long long)a[1], (unsigned long long)a[2], (unsigned long long)a[3],
                (unsigned long long)a[4], (unsigned long long)a[5], (long long)ret);
    }
}
