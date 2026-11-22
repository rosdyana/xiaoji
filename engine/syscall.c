/* Linux arm64 syscalls. Numbers are from asm-generic/unistd.h (checked against
 * the builder VM's headers). File and directory calls live in sysfs.c. */
#include "syscall.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "linux.h"
#include "mem.h"
#include "sysfs.h"
#include "vfs.h"

#define SYS_IOCTL 29
#define SYS_EXIT 93
#define SYS_EXIT_GROUP 94
#define SYS_SET_TID_ADDRESS 96
#define SYS_FUTEX 98
#define SYS_SET_ROBUST_LIST 99
#define SYS_NANOSLEEP 101
#define SYS_SCHED_GETSCHEDULER 120
#define SYS_SCHED_YIELD 124
#define SYS_SCHED_GETAFFINITY 123
#define SYS_KILL 129
#define SYS_TKILL 130
#define SYS_TGKILL 131
#define SYS_SIGALTSTACK 132
#define SYS_RT_SIGACTION 134
#define SYS_RT_SIGPROCMASK 135
#define SYS_SETPRIORITY 140
#define SYS_GETPRIORITY 141
#define SYS_GETPGID 155
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

#define GUEST_TID 1000
#define GUEST_PPID 1
#define GUEST_PGID 1000
#define PAGE_MASK 4095ull
#define RLIM_INFINITY (~0ull)
#define RLIMIT_STACK 3
#define STACK_RLIMIT (8ull << 20)
#define SIGSET_SIZE 8
#define SIG_MAX 64
#define SA_SIZE 32
#define STACK_T_SIZE 24
#define IO_CHUNK 65536

typedef struct {
    uint64_t handler, flags, restorer, mask;
} guest_sigaction;

static uint64_t brk_base;
static uint64_t brk_cur;
static uint64_t unimpl_seen[1024 / 64];
static guest_sigaction sigactions[SIG_MAX + 1];
static uint64_t altstack[3]; /* ss_sp, ss_flags, ss_size */
static char comm[16] = "xiaoji";
static uint8_t fill_buf[IO_CHUNK];

static uint64_t page_up(uint64_t x) { return (x + PAGE_MASK) & ~PAGE_MASK; }

static const char *syscall_name(uint64_t nr)
{
    switch (nr) {
    case 17: return "getcwd";
    case 23: return "dup";
    case 24: return "dup3";
    case 25: return "fcntl";
    case 29: return "ioctl";
    case 43: return "statfs";
    case 44: return "fstatfs";
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
    case 67: return "pread64";
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
    case 120: return "sched_getscheduler";
    case 123: return "sched_getaffinity";
    case 124: return "sched_yield";
    case 129: return "kill";
    case 130: return "tkill";
    case 131: return "tgkill";
    case 132: return "sigaltstack";
    case 134: return "rt_sigaction";
    case 135: return "rt_sigprocmask";
    case 140: return "setpriority";
    case 141: return "getpriority";
    case 155: return "getpgid";
    case 160: return "uname";
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
    case 291: return "statx";
    case 293: return "rseq";
    case 439: return "faccessat2";
    default: return "?";
    }
}

void syscall_init(uint64_t base)
{
    brk_base = base;
    brk_cur = base;
}

void syscall_set_comm(const char *argv0)
{
    const char *base = strrchr(argv0, '/');
    base = base ? base + 1 : argv0;
    memset(comm, 0, sizeof comm);
    strncpy(comm, base, sizeof comm - 1);
}

/* ---------- memory ---------- */

static int64_t sys_brk(uint64_t want)
{
    if (want == 0 || want < brk_base || want >= MEM_VA_LIMIT)
        return (int64_t)brk_cur;
    uint64_t cur_top = page_up(brk_cur), new_top = page_up(want);
    if (new_top > cur_top) {
        if (!mem_range_free(cur_top, new_top - cur_top))
            return (int64_t)brk_cur;
        if (mem_map(cur_top, new_top - cur_top, MEM_PROT_R | MEM_PROT_W) < 0)
            return (int64_t)brk_cur;
    } else if (new_top < cur_top) {
        mem_unmap(new_top, cur_top - new_top);
    }
    brk_cur = want;
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
        if (path) {
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
    if (clk == 0)
        ns = host_realtime_ns();
    else if (clk == 1)
        ns = host_monotonic_ns();
    else
        return -LE_INVAL;
    uint64_t ts[2] = {ns / 1000000000ull, ns % 1000000000ull};
    if (mem_copy_in(tp, ts, sizeof ts) < 0)
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

static int64_t sys_prlimit64(uint64_t resource, uint64_t old)
{
    if (old) {
        uint64_t lim[2];
        lim[0] = resource == RLIMIT_STACK ? STACK_RLIMIT : RLIM_INFINITY;
        lim[1] = RLIM_INFINITY;
        if (mem_copy_in(old, lim, sizeof lim) < 0)
            return -LE_FAULT;
    }
    return 0;
}

static int64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val)
{
    /* One thread: a wait can never be woken, so it fails at once. */
    switch ((uint32_t)op & LFUTEX_OP_MASK) {
    case LFUTEX_WAIT: {
        uint8_t b[4];
        if (mem_copy_out(b, uaddr, 4) < 0)
            return -LE_FAULT;
        uint32_t cur = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        return cur != (uint32_t)val ? -LE_AGAIN : -LE_TIMEDOUT;
    }
    case LFUTEX_WAKE:
        return 0;
    default:
        return -LE_NOSYS;
    }
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

static int64_t sys_sigaltstack(uint64_t ss, uint64_t oss)
{
    if (oss && mem_copy_in(oss, altstack, STACK_T_SIZE) < 0)
        return -LE_FAULT;
    if (ss) {
        uint64_t n[3];
        if (mem_copy_out(n, ss, STACK_T_SIZE) < 0)
            return -LE_FAULT;
        altstack[0] = n[0];
        altstack[1] = n[1] & 0xFFFFFFFFu;
        altstack[2] = n[2];
    }
    return 0;
}

static int64_t sys_rt_sigaction(uint64_t sig, uint64_t act, uint64_t oact, uint64_t sigsetsize)
{
    if (sigsetsize != SIGSET_SIZE || sig < 1 || sig > SIG_MAX)
        return -LE_INVAL;
    if (act && (sig == 9 || sig == 19)) /* SIGKILL and SIGSTOP */
        return -LE_INVAL;
    if (oact && mem_copy_in(oact, &sigactions[sig], SA_SIZE) < 0)
        return -LE_FAULT;
    if (act) {
        guest_sigaction n;
        if (mem_copy_out(&n, act, SA_SIZE) < 0)
            return -LE_FAULT;
        sigactions[sig] = n; /* stored only: no signal is delivered yet */
    }
    return 0;
}

static void die_by_signal(cpu_t *c, uint64_t sig)
{
    fflush(stdout);
    fprintf(stderr, "xiaoji: killed by signal %llu\n", (unsigned long long)sig);
    c->exited = 1;
    c->exit_code = (int)(128 + sig);
}

static int64_t sys_prctl(uint64_t option, uint64_t arg2)
{
    if (option == LPR_SET_VMA || option == LPR_SET_NAME)
        return 0;
    if (option == LPR_GET_NAME) {
        uint8_t name[16];
        memset(name, 0, sizeof name);
        memcpy(name, comm, sizeof comm);
        return mem_copy_in(arg2, name, sizeof name) < 0 ? -LE_FAULT : 0;
    }
    return -LE_INVAL;
}

/* ---------- dispatch ---------- */

static void dispatch(cpu_t *c, uint64_t nr, const uint64_t a[6], int64_t *ret)
{
    if (sysfs_call(nr, a, ret))
        return;
    switch (nr) {
    case SYS_EXIT:
    case SYS_EXIT_GROUP:
        c->exited = 1;
        c->exit_code = (int)(a[0] & 0xFF);
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
        *ret = GUEST_TID;
        return;
    case SYS_SET_ROBUST_LIST:
        *ret = 0;
        return;
    case SYS_UNAME:
        *ret = sys_uname(a[0]);
        return;
    case SYS_GETPID:
    case SYS_GETTID:
    case SYS_GETUID:
    case SYS_GETEUID:
    case SYS_GETGID:
    case SYS_GETEGID:
        *ret = GUEST_TID;
        return;
    case SYS_GETPPID:
        *ret = GUEST_PPID;
        return;
    case SYS_GETPGID:
        *ret = GUEST_PGID;
        return;
    case SYS_GETPRIORITY:
    case SYS_SETPRIORITY:
    case SYS_SCHED_YIELD:
    case SYS_SCHED_GETSCHEDULER:
        *ret = 0;
        return;
    case SYS_SCHED_GETAFFINITY:
        *ret = sys_sched_getaffinity(a[1], a[2]);
        return;
    case SYS_FUTEX:
        *ret = sys_futex(a[0], a[1], a[2]);
        return;
    case SYS_SIGALTSTACK:
        *ret = sys_sigaltstack(a[0], a[1]);
        return;
    case SYS_RT_SIGACTION:
        *ret = sys_rt_sigaction(a[0], a[1], a[2], a[3]);
        return;
    case SYS_RT_SIGPROCMASK:
        /* Zero the old mask (8 bytes); no signal masking yet. */
        *ret = (a[2] && mem_set(a[2], 0, 8) < 0) ? -LE_FAULT : 0;
        return;
    case SYS_KILL:
    case SYS_TKILL:
        if (a[1] == 0) {
            *ret = 0;
            return;
        }
        die_by_signal(c, a[1]);
        *ret = 0;
        return;
    case SYS_TGKILL:
        if (a[2] == 0) {
            *ret = 0;
            return;
        }
        die_by_signal(c, a[2]);
        *ret = 0;
        return;
    case SYS_PRCTL:
        *ret = sys_prctl(a[0], a[1]);
        return;
    case SYS_GETRANDOM:
        *ret = sys_getrandom(a[0], a[1]);
        return;
    case SYS_CLOCK_GETTIME:
        *ret = sys_clock_gettime(a[0], a[1]);
        return;
    case SYS_IOCTL:
        *ret = -LE_NOTTY;
        return;
    case SYS_PRLIMIT64:
        *ret = sys_prlimit64(a[1], a[3]);
        return;
    case SYS_RSEQ:
        *ret = -LE_NOSYS;
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
    dispatch(c, nr, a, &ret);
    c->x[0] = (uint64_t)ret;
    if (c->trace) {
        fprintf(stderr, "xiaoji: %s(%llu) (0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx) = %lld\n",
                syscall_name(nr), (unsigned long long)nr, (unsigned long long)a[0],
                (unsigned long long)a[1], (unsigned long long)a[2], (unsigned long long)a[3],
                (unsigned long long)a[4], (unsigned long long)a[5], (long long)ret);
    }
}
