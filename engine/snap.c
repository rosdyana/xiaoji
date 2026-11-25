/* Snapshots of the whole engine (docs/snapshot.md).
 *
 * The engine is one host thread that runs every guest thread as a green thread (sched.c), and all of its state
 * (processes, threads and their registers, the page tables and guest pages, fds and the objects behind them, binder,
 * properties, logd, timers, GL decoders) lives in the engine's own memory. A snapshot is an image of that memory,
 * taken at a slice boundary of the scheduler loop (snap_point), plus what is outside it:
 * - the host descriptors the engine holds (host.c records them: path, flags, offset; reopened at the same numbers);
 * - the writable trees under the guest root (/data, /cache, /metadata), which live in host files;
 * - the monotonic clock (the guest's goes on from the snapshot's time: host_snap_clock);
 * - caches of the host process: the TLBs and the translated code (emptied), the directory streams (vfs_dir_gen).
 * Its memory is, natively, the engine's data and bss sections and the arena (arena.c), which sit at fixed addresses in
 * a run with ASLR off (main re-executes itself so); in the wasm builds it is linear memory without the stack. An
 * image is therefore tied to the build that wrote it (snap_build_id).
 *
 * The file: a 4 KiB header, the writable trees (records: kind, mode, path, bytes), the extent table (address and
 * length of each run of non-zero pages, in address order), then the extents' bytes. The ranges in the header are
 * what the image covers; a restore zeroes what no extent holds. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#elif !defined(__EMSCRIPTEN__)
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "snap.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "arena.h"
#include "gl_host.h"
#include "init.h"
#include "host.h"
#include "mem.h"
#include "sched.h"
#include "vfs.h"
#include "mntns.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/heap.h>
#include <emscripten/stack.h>
#include "jit.h"
#else
#include <sys/mman.h>
#endif
#if defined(__APPLE__)
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#endif

#ifndef XJ_BUILD_ID
#define XJ_BUILD_ID "dev"
#endif
#if defined(XIAOJI_WEB)
#define FLAVOR "web"
#elif defined(__EMSCRIPTEN__)
#define FLAVOR "node"
#else
#define FLAVOR "native"
#endif

#define SNAP_MAGIC "XJSNAP\0\1"
#define SNAP_VERSION 1u
#define HDR_SIZE 4096u
#define PAGE 4096u
#define MAX_RANGES 8
#define F_INIT 1u /* the loop was in init mode */

typedef struct {
    char magic[8];
    uint32_t version, flags;
    char build[64];
    uint64_t mono_ns, real_ns, icount;
    uint64_t nranges;
    uint64_t range[MAX_RANGES][2];
    uint64_t nextents, ext_off, data_off, image_bytes;
    uint64_t tree_off, tree_len, tree_entries;
    char root[1024];
    char tree[64]; /* the root's fingerprint (tree_id); empty in the web build, whose page checks its tree itself */
} hdr_t;
_Static_assert(sizeof(hdr_t) <= HDR_SIZE, "header fits its block");

/* A record of the writable trees. */
typedef struct {
    uint8_t kind; /* 'd', 'f', 'l' */
    uint8_t pad[3];
    uint32_t mode;
    uint32_t plen; /* path bytes (no NUL), relative to the root, starting with '/' */
    uint32_t pad2;
    uint64_t dlen; /* file bytes, or the link target's */
} trec_t;

static const char *const tree_dirs[] = {"/data", "/cache", "/metadata"};

/* ---------- the trigger ---------- */

enum { T_NONE, T_TIME, T_ICOUNT, T_LOG, T_PROP };
static int trig_kind;
static uint64_t trig_n;       /* nanoseconds (time) or instructions (icount) */
static char trig_text[512];   /* log: the text; prop: NAME=VALUE */
static uint64_t trig_after_ns;
static uint64_t trig_hit_ns;  /* when the condition first held (0: not yet) */
static uint64_t trig_t0;      /* engine time at the start */
static char out_path[1024];
static char restore_path[1024]; /* the snapshot a restore read (its trees are read again by snap_resume) */
static int restored;

static void (*exit_fns[16])(void);
static int nexit_fns;

void snap_atexit(void (*fn)(void))
{
    if (nexit_fns < 16)
        exit_fns[nexit_fns++] = fn;
}

void snap_run_atexit(void)
{
    /* Last registered, first run, as atexit. */
    while (nexit_fns > 0)
        exit_fns[--nexit_fns]();
}

int snap_restored(void) { return restored; }

int snap_set_trigger(const char *cond)
{
    trig_t0 = host_monotonic_ns();
    trig_hit_ns = 0;
    if (!strncmp(cond, "time:", 5)) {
        char *end;
        double secs = strtod(cond + 5, &end);
        if (!cond[5] || *end || !(secs >= 0))
            return -1;
        trig_kind = T_TIME;
        trig_n = (uint64_t)(secs * 1e9);
        return 0;
    }
    if (!strncmp(cond, "icount:", 7)) {
        char *end;
        unsigned long long n = strtoull(cond + 7, &end, 10);
        if (!cond[7] || *end)
            return -1;
        trig_kind = T_ICOUNT;
        trig_n = n;
        return 0;
    }
    if (!strncmp(cond, "log:", 4) || !strncmp(cond, "prop:", 5)) {
        const char *v = strchr(cond, ':') + 1;
        if (!*v || strlen(v) >= sizeof trig_text || (cond[0] == 'p' && !strchr(v, '=')))
            return -1;
        trig_kind = cond[0] == 'l' ? T_LOG : T_PROP;
        strcpy(trig_text, v);
        return 0;
    }
    return -1;
}

void snap_set_after(uint64_t seconds) { trig_after_ns = seconds * 1000000000ull; }

/* --snapshot-quiet: the busy share allowed (PCT), the window it is measured over, the longest wait for one; the
 * current window's start and the engine's idle time then (sched.c). quiet_win_ns 0: no quiet wait. */
static unsigned quiet_pct;
static uint64_t quiet_win_ns, quiet_max_ns, quiet_t0, quiet_idle0;

int snap_set_quiet(const char *arg)
{
    quiet_win_ns = 0;
    quiet_t0 = 0;
    if (!arg)
        return 0;
    char *end;
    unsigned long pct = strtoul(arg, &end, 10);
    if (end == arg || *end != ':' || pct > 100)
        return -1;
    double win = strtod(end + 1, &end);
    if (*end != ':' || !(win > 0))
        return -1;
    double max = strtod(end + 1, &end);
    if (*end || !(max >= 0))
        return -1;
    quiet_pct = (unsigned)pct;
    quiet_win_ns = (uint64_t)(win * 1e9);
    quiet_max_ns = (uint64_t)(max * 1e9);
    quiet_t0 = 0;
    return 0;
}

/* After the condition and the wait: 1 when the snapshot may be taken now (no quiet wait, a quiet window, or the
 * longest wait over). A window that was too busy starts the next one. */
static int quiet_now(uint64_t now)
{
    if (!quiet_win_ns)
        return 1;
    uint64_t ready = trig_hit_ns + trig_after_ns;
    if (!quiet_t0 || quiet_t0 < ready) {
        quiet_t0 = now;
        quiet_idle0 = sched_idle_ns();
        return 0;
    }
    if (now - quiet_t0 < quiet_win_ns)
        return 0;
    uint64_t wall = now - quiet_t0, idle = sched_idle_ns() - quiet_idle0;
    unsigned busy = idle >= wall ? 0 : (unsigned)(100 * (wall - idle) / wall);
    if (busy <= quiet_pct || now - ready >= quiet_max_ns) {
        fprintf(stderr, "xiaoji: snapshot: %s: busy %u%% over the last %.1f s, %.1f s after the condition's wait\n",
                busy <= quiet_pct ? "quiet" : "no quiet window, the longest wait is over", busy, (double)wall / 1e9,
                (double)(now - ready) / 1e9);
        return 1;
    }
    quiet_t0 = now;
    quiet_idle0 = sched_idle_ns();
    return 0;
}

void snap_set_out(const char *path)
{
    size_t n = strlen(path);
    if (n >= sizeof out_path)
        n = sizeof out_path - 1;
    memcpy(out_path, path, n);
    out_path[n] = 0;
}

void snap_note_log(const char *msg)
{
    if (trig_kind == T_LOG && !trig_hit_ns && strstr(msg, trig_text))
        trig_hit_ns = host_monotonic_ns();
}

void snap_note_prop(const char *name, size_t nlen, const char *value, size_t vlen)
{
    if (trig_kind != T_PROP || trig_hit_ns)
        return;
    const char *eq = strchr(trig_text, '=');
    size_t tn = (size_t)(eq - trig_text);
    if (tn == nlen && !memcmp(trig_text, name, nlen) && strlen(eq + 1) == vlen && !memcmp(eq + 1, value, vlen))
        trig_hit_ns = host_monotonic_ns();
}

/* ---------- the image's ranges ---------- */

static int image_ranges(uint64_t r[MAX_RANGES][2])
{
    int n = 0;
#ifdef __EMSCRIPTEN__
    /* Linear memory: the static data (from 1024, Emscripten's global base) up to the stack, and the heap above the
     * stack up to the break. The stack is the running code's, not the image's. */
    uintptr_t slo = emscripten_stack_get_end(), shi = emscripten_stack_get_base();
    uintptr_t brk = *emscripten_get_sbrk_ptr();
    if (slo > 1024) {
        r[n][0] = 1024;
        r[n][1] = slo;
        n++;
    }
    r[n][0] = shi;
    r[n][1] = brk;
    n++;
#else
#if defined(__APPLE__)
    static const char *const sects[] = {"__data", "__bss", "__common"};
    for (int i = 0; i < 3; i++) {
        unsigned long size = 0;
        uint8_t *p = getsectiondata(&_mh_execute_header, "__DATA", sects[i], &size);
        if (p && size) {
            r[n][0] = (uint64_t)(uintptr_t)p;
            r[n][1] = (uint64_t)(uintptr_t)p + size;
            n++;
        }
    }
#else
    extern char __data_start[], _end[];
    r[n][0] = (uint64_t)(uintptr_t)__data_start;
    r[n][1] = (uint64_t)(uintptr_t)_end;
    n++;
#endif
    arena_range_t ar[2];
    int na = arena_ranges(ar, 2);
    if (na < 0)
        return -1;
    for (int i = 0; i < na; i++) {
        r[n][0] = ar[i].lo;
        r[n][1] = ar[i].hi;
        n++;
    }
#endif
    /* In address order, with touching ranges merged (the sections share pages). */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && r[j][0] < r[j - 1][0]; j--) {
            uint64_t t0 = r[j][0], t1 = r[j][1];
            r[j][0] = r[j - 1][0];
            r[j][1] = r[j - 1][1];
            r[j - 1][0] = t0;
            r[j - 1][1] = t1;
        }
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (m && r[i][0] <= r[m - 1][1]) {
            if (r[i][1] > r[m - 1][1])
                r[m - 1][1] = r[i][1];
            continue;
        }
        r[m][0] = r[i][0];
        r[m][1] = r[i][1];
        m++;
    }
    return m;
}

/* The guest root's fingerprint: the size and modification time of files every build of a tree changes. A restore under
 * another tree would read clean pages (dropped from the image) from different files, so it is refused. The web build
 * leaves it empty: its files are MEMFS nodes made fresh by each page load, and the page compares the tree manifest's
 * hash with the snapshot manifest's instead (web/worker.js). */
static void tree_id(const char *root, char out[64])
{
    out[0] = 0;
#ifndef XIAOJI_WEB
    static const char *const marks[] = {"/system/build.prop", "/system/framework/framework.jar", "/system/lib64/libc.so"};
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; root[0] && i < sizeof marks / sizeof marks[0]; i++) {
        char p[1200];
        struct stat st;
        snprintf(p, sizeof p, "%s%s", root, marks[i]);
        uint64_t v[2] = {0, 0};
        if (stat(p, &st) == 0) {
            v[0] = (uint64_t)st.st_size;
            v[1] = (uint64_t)st.st_mtime;
        }
        for (int k = 0; k < 2; k++)
            for (int b = 0; b < 8; b++)
                h = (h ^ ((v[k] >> (8 * b)) & 0xff)) * 1099511628211ull;
    }
    if (root[0])
        snprintf(out, 64, "%016llx", (unsigned long long)h);
#else
    (void)root;
#endif
}

/* The build that wrote an image: the sources' hash (the Makefile's XJ_BUILD_ID), the flavor, and where this file's
 * data sits (a check that the layout is the one the image was written with). */
static void snap_build_id(char out[64])
{
    snprintf(out, 64, "%s-%s-%llx", FLAVOR, XJ_BUILD_ID, (unsigned long long)(uintptr_t)&trig_kind);
}

/* ---------- writing ---------- */

static int out_fd = -1;

#ifdef XIAOJI_WEB
/* The web build writes its snapshot to the page's server (web/serve.mjs --snapshot-upload): out_path is a URL, and each
 * part is a synchronous POST of its bytes at their file offset (allowed in a worker). */
EM_JS(int, xj_snap_post, (const char *url, double off, const void *p, double len, int trunc), {
    var x = new XMLHttpRequest();
    x.open('POST', UTF8ToString(url) + '?off=' + off + (trunc ? "&truncate=1" : ""), false);
    try {
        /* p >>> 0: a C pointer at or above 2 GB arrives as a negative number (P1: the parts above 2 GB were posted
         * from the wrong place, and the restored image was corrupt there). */
        x.send(len ? HEAPU8.slice(p >>> 0, (p >>> 0) + len) : new Uint8Array(0));
    } catch (e) {
        return -1;
    }
    return x.status === 200 ? 0 : -1;
});

static int sink_open(void) { return xj_snap_post(out_path, 0, NULL, 0, 1) == 0 ? 0 : -1; }
static int sink_close(void) { return 0; }

static int put(uint64_t off, const void *p, uint64_t len)
{
    const uint8_t *b = p;
    while (len) { /* parts of at most 64 MiB */
        uint64_t n = len > (64u << 20) ? (64u << 20) : len;
        if (xj_snap_post(out_path, (double)off, b, (double)n, 0) < 0)
            return -1;
        b += n;
        off += n;
        len -= n;
    }
    return 0;
}

/* The page's loader (web/worker.js): the build id it compares with the snapshot manifest's, memory grown to the image's
 * top before it writes the image, and the mark that the image is in (snap_restore then reads only the header). */
static int image_preloaded;
EMSCRIPTEN_KEEPALIVE const char *xj_snap_build(void)
{
    static char b[64];
    snap_build_id(b);
    return b;
}
EMSCRIPTEN_KEEPALIVE int xj_snap_grow(double top)
{
    return (size_t)top <= emscripten_get_heap_size() || emscripten_resize_heap((size_t)top) ? 1 : 0;
}
EMSCRIPTEN_KEEPALIVE void xj_snap_preloaded(void) { image_preloaded = 1; }
#else
static int sink_open(void)
{
    out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    return out_fd < 0 ? -1 : 0;
}

static int sink_close(void)
{
    int rc = fsync(out_fd) != 0 || close(out_fd) != 0 ? -1 : 0;
    out_fd = -1;
    return rc;
}

static int put(uint64_t off, const void *p, uint64_t len)
{
    const uint8_t *b = p;
    while (len) {
        ssize_t k = pwrite(out_fd, b, len > (1u << 30) ? (1u << 30) : (size_t)len, (off_t)off);
        if (k <= 0)
            return -1;
        b += k;
        off += (uint64_t)k;
        len -= (uint64_t)k;
    }
    return 0;
}
#endif

static int page_zero(const uint8_t *p)
{
    const uint64_t *w = (const uint64_t *)p;
    for (unsigned i = 0; i < PAGE / 8; i += 4)
        if (w[i] | w[i + 1] | w[i + 2] | w[i + 3])
            return 0;
    return 1;
}

#ifndef __EMSCRIPTEN__
/* Natively, pages the host never touched are skipped without reading them (reading would make them resident). */
static int page_resident(uintptr_t a)
{
    static uintptr_t win = 1; /* the window of res[] (1: none) */
    static unsigned char res[256];
    long hp = sysconf(_SC_PAGESIZE);
    uintptr_t span = (uintptr_t)hp * sizeof res;
    uintptr_t w = a & ~(span - 1);
    if (w != win) {
        if (mincore((void *)w, span, (void *)res) != 0)
            memset(res, 1, sizeof res);
        win = w;
    }
    return getenv("XJ_NOMINCORE") ? 1 : res[(a - w) / (uintptr_t)hp] != 0;
}
#endif

/* Walks the ranges' pages: each run of non-zero pages is an extent. With write set, the extent table entries and
 * bytes are written; otherwise only counted. */
static int walk_extents(uint64_t r[MAX_RANGES][2], int nr, int write, uint64_t ext_off, uint64_t data_off,
                        uint64_t *n_out, uint64_t *bytes_out)
{
    uint64_t n = 0, bytes = 0;
    for (int i = 0; i < nr; i++) {
        uint64_t lo = r[i][0], a = lo & ~(uint64_t)(PAGE - 1);
        uint64_t end = r[i][1];
        uint64_t run = 0, run_len = 0;
        for (;; a += PAGE) {
            int nz = 0;
            if (a < end) {
#ifndef __EMSCRIPTEN__
                nz = page_resident((uintptr_t)a) && !page_zero((const uint8_t *)(uintptr_t)a);
#else
                nz = !page_zero((const uint8_t *)(uintptr_t)a);
#endif
            }
            if (nz && run_len && run_len < (64u << 20) && run + run_len == a) {
                run_len += PAGE;
                continue;
            }
            if (run_len) {
                /* The last page of a range may be partial: the extent stops at the range's end. */
                uint64_t len = run + run_len > end ? end - run : run_len;
                if (write) {
                    uint64_t e[2] = {run, len};
                    if (put(ext_off + n * 16, e, 16) < 0 || put(data_off + bytes, (const void *)(uintptr_t)run, len) < 0)
                        return -1;
                }
                n++;
                bytes += len;
                run_len = 0;
            }
            if (a >= end)
                break;
            if (nz) { /* an extent never starts before its range (the page may hold the previous range's end) */
                run = a < lo ? lo : a;
                run_len = a + PAGE - run;
            }
        }
    }
    *n_out = n;
    *bytes_out = bytes;
    return 0;
}

/* The writable trees, as records at off. */
static int tree_walk(const char *host, const char *rel, uint64_t *off, uint64_t *entries, uint8_t *buf, size_t cap,
                     int absolute)
{
    const uint8_t up = absolute ? 'A' - 'a' : 0; /* a record of a host path outside the root: kind in capitals */
    struct stat st;
    if (lstat(host, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    trec_t t;
    memset(&t, 0, sizeof t);
    t.mode = (uint32_t)st.st_mode & 07777;
    t.plen = (uint32_t)strlen(rel);
    if (S_ISLNK(st.st_mode)) {
        ssize_t k = readlink(host, (char *)buf, cap);
        if (k < 0)
            return -1;
        t.kind = (uint8_t)('l' + up);
        t.dlen = (uint64_t)k;
        if (put(*off, &t, sizeof t) < 0 || put(*off + sizeof t, rel, t.plen) < 0 ||
            put(*off + sizeof t + t.plen, buf, (uint64_t)k) < 0)
            return -1;
        *off += sizeof t + t.plen + (uint64_t)k;
        (*entries)++;
        return 0;
    }
    if (S_ISREG(st.st_mode)) {
        int fd = open(host, O_RDONLY);
        if (fd < 0)
            return -1;
        t.kind = (uint8_t)('f' + up);
        t.dlen = (uint64_t)st.st_size;
        if (put(*off, &t, sizeof t) < 0 || put(*off + sizeof t, rel, t.plen) < 0) {
            close(fd);
            return -1;
        }
        uint64_t at = *off + sizeof t + t.plen, got = 0;
        while (got < t.dlen) {
            ssize_t k = read(fd, buf, cap);
            if (k <= 0)
                break;
            if ((uint64_t)k > t.dlen - got)
                k = (ssize_t)(t.dlen - got);
            if (put(at + got, buf, (uint64_t)k) < 0) {
                close(fd);
                return -1;
            }
            got += (uint64_t)k;
        }
        close(fd);
        if (got < t.dlen) { /* shorter than its size (it shrank): pad with zeros, as the size says */
            memset(buf, 0, cap);
            while (got < t.dlen) {
                uint64_t k = t.dlen - got < cap ? t.dlen - got : cap;
                if (put(at + got, buf, k) < 0)
                    return -1;
                got += k;
            }
        }
        *off = at + t.dlen;
        (*entries)++;
        return 0;
    }
    if (!S_ISDIR(st.st_mode))
        return 0; /* sockets and fifos have no bytes to keep */
    t.kind = (uint8_t)('d' + up);
    if (put(*off, &t, sizeof t) < 0 || put(*off + sizeof t, rel, t.plen) < 0)
        return -1;
    *off += sizeof t + t.plen;
    (*entries)++;
    DIR *d = opendir(host);
    if (!d)
        return -1;
    struct dirent *de;
    int rc = 0;
    while (rc == 0 && (de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        size_t hl = strlen(host), rl = strlen(rel), nl = strlen(de->d_name);
        char *h2 = malloc(hl + nl + 2), *r2 = malloc(rl + nl + 2);
        if (!h2 || !r2) {
            free(h2);
            free(r2);
            rc = -1;
            break;
        }
        snprintf(h2, hl + nl + 2, "%s/%s", host, de->d_name);
        snprintf(r2, rl + nl + 2, "%s/%s", rel, de->d_name);
        rc = tree_walk(h2, r2, off, entries, buf, cap, absolute);
        free(h2);
        free(r2);
    }
    closedir(d);
    return rc;
}

#if defined(__APPLE__)
#include <dlfcn.h>
#include <malloc/malloc.h>
#include <pthread.h>
/* XIAOJI_SNAP_CHECK=1 (native, macOS): every word of the image outside the page pool that points outside it (into a
 * library, the system allocator or the host stack) is reported: such a pointer would dangle in another process. */
static void closure_check(uint64_t r[MAX_RANGES][2], int nr)
{
    Dl_info self;
    if (!dladdr((void *)closure_check, &self))
        return;
    pthread_t th = pthread_self();
    uintptr_t st_hi = (uintptr_t)pthread_get_stackaddr_np(th), st_lo = st_hi - pthread_get_stacksize_np(th);
    uint64_t hits = 0, words = 0;
    for (int i = 0; i < nr; i++) {
        for (uint64_t a = (r[i][0] + 7) & ~7ull; a + 8 <= r[i][1]; a += 8) {
            if ((a & (PAGE - 1)) == 0) {
                if (mem_pool_contains((void *)(uintptr_t)a) || !page_resident((uintptr_t)a)) {
                    a += PAGE - 8;
                    continue;
                }
            }
            uint64_t v = *(const uint64_t *)(uintptr_t)a;
            words++;
            if (v < 0x100000000ull || v >= 0x800000000000ull)
                continue;
            int inside = 0;
            for (int k = 0; k < nr; k++)
                if (v >= r[k][0] && v < r[k][1])
                    inside = 1;
            if (inside)
                continue;
            const char *why = NULL;
            Dl_info di;
            if (v >= st_lo && v < st_hi)
                why = "host stack";
            else if (dladdr((void *)(uintptr_t)v, &di) && di.dli_fbase != self.dli_fbase)
                why = di.dli_fname ? di.dli_fname : "a library";
            else if (malloc_zone_from_ptr((void *)(uintptr_t)v))
                why = "system malloc";
            if (!why)
                continue;
            if (hits < 20) {
                Dl_info where;
                const char *sym = dladdr((void *)(uintptr_t)a, &where) && where.dli_sname ? where.dli_sname : "heap";
                fprintf(stderr, "xiaoji: snapshot: check: 0x%llx (%s) holds 0x%llx: %s\n", (unsigned long long)a, sym,
                        (unsigned long long)v, why);
            }
            hits++;
        }
    }
    fprintf(stderr, "xiaoji: snapshot: check: %llu words, %llu point outside the image\n", (unsigned long long)words,
            (unsigned long long)hits);
}
#endif

static void census(const hdr_t *h)
{
    uint64_t st[7], procs, pool, pool_free;
    sched_census(st, &procs);
    mem_pool_counts(&pool, &pool_free);
    fprintf(stderr,
            "xiaoji: snapshot: %llu processes; threads: %llu runnable, %llu futex, %llu sleep, %llu sigwait, %llu "
            "restart, %llu vfork, %llu suspend\n",
            (unsigned long long)procs, (unsigned long long)st[0], (unsigned long long)st[1], (unsigned long long)st[2],
            (unsigned long long)st[3], (unsigned long long)st[4], (unsigned long long)st[5], (unsigned long long)st[6]);
    sched_census_rss();
    fprintf(stderr, "xiaoji: snapshot: page pool %llu pages (%llu MiB), %llu free; host fds %d\n",
            (unsigned long long)pool, (unsigned long long)(pool * PAGE >> 20), (unsigned long long)pool_free,
            host_snap_fd_count());
    for (uint64_t i = 0; i < h->nranges; i++)
        fprintf(stderr, "xiaoji: snapshot: range %llu: 0x%llx..0x%llx (%llu MiB)\n", (unsigned long long)i,
                (unsigned long long)h->range[i][0], (unsigned long long)h->range[i][1],
                (unsigned long long)((h->range[i][1] - h->range[i][0]) >> 20));
}

/* Writes the snapshot. 0, or -1. */
static int take(void)
{
    hdr_t *h = calloc(1, HDR_SIZE);
    size_t cap = 1u << 16;
    uint8_t *buf = malloc(cap);
    if (!h || !buf)
        return -1;
    memcpy(h->magic, SNAP_MAGIC, 8);
    h->version = SNAP_VERSION;
    h->flags = sched_is_init_mode() ? F_INIT : 0;
    snap_build_id(h->build);
    snprintf(h->root, sizeof h->root, "%s", vfs_host_root());
    tree_id(h->root, h->tree);

    if (sink_open() < 0) {
        fprintf(stderr, "xiaoji: snapshot: cannot write %s: %s\n", out_path, strerror(errno));
        return -1;
    }
    /* Pages that are a copy of their file are not kept: they come from the file again (the chunk store, on the web). */
    uint64_t clean = 0, dirty = 0;
#ifdef __EMSCRIPTEN__
    jit_snap_mark(); /* the pre-hot set (jit.c), before the drop invalidates the blocks on file pages */
#endif
    if (!getenv("XIAOJI_SNAP_KEEP_CLEAN"))
        mem_snap_drop_clean(&clean, &dirty);
    fprintf(stderr, "xiaoji: snapshot: file pages: %llu dropped (as the file has them, %llu MiB), %llu kept (written)\n",
            (unsigned long long)clean, (unsigned long long)(clean * PAGE >> 20), (unsigned long long)dirty);
    uint64_t kept_files = 0, kept_bytes = 0;
    if (host_snap_prepare(&kept_files, &kept_bytes) < 0) {
        fprintf(stderr, "xiaoji: snapshot: cannot record the host descriptors\n");
        return -1;
    }

    /* The writable trees, after the header. */
    h->tree_off = HDR_SIZE;
    uint64_t off = h->tree_off;
    char host[1100];
    /* Only a run with --root has trees of its own (the host's root is never captured or replaced). */
    for (size_t i = 0; h->root[0] && i < sizeof tree_dirs / sizeof tree_dirs[0]; i++) {
        snprintf(host, sizeof host, "%s%s", h->root, tree_dirs[i]);
        if (tree_walk(host, tree_dirs[i], &off, &h->tree_entries, buf, cap, 0) < 0) {
            fprintf(stderr, "xiaoji: snapshot: cannot read the tree %s\n", host);
            return -1;
        }
    }
    /* The tmpfs mounts (/data_mirror): host directories under /tmp, recorded by their host path. */
    const char *td;
    for (int i = 0; (td = mntns_tmpfs_dir(i)) != NULL; i++)
        if (tree_walk(td, td, &off, &h->tree_entries, buf, cap, 1) < 0) {
            fprintf(stderr, "xiaoji: snapshot: cannot read the tmpfs %s\n", td);
            return -1;
        }
    h->tree_len = off - h->tree_off;
    free(buf);
    gl_host_snap_save(); /* the WebGL objects, as a blob in engine memory (gl_webgl.c, gl_snap.js) */

    /* From here on nothing allocates: the image must be the memory as it is now. */
    mem_pool_zero_free();
    uint64_t heap_zeroed = arena_zero_free();
    h->mono_ns = host_monotonic_ns();
    h->real_ns = host_realtime_ns();
    h->icount = sched_icount();
    int nr = image_ranges(h->range);
    if (nr < 0) {
        fprintf(stderr, "xiaoji: snapshot: the heap is not at its fixed address\n");
        return -1;
    }
    h->nranges = (uint64_t)nr;
    census(h);
    uint64_t hu, hf;
    arena_heap_stats(&hu, &hf);
    fprintf(stderr, "xiaoji: snapshot: heap %llu MiB in use, %llu MiB free (%llu MiB of free chunks zeroed); guest and memfd "
            "pages not zero: %llu MiB\n", (unsigned long long)(hu >> 20), (unsigned long long)(hf >> 20),
            (unsigned long long)(heap_zeroed >> 20), (unsigned long long)(mem_pool_nonzero() * PAGE >> 20));
#if defined(__APPLE__)
    if (getenv("XIAOJI_SNAP_CHECK"))
        closure_check(h->range, nr);
#endif
    fprintf(stderr, "xiaoji: snapshot: host files kept (unlinked or replaced): %llu (%llu bytes); trees %llu entries, "
            "%llu MiB\n", (unsigned long long)kept_files, (unsigned long long)kept_bytes,
            (unsigned long long)h->tree_entries, (unsigned long long)(h->tree_len >> 20));
    uint64_t n, bytes;
    if (walk_extents(h->range, nr, 0, 0, 0, &n, &bytes) < 0)
        return -1;
    h->nextents = n;
    h->ext_off = (h->tree_off + h->tree_len + 15) & ~15ull;
    h->data_off = (h->ext_off + n * 16 + PAGE - 1) & ~(uint64_t)(PAGE - 1);
    h->image_bytes = bytes;
    uint64_t n2, bytes2;
    if (walk_extents(h->range, nr, 1, h->ext_off, h->data_off, &n2, &bytes2) < 0 || n2 != n || bytes2 != bytes) {
        fprintf(stderr, "xiaoji: snapshot: the image changed while it was written\n");
        return -1;
    }
    if (put(0, h, HDR_SIZE) < 0 || sink_close() < 0)
        return -1;
    double took = (double)(host_monotonic_ns() - h->mono_ns) / 1e9;
    out_fd = -1;
    fprintf(stderr, "xiaoji: snapshot: wrote %s: %llu extents, %llu MiB of memory, at %.3f s, %llu instructions, in %.3f s\n",
            out_path, (unsigned long long)n, (unsigned long long)(bytes >> 20), (double)(h->mono_ns - trig_t0) / 1e9,
            (unsigned long long)h->icount, took);
    return 0;
}

int snap_deadline(uint64_t *dl)
{
    if (trig_kind == T_NONE)
        return 0;
    if (trig_hit_ns && quiet_win_ns && quiet_t0 >= trig_hit_ns + trig_after_ns)
        *dl = quiet_t0 + quiet_win_ns; /* the end of the quiet window being measured */
    else if (trig_hit_ns)
        *dl = trig_hit_ns + trig_after_ns;
    else if (trig_kind == T_TIME)
        *dl = trig_t0 + trig_n;
    else
        return 0;
    return 1;
}

void snap_point(void)
{
    if (!trig_kind)
        return;
    uint64_t now = host_monotonic_ns();
    if (!trig_hit_ns) {
        if ((trig_kind == T_TIME && now - trig_t0 >= trig_n) ||
            (trig_kind == T_ICOUNT && sched_icount() >= trig_n))
            trig_hit_ns = now;
        else
            return;
    }
    if (now - trig_hit_ns < trig_after_ns || !quiet_now(now))
        return;
    trig_kind = T_NONE;
    fflush(stdout);
    fflush(stderr);
    int rc = take();
    exit(rc == 0 ? 0 : 1);
}

/* ---------- restoring ---------- */

static int get(int fd, uint64_t off, void *p, uint64_t len)
{
    uint8_t *b = p;
    while (len) {
        ssize_t k = pread(fd, b, len > (1u << 30) ? (1u << 30) : (size_t)len, (off_t)off);
        if (k <= 0)
            return -1;
        b += k;
        off += (uint64_t)k;
        len -= (uint64_t)k;
    }
    return 0;
}

/* Zeroes [a, b) of the image's memory. Natively the arena gets fresh pages; the sections are cleared. */
static void zero_span(uint64_t a, uint64_t b)
{
    if (b <= a)
        return;
#ifndef __EMSCRIPTEN__
    uint64_t pa = (a + 16383) & ~16383ull, pb = b & ~16383ull;
    if (pb > pa && arena_zero((uintptr_t)pa, (uintptr_t)pb) == 0) {
        memset((void *)(uintptr_t)a, 0, (size_t)(pa - a));
        memset((void *)(uintptr_t)pb, 0, (size_t)(b - pb));
        return;
    }
#endif
    memset((void *)(uintptr_t)a, 0, (size_t)(b - a));
}

/* The header of the restored snapshot, for snap_resume (set once the image is in: this variable is part of it). */
static hdr_t snap_hdr;
static uint64_t restore_load_ns; /* the time the image took to read in */

int snap_restore(const char *path)
{
    /* Only the stack is safe while the image is read in: everything else is about to be replaced. */
    hdr_t h;
    char build[64];
    uint64_t t_start = host_monotonic_ns();
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "xiaoji: restore: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (get(fd, 0, &h, sizeof h) < 0 || memcmp(h.magic, SNAP_MAGIC, 8) != 0 || h.version != SNAP_VERSION) {
        fprintf(stderr, "xiaoji: restore: %s is not a snapshot of this version\n", path);
        close(fd);
        return -1;
    }
    snap_build_id(build);
    if (strncmp(build, h.build, sizeof build) != 0) {
        fprintf(stderr, "xiaoji: restore: %s was written by another engine build (%.64s; this is %s)\n", path, h.build,
                build);
        close(fd);
        return -1;
    }
    char tid[64];
    tree_id(h.root, tid);
    if (strncmp(tid, h.tree, sizeof tid) != 0) {
        fprintf(stderr, "xiaoji: restore: the guest root %s is not the tree the snapshot was made from (%.64s; now %s)\n",
                h.root, h.tree, tid);
        close(fd);
        return -1;
    }
    uint64_t r[MAX_RANGES][2];
    int nr = image_ranges(r);
    if (nr < 0 || (uint64_t)nr != h.nranges || r[0][0] != h.range[0][0]) {
        fprintf(stderr, "xiaoji: restore: the memory layout differs from the snapshot's\n");
        close(fd);
        return -1;
    }
#ifdef __EMSCRIPTEN__
    /* The heap must reach the image's break before it is written. */
    uint64_t top = h.range[h.nranges - 1][1];
    if (top > (uint64_t)emscripten_get_heap_size() && !emscripten_resize_heap((size_t)top)) {
        fprintf(stderr, "xiaoji: restore: cannot grow memory to %llu bytes\n", (unsigned long long)top);
        close(fd);
        return -1;
    }
#endif
#ifdef XIAOJI_WEB
    if (image_preloaded) { /* the page wrote the image into memory before main (web/worker.js) */
        close(fd);
        goto loaded;
    }
#endif
    /* From here the engine's memory is being replaced: a failure ends the run. */
    uint64_t e[64][2];
    uint64_t ei = 0, nb = 0, data = h.data_off;
    uint64_t ri = 0, cur = h.range[0][0];
    while (ei < h.nextents || ri < h.nranges) {
        if (nb == 0 && ei < h.nextents) {
            uint64_t want = h.nextents - ei < 64 ? h.nextents - ei : 64;
            if (get(fd, h.ext_off + ei * 16, e, want * 16) < 0)
                goto fail;
            nb = want;
        }
        if (ei < h.nextents) {
            uint64_t ea = e[ei % 64][0], el = e[ei % 64][1];
            /* Zero the ranges (and the gap in the current one) below this extent. */
            while (ri < h.nranges && ea >= h.range[ri][1]) {
                zero_span(cur, h.range[ri][1]);
                ri++;
                cur = ri < h.nranges ? h.range[ri][0] : 0;
            }
            if (ri >= h.nranges)
                goto fail;
            zero_span(cur, ea);
            if (get(fd, data, (void *)(uintptr_t)ea, el) < 0)
                goto fail;
            data += el;
            cur = ea + el;
            ei++;
            nb--;
        } else {
            zero_span(cur, h.range[ri][1]);
            ri++;
            cur = ri < h.nranges ? h.range[ri][0] : 0;
        }
    }
    if (getenv("XIAOJI_SNAP_VERIFY")) {
        /* Debug: every extent reads back as the file has it. */
        uint8_t vb[4096];
        uint64_t vd = h.data_off, bad = 0;
        for (uint64_t k = 0; k < h.nextents; k++) {
            uint64_t ve[2];
            if (get(fd, h.ext_off + k * 16, ve, 16) < 0)
                goto fail;
            for (uint64_t o = 0; o < ve[1]; o += sizeof vb) {
                uint64_t m = ve[1] - o < sizeof vb ? ve[1] - o : sizeof vb;
                if (get(fd, vd + o, vb, m) < 0)
                    goto fail;
                if (memcmp(vb, (void *)(uintptr_t)(ve[0] + o), m) != 0) {
                    uint64_t z = 0;
                    while (z < m && vb[z] == ((uint8_t *)(uintptr_t)(ve[0] + o))[z])
                        z++;
                    fprintf(stderr, "xiaoji: restore: verify: 0x%llx differs (file %02x, memory %02x)\n",
                            (unsigned long long)(ve[0] + o + z), vb[z], ((uint8_t *)(uintptr_t)(ve[0] + o))[z]);
                    bad++;
                }
            }
            vd += ve[1];
        }
        fprintf(stderr, "xiaoji: restore: verify: %llu extents, %llu differ\n", (unsigned long long)h.nextents,
                (unsigned long long)bad);
    }
    close(fd);
#ifdef XIAOJI_WEB
loaded:
#endif
    snap_hdr = h; /* the writable trees are read from the file by snap_resume */
    /* The clock goes on from the snapshot's time before anything reads it: main's options (the input script's origin,
     * the stats and dump times, a new snapshot condition) are applied next. */
    uint64_t t_loaded = host_monotonic_ns();
    host_snap_clock(h.mono_ns);
    restore_load_ns = t_loaded - t_start;
    snprintf(restore_path, sizeof restore_path, "%s", path);
    restored = 1;
    trig_kind = T_NONE;
    return 0;
fail:
    fprintf(stderr, "xiaoji: restore: %s is truncated or unreadable; the run cannot go on\n", path);
    _exit(1);
}

/* Removes everything under dir (not dir itself), without following links. */
static void wipe(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        size_t n = strlen(dir) + strlen(de->d_name) + 2;
        char *p = malloc(n);
        if (!p)
            continue;
        snprintf(p, n, "%s/%s", dir, de->d_name);
        struct stat st;
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            wipe(p);
            rmdir(p);
        } else {
            unlink(p);
        }
        free(p);
    }
    closedir(d);
}

/* The writable trees of the snapshot, put back under the root. */
static int tree_restore(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    const char *root = vfs_host_root();
    if (!root[0]) { /* no --root: the snapshot has no trees, and the host's root is never touched */
        close(fd);
        return snap_hdr.tree_len ? -1 : 0;
    }
    for (size_t i = 0; i < sizeof tree_dirs / sizeof tree_dirs[0]; i++) {
        char *p = malloc(strlen(root) + strlen(tree_dirs[i]) + 1);
        if (!p)
            break;
        sprintf(p, "%s%s", root, tree_dirs[i]);
        wipe(p);
        free(p);
    }
    size_t cap = 1u << 16;
    uint8_t *buf = malloc(cap);
    int rc = buf ? 0 : -1;
    uint64_t off = snap_hdr.tree_off, end = snap_hdr.tree_off + snap_hdr.tree_len;
    while (rc == 0 && off < end) {
        trec_t t;
        if (get(fd, off, &t, sizeof t) < 0) {
            rc = -1;
            break;
        }
        int absolute = t.kind >= 'A' && t.kind <= 'Z'; /* a tmpfs's host directory: its own path */
        if (absolute)
            t.kind = (uint8_t)(t.kind - 'A' + 'a');
        size_t rl = absolute ? 0 : strlen(root);
        size_t hl = rl + t.plen + 1;
        char *hp = malloc(hl);
        if (!hp) {
            rc = -1;
            break;
        }
        memcpy(hp, root, rl);
        if (get(fd, off + sizeof t, hp + rl, t.plen) < 0) {
            free(hp);
            rc = -1;
            break;
        }
        hp[hl - 1] = 0;
        uint64_t dat = off + sizeof t + t.plen;
        if (t.kind == 'd') {
            const char *sl = strrchr(hp, '/');
            if (absolute && sl && sl - hp == 4 && !strncmp(hp, "/tmp/", 5))
                wipe(hp); /* a tmpfs's top: whatever an earlier run left there goes */
            if (mkdir(hp, (mode_t)(t.mode | 0700)) != 0 && errno != EEXIST)
                rc = -1;
            chmod(hp, (mode_t)(t.mode | 0700));
        } else if (t.kind == 'l') {
            char *tg = malloc(t.dlen + 1);
            if (!tg || get(fd, dat, tg, t.dlen) < 0) {
                rc = -1;
            } else {
                tg[t.dlen] = 0;
                unlink(hp);
                if (symlink(tg, hp) != 0)
                    rc = -1;
            }
            free(tg);
        } else if (t.kind == 'f') {
            int o = open(hp, O_WRONLY | O_CREAT | O_TRUNC, (mode_t)(t.mode | 0600));
            if (o < 0) {
                rc = -1;
            } else {
                for (uint64_t got = 0; rc == 0 && got < t.dlen;) {
                    uint64_t k = t.dlen - got < cap ? t.dlen - got : cap;
                    if (get(fd, dat + got, buf, k) < 0 || write(o, buf, (size_t)k) != (ssize_t)k)
                        rc = -1;
                    got += k;
                }
                close(o);
                chmod(hp, (mode_t)(t.mode | 0600));
            }
        } else {
            rc = -1;
        }
        if (rc)
            fprintf(stderr, "xiaoji: restore: cannot make %s\n", hp);
        free(hp);
        off = dat + (t.kind == 'd' ? 0 : t.dlen);
    }
    free(buf);
    close(fd);
    return rc;
}

int snap_resume(uint64_t init_timeout_s)
{
    vfs_dir_gen++;
    mem_tlb_reset();
#ifdef __EMSCRIPTEN__
    jit_snap_reset();
#endif
    if (tree_restore(restore_path) < 0) {
        fprintf(stderr, "xiaoji: restore: the writable trees could not be put back\n");
        return 1;
    }
    /* The directories and links init's rc made outside the trees (a page's tree has only the manifest's). */
    if (snap_hdr.flags & F_INIT)
        init_snap_replay();
    const char *tmp = getenv("TMPDIR");
    if (host_snap_reopen(tmp && *tmp ? tmp : "/tmp") < 0)
        fprintf(stderr, "xiaoji: restore: some host files could not be opened again (above)\n");
    if (gl_host_snap_resume() < 0)
        fprintf(stderr, "xiaoji: restore: the WebGL objects could not be made again\n");
    fprintf(stderr, "xiaoji: restore: resumed at %llu instructions; the image (%llu MiB) read in %.3f s, the rest of the "
            "restore %.3f s\n", (unsigned long long)snap_hdr.icount, (unsigned long long)(snap_hdr.image_bytes >> 20),
            (double)restore_load_ns / 1e9, (double)(host_monotonic_ns() - snap_hdr.mono_ns) / 1e9);
    if (snap_hdr.flags & F_INIT) {
        int init_resume(uint64_t timeout_s);
        return init_resume(init_timeout_s);
    }
    return sched_resume();
}
