/* Linux arm64 syscalls for M0. Numbers are from asm-generic/unistd.h. */
#include "syscall.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host.h"
#include "mem.h"

#define SYS_IOCTL 29
#define SYS_READLINKAT 78
#define SYS_WRITE 64
#define SYS_WRITEV 66
#define SYS_EXIT 93
#define SYS_EXIT_GROUP 94
#define SYS_SET_TID_ADDRESS 96
#define SYS_SET_ROBUST_LIST 99
#define SYS_CLOCK_GETTIME 113
#define SYS_RT_SIGACTION 134
#define SYS_RT_SIGPROCMASK 135
#define SYS_UNAME 160
#define SYS_GETPID 172
#define SYS_GETUID 174
#define SYS_GETEUID 175
#define SYS_GETGID 176
#define SYS_GETEGID 177
#define SYS_GETTID 178
#define SYS_BRK 214
#define SYS_MUNMAP 215
#define SYS_MMAP 222
#define SYS_MPROTECT 226
#define SYS_PRLIMIT64 261
#define SYS_GETRANDOM 278
#define SYS_RSEQ 293

#define E_NOENT 2
#define E_BADF 9
#define E_NOMEM 12
#define E_FAULT 14
#define E_NODEV 19
#define E_INVAL 22
#define E_IO 5
#define E_NOTTY 25
#define E_NOSYS 38

#define GUEST_TID 1000
#define PAGE_MASK 4095ull
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define IOV_MAX 1024
#define RLIM_INFINITY (~0ull)
#define RLIMIT_STACK 3
#define STACK_RLIMIT (8ull << 20)

static uint64_t brk_base;
static uint64_t brk_cur;
static uint64_t unimpl_seen[1024 / 64];

static uint64_t page_up(uint64_t x) { return (x + PAGE_MASK) & ~PAGE_MASK; }

static const char *syscall_name(uint64_t nr)
{
    switch (nr) {
    case SYS_IOCTL: return "ioctl";
    case SYS_READLINKAT: return "readlinkat";
    case SYS_WRITE: return "write";
    case SYS_WRITEV: return "writev";
    case SYS_EXIT: return "exit";
    case SYS_EXIT_GROUP: return "exit_group";
    case SYS_SET_TID_ADDRESS: return "set_tid_address";
    case SYS_SET_ROBUST_LIST: return "set_robust_list";
    case SYS_CLOCK_GETTIME: return "clock_gettime";
    case SYS_RT_SIGACTION: return "rt_sigaction";
    case SYS_RT_SIGPROCMASK: return "rt_sigprocmask";
    case SYS_UNAME: return "uname";
    case SYS_GETPID: return "getpid";
    case SYS_GETUID: return "getuid";
    case SYS_GETEUID: return "geteuid";
    case SYS_GETGID: return "getgid";
    case SYS_GETEGID: return "getegid";
    case SYS_GETTID: return "gettid";
    case SYS_BRK: return "brk";
    case SYS_MUNMAP: return "munmap";
    case SYS_MMAP: return "mmap";
    case SYS_MPROTECT: return "mprotect";
    case SYS_PRLIMIT64: return "prlimit64";
    case SYS_GETRANDOM: return "getrandom";
    case SYS_RSEQ: return "rseq";
    default: return "?";
    }
}

void syscall_init(uint64_t base)
{
    brk_base = base;
    brk_cur = base;
}

/* Writes guest bytes to host fd 1 or 2. Returns the byte count or -errno. */
static int64_t guest_write(int fd, uint64_t va, uint64_t count)
{
    uint8_t buf[4096];
    uint64_t done = 0;
    while (done < count) {
        size_t chunk = count - done > sizeof buf ? sizeof buf : (size_t)(count - done);
        if (mem_copy_out(buf, va + done, chunk) < 0)
            return done ? (int64_t)done : -E_FAULT;
        if (host_write(fd, buf, chunk) < 0)
            return done ? (int64_t)done : -E_IO;
        done += chunk;
    }
    return (int64_t)done;
}

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count)
{
    if (fd != 1 && fd != 2)
        return -E_BADF;
    return guest_write((int)fd, buf, count);
}

static int64_t sys_writev(uint64_t fd, uint64_t iov, uint64_t iovcnt)
{
    if (fd != 1 && fd != 2)
        return -E_BADF;
    if (iovcnt > IOV_MAX)
        return -E_INVAL;
    int64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        uint64_t ent[2];
        if (mem_copy_out(ent, iov + i * 16, 16) < 0)
            return total ? total : -E_FAULT;
        if (ent[1] == 0)
            continue;
        int64_t n = guest_write((int)fd, ent[0], ent[1]);
        if (n < 0)
            return total ? total : n;
        total += n;
    }
    return total;
}

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

static int64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags)
{
    if (!(flags & MAP_ANONYMOUS))
        return -E_NODEV;
    if (len == 0)
        return -E_INVAL;
    if (len > MEM_VA_LIMIT)
        return -E_NOMEM;
    len = page_up(len);
    unsigned p = (unsigned)(prot & 7);
    if (flags & MAP_FIXED) {
        if (addr & PAGE_MASK)
            return -E_INVAL;
        if (addr >= MEM_VA_LIMIT || len > MEM_VA_LIMIT - addr)
            return -E_NOMEM;
        mem_unmap(addr, len);
        if (mem_map(addr, len, p) < 0)
            return -E_NOMEM;
        return (int64_t)addr;
    }
    uint64_t base = mem_find_free(len, addr);
    if (!base)
        return -E_NOMEM;
    if (mem_map(base, len, p) < 0)
        return -E_NOMEM;
    return (int64_t)base;
}

static int64_t sys_munmap(uint64_t addr, uint64_t len)
{
    if ((addr & PAGE_MASK) || len == 0)
        return -E_INVAL;
    if (addr >= MEM_VA_LIMIT || len > MEM_VA_LIMIT - addr)
        return -E_INVAL;
    mem_unmap(addr, page_up(len));
    return 0;
}

static int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot)
{
    if (addr & PAGE_MASK)
        return -E_INVAL;
    if (len == 0)
        return 0;
    if (addr >= MEM_VA_LIMIT || page_up(len) > MEM_VA_LIMIT - addr)
        return -E_NOMEM;
    if (mem_protect(addr, page_up(len), (unsigned)(prot & 7)) < 0)
        return -E_NOMEM;
    return 0;
}

static int64_t sys_uname(uint64_t buf)
{
    /* struct utsname: six fields of 65 bytes. */
    char u[6 * 65];
    memset(u, 0, sizeof u);
    strcpy(u + 0 * 65, "Linux");
    strcpy(u + 1 * 65, "xiaoji");
    strcpy(u + 2 * 65, "6.1.0-xiaoji");
    strcpy(u + 3 * 65, "#1 xiaoji M0");
    strcpy(u + 4 * 65, "aarch64");
    strcpy(u + 5 * 65, "(none)");
    if (mem_copy_in(buf, u, sizeof u) < 0)
        return -E_FAULT;
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
        return -E_INVAL;
    uint64_t ts[2] = {ns / 1000000000ull, ns % 1000000000ull};
    if (mem_copy_in(tp, ts, sizeof ts) < 0)
        return -E_FAULT;
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
            return done ? (int64_t)done : -E_FAULT;
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
            return -E_FAULT;
    }
    return 0;
}

static void dispatch(cpu_t *c, uint64_t nr, const uint64_t a[6], int64_t *ret)
{
    switch (nr) {
    case SYS_WRITE:
        *ret = sys_write(a[0], a[1], a[2]);
        return;
    case SYS_WRITEV:
        *ret = sys_writev(a[0], a[1], a[2]);
        return;
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
        *ret = sys_mmap(a[0], a[1], a[2], a[3]);
        return;
    case SYS_MUNMAP:
        *ret = sys_munmap(a[0], a[1]);
        return;
    case SYS_MPROTECT:
        *ret = sys_mprotect(a[0], a[1], a[2]);
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
    case SYS_RT_SIGACTION:
        /* Zero the old action (32 bytes: handler, flags, restorer, mask). */
        *ret = (a[2] && mem_set(a[2], 0, 32) < 0) ? -E_FAULT : 0;
        return;
    case SYS_RT_SIGPROCMASK:
        *ret = (a[2] && mem_set(a[2], 0, 8) < 0) ? -E_FAULT : 0;
        return;
    case SYS_GETRANDOM:
        *ret = sys_getrandom(a[0], a[1]);
        return;
    case SYS_CLOCK_GETTIME:
        *ret = sys_clock_gettime(a[0], a[1]);
        return;
    case SYS_IOCTL:
        *ret = -E_NOTTY;
        return;
    case SYS_PRLIMIT64:
        *ret = sys_prlimit64(a[1], a[3]);
        return;
    case SYS_READLINKAT:
        *ret = -E_NOENT;
        return;
    case SYS_RSEQ:
        *ret = -E_NOSYS;
        return;
    default:
        if (nr < 1024) {
            uint64_t bit = 1ull << (nr & 63);
            if (unimpl_seen[nr >> 6] & bit) {
                *ret = -E_NOSYS;
                return;
            }
            unimpl_seen[nr >> 6] |= bit;
        }
        fprintf(stderr, "xiaoji: unimplemented syscall %llu\n", (unsigned long long)nr);
        *ret = -E_NOSYS;
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
