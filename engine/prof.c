/* Profiler: per-thread counts, sampled hot spots and spin-like events. See prof.h. */
#include "prof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "linux.h"
#include "loader.h"
#include "mem.h"
#include "sched.h"
#include "syscall.h"

/* Syscall numbers (asm-generic, as in syscall.c). */
#define SYS_FUTEX 98
#define SYS_NANOSLEEP 101
#define SYS_CLOCK_NANOSLEEP 115
#define SYS_SCHED_YIELD 124

#define SAMPLE_MASK 4095u /* one sample per 4096 instructions of a thread */
#define HOT_BITS 17
#define HOT_CAP (1u << HOT_BITS)
#define HOT_MAX_USED (HOT_CAP / 4 * 3)
#define TOP_HOT 40
#define TOP_MAP 20
#define NSYS 512

#define PATH_ANON (-1)  /* anonymous mapping */
#define PATH_STACK (-2) /* the guest stack */

typedef struct {
    uint64_t tid;
    char name[16];
    int ended;
    uint64_t icount; /* final count, once ended */
    uint64_t slices, full_slices, samples;
    uint64_t sys[NSYS];
    uint64_t ev[PE_COUNT];
} rec_t;

typedef struct {
    int nr;
    uint64_t n;
} sys_item;

typedef struct {
    uint64_t pc, tid, n;
    uint64_t base;  /* start of the mapping that holds pc */
    uint64_t foff;  /* file offset of pc, for file mappings */
    int path;       /* index into paths, or PATH_ANON / PATH_STACK */
} hot_t;

int prof_on;

static rec_t *recs;
static int nrecs, caprecs;
static rec_t *cache; /* the last record found: a thread runs many instructions in a row */

static hot_t *hot;
static uint64_t hot_used, hot_dropped, total_samples;

static char **paths;
static uint64_t *path_n;
static int npaths, cappaths;
static uint64_t anon_n, stack_n;

static uint64_t idle_ns, idle_waits;

static const char *ev_name[PE_COUNT] = {
    "wfe", "wfi", "yield", "isb", "ldxr", "stxr", "stxr_fail", "futex_wait", "futex_block",
    "futex_eagain", "futex_timeout", "sched_yield", "nanosleep",
};

static rec_t *find_rec(uint64_t tid, int create)
{
    if (cache && cache->tid == tid)
        return cache;
    for (int i = 0; i < nrecs; i++)
        if (recs[i].tid == tid)
            return cache = &recs[i];
    if (!create)
        return NULL;
    if (nrecs == caprecs) {
        int nc = caprecs ? caprecs * 2 : 16;
        rec_t *n = realloc(recs, (size_t)nc * sizeof *recs);
        if (!n)
            return NULL;
        recs = n;
        caprecs = nc;
        cache = NULL;
    }
    rec_t *r = &recs[nrecs++];
    memset(r, 0, sizeof *r);
    r->tid = tid;
    strcpy(r->name, "?");
    return cache = r;
}

static char *root_prefix; /* the --root directory, removed from mapping paths */

static int intern_path(const char *host)
{
    const char *p = host;
    if (root_prefix && !strncmp(host, root_prefix, strlen(root_prefix)) && host[strlen(root_prefix)] == '/')
        p = host + strlen(root_prefix);
    for (int i = 0; i < npaths; i++)
        if (!strcmp(paths[i], p))
            return i;
    if (npaths == cappaths) {
        int nc = cappaths ? cappaths * 2 : 64;
        char **np = realloc(paths, (size_t)nc * sizeof *paths);
        uint64_t *nn = realloc(path_n, (size_t)nc * sizeof *path_n);
        if (!np || !nn)
            return PATH_ANON;
        paths = np;
        path_n = nn;
        cappaths = nc;
    }
    size_t len = strlen(p) + 1;
    paths[npaths] = malloc(len);
    if (!paths[npaths])
        return PATH_ANON;
    memcpy(paths[npaths], p, len);
    path_n[npaths] = 0;
    return npaths++;
}

/* Maps pc to its mapping: the file path and offset, or anonymous. Called once per
 * hot spot, so the path is copied here (mapping pointers are not stable). */
static void resolve(hot_t *e, uint64_t pc)
{
    uint64_t start = 0, end = 0, off = 0;
    const char *path = NULL;
    e->base = 0;
    e->foff = 0;
    e->path = PATH_ANON;
    if (!mem_region_next(pc, &start, &end, &path, &off) || start > pc) {
        return;
    }
    e->base = start;
    if (!path) {
        uint64_t lo = 0, hi = 0;
        loader_stack_range(&lo, &hi);
        e->path = (pc >= lo && pc < hi) ? PATH_STACK : PATH_ANON;
        return;
    }
    e->path = intern_path(path);
    e->foff = off + (pc - start);
}

static void count_path(int path)
{
    if (path == PATH_ANON)
        anon_n++;
    else if (path == PATH_STACK)
        stack_n++;
    else if (path >= 0 && path < npaths)
        path_n[path]++;
}

void prof_sample(uint64_t tid, uint64_t pc)
{
    total_samples++;
    rec_t *r = find_rec(tid, 1);
    if (r)
        r->samples++;
    uint64_t h = (pc * 0x9E3779B97F4A7C15ull) ^ (tid * 0xC2B2AE3D27D4EB4Full);
    uint32_t i = (uint32_t)(h >> (64 - HOT_BITS));
    for (uint32_t probe = 0; probe < HOT_CAP; probe++, i = (i + 1) & (HOT_CAP - 1)) {
        hot_t *e = &hot[i];
        if (e->n == 0) {
            if (hot_used >= HOT_MAX_USED) {
                hot_dropped++;
                return;
            }
            e->pc = pc;
            e->tid = tid;
            resolve(e, pc);
            hot_used++;
        } else if (e->pc != pc || e->tid != tid) {
            continue;
        }
        e->n++;
        count_path(e->path);
        return;
    }
    hot_dropped++;
}

void prof_thread_new(uint64_t tid, const char *name)
{
    rec_t *r = find_rec(tid, 1);
    if (r && name) {
        memset(r->name, 0, sizeof r->name);
        strncpy(r->name, name, sizeof r->name - 1);
    }
}

void prof_thread_name(uint64_t tid, const char *name)
{
    prof_thread_new(tid, name);
}

void prof_thread_end(uint64_t tid, uint64_t icount)
{
    rec_t *r = find_rec(tid, 1);
    if (r) {
        r->ended = 1;
        r->icount = icount;
    }
}

void prof_slice(uint64_t tid, int full)
{
    rec_t *r = find_rec(tid, 1);
    if (!r)
        return;
    r->slices++;
    if (full)
        r->full_slices++;
}

void prof_idle(uint64_t ns)
{
    idle_ns += ns;
    idle_waits++;
}

void prof_syscall(uint64_t tid, uint64_t nr, const uint64_t a[6], int64_t ret, int blocked)
{
    rec_t *r = find_rec(tid, 1);
    if (!r)
        return;
    if (nr < NSYS)
        r->sys[nr]++;
    if (nr == SYS_FUTEX) {
        uint64_t op = a[1] & 0x7F; /* FUTEX_PRIVATE_FLAG and FUTEX_CLOCK_REALTIME masked off */
        if (op == 0 || op == 9) { /* FUTEX_WAIT, FUTEX_WAIT_BITSET */
            r->ev[PE_FUTEX_WAIT]++;
            if (blocked)
                r->ev[PE_FUTEX_BLOCK]++;
            else if (ret == -(int64_t)LE_AGAIN)
                r->ev[PE_FUTEX_EAGAIN]++;
            else if (ret == -(int64_t)LE_TIMEDOUT)
                r->ev[PE_FUTEX_TIMEOUT]++;
        }
    } else if (nr == SYS_SCHED_YIELD) {
        r->ev[PE_SCHED_YIELD]++;
    } else if (nr == SYS_NANOSLEEP || nr == SYS_CLOCK_NANOSLEEP) {
        r->ev[PE_NANOSLEEP]++;
    }
}

/* Classes of A64 instructions that the profiling loop counts. */
#define HINT_BASE 0xD503201Fu /* HINT #CRm:op2 encodes as base | CRm<<8 | op2<<5 */

void prof_pre(cpu_t *c, uint32_t insn, uint64_t pc)
{
    if ((c->icount & SAMPLE_MASK) == 0)
        prof_sample(c->tid, pc);
    rec_t *r = find_rec(c->tid, 1);
    if (!r)
        return;
    if ((insn & 0xFFFFF01Fu) == HINT_BASE) {
        unsigned crm = (insn >> 8) & 0xF, op2 = (insn >> 5) & 7;
        if (crm == 0 && op2 == 2)
            r->ev[PE_WFE]++;
        else if (crm == 0 && op2 == 3)
            r->ev[PE_WFI]++;
        else if (crm == 0 && op2 == 1)
            r->ev[PE_YIELD]++;
    } else if (insn == 0xD5033FDFu) {
        r->ev[PE_ISB]++;
    } else if (((insn >> 24) & 0x3F) == 0x08 && !((insn >> 23) & 1) && !((insn >> 21) & 1)) {
        /* exclusive, non-pair: LDXR/LDAXR (L=1) and STXR/STLXR (L=0) */
        if ((insn >> 22) & 1)
            r->ev[PE_LDXR]++;
        else
            r->ev[PE_STXR]++;
    }
}

void prof_post(cpu_t *c, uint32_t insn)
{
    /* STXR/STLXR (single register, not pair): a nonzero status means it failed. */
    if (((insn >> 24) & 0x3F) != 0x08 || ((insn >> 23) & 1) || ((insn >> 22) & 1) || ((insn >> 21) & 1))
        return;
    unsigned rs = (insn >> 16) & 31;
    if (rs == 31 || (uint32_t)c->x[rs] == 0)
        return;
    rec_t *r = find_rec(c->tid, 1);
    if (r)
        r->ev[PE_STXR_FAIL]++;
}

static uint64_t live_icount(const rec_t *r)
{
    if (r->ended)
        return r->icount;
    cpu_t *c = sched_find_thread(r->tid);
    return c ? c->icount : 0;
}

static int cmp_hot(const void *a, const void *b)
{
    const hot_t *x = a, *y = b;
    if (x->n != y->n)
        return x->n < y->n ? 1 : -1;
    return x->pc < y->pc ? -1 : x->pc > y->pc;
}

static int cmp_sys(const void *a, const void *b)
{
    const sys_item *x = a, *y = b;
    if (x->n != y->n)
        return x->n < y->n ? 1 : -1;
    return x->nr < y->nr ? -1 : x->nr > y->nr;
}

static const char *tname(uint64_t tid)
{
    rec_t *r = find_rec(tid, 0);
    return r ? r->name : "?";
}

static void print_loc(const hot_t *e)
{
    if (e->path == PATH_ANON)
        fprintf(stderr, "[anon] +0x%llx", (unsigned long long)(e->pc - e->base));
    else if (e->path == PATH_STACK)
        fprintf(stderr, "[stack] +0x%llx", (unsigned long long)(e->pc - e->base));
    else if (e->path >= 0 && e->path < npaths)
        fprintf(stderr, "%s +0x%llx", paths[e->path], (unsigned long long)e->foff);
    else
        fprintf(stderr, "[unknown]");
}

static void report(void)
{
    fflush(stdout);
    uint64_t total_instr = 0, slices = 0, full = 0;
    uint64_t ev_all[PE_COUNT];
    memset(ev_all, 0, sizeof ev_all);
    for (int i = 0; i < nrecs; i++) {
        total_instr += live_icount(&recs[i]);
        slices += recs[i].slices;
        full += recs[i].full_slices;
        for (int e = 0; e < PE_COUNT; e++)
            ev_all[e] += recs[i].ev[e];
    }

    fprintf(stderr, "xiaoji-prof: %d threads, %llu instructions, %llu slices (%llu used the whole budget)\n",
            nrecs, (unsigned long long)total_instr, (unsigned long long)slices, (unsigned long long)full);
    fprintf(stderr, "xiaoji-prof: host idle (all threads blocked, sleeping): %.3f s in %llu waits\n",
            (double)idle_ns / 1e9, (unsigned long long)idle_waits);

    fprintf(stderr, "\nxiaoji-prof: threads\n");
    fprintf(stderr, "  %-6s %-16s %14s %9s %9s %9s\n", "tid", "name", "instructions", "slices", "full", "samples");
    for (int i = 0; i < nrecs; i++) {
        rec_t *r = &recs[i];
        fprintf(stderr, "  %-6llu %-16s %14llu %9llu %9llu %9llu%s\n", (unsigned long long)r->tid, r->name,
                (unsigned long long)live_icount(r), (unsigned long long)r->slices,
                (unsigned long long)r->full_slices, (unsigned long long)r->samples,
                r->ended ? "" : "  (live at exit)");
        /* every syscall this thread made, most frequent first */
        fprintf(stderr, "      syscalls:");
        int shown = 0;
        sys_item items[NSYS];
        for (int n = 0; n < NSYS; n++)
            if (r->sys[n]) {
                items[shown].nr = n;
                items[shown].n = r->sys[n];
                shown++;
            }
        qsort(items, shown, sizeof items[0], cmp_sys);
        for (int k = 0; k < shown; k++) {
            const char *nm = syscall_name(items[k].nr);
            fprintf(stderr, " %s(%d)=%llu", nm ? nm : "?", items[k].nr, (unsigned long long)items[k].n);
        }
        if (!shown)
            fprintf(stderr, " none");
        fprintf(stderr, "\n      events:");
        for (int e = 0; e < PE_COUNT; e++)
            if (r->ev[e])
                fprintf(stderr, " %s=%llu", ev_name[e], (unsigned long long)r->ev[e]);
        fprintf(stderr, "\n");
    }

    fprintf(stderr, "\nxiaoji-prof: spin-like events, all threads\n");
    fprintf(stderr, "  wfe=%llu wfi=%llu yield=%llu isb=%llu  ldxr=%llu stxr=%llu stxr_fail=%llu\n",
            (unsigned long long)ev_all[PE_WFE], (unsigned long long)ev_all[PE_WFI],
            (unsigned long long)ev_all[PE_YIELD], (unsigned long long)ev_all[PE_ISB],
            (unsigned long long)ev_all[PE_LDXR], (unsigned long long)ev_all[PE_STXR],
            (unsigned long long)ev_all[PE_STXR_FAIL]);
    fprintf(stderr, "  futex_wait=%llu (blocked %llu, eagain %llu, timeout %llu) sched_yield=%llu nanosleep=%llu\n",
            (unsigned long long)ev_all[PE_FUTEX_WAIT], (unsigned long long)ev_all[PE_FUTEX_BLOCK],
            (unsigned long long)ev_all[PE_FUTEX_EAGAIN], (unsigned long long)ev_all[PE_FUTEX_TIMEOUT],
            (unsigned long long)ev_all[PE_SCHED_YIELD], (unsigned long long)ev_all[PE_NANOSLEEP]);

    /* Mappings, then hot spots. */
    double tot = total_samples ? (double)total_samples : 1.0;
    fprintf(stderr, "\nxiaoji-prof: samples by mapping (%llu samples, 1 per %u instructions per thread)\n",
            (unsigned long long)total_samples, SAMPLE_MASK + 1);
    for (int k = 0; k < TOP_MAP; k++) {
        uint64_t best = 0;
        int bi = -100;
        if (anon_n > best) { best = anon_n; bi = PATH_ANON; }
        if (stack_n > best) { best = stack_n; bi = PATH_STACK; }
        for (int i = 0; i < npaths; i++)
            if (path_n[i] > best) { best = path_n[i]; bi = i; }
        if (bi == -100 || !best)
            break;
        if (bi == PATH_ANON) {
            fprintf(stderr, "  %8llu %6.2f%%  [anon]\n", (unsigned long long)best, 100.0 * best / tot);
            anon_n = 0;
        } else if (bi == PATH_STACK) {
            fprintf(stderr, "  %8llu %6.2f%%  [stack]\n", (unsigned long long)best, 100.0 * best / tot);
            stack_n = 0;
        } else {
            fprintf(stderr, "  %8llu %6.2f%%  %s\n", (unsigned long long)best, 100.0 * best / tot, paths[bi]);
            path_n[bi] = 0;
        }
    }

    fprintf(stderr, "\nxiaoji-prof: top %d hot spots (pc, sampled)%s\n", TOP_HOT,
            hot_dropped ? " (table full: some samples dropped)" : "");
    fprintf(stderr, "  %4s %8s %7s %-16s %-18s %s\n", "rank", "samples", "pct", "thread", "pc", "mapping offset");
    qsort(hot, HOT_CAP, sizeof *hot, cmp_hot);
    for (int i = 0; i < TOP_HOT && hot[i].n; i++) {
        hot_t *e = &hot[i];
        fprintf(stderr, "  %4d %8llu %6.2f%% %-16s 0x%016llx ", i + 1, (unsigned long long)e->n,
                100.0 * e->n / tot, tname(e->tid), (unsigned long long)e->pc);
        print_loc(e);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "xiaoji-prof: end\n");
}

void prof_init(const char *root)
{
    const char *env = getenv("XIAOJI_PROFILE");
    if (!env || env[0] != '1')
        return;
    hot = calloc(HOT_CAP, sizeof *hot);
    if (!hot)
        return;
    if (root) {
        root_prefix = malloc(strlen(root) + 1);
        if (root_prefix)
            strcpy(root_prefix, root);
    }
    prof_on = 1;
    atexit(report);
}
