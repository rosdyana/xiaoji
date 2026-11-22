/* Synthetic /proc/self/stat, /proc/self/maps, /proc/filesystems and
 * /sys/devices/system/cpu/online. Only these files are provided: they are what
 * the oracle run of ART opens. */
#include "procfs.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loader.h"
#include "mem.h"
#include "proc.h"
#include "sched.h"

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

int procfs_known(const char *guest)
{
    return !strcmp(guest, "/proc/self/stat") || !strcmp(guest, "/proc/self/maps") ||
           !strcmp(guest, "/proc/filesystems") || !strcmp(guest, "/sys/devices/system/cpu/online");
}

int procfs_dir(const char *guest)
{
    return !strcmp(guest, "/sys/devices") || !strcmp(guest, "/sys/devices/system") ||
           !strcmp(guest, "/sys/devices/system/cpu");
}

/* /proc/self/stat: the 52 fields of proc(5). f[k] is field k+4 (fields 1 to 3 are
 * printed before). Fields the engine keeps no state for are zero. */
static void build_stat(sbuf *b)
{
    long long f[49] = {0};
    cpu_t *list[SCHED_MAX_THREADS];
    int live = sched_threads(list, SCHED_MAX_THREADS);
    uint64_t vsize = 0, va = 0, s, e, off;
    const char *path;
    while (mem_region_next(va, &s, &e, &path, &off)) {
        vsize += e - s;
        va = e;
    }
    f[0] = proc_cur()->ppid;                       /* 4 ppid */
    f[1] = proc_cur()->pgid;                       /* 5 pgrp */
    f[2] = proc_cur()->sid;                       /* 6 session */
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
    f[24] = (long long)(shi - 16);
    sb_fmt(b, "%d (%s) S", proc_cur()->pid, sched_name());
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
    sbuf b = {0};
    if (!strcmp(guest, "/proc/self/stat")) {
        build_stat(&b);
    } else if (!strcmp(guest, "/proc/self/maps")) {
        build_maps(&b);
    } else if (!strcmp(guest, "/proc/filesystems")) {
        sb_fmt(&b, "nodev\tsysfs\nnodev\ttmpfs\nnodev\tproc\n\text4\n");
    } else if (!strcmp(guest, "/sys/devices/system/cpu/online")) {
        sb_fmt(&b, "0\n");
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
