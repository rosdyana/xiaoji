/* xiaoji engine entry point: parse arguments, load an AArch64 program (static,
 * or dynamic with its interpreter), run it. */
/* realpath (POSIX) under -std=c11. */
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_a64.h"
#include "aslr.h"
#include "evdev.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include "jit.h"
/* Called once, just before the first guest instruction runs. The web worker sets
 * Module.onRunStart to time the start of the guest; Node and native builds have no
 * such hook, so this does nothing there. */
EM_JS(void, xn_run_start, (void), { if (Module.onRunStart) Module.onRunStart(); });
#endif
#include "gl_host.h"
#include "host.h"
#include "init.h"
#include "linux.h"
#include "logd.h"
#include "meta.h"
#include "loader.h"
#include "mem.h"
#include "proc.h"
#include "prof.h"
#include "props.h"
#include "sched.h"
#include "signals.h"
#include "snap.h"
#include "syscall.h"
#include "vfs.h"

#define MAX_ENV 256
#define MAX_PROP 64

static const char *g_root = NULL; /* NULL: the host root "/" */

/* XIAOJI_STATS=1: the stats line is printed by an atexit handler, so it appears on
 * every exit path (exit_group from any thread, fatal signals, main's return). */
static int stats_on;
static uint64_t stats_t0;

/* XIAOJI_STATS_AT=TEXT: the first log line that contains TEXT prints the instruction count, time and lazy
 * file reads at that point (the A/B runs read phase 100 this way, since the engine runs on after it). */
static const char *stats_at;
static int stats_at_done;
static void stats_line(const char *tag, double secs, uint64_t icount)
{
    uint64_t lcalls, lbytes;
    mem_lazy_counts(&lcalls, &lbytes);
    fprintf(stderr, "xiaoji: %s: %llu instructions in %.3f s, lazy file reads %llu, %llu bytes\n", tag,
            (unsigned long long)icount, secs, (unsigned long long)lcalls, (unsigned long long)lbytes);
}

void stats_log_hook(const char *msg)
{
    if (!stats_at || stats_at_done || !strstr(msg, stats_at))
        return;
    stats_at_done = 1;
    fflush(stdout);
    stats_line(stats_at, (double)(host_monotonic_ns() - stats_t0) / 1e9, sched_icount());
}

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
    stats_line("end", secs, icount);
#ifdef __EMSCRIPTEN__
    jit_print_stats(icount);
#endif
}

/* The options that are this run's, not the engine state's: applied at the start, and again after a restore (which
 * replaced them with the snapshot's). 0, or -1. */
static int apply_options(int stats, int logcat, int lazy_mmap, const char *lazy_window_arg, int debug_faults_arg,
                         const char *stats_at_arg, const char *dump_at_arg, const char *input_script, const char *snap_at,
                         uint64_t snap_after, const char *snap_out)
{
    const char *stats_env = getenv("XIAOJI_STATS");
    stats_on = stats || (stats_env && stats_env[0] == '1');
    const char *logcat_env = getenv("XIAOJI_LOGCAT");
    logd_set_print(logcat || (logcat_env && logcat_env[0] == '1'));
    /* Lazy file mappings (docs/loading.md, L2): --no-lazy-mmap or XIAOJI_LAZY=0 fills them at mmap time (the
     * A/B baseline). --lazy-window BYTES, or XIAOJI_LAZY_WINDOW, sets the fill window (a power of two). */
    const char *lazy_env = getenv("XIAOJI_LAZY");
    if (lazy_env && lazy_env[0] == '0')
        lazy_mmap = 0;
    mem_set_lazy_mmap(lazy_mmap);
    const char *window_env = lazy_window_arg ? lazy_window_arg : getenv("XIAOJI_LAZY_WINDOW");
    if (window_env && window_env[0])
        mem_set_lazy_window((size_t)strtoul(window_env, NULL, 0));
    const char *faults_env = getenv("XIAOJI_DEBUG_FAULTS");
    sig_set_debug_faults(debug_faults_arg || (faults_env && faults_env[0] == '1'));
    stats_t0 = host_monotonic_ns();
    stats_at = stats_at_arg ? stats_at_arg : getenv("XIAOJI_STATS_AT");
    if (stats_at && !stats_at[0])
        stats_at = NULL;
    sched_dump_clear();
    for (const char *t = dump_at_arg; t && *t;) { /* a comma-separated list of times */
        char *end;
        sched_dump_at(strtoull(t, &end, 10));
        t = *end == ',' ? end + 1 : end + strlen(end);
    }
    if (evdev_script_restart(input_script) < 0) {
        fprintf(stderr, "xiaoji: --input-script: cannot read %s\n", input_script);
        return -1;
    }
    if (snap_at) {
        snap_set_trigger(snap_at);
        snap_set_after(snap_after);
        snap_set_out(snap_out);
    }
    return 0;
}

static int usage(FILE *out)
{
    fputs("usage: xiaoji [--root DIR] [--env K=V]... [--trace] [--stats] [--] /path/in/guest [args...]\n"
          "  --root DIR   guest root directory (default: the host root /)\n"
          "  --env K=V    add a guest environment variable\n"
          "  --prop K=V   set a system property (after the guest's build.prop files)\n"
          "  --input-script FILE  the input devices' test source: timed events, one per line (\"ms dev type code value\"; docs/input.md)\n"
          "  --trace      print each syscall and its result to stderr\n"
          "  --stats      print the instruction count and rate on exit (as XIAOJI_STATS=1)\n"
          "  --logcat     print the guest's log messages (logd) to stderr (as XIAOJI_LOGCAT=1)\n"
          "  --no-lazy-mmap  fill file mappings at mmap time (as XIAOJI_LAZY=0); the default fills on first access\n"
          "  --debug-faults  report faults, process ends and binder refusals on stderr (as XIAOJI_DEBUG_FAULTS=1)\n"
          "  --dump-at S[,S]  dump every guest thread (pc, wait, frame chain) to stderr at these seconds (up to 8)\n"
          "  --lazy-window BYTES  the lazy fill window, a power of two from 4096 to 1 MiB (as XIAOJI_LAZY_WINDOW)\n"
          "  --stats-at TEXT  print the instructions and lazy reads at the first log line containing TEXT (as XIAOJI_STATS_AT)\n"
          "  --snapshot-at COND  write a snapshot when COND holds, then stop: time:S, icount:N, log:TEXT (a guest log line\n"
          "               containing TEXT) or prop:NAME=VALUE (docs/snapshot.md); needs --snapshot-out\n"
          "  --snapshot-after S  wait S more seconds after COND holds\n"
          "  --snapshot-out FILE  the snapshot file\n"
          "  --restore FILE  go on from a snapshot (no program, no --init; --input-script times count from the restore)\n"
          "  --gl null|webgl  the GL backend for the guest's GLES calls (default null; webgl is the web build's, gl_webgl.c)\n"
          "  XIAOJI_STATS=1 prints the instruction count and rate on exit\n",
          out);
    return out == stdout ? 0 : 2;
}

int main(int argc, char **argv)
{
    char *envs[MAX_ENV];
    const char *props[MAX_PROP];
    int nenv = 0, nprop = 0, trace = 0, stats = 0, logcat = 0, root_given = 0;
    int lazy_mmap = 1;
    int debug_faults_arg = 0;
    const char *lazy_window_arg = NULL;
    const char *stats_at_arg = NULL;
    const char *init_config = NULL;
    uint64_t init_timeout = 0;
    const char *dump_at_arg = NULL, *input_script = NULL;
    const char *snap_at = NULL, *snap_out = NULL, *restore_path = NULL;
    uint64_t snap_after = 0;
    int i = 1;

    /* A snapshot (docs/snapshot.md) is taken or restored with the engine at fixed addresses: natively that is a run with
     * ASLR off, so the engine executes itself again that way first. */
    for (int k = 1; k < argc && strcmp(argv[k], "--"); k++)
        if (!strcmp(argv[k], "--snapshot-at") || !strcmp(argv[k], "--restore")) {
            if (aslr_off(argv) < 0) {
                fprintf(stderr, "xiaoji: cannot run with a fixed layout (ASLR off), which snapshots need\n");
                return 2;
            }
            break;
        }
    /* The engine's exit handlers (snap_atexit) are a list in engine memory, which a restore replaces. */
    atexit(snap_run_atexit);

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
        if (!strcmp(a, "--prop")) {
            if (i + 1 >= argc || !strchr(argv[i + 1], '=') || nprop == MAX_PROP)
                return usage(stderr);
            props[nprop++] = argv[++i];
            continue;
        }
        if (!strcmp(a, "--trace")) {
            trace = 1;
            continue;
        }
        if (!strcmp(a, "--logcat")) {
            logcat = 1;
            continue;
        }
        if (!strcmp(a, "--stats")) {
            stats = 1;
            continue;
        }
        if (!strcmp(a, "--stats-at")) {
            if (i + 1 >= argc)
                return usage(stderr);
            stats_at_arg = argv[++i];
            continue;
        }
        if (!strcmp(a, "--no-lazy-mmap")) {
            lazy_mmap = 0;
            continue;
        }
        if (!strcmp(a, "--dump-at")) { /* a dump of every guest thread at this many seconds (sched.c) */
            if (i + 1 >= argc)
                return usage(stderr);
            dump_at_arg = argv[++i]; /* applied after a restore, which would replace it */
            continue;
        }
        if (!strcmp(a, "--debug-faults")) { /* as XIAOJI_DEBUG_FAULTS=1, for the page (?debug=1), which has no environment */
            debug_faults_arg = 1;
            continue;
        }
        if (!strcmp(a, "--input-script")) { /* the test source of the input devices (evdev.c, docs/input.md) */
            if (i + 1 >= argc)
                return usage(stderr);
            input_script = argv[++i];
            continue;
        }
        if (!strcmp(a, "--snapshot-at")) { /* docs/snapshot.md */
            if (i + 1 >= argc || snap_set_trigger(argv[i + 1]) < 0)
                return usage(stderr);
            snap_at = argv[++i];
            continue;
        }
        if (!strcmp(a, "--snapshot-after")) {
            if (i + 1 >= argc)
                return usage(stderr);
            snap_after = strtoull(argv[++i], NULL, 10);
            continue;
        }
        if (!strcmp(a, "--snapshot-out")) {
            if (i + 1 >= argc)
                return usage(stderr);
            snap_out = argv[++i];
            continue;
        }
        if (!strcmp(a, "--restore")) {
            if (i + 1 >= argc)
                return usage(stderr);
            restore_path = argv[++i];
            continue;
        }
        if (!strcmp(a, "--lazy-window")) {
            if (i + 1 >= argc)
                return usage(stderr);
            lazy_window_arg = argv[++i];
            continue;
        }
        if (!strcmp(a, "--gl")) {
            if (i + 1 >= argc || gl_host_select(argv[i + 1]) != 0)
                return usage(stderr);
            i++;
            continue;
        }
        if (!strcmp(a, "--init")) {
            if (i + 1 >= argc)
                return usage(stderr);
            init_config = argv[++i];
            continue;
        }
        if (!strcmp(a, "--init-timeout")) {
            if (i + 1 >= argc)
                return usage(stderr);
            init_timeout = strtoull(argv[++i], NULL, 10);
            continue;
        }
        if (!strcmp(a, "--init-stdio")) {
            /* null (the default, as on a device): services get /dev/null on 0, 1 and 2.
             * host: services keep the engine's own stdio (for debugging only; the names
             * are then /dev/stdin and so on, which ART's fd allowlist refuses). */
            if (i + 1 >= argc)
                return usage(stderr);
            const char *mode = argv[++i];
            if (!strcmp(mode, "host"))
                init_set_stdio_host(1);
            else if (!strcmp(mode, "null"))
                init_set_stdio_host(0);
            else
                return usage(stderr);
            continue;
        }
        if (a[0] == '-')
            return usage(stderr);
        break; /* PROGRAM starts here; "--" is optional */
    }
    if (snap_at && !snap_out) {
        fprintf(stderr, "xiaoji: --snapshot-at needs --snapshot-out FILE\n");
        return 2;
    }
    if (restore_path) {
        /* The run goes on from the snapshot: no program and no init config of its own. */
        if (init_config || i < argc) {
            fprintf(stderr, "xiaoji: --restore takes no program and no --init\n");
            return 2;
        }
        if (snap_restore(restore_path) < 0)
            return 2;
        /* The engine's state is the snapshot's now. This run's options are applied to it, then it goes on. */
        g_root = NULL;
        stats_at_done = 0;
        if (apply_options(stats, logcat, lazy_mmap, lazy_window_arg, debug_faults_arg, stats_at_arg, dump_at_arg,
                          input_script, snap_at, snap_after, snap_out) < 0)
            return 2;
        return snap_resume(init_timeout);
    }
    /* --init runs the built-in init (init.c) instead of a program; it needs a root. */
    static char *no_argv[1] = {NULL};
    if (init_config && !root_given) {
        fprintf(stderr, "xiaoji: --init needs --root\n");
        return 2;
    }
    if (!init_config && i >= argc)
        return usage(stderr);

    char **gargv = init_config ? no_argv : &argv[i];
    int gargc = init_config ? 0 : argc - i;
    const char *path = init_config ? "/init" : gargv[0];

    proc_init(); // the initial process: its address space, fd table and signal handlers
    if (vfs_init(g_root, path, gargc, gargv) < 0) {
        fprintf(stderr, "xiaoji: root path too long\n");
        return 2;
    }
    /* With --root, the guest metadata of the writable trees is kept in the root (meta.c): loaded now, written at exit. */
    if (root_given) {
        if (meta_persist_open(g_root) < 0) {
            fprintf(stderr, "xiaoji: root path too long\n");
            return 2;
        }
        snap_atexit(meta_persist_save);
    }
    /* The system properties: the guest's build.prop files, the engine's values, then --prop. */
    if (props_init((const char *const *)props, nprop) < 0) {
        fprintf(stderr, "xiaoji: cannot create the property area\n");
        return 2;
    }

    if (apply_options(stats, logcat, lazy_mmap, lazy_window_arg, debug_faults_arg, stats_at_arg, dump_at_arg,
                      input_script, snap_at, snap_after, snap_out) < 0)
        return 2;
    snap_atexit(print_stats); /* it prints only with --stats (stats_on) */
    prof_init(g_root); // XIAOJI_PROFILE=1: per-thread counts and hot spots at exit
    if (init_config) {
        init_set_trace(trace);
        return init_main(init_config, init_timeout);
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

#ifdef __EMSCRIPTEN__
    xn_run_start();
#endif
    return sched_run(c);
}
