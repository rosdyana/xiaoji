/*
 * xiaoji: runs Android's ARM64 user-space code by translating it and
 * providing the Linux kernel interface itself (no emulated machine).
 *
 * This is the entry point. The engine is not implemented yet; see
 * docs/spike-0.md for the plan and milestones.
 *
 * Copyright (c) 2026 xiaoji authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>

static int usage(FILE *out)
{
    fputs("usage: xiaoji [--root DIR] [--env K=V]... -- /path/in/guest [args...]\n"
          "  --root DIR   guest root directory (default build/rootfs)\n"
          "  --env K=V    add a guest environment variable\n", out);
    return out == stdout ? 0 : 2;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help"))
            return usage(stdout);
    fputs("xiaoji: engine not implemented yet (spike 0, milestone M0)\n", stderr);
    return usage(stderr);
}
