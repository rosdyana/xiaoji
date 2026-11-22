/* xiaoji engine entry point: parse arguments, load an AArch64 program (static,
 * or dynamic with its interpreter), run it. */
/* realpath (POSIX) under -std=c11. */
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_a64.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include "jit.h"
/* Called once, just before the first guest instruction runs. The web worker sets
 * Module.onRunStart to time the start of the guest; Node and native builds have no
 * such hook, so this does nothing there. */
EM_JS(void, xn_run_start, (void), { if (Module.onRunStart) Module.onRunStart(); });
#endif
#include "host.h"
#include "linux.h"
#include "loader.h"
#include "mem.h"
#include "proc.h"
#include "prof.h"
#include "sched.h"
#include "signals.h"
#include "syscall.h"
#include "vfs.h"

#define MAX_ENV 256

static const char *g_root = NULL; /* NULL: the host root "/" */

/* XIAOJI_STATS=1: the stats line is printed by an atexit handler, so it appears on
 * every exit path (exit_group from any thread, fatal signals, main's return). */
static int stats_on;
static uint64_t stats_t0;

static void print_stats(void)
{
    if (!stats_on)
        return;
    uint64_t dt = host_monotonic_ns() - stats_t0;
    fflush(stdout);
    double secs = (double)dt / 1e9;
    uint64_t icount = sched_icount();
    double rate = secs > 0 ? (double)icount / secs / 1e6 : 0.0;
    fprintf(stderr, "xiaoji: %llu instructions in %.3f s (%.2f M instructions/s)\n",
            (unsigned long long)icount, secs, rate);
#ifdef __EMSCRIPTEN__
    jit_print_stats(icount);
#endif
}

static int usage(FILE *out)
{
    fputs("usage: xiaoji [--root DIR] [--env K=V]... [--trace] [--stats] [--] /path/in/guest [args...]\n"
          "  --root DIR   guest root directory (default: the host root /)\n"
          "  --env K=V    add a guest environment variable\n"
          "  --trace      print each syscall and its result to stderr\n"
          "  --stats      print the instruction count and rate on exit (as XIAOJI_STATS=1)\n"
          "  XIAOJI_STATS=1 prints the instruction count and rate on exit\n",
          out);
    return out == stdout ? 0 : 2;
}

int main(int argc, char **argv)
{
    char *envs[MAX_ENV];
    int nenv = 0, trace = 0, stats = 0, root_given = 0;
    int i = 1;

    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help"))
            return usage(stdout);
        if (!strcmp(a, "--")) {
            i++;
            break;
        }
        if (!strcmp(a, "--root")) {
            if (i + 1 >= argc)
                return usage(stderr);
            g_root = argv[++i];
            root_given = 1;
            continue;
        }
        if (!strcmp(a, "--env")) {
            if (i + 1 >= argc || !strchr(argv[i + 1], '=') || nenv == MAX_ENV)
                return usage(stderr);
            envs[nenv++] = argv[++i];
            continue;
        }
        if (!strcmp(a, "--trace")) {
            trace = 1;
            continue;
        }
        if (!strcmp(a, "--stats")) {
            stats = 1;
            continue;
        }
        if (a[0] == '-')
            return usage(stderr);
        break; /* PROGRAM starts here; "--" is optional */
    }
    if (i >= argc)
        return usage(stderr);

    char **gargv = &argv[i];
    int gargc = argc - i;
    const char *path = gargv[0];

    proc_init(); // the initial process: its address space, fd table and signal handlers
    if (vfs_init(g_root, path, gargc, gargv) < 0) {
        fprintf(stderr, "xiaoji: root path too long\n");
        return 2;
    }

    /* With no --root, a relative program path is a host path (the M0 tests). */
    uint8_t *img = NULL;
    size_t len = 0;
    char canon[VFS_PATH_MAX];
    int rc;
    if (!root_given && path[0] != '/') {
        if (host_read_file(path, &img, &len) < 0) {
            fprintf(stderr, "xiaoji: cannot read %s\n", path);
            return 127;
        }
        /* The guest path is the host path, so /proc/self/exe must be absolute (as on Linux). */
        if (!realpath(path, canon)) {
            strncpy(canon, path, sizeof canon - 1);
            canon[sizeof canon - 1] = 0;
        }
    } else if (vfs_load_file(path, &img, &len, canon, sizeof canon) < 0) {
        fprintf(stderr, "xiaoji: cannot read %s\n", path);
        return 127;
    }
    vfs_set_prog(canon);

    uint64_t entry = 0, sp = 0;
    rc = loader_load(img, len, path, gargc, gargv, nenv, envs, &entry, &sp);
    free(img);
    if (rc == -LE_NOENT)
        return 127;
    if (rc == -LE_NOEXEC) {
        fprintf(stderr, "xiaoji: %s: cannot execute\n", path);
        return 126;
    }
    if (rc < 0)
        return 126; /* the loader has printed why */

    syscall_set_comm(gargv[0]);
    static cpu_t cpu;
    cpu_t *c = &cpu;
    c->trace = trace;
    c->sp = sp;
    c->pc = entry;

    const char *stats_env = getenv("XIAOJI_STATS");
    stats_on = stats || (stats_env && stats_env[0] == '1');
    const char *faults_env = getenv("XIAOJI_DEBUG_FAULTS");
    sig_set_debug_faults(faults_env && faults_env[0] == '1');
    stats_t0 = host_monotonic_ns();
    if (stats_on)
        atexit(print_stats);
    prof_init(g_root); // XIAOJI_PROFILE=1: per-thread counts and hot spots at exit
#ifdef __EMSCRIPTEN__
    xn_run_start();
#endif
    return sched_run(c);
}
