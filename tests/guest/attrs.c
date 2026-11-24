/* The attr files libselinux opens (B16): setfscreatecon writes /proc/thread-self/attr/fscreate, and vold's
 * prepare-subdirs helper fails without it. Each file must open for writing. The oracle kernel has no security module:
 * its writes fail with EIO and its reads of an unset context with EINVAL, so neither is compared; only the opens are.
 * Only results are printed. */
#include "sys.h"

#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define AT_FDCWD (-100)
#define O_WRONLY 1

static i64 open_path(const char *path, int flags)
{
    return sc6(SYS_OPENAT, AT_FDCWD, (i64)path, flags, 0, 0, 0);
}

static void report(const char *name, i64 rc)
{
    put(name);
    put(": ");
    putd(rc < 0 ? rc : 0);
    put("\n");
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static const char *const paths[] = {
        "/proc/thread-self/attr/fscreate", "/proc/self/attr/sockcreate", "/proc/self/attr/keycreate",
        "/proc/thread-self/attr/exec", "/proc/self/attr/current",
    };
    for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        i64 fd = open_path(paths[i], O_WRONLY);
        report(paths[i], fd < 0 ? fd : 0);
        if (fd >= 0)
            sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    }
    return 0;
}
