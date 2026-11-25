/* Snapshots need the engine's data and code at the same addresses in every run (snap.c, docs/snapshot.md): main calls
 * aslr_off before it takes or restores one, and this re-executes the engine in place with address-space layout
 * randomization off (macOS: posix_spawn's SETEXEC with the private DISABLE_ASLR flag, as lldb uses; Linux:
 * personality ADDR_NO_RANDOMIZE). The environment variable XIAOJI_ASLR_OFF marks the second execution. The wasm
 * builds need none of this: linear memory has no randomization. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#elif defined(__linux__)
#define _GNU_SOURCE
#endif

#include "aslr.h"

#if !defined(__EMSCRIPTEN__)
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <spawn.h>
#define XJ_SPAWN_DISABLE_ASLR 0x0100
#elif defined(__linux__)
#include <sys/personality.h>
#endif
extern char **environ;
#endif

int aslr_off(char **argv)
{
#if defined(__EMSCRIPTEN__)
    (void)argv;
    return 0;
#else
    if (getenv("XIAOJI_ASLR_OFF"))
        return 0;
    if (setenv("XIAOJI_ASLR_OFF", "1", 1) != 0)
        return -1;
#if defined(__APPLE__)
    char path[4096];
    uint32_t n = sizeof path;
    if (_NSGetExecutablePath(path, &n) != 0)
        return -1;
    posix_spawnattr_t a;
    if (posix_spawnattr_init(&a) != 0)
        return -1;
    posix_spawnattr_setflags(&a, POSIX_SPAWN_SETEXEC | XJ_SPAWN_DISABLE_ASLR);
    posix_spawn(NULL, path, NULL, &a, argv, environ); /* returns only when it failed */
    posix_spawnattr_destroy(&a);
    return -1;
#elif defined(__linux__)
    int cur = personality(0xffffffff);
    if (cur == -1 || personality((unsigned long)cur | ADDR_NO_RANDOMIZE) == -1)
        return -1;
    execv("/proc/self/exe", argv);
    return -1;
#else
    (void)argv;
    return -1;
#endif
#endif
}
