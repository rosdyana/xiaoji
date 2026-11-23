/* goldfish-as (G1): /dev/goldfish_address_space and /dev/goldfish_sync, driven the way the ranchu allocator
 * and HostComposer drive them (device/generic/goldfish-opengl: shared/GoldfishAddressSpace, system/egl/goldfish_sync.h).
 *
 *   1. PING sets the subdevice (HostMemoryAllocator, 5); the reply echoes it in metadata.
 *   2. ALLOCATE_BLOCK of 8192 bytes: the rounded size, an offset and phys_addr 0 come back.
 *   3. PING ALLOCATE_ID (metadata 1) for the block: the result is 0.
 *   4. mmap of the block twice (MAP_SHARED): a byte written through one view is seen through the other.
 *   5. PING UNALLOCATE_ID (2), then DEALLOCATE_BLOCK; an unknown ioctl is ENOTTY.
 *   6. /dev/goldfish_sync: QUEUE_WORK returns a signalled fence fd (a read gives the counter, 1); SIGNAL is 0.
 *
 * There is no native oracle (the builder VM has no goldfish device); tests/expect/goldfish-as.out holds the
 * expected lines. */
#include "sys.h"

#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_READ 63
#define SYS_IOCTL 29
#define AT_FDCWD (-100)
#define O_RDWR 2
#define PROT_RW 3
#define MAP_SHARED 1
#define IOC_WRITE_READ 3u /* _IOWR: dir 3 */

static u32 ioc(u32 type, u32 nr, u32 size) { return (IOC_WRITE_READ << 30) | (size << 16) | (type << 8) | nr; }

static i64 sys_openat(const char *path) { return sc6(SYS_OPENAT, AT_FDCWD, (i64)path, O_RDWR, 0, 0, 0); }
static i64 sys_ioctl(int fd, u32 cmd, void *arg) { return sc6(SYS_IOCTL, fd, cmd, (i64)arg, 0, 0, 0); }
static i64 sys_read(int fd, void *buf, size_t n) { return sc6(SYS_READ, fd, (i64)buf, (i64)n, 0, 0, 0); }
static i64 sys_close(int fd) { return sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0); }

static void put64(u8 *p, u64 v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (u8)(v >> (8 * i));
}

static u64 get64(const u8 *p)
{
    u64 v = 0;
    for (int i = 0; i < 8; i++)
        v |= (u64)p[i] << (8 * i);
    return v;
}

static void line(const char *tag, i64 r)
{
    put(tag);
    put(" ");
    putd(r);
    put("\n");
}

/* struct address_space_ping: offset, size, metadata, version (40 bytes). */
static i64 ping(int fd, u64 offset, u64 size, u64 metadata, u8 out_md[8])
{
    u8 b[40];
    for (int i = 0; i < 40; i++)
        b[i] = 0;
    put64(b, offset);
    put64(b + 8, size);
    put64(b + 16, metadata);
    put64(b + 24, 0);
    b[24] = 40; /* version = sizeof(struct), as the guest sets it */
    i64 r = sys_ioctl(fd, ioc('G', 12, 40), b);
    if (out_md)
        for (int i = 0; i < 8; i++)
            out_md[i] = b[16 + i];
    return r;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static const char as[] = "/dev/goldfish_address_space";
    static const char sync[] = "/dev/goldfish_sync";
    u8 md[8];

    int fd = (int)sys_openat(as);
    if (fd < 0) {
        line("open address space", fd);
        return 1;
    }
    /* 1. The subdevice: HostMemoryAllocator (5). */
    i64 r = ping(fd, 0, 0, 5, md);
    line("set subdevice", r);
    line("subdevice metadata", (i64)get64(md));

    /* 2. ALLOCATE_BLOCK: size in, rounded size, offset and phys_addr out. */
    u8 al[24];
    put64(al, 8000); /* not a page multiple: the block is rounded to 8192 */
    put64(al + 8, 0);
    put64(al + 16, 0xffff);
    r = sys_ioctl(fd, ioc('G', 10, 24), al);
    line("allocate", r);
    u64 size = get64(al), off = get64(al + 8), phys = get64(al + 16);
    line("block size", (i64)size);
    line("block offset", (i64)off);
    line("block phys", (i64)phys);

    /* 3. PING ALLOCATE_ID for the block. */
    r = ping(fd, off, size, 1, md);
    line("allocate id ping", r);
    line("allocate id result", (i64)get64(md));

    /* 4. Two mappings of the block share their bytes. */
    u8 *a = (u8 *)(uintptr_t)sys_mmap(0, size, PROT_RW, MAP_SHARED, fd, (i64)off);
    u8 *b = (u8 *)(uintptr_t)sys_mmap(0, size, PROT_RW, MAP_SHARED, fd, (i64)off);
    line("mmap ok", (a != (u8 *)(uintptr_t)-1 && b != (u8 *)(uintptr_t)-1) ? 1 : 0);
    a[100] = 0x5a;
    line("second view reads", (i64)b[100]);
    a[size - 1] = 0xa5;
    line("second view reads last byte", (i64)b[size - 1]);

    /* 5. Free: UNALLOCATE_ID, then DEALLOCATE_BLOCK. An unknown ioctl is ENOTTY. */
    r = ping(fd, off, size, 2, md);
    line("unallocate id ping", r);
    line("unallocate id result", (i64)get64(md));
    u8 dealloc[8];
    put64(dealloc, off);
    line("deallocate", sys_ioctl(fd, ioc('G', 11, 8), dealloc));
    line("unknown ioctl", sys_ioctl(fd, ioc('G', 99, 8), dealloc));
    sys_close(fd);

    /* 6. Sync fences. QUEUE_WORK returns a fence fd (already signalled); SIGNAL is accepted. */
    int sf = (int)sys_openat(sync);
    if (sf < 0) {
        line("open sync", sf);
        return 1;
    }
    u8 info[24];
    for (int i = 0; i < 24; i++)
        info[i] = 0;
    put64(info, 1);
    put64(info + 8, 2);
    r = sys_ioctl(sf, ioc('@', 0, 24), info);
    line("queue work", r);
    i32 fence = (i32)((u32)info[16] | (u32)info[17] << 8 | (u32)info[18] << 16 | (u32)info[19] << 24);
    line("fence is an fd", fence > 2 ? 1 : 0);
    u8 cnt[8];
    line("fence read", sys_read(fence, cnt, 8));
    line("fence counter", (i64)get64(cnt));
    line("signal", sys_ioctl(sf, ioc('@', 1, 24), info));
    sys_close(sf);
    put("done\n");
    return 0;
}
