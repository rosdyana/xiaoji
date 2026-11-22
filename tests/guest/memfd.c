/* memfd: memfd_create, ftruncate, fstat, MAP_SHARED views that alias each other
 * (including a view at a file offset), write and pread coherence, mprotect,
 * mremap growth into a shared tail, close while mapped, and a MAP_PRIVATE copy.
 * Addresses are never printed. */
#include "sys.h"

#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MREMAP_MAYMOVE 1
#define PAGE 4096u

#define SYS_MREMAP 216
#define SYS_CLOSE 57
#define SYS_LSEEK 62
#define SYS_PREAD64 67
#define SYS_FSTAT 80
#define SYS_FTRUNCATE 46
#define SYS_MEMFD_CREATE 279

static void report(const char *label, int ok)
{
    put(label);
    put(ok ? "=ok\n" : "=FAIL\n");
}

static i64 sys_memfd(const char *name, u64 flags) { return sc6(SYS_MEMFD_CREATE, (i64)name, (i64)flags, 0, 0, 0, 0); }
static i64 sys_ftruncate(i64 fd, u64 len) { return sc6(SYS_FTRUNCATE, fd, (i64)len, 0, 0, 0, 0); }
static i64 sys_close(i64 fd) { return sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0); }
static i64 sys_lseek(i64 fd, i64 off, i64 whence) { return sc6(SYS_LSEEK, fd, off, whence, 0, 0, 0); }
static i64 sys_pread(i64 fd, void *buf, u64 n, u64 off) { return sc6(SYS_PREAD64, fd, (i64)buf, (i64)n, (i64)off, 0, 0); }
static i64 sys_mremap(u64 old, u64 oldlen, u64 newlen, u64 flags)
{
    return sc6(SYS_MREMAP, (i64)old, (i64)oldlen, (i64)newlen, (i64)flags, 0, 0);
}
static i64 sys_fstat(i64 fd, void *st) { return sc6(SYS_FSTAT, fd, (i64)st, 0, 0, 0, 0); }

static u8 pat(u32 i) { return (u8)(i * 7u + 3u); }

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    i64 fd = sys_memfd("jit-cache", 0);
    report("memfd_create", fd >= 0);
    report("ftruncate", sys_ftruncate(fd, 2 * 65536) == 0);

    u8 st[128];
    report("fstat-size", sys_fstat(fd, st) == 0 && *(u64 *)(st + 48) == 2 * 65536);
    report("fstat-regular", sys_fstat(fd, st) == 0 && ((*(u32 *)(st + 16)) & 0170000) == 0100000);

    /* Two views of the same pages: RW at offset 0, read-only at offset 0. */
    u8 *rw = (u8 *)sys_mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    u8 *ro = (u8 *)sys_mmap(0, 65536, PROT_READ, MAP_SHARED, fd, 0);
    report("views-mapped", (i64)rw > 0 && (i64)ro > 0);
    for (u32 i = 0; i < 65536; i++)
        rw[i] = pat(i);
    int ok = 1;
    for (u32 i = 0; i < 65536; i++)
        if (ro[i] != pat(i))
            ok = 0;
    report("rw-visible-in-ro", ok);

    /* A write through the file descriptor is visible through the views. */
    static const char abc[] = "abc";
    report("write-fd", sys_write(fd, abc, 3) == 3);
    report("write-fd-visible", ro[0] == 'a' && ro[1] == 'b' && ro[2] == 'c' && ro[3] == pat(3));
    u8 buf[16];
    report("pread-sees-view", sys_pread(fd, buf, 4, 4096) == 4 && buf[0] == pat(4096) && buf[3] == pat(4099));

    /* A view at a file offset aliases the same pages. */
    u8 *hi = (u8 *)sys_mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 65536);
    report("offset-view", (i64)hi > 0);
    hi[5] = 0xAB;
    report("offset-write-seen-by-pread", sys_pread(fd, buf, 1, 65536 + 5) == 1 && buf[0] == 0xAB);
    report("offset-write-not-low-page", ro[5] == pat(5));

    /* mprotect a view to read-only: reads keep working and see the same bytes. */
    report("mprotect-ro", sc6(SYS_MPROTECT, (i64)rw, PAGE, PROT_READ, 0, 0, 0) == 0);
    report("mprotect-read", rw[10] == pat(10) && ro[10] == pat(10));
    report("mprotect-back", sc6(SYS_MPROTECT, (i64)rw, PAGE, PROT_READ | PROT_WRITE, 0, 0, 0) == 0);
    rw[11] = 0x5A;
    report("mprotect-write-after", ro[11] == 0x5A);

    /* Grow the RW view in place or by moving it; the new tail is shared with the file. */
    u8 *grown = (u8 *)sys_mremap((u64)rw, 65536, 2 * 65536, MREMAP_MAYMOVE);
    report("mremap-grow", (i64)grown > 0);
    report("mremap-kept-data", grown[4096] == pat(4096) && grown[11] == 0x5A);
    grown[65536 + 9] = 0x77;
    report("mremap-tail-shared", sys_pread(fd, buf, 1, 65536 + 9) == 1 && buf[0] == 0x77);

    /* Close the fd while views are live: they keep the memfd. */
    report("close-while-mapped", sys_close(fd) == 0);
    report("views-survive-close", ro[65536 - 1] == pat(65535) && ro[11] == 0x5A);
    /* MADV_DONTNEED (4) on a shared view drops nothing: the data lives in the memfd. */
    report("madvise-dontneed-shared", sc6(233, (i64)ro, PAGE, 4, 0, 0, 0) == 0 && ro[11] == 0x5A);

    /* Shrinking the view range: the remaining pages stay. */
    report("munmap-hi", sc6(SYS_MUNMAP, (i64)hi, PAGE, 0, 0, 0, 0) == 0);
    report("munmap-ro", sc6(SYS_MUNMAP, (i64)ro, 65536, 0, 0, 0, 0) == 0);
    report("munmap-grown", sc6(SYS_MUNMAP, (i64)grown, 2 * 65536, 0, 0, 0, 0) == 0);

    /* MAP_PRIVATE of a memfd copies the contents; writes stay private. */
    i64 fd2 = sys_memfd("private", 0);
    sys_ftruncate(fd2, PAGE);
    u8 *seed = (u8 *)sys_mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0);
    for (u32 i = 0; i < PAGE; i++)
        seed[i] = pat(i + 1);
    u8 *priv = (u8 *)sys_mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd2, 0);
    report("private-copy", priv[0] == pat(1) && priv[100] == pat(101));
    priv[0] = 0x11;
    report("private-write-stays-private", seed[0] == pat(1) && priv[0] == 0x11);
    report("private-fd-unchanged", sys_pread(fd2, buf, 1, 0) == 1 && buf[0] == pat(1));

    /* lseek on the fd: SEEK_END reports the size; SEEK_SET then read. */
    report("lseek-end", sys_lseek(fd2, 0, 2) == PAGE);
    report("lseek-set", sys_lseek(fd2, 2, 0) == 2);
    sys_close(fd2);

    /* Dual-view code cache, as ART's JIT uses it: write code through the RW view,
     * maintain the caches, then run it through the RX view. Rewriting the code
     * and running again must run the new code. */
    typedef u64 (*fn0_t)(void);
    i64 cfd = sys_memfd("code", 0);
    sys_ftruncate(cfd, PAGE);
    u32 *cw = (u32 *)sys_mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 0);
    u32 *cx = (u32 *)sys_mmap(0, PAGE, PROT_READ | PROT_EXEC, MAP_SHARED, cfd, 0);
    cw[0] = 0xD2800540u; /* mov x0, #42 */
    cw[1] = 0xD65F03C0u; /* ret */
    __asm__ volatile("dc cvau, %0\n dsb ish\n ic ivau, %0\n dsb ish\n isb" : : "r"(cw) : "memory");
    report("jit-run", ((fn0_t)cx)() == 42);
    cw[0] = 0xD28000E0u; /* mov x0, #7 */
    __asm__ volatile("dc cvau, %0\n dsb ish\n ic ivau, %0\n dsb ish\n isb" : : "r"(cw) : "memory");
    report("jit-rerun-new-code", ((fn0_t)cx)() == 7);

    /* DC ZVA zeroes the 64-byte block that contains its address, and nothing else. */
    static u8 zb[192] __attribute__((aligned(64)));
    for (u32 i = 0; i < sizeof zb; i++)
        zb[i] = 0xFF;
    __asm__ volatile("dc zva, %0" : : "r"(zb + 100) : "memory");
    ok = 1;
    for (u32 i = 0; i < sizeof zb; i++) {
        u8 want = (i >= 64 && i < 128) ? 0 : 0xFF;
        if (zb[i] != want)
            ok = 0;
    }
    report("dc-zva-block", ok);

    put("memfd done\n");
    return 0;
}
