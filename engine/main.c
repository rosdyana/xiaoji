/* xiaoji engine entry point: parse arguments, load a static AArch64 program, run it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_a64.h"
#include "elf.h"
#include "host.h"
#include "loader.h"
#include "syscall.h"

#define MAX_ENV 256

/* Accepted for the M1 rootfs mapping; not used in M0. */
static const char *g_root = "build/rootfs";

static int usage(FILE *out)
{
    fputs("usage: xiaoji [--root DIR] [--env K=V]... [--trace] [--] /path/in/guest [args...]\n"
          "  --root DIR   guest root directory (default build/rootfs; unused until M1)\n"
          "  --env K=V    add a guest environment variable\n"
          "  --trace      print each syscall and its result to stderr\n"
          "  XIAOJI_STATS=1 prints the instruction count and rate on exit\n",
          out);
    return out == stdout ? 0 : 2;
}

int main(int argc, char **argv)
{
    char *envs[MAX_ENV];
    int nenv = 0, trace = 0;
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
        if (a[0] == '-')
            return usage(stderr);
        break; /* PROGRAM starts here; "--" is optional */
    }
    if (i >= argc)
        return usage(stderr);
    (void)g_root; /* read by the M1 rootfs mapping */

    char **gargv = &argv[i];
    int gargc = argc - i;
    const char *path = gargv[0];

    uint8_t *img = NULL;
    size_t len = 0;
    if (host_read_file(path, &img, &len) < 0) {
        fprintf(stderr, "xiaoji: cannot read %s\n", path);
        return 127;
    }
    elf_info ei;
    int rc = elf_load(img, len, path, &ei);
    free(img);
    if (rc)
        return rc;

    syscall_init(ei.brk);
    static cpu_t cpu;
    cpu_t *c = &cpu;
    c->trace = trace;
    if (loader_setup_stack(c, &ei, gargc, gargv, nenv, envs) < 0) {
        fprintf(stderr, "xiaoji: cannot set up the guest stack\n");
        return 126;
    }
    c->pc = ei.entry;

    const char *stats_env = getenv("XIAOJI_STATS");
    int stats = stats_env && stats_env[0] == '1';
    uint64_t t0 = host_monotonic_ns();
    cpu_run(c);
    uint64_t dt = host_monotonic_ns() - t0;
    fflush(stdout);
    if (stats) {
        double secs = (double)dt / 1e9;
        double rate = secs > 0 ? (double)c->icount / secs / 1e6 : 0.0;
        fprintf(stderr, "xiaoji: %llu instructions in %.3f s (%.2f M instructions/s)\n",
                (unsigned long long)c->icount, secs, rate);
    }
    return c->exit_code;
}
