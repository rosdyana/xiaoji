/* prot-write (B17): a PROT_WRITE mapping without PROT_READ is readable on arm64. Linux's protection_map gives such a
 * mapping a read-write page (the architecture has no write-only pages), so loads, stores and the kernel's own reads
 * (a write(2) from the mapping) succeed. The engine refused the loads: system_server's UI thread died with SIGSEGV
 * (SEGV_ACCERR) reading a gralloc buffer that was mapped PROT_WRITE (libhwui, B17).
 *
 * Covered: an anonymous private mapping made PROT_WRITE, one changed to PROT_WRITE by mprotect, and a shared mapping
 * of a memfd made PROT_WRITE. The native run (the builder VM) is the oracle. */
#include "sys.h"

#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define SYS_MEMFD_CREATE 279
#define SYS_FTRUNCATE 46
#define SYS_CLOSE 57

static void check(const char *tag, volatile u8 *p)
{
    for (int i = 0; i < 64; i++)
        p[i] = (u8)('a' + i % 26);
    u64 sum = 0;
    for (int i = 0; i < 64; i++)
        sum += p[i];
    put(tag);
    put(" sum ");
    putu(sum);
    put(" first ");
    putu(p[0]);
    put(" write(2) ");
    i64 n = sys_write(1, (const void *)p, 8);
    put(" -> ");
    putd(n);
    put("\n");
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    i64 a = sys_mmap(0, 4096, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    put("anon mmap ok ");
    putd(a > 0);
    put("\n");
    if (a > 0)
        check("anon PROT_WRITE", (volatile u8 *)a);

    i64 b = sys_mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (b > 0) {
        ((volatile u8 *)b)[100] = 7;
        put("mprotect ");
        putd(sys_mprotect((u64)b, 4096, PROT_WRITE));
        put(" kept ");
        putu(((volatile u8 *)b)[100]);
        put("\n");
        check("mprotect PROT_WRITE", (volatile u8 *)b);
    }

    i64 fd = sc6(SYS_MEMFD_CREATE, (i64)"pw", 0, 0, 0, 0, 0);
    if (fd >= 0 && sc6(SYS_FTRUNCATE, fd, 4096, 0, 0, 0, 0) == 0) {
        i64 c = sys_mmap(0, 4096, PROT_WRITE, MAP_SHARED, fd, 0);
        i64 d = sys_mmap(0, 4096, PROT_READ, MAP_SHARED, fd, 0);
        if (c > 0 && d > 0) {
            check("memfd shared PROT_WRITE", (volatile u8 *)c);
            put("the read-only view sees ");
            putu(((volatile u8 *)d)[1]);
            put("\n");
        }
        sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    }
    return 0;
}
