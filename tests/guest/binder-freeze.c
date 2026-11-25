/* The binder freezer ioctls (docs/demo.md, P1). The engine does not model freezing, and says so as an older kernel's
 * driver does: BINDER_GET_FROZEN_INFO and BINDER_FREEZE are EINVAL. ActivityManager's freezer check
 * (CachedAppOptimizer.isFreezerSupported) asks BINDER_GET_FROZEN_INFO for its own pid; an error turns the freezer
 * off, so a cached app is not killed for "Unable to freeze binder interface". Each line is a return value.
 *
 * The expectation is tests/expect/binder-freeze.out. The builder VM's binder models freezing, so it is no oracle. */
#include <stdint.h>
#include "sys.h"

#define SYS_OPENAT 56
#define SYS_IOCTL 29
#define SYS_GETPID 172
#define AT_FDCWD (-100)
#define O_RDWR 2
#define O_CLOEXEC_V 02000000

#define IOC(dir, type, nr, size) (((u32)(dir) << 30) | ((u32)(size) << 16) | ((u32)(type) << 8) | (u32)(nr))
#define BINDER_VERSION IOC(3, 'b', 9, 4)
#define BINDER_FREEZE IOC(1, 'b', 14, 12)
#define BINDER_GET_FROZEN_INFO IOC(3, 'b', 15, 12)

static i64 sys_openat(const char *path, int flags) { return sc6(SYS_OPENAT, AT_FDCWD, (i64)path, flags, 0, 0, 0); }
static i64 sys_ioctl(int fd, u32 cmd, i64 arg) { return sc6(SYS_IOCTL, fd, (i64)cmd, arg, 0, 0, 0); }

static void line(const char *tag, i64 r)
{
    put(tag);
    put(" ");
    putd(r);
    put("\n");
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int fd = (int)sys_openat("/dev/binder", O_RDWR | O_CLOEXEC_V);
    if (fd < 0) {
        line("open failed", fd);
        return 1;
    }
    put("open ok\n");
    i32 ver = -1;
    line("version", sys_ioctl(fd, BINDER_VERSION, (i64)&ver));
    line("protocol", ver);
    u32 pid = (u32)sc6(SYS_GETPID, 0, 0, 0, 0, 0, 0);
    /* struct binder_frozen_status_info { u32 pid; u32 sync_recv; u32 async_recv; } */
    u32 info[3] = {pid, 7, 7};
    line("get frozen info", sys_ioctl(fd, BINDER_GET_FROZEN_INFO, (i64)info));
    /* struct binder_freeze_info { u32 pid; u32 enable; u32 timeout_ms; } */
    u32 fr[3] = {pid, 1, 100};
    line("freeze", sys_ioctl(fd, BINDER_FREEZE, (i64)fr));
    put("done\n");
    return 0;
}
