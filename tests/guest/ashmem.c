/* /dev/ashmem (B7): the ioctls that libcutils' ashmem_create_region and libfmq use (engine/ashmem.c), from a
 * freestanding guest program. Each line is a return value, a byte read back, or a name the test chose.
 *
 *   - open, SET_NAME, GET_SIZE (0 before a size), SET_SIZE 8192 (a page multiple, so GET_SIZE reports it as
 *     set), SET_PROT_MASK and GET_PROT_MASK;
 *   - a shared mapping: a second SET_SIZE after the mapping is EINVAL, and a child's write through the mapping
 *     is seen by the parent (the region is shared across fork);
 *   - PIN, UNPIN and GET_PIN_STATUS;
 *   - errors: an ioctl that is not an ashmem ioctl, and the same on /dev/null, are ENOTTY.
 *
 * The expectation is tests/expect/ashmem.out. The builder VM has no /dev/ashmem to act as an oracle. */
#include <stdint.h>
#include "sys.h"

#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_FSTAT 80
#define SYS_IOCTL 29
#define SYS_CLONE 220
#define SYS_WAIT4 260
#define AT_FDCWD (-100)
#define O_RDWR 2
#define O_CLOEXEC_V 02000000
#define PROT_READ_V 1
#define PROT_WRITE_V 2
#define MAP_SHARED_V 1
#define SIGCHLD_V 17

#define IOC(dir, type, nr, size) (((u32)(dir) << 30) | ((u32)(size) << 16) | ((u32)(type) << 8) | (u32)(nr))
#define ASH_SET_NAME IOC(1, 0x77, 1, 256)
#define ASH_GET_SIZE IOC(0, 0x77, 4, 0)
#define ASH_SET_SIZE IOC(1, 0x77, 3, 8)
#define ASH_SET_PROT_MASK IOC(1, 0x77, 5, 8)
#define ASH_GET_PROT_MASK IOC(0, 0x77, 6, 0)
#define ASH_PIN IOC(1, 0x77, 7, 8)
#define ASH_UNPIN IOC(1, 0x77, 8, 8)
#define ASH_GET_PIN_STATUS IOC(0, 0x77, 9, 0)
#define ASH_BOGUS IOC(0, 0x77, 99, 0)

static i64 sys_openat(const char *path, int flags) { return sc6(SYS_OPENAT, AT_FDCWD, (i64)path, flags, 0, 0, 0); }
static i64 sys_ioctl(int fd, u32 cmd, i64 arg) { return sc6(SYS_IOCTL, fd, (i64)cmd, arg, 0, 0, 0); }
static i64 sys_close(int fd) { return sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0); }
/* struct stat on arm64: st_mode (u32) at 16, st_rdev (u64) at 32. */
static i64 sys_fstat(int fd, u8 *st) { return sc6(SYS_FSTAT, fd, (i64)st, 0, 0, 0, 0); }

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
    static const char dev[] = "/dev/ashmem";
    static const char null_dev[] = "/dev/null";
    char name[256] = "xiaoji-ashmem-test";

    int fd = (int)sys_openat(dev, O_RDWR | O_CLOEXEC_V);
    if (fd < 0) {
        line("open failed", fd);
        return 1;
    }
    put("open ok\n");
    /* libcutils takes /dev/ashmem only as a character device with a device number (ashmem-dev.cpp). */
    u8 st[128] = {0};
    u32 mode = 0;
    u64 rdev = 0;
    if (sys_fstat(fd, st) == 0) {
        mode = (u32)st[16] | (u32)st[17] << 8 | (u32)st[18] << 16 | (u32)st[19] << 24;
        for (int i = 0; i < 8; i++)
            rdev |= (u64)st[32 + i] << (8 * i);
    }
    put("fstat chr ");
    put((mode & 0170000u) == 0020000u && rdev != 0 ? "1\n" : "0\n");
    line("set name", sys_ioctl(fd, ASH_SET_NAME, (i64)name));
    line("size before set", sys_ioctl(fd, ASH_GET_SIZE, 0));

    u64 size = 8192;
    line("set size", sys_ioctl(fd, ASH_SET_SIZE, (i64)size));
    line("get size", sys_ioctl(fd, ASH_GET_SIZE, 0));

    line("set prot 3", sys_ioctl(fd, ASH_SET_PROT_MASK, 3));
    line("get prot", sys_ioctl(fd, ASH_GET_PROT_MASK, 0));

    i64 m = sc6(SYS_MMAP, 0, (i64)size, PROT_READ_V | PROT_WRITE_V, MAP_SHARED_V, fd, 0);
    if (m < 0) {
        line("mmap failed", m);
        return 1;
    }
    volatile u8 *p = (volatile u8 *)(u64)m;
    p[0] = 'x';
    p[4096] = 'y';
    line("set size after map", sys_ioctl(fd, ASH_SET_SIZE, (i64)size));

    i64 pid = sc6(SYS_CLONE, SIGCHLD_V, 0, 0, 0, 0, 0);
    if (pid == 0) {
        p[1] = 'c';
        sys_exit_group(0);
    }
    i32 status = -1;
    sc6(SYS_WAIT4, pid, (i64)&status, 0, 0, 0, 0);
    line("child exit", (i64)((status >> 8) & 0xff));
    put("parent sees ");
    put(p[0] == 'x' && p[1] == 'c' && p[4096] == 'y' ? "shared ok\n" : "not shared\n");

    /* struct ashmem_pin { u32 offset; u32 len; }: the whole region. */
    u32 pin_range[2] = {0, 8192};
    line("pin", sys_ioctl(fd, ASH_PIN, (i64)pin_range));
    line("pin status", sys_ioctl(fd, ASH_GET_PIN_STATUS, 0));
    line("unpin", sys_ioctl(fd, ASH_UNPIN, (i64)pin_range));
    line("pin status after unpin", sys_ioctl(fd, ASH_GET_PIN_STATUS, 0));

    line("bogus ashmem ioctl", sys_ioctl(fd, ASH_BOGUS, 0));
    int nfd = (int)sys_openat(null_dev, O_RDWR);
    line("set size on null", sys_ioctl(nfd, ASH_SET_SIZE, (i64)size));
    sys_close(nfd);

    sys_close(fd);
    put("done\n");
    return 0;
}
