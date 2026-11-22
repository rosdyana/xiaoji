/* Synthetic /proc/self/stat, /proc/self/maps, /proc/filesystems and
 * /sys/devices/system/cpu/online. Only these files are provided: they are what
 * the oracle run of ART opens. */
#include "procfs.h"
#include "selinuxfs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loader.h"
#include "mem.h"
#include "mntns.h"
#include "proc.h"
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

/* The security context of the process (attr/current): the same for every process. */
static int is_self_attr(const char *guest)
{
    return !strcmp(guest, "/proc/self/attr/current") || !strcmp(guest, "/proc/thread-self/attr/current");
}

int procfs_known(const char *guest)
{
    if (!strcmp(guest, "/proc/self/stat") || !strcmp(guest, "/proc/self/maps") ||
        !strcmp(guest, "/proc/filesystems") || !strcmp(guest, "/sys/devices/system/cpu/online") ||
        is_self_attr(guest) || !strcmp(guest, "/proc/mounts") || !strcmp(guest, "/proc/self/mounts") ||
        !strcmp(guest, "/proc/self/mountinfo"))
        return 1;
    const char *rest;
    if (live_proc(guest, &rest))
        return !strcmp(rest, "/stat") || !strcmp(rest, "/status") || !strcmp(rest, "/cmdline") ||
               !strcmp(rest, "/attr/current") || !strcmp(rest, "/mounts") || !strcmp(rest, "/mountinfo");
    return 0;
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
           !strcmp(guest, "/sys/devices/system/cpu") || !strcmp(guest, "/proc/self/attr") ||
           !strcmp(guest, "/proc/thread-self") || !strcmp(guest, "/proc/thread-self/attr");
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
    sb_fmt(b, "Name:\t%s\nState:\tS (sleeping)\nTgid:\t%d\nPid:\t%d\nPPid:\t%d\n"
              "Uid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\n",
           prog_base(p), p->pid, p->pid, p->ppid);
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
        } else {
            return -1;
        }
    } else if (!strcmp(guest, "/proc/self/stat")) {
        build_stat(&b, proc_cur());
    } else if (!strcmp(guest, "/proc/self/maps")) {
        build_maps(&b);
    } else if (!strcmp(guest, "/proc/filesystems")) {
        sb_fmt(&b, "nodev\tsysfs\nnodev\ttmpfs\nnodev\tproc\n\text4\n");
    } else if (!strcmp(guest, "/sys/devices/system/cpu/online")) {
        sb_fmt(&b, "0\n");
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
