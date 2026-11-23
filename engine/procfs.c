/* Synthetic /proc/self/stat, /proc/self/maps, /proc/filesystems and
 * /sys/devices/system/cpu/online. Only these files are provided: they are what
 * the oracle run of ART opens. */
#include "procfs.h"
#include "selinuxfs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cgroupfs.h"
#include "linux.h"
#include "loader.h"
#include "mem.h"
#include "mntns.h"
#include "proc.h"
#include "signals.h"
#include "sched.h"
#include "vfs.h"

/* The guest pid. The main thread's tid is the process pid (see syscall.c). */

typedef struct {
    char *p;
    size_t n, cap;
    int oom;
} sbuf;

static void sb_add(sbuf *b, const char *s, size_t n)
{
    if (b->oom)
        return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->n + n + 1)
            cap *= 2;
        char *np = realloc(b->p, cap);
        if (!np) {
            b->oom = 1;
            return;
        }
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void sb_fmt(sbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    char *tmp = malloc((size_t)n + 1);
    if (!tmp) {
        b->oom = 1;
        return;
    }
    va_start(ap, fmt);
    vsnprintf(tmp, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb_add(b, tmp, (size_t)n);
    free(tmp);
}

/* "/proc/<digits>" optionally followed by "/rest" (rest is "" or starts with '/').
 * Returns 1 and sets *pid and *rest, or 0 (not a numbered entry). */
int procfs_pid_path(const char *guest, int *pid, const char **rest)
{
    if (strncmp(guest, "/proc/", 6))
        return 0;
    const char *q = guest + 6;
    if (*q < '0' || *q > '9')
        return 0;
    long v = 0;
    while (*q >= '0' && *q <= '9') {
        v = v * 10 + (*q - '0');
        if (v > (1L << 30))
            return 0;
        q++;
    }
    if (*q && *q != '/')
        return 0;
    *pid = (int)v;
    *rest = q;
    return 1;
}

/* A live (not yet reaped, not ended) process with this pid. */
static proc_t *live_proc(const char *guest, const char **rest)
{
    int pid;
    if (!procfs_pid_path(guest, &pid, rest))
        return NULL;
    proc_t *p = proc_find(pid);
    return p && !p->zombie && !p->exited ? p : NULL;
}

/* The PSI files (B9): /proc/pressure/memory, cpu and io. Their text is the kernel's format, with a some and a
 * full line; the engine never reports a stall, so every counter is zero. lmkd writes its trigger to the open
 * file (procfs_write), and that trigger never fires. */
static int pressure_file(const char *guest)
{
    return !strcmp(guest, "/proc/pressure/memory") || !strcmp(guest, "/proc/pressure/cpu") ||
           !strcmp(guest, "/proc/pressure/io");
}

/* /proc/self/oom_score_adj and /proc/<pid>/oom_score_adj: the process it names, or NULL. */
static proc_t *oom_target(const char *guest)
{
    if (!strcmp(guest, "/proc/self/oom_score_adj"))
        return proc_cur();
    const char *rest;
    proc_t *p = live_proc(guest, &rest);
    return p && !strcmp(rest, "/oom_score_adj") ? p : NULL;
}

/* The security context of the process (attr/current): the same for every process. */
static int is_self_attr(const char *guest)
{
    return !strcmp(guest, "/proc/self/attr/current") || !strcmp(guest, "/proc/thread-self/attr/current");
}

#define BOOT_ID_PATH "/proc/sys/kernel/random/boot_id"

int procfs_known(const char *guest)
{
    if (!strcmp(guest, BOOT_ID_PATH))
        return 1;
    if (!strcmp(guest, "/proc/self/stat") || !strcmp(guest, "/proc/self/status") || !strcmp(guest, "/proc/self/maps") ||
        !strcmp(guest, "/proc/filesystems") || !strcmp(guest, "/sys/devices/system/cpu/online") ||
        is_self_attr(guest) || !strcmp(guest, "/proc/mounts") || !strcmp(guest, "/proc/self/mounts") ||
        !strcmp(guest, "/proc/self/mountinfo") || !strcmp(guest, "/proc/meminfo") || pressure_file(guest) ||
        !strcmp(guest, "/proc/self/oom_score_adj") || !strcmp(guest, "/proc/self/cgroup"))
        return 1;
    const char *rest;
    if (live_proc(guest, &rest))
        return !strcmp(rest, "/stat") || !strcmp(rest, "/status") || !strcmp(rest, "/cmdline") ||
               !strcmp(rest, "/attr/current") || !strcmp(rest, "/mounts") || !strcmp(rest, "/mountinfo") ||
               !strcmp(rest, "/oom_score_adj") || !strcmp(rest, "/cgroup");
    return 0;
}

int procfs_writable(const char *guest)
{
    return pressure_file(guest) || oom_target(guest) != NULL;
}

/* A PSI trigger: "some <stall_us> <window_us>" or "full <stall_us> <window_us>" (the kernel's
 * psi_trigger_create, for the window range it accepts). The trigger is checked and then never fires. */
static int psi_trigger_ok(const uint8_t *buf, size_t n)
{
    size_t len = 0;
    while (len < n && buf[len])
        len++;
    char s[64];
    if (len == 0 || len >= sizeof s)
        return 0;
    memcpy(s, buf, len);
    s[len] = 0;
    char type[8];
    unsigned long long stall = 0, window = 0;
    if (sscanf(s, "%7s %llu %llu", type, &stall, &window) != 3)
        return 0;
    if (strcmp(type, "some") && strcmp(type, "full"))
        return 0;
    return window >= 500000ull && window <= 10000000ull && stall > 0 && stall <= window;
}

int64_t procfs_write(const char *guest, const uint8_t *buf, size_t n)
{
    if (pressure_file(guest)) {
        if (!psi_trigger_ok(buf, n))
            return -LE_INVAL;
        return (int64_t)n;
    }
    proc_t *p = oom_target(guest);
    if (!p)
        return -LE_BADF;
    /* A decimal from -1000 to 1000, with optional leading blanks and a trailing newline (procfs writes). */
    char s[16];
    size_t len = n < sizeof s - 1 ? n : sizeof s - 1;
    memcpy(s, buf, len);
    s[len] = 0;
    char *end = NULL;
    while (*s == ' ' || *s == '\t')
        memmove(s, s + 1, strlen(s));
    long v = strtol(s, &end, 10);
    if (end == s || (*end && *end != '\n' && *end != ' ') || v < -1000 || v > 1000)
        return -LE_INVAL;
    p->oom_score_adj = (int)v;
    if (sig_debug_faults()) /* the procprio evidence in a traced boot (B9) */
        fprintf(stderr, "xiaoji: oom_score_adj of pid %d is %d\n", p->pid, p->oom_score_adj);
    return (int64_t)n;
}

/* /proc/mounts and /proc/<pid>/mounts (and the mountinfo forms): the mount namespace of the
 * process (mntns.c). Returns 1 when guest is one of them, with *rc set. */
static int mounts_read(const char *guest, uint8_t **data, size_t *len, int *rc)
{
    const char *rest = NULL;
    proc_t *target = NULL;
    int info;
    if (!strcmp(guest, "/proc/mounts") || !strcmp(guest, "/proc/self/mounts"))
        info = 0;
    else if (!strcmp(guest, "/proc/self/mountinfo"))
        info = 1;
    else {
        target = live_proc(guest, &rest);
        if (!target || !rest)
            return 0;
        if (!strcmp(rest, "/mounts"))
            info = 0;
        else if (!strcmp(rest, "/mountinfo"))
            info = 1;
        else
            return 0;
    }
    mntns_t *ns = target ? vfs_fdtab_ns(target->fds) : vfs_ns();
    *rc = mntns_text(ns, info, data, len) ? -1 : 0;
    return 1;
}

int procfs_dir(const char *guest)
{
    const char *rest;
    if (live_proc(guest, &rest))
        return rest[0] == 0 || !strcmp(rest, "/attr") || !strcmp(rest, "/task");
    return !strcmp(guest, "/proc/self/task") || !strcmp(guest, "/sys/devices") || !strcmp(guest, "/sys/devices/system") ||
           !strcmp(guest, "/proc/sys") || !strcmp(guest, "/proc/sys/kernel") || !strcmp(guest, "/proc/sys/kernel/random") ||
           !strcmp(guest, "/sys/devices/system/cpu") || !strcmp(guest, "/proc/self/attr") ||
           !strcmp(guest, "/proc/thread-self") || !strcmp(guest, "/proc/thread-self/attr") ||
           !strcmp(guest, "/proc/pressure");
}

int procfs_task_dir(const char *guest)
{
    int pid = 0;
    const char *rest = NULL;
    if (!strcmp(guest, "/proc/self/task") || !strcmp(guest, "/proc/self/fd"))
        return 1;
    return procfs_pid_path(guest, &pid, &rest) && rest && !strcmp(rest, "/task");
}

/* The index-th entry of a /proc/<pid>/task (or /proc/self/task) directory: its thread id. */
int procfs_task_child(const char *guest, int index, const char **name)
{
    static char buf[24];
    int pid = 0;
    const char *rest = NULL;
    proc_t *p = NULL;
    if (!strcmp(guest, "/proc/self/task"))
        p = proc_cur();
    else if (procfs_pid_path(guest, &pid, &rest) && rest && !strcmp(rest, "/task"))
        p = proc_find(pid);
    if (!p || index < 0)
        return 0;
    cpu_t *ts[SCHED_MAX_THREADS];
    int n = sched_threads_of(p, ts, SCHED_MAX_THREADS);
    if (index >= n)
        return 0;
    snprintf(buf, sizeof buf, "%llu", (unsigned long long)ts[index]->tid);
    *name = buf;
    return 1;
}

/* The last component of a process's program path (its name, when it is not the caller). */
static const char *prog_base(const proc_t *p)
{
    const char *prog = vfs_prog_of(p->fds);
    const char *slash = strrchr(prog, '/');
    return slash ? slash + 1 : prog;
}

/* /proc/<pid>/status: the few fields a caller reads. */
static void build_status(sbuf *b, const proc_t *p)
{
    const proc_cred_t *c = &p->cred;
    sb_fmt(b, "Name:\t%s\nState:\tS (sleeping)\nTgid:\t%d\nPid:\t%d\nPPid:\t%d\n"
              "Uid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nGroups:\t",
           prog_base(p), p->pid, p->pid, p->ppid, c->ruid, c->euid, c->suid, c->fsuid, c->rgid, c->egid,
           c->sgid, c->fsgid);
    for (int i = 0; i < c->ngroups; i++)
        sb_fmt(b, "%u ", c->groups[i]);
    sb_fmt(b, "\nCapInh:\t%016llx\nCapPrm:\t%016llx\nCapEff:\t%016llx\nCapBnd:\t%016llx\nCapAmb:\t%016llx\n",
           (unsigned long long)c->cap_inh, (unsigned long long)c->cap_perm, (unsigned long long)c->cap_eff,
           (unsigned long long)p->capbset, (unsigned long long)c->cap_amb);
}

/* /proc/self/stat: the 52 fields of proc(5). f[k] is field k+4 (fields 1 to 3 are
 * printed before). Fields the engine keeps no state for are zero. */
static void build_stat(sbuf *b, const proc_t *p)
{
    long long f[49] = {0};
    cpu_t *list[SCHED_MAX_THREADS];
    int live = sched_threads_of(p, list, SCHED_MAX_THREADS);
    /* The address space is the running one's: for another process the sizes are zero. */
    int self = p == proc_cur();
    uint64_t vsize = 0, va = 0, s, e, off;
    const char *path;
    while (self && mem_region_next(va, &s, &e, &path, &off)) {
        vsize += e - s;
        va = e;
    }
    f[0] = p->ppid;                       /* 4 ppid */
    f[1] = p->pgid;                       /* 5 pgrp */
    f[2] = p->sid;                       /* 6 session */
    f[4] = -1;                              /* 8 tpgid */
    f[5] = 4194304;                         /* 9 flags */
    f[14] = 20;                             /* 18 priority */
    f[16] = live > 0 ? live : 1;            /* 20 num_threads */
    f[19] = (long long)vsize;               /* 23 vsize */
    f[20] = (long long)(vsize / MEM_PAGE_SIZE); /* 24 rss, in pages */
    f[34] = 17;                             /* 38 exit_signal (SIGCHLD) */
    /* 28 startstack: bionic's pthread_getattr_np finds the main stack by the mapping
     * that contains this address (in /proc/self/maps), so it must lie in the stack. */
    uint64_t slo, shi;
    loader_stack_range(&slo, &shi);
    f[24] = self ? (long long)(shi - 16) : 0;
    if (p == proc_cur())
        sb_fmt(b, "%d (%s) S", p->pid, sched_name());
    else
        sb_fmt(b, "%d (%s) S", p->pid, prog_base(p));
    for (int i = 0; i < 49; i++) {
        if (i == 21) /* 25 rsslim: unlimited */
            sb_fmt(b, " 18446744073709551615");
        else
            sb_fmt(b, " %lld", f[i]);
    }
    sb_fmt(b, "\n");
}

/* /proc/self/maps: one line per region, "start-end perms offset dev inode path". */
static void build_maps(sbuf *b)
{
    uint64_t va = 0, s, e, off, slo, shi;
    const char *path;
    loader_stack_range(&slo, &shi);
    while (mem_region_next(va, &s, &e, &path, &off)) {
        unsigned prot = mem_prot_at(s);
        char r = (prot & MEM_PROT_R) ? 'r' : '-';
        char w = (prot & MEM_PROT_W) ? 'w' : '-';
        char x = (prot & MEM_PROT_X) ? 'x' : '-';
        if (!path && s < shi && e > slo)
            path = "[stack]"; /* the main thread's stack; bionic finds it by this name */
        if (path)
            sb_fmt(b, "%llx-%llx %c%c%cp %08llx 00:00 0 %s\n", (unsigned long long)s,
                   (unsigned long long)e, r, w, x, (unsigned long long)off, path);
        else
            sb_fmt(b, "%llx-%llx %c%c%cp 00000000 00:00 0\n", (unsigned long long)s,
                   (unsigned long long)e, r, w, x);
        va = e;
    }
}

int procfs_read(const char *guest, uint8_t **data, size_t *len)
{
    int mrc;
    if (mounts_read(guest, data, len, &mrc))
        return mrc;
    sbuf b = {0};
    const char *rest;
    proc_t *target = live_proc(guest, &rest);
    if (target) {
        if (!strcmp(rest, "/stat"))
            build_stat(&b, target);
        else if (!strcmp(rest, "/status"))
            build_status(&b, target);
        else if (!strcmp(rest, "/cmdline")) {
            size_t n = 0;
            const uint8_t *bytes = vfs_cmdline_bytes(target->fds, &n);
            sb_add(&b, (const char *)bytes, n);
        } else if (!strcmp(rest, "/attr/current")) {
            sb_fmt(&b, "%s", target->attr_current);
        } else if (!strcmp(rest, "/oom_score_adj")) {
            sb_fmt(&b, "%d\n", target->oom_score_adj);
        } else if (!strcmp(rest, "/cgroup")) {
            /* The membership in each cgroup mount (cgroupfs.c, B11). */
            uint8_t *t = NULL;
            size_t tl = 0;
            if (cgroupfs_proc_text(target->pid, &t, &tl) < 0)
                return -1;
            sb_add(&b, (const char *)t, tl);
            free(t);
        } else {
            return -1;
        }
    } else if (!strcmp(guest, "/proc/self/stat")) {
        build_stat(&b, proc_cur());
    } else if (!strcmp(guest, "/proc/self/status")) {
        build_status(&b, proc_cur());
    } else if (!strcmp(guest, "/proc/self/cgroup")) {
        uint8_t *t = NULL;
        size_t tl = 0;
        if (cgroupfs_proc_text(proc_cur()->pid, &t, &tl) < 0)
            return -1;
        sb_add(&b, (const char *)t, tl);
        free(t);
    } else if (!strcmp(guest, "/proc/self/maps")) {
        build_maps(&b);
    } else if (!strcmp(guest, "/proc/filesystems")) {
        sb_fmt(&b, "nodev\tsysfs\nnodev\ttmpfs\nnodev\tproc\n\text4\n");
    } else if (!strcmp(guest, BOOT_ID_PATH)) {
        /* The kernel's boot UUID (random per boot in Linux). libcutils names /dev/ashmem<boot_id> from it
         * and falls back to /dev/ashmem when that node is absent (ashmem-dev.cpp). A fixed value keeps runs
         * the same. */
        sb_fmt(&b, "5c1d9a40-2f3e-4b7a-9e61-0d2c8f7a3b10\n");
    } else if (!strcmp(guest, "/sys/devices/system/cpu/online")) {
        sb_fmt(&b, "0\n");
    } else if (pressure_file(guest)) {
        /* A stall never happens in the engine: the averages and totals are zero (the kernel's text). */
        const char *line = " avg10=0.00 avg60=0.00 avg300=0.00 total=0\n";
        sb_fmt(&b, "some%sfull%s", line, line);
    } else if (!strcmp(guest, "/proc/self/oom_score_adj")) {
        sb_fmt(&b, "%d\n", proc_cur()->oom_score_adj);
    } else if (!strcmp(guest, "/proc/meminfo")) {
        /* An engine stub: a fixed 2 GiB guest with 1 GiB free and nothing cached or swapped. The
         * keys are the ones libmeminfo reads (system/memory/libmeminfo, kDefaultSysMemInfoTags). The
         * parser makes one read of up to 4095 bytes, so the file must stay under that. */
        sb_fmt(&b, "MemTotal:        2097152 kB\n"
                   "MemFree:         1048576 kB\n"
                   "MemAvailable:    1048576 kB\n"
                   "Buffers:               0 kB\n"
                   "Cached:                0 kB\n"
                   "Shmem:                 0 kB\n"
                   "Slab:                  0 kB\n"
                   "SReclaimable:          0 kB\n"
                   "SUnreclaim:            0 kB\n"
                   "KReclaimable:          0 kB\n"
                   "KernelStack:           0 kB\n"
                   "PageTables:            0 kB\n"
                   "VmallocUsed:           0 kB\n"
                   "Mapped:                0 kB\n"
                   "Active:                0 kB\n"
                   "Inactive:              0 kB\n"
                   "Unevictable:           0 kB\n"
                   "SwapTotal:             0 kB\n"
                   "SwapFree:              0 kB\n");
    } else if (is_self_attr(guest)) {
        sb_fmt(&b, "%s", proc_cur()->attr_current);
    } else {
        return -1;
    }
    if (b.oom) {
        free(b.p);
        return -1;
    }
    if (!b.p) {
        b.p = malloc(1);
        if (!b.p)
            return -1;
        b.p[0] = 0;
    }
    *data = (uint8_t *)b.p;
    *len = b.n;
    return 0;
}

/* The process whose attr/current a guest path names, or NULL. */
static proc_t *attr_owner(const char *guest)
{
    if (!strcmp(guest, "/proc/self/attr/current") || !strcmp(guest, "/proc/thread-self/attr/current"))
        return proc_cur();
    const char *rest;
    proc_t *p = live_proc(guest, &rest);
    return p && rest && !strcmp(rest, "/attr/current") ? p : NULL;
}

int procfs_is_attr_current(const char *guest) { return attr_owner(guest) != NULL; }

int64_t procfs_attr_write(const char *guest, const uint8_t *buf, size_t n)
{
    proc_t *p = attr_owner(guest);
    if (!p)
        return -LE_BADF;
    while (n > 0 && buf[n - 1] == 0)
        n--; /* the NUL that libselinux writes with the context */
    if (n == 0 || n >= PROC_ATTR_MAX)
        return -LE_INVAL;
    memcpy(p->attr_current, buf, n);
    p->attr_current[n] = 0;
    return (int64_t)n;
}
