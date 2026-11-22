/* Freestanding helpers for the xiaoji M0 guest tests. Include from exactly one
 * translation unit per program. Built with -ffreestanding -nostdlib. */
#ifndef XIAOJI_GUEST_SYS_H
#define XIAOJI_GUEST_SYS_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t i32;
typedef int64_t i64;

#define SYS_WRITE 64
#define SYS_EXIT_GROUP 94
#define SYS_BRK 214
#define SYS_MMAP 222
#define SYS_MUNMAP 215
#define SYS_MPROTECT 226
#define SYS_GETRANDOM 278
#define SYS_CLOCK_GETTIME 113

static inline i64 sc6(i64 nr, i64 a0, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5)
{
    register i64 x8 __asm__("x8") = nr;
    register i64 x0 __asm__("x0") = a0;
    register i64 x1 __asm__("x1") = a1;
    register i64 x2 __asm__("x2") = a2;
    register i64 x3 __asm__("x3") = a3;
    register i64 x4 __asm__("x4") = a4;
    register i64 x5 __asm__("x5") = a5;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}

static inline i64 sys_write(int fd, const void *buf, size_t len)
{
    return sc6(SYS_WRITE, fd, (i64)buf, (i64)len, 0, 0, 0);
}

static inline __attribute__((noreturn)) void sys_exit_group(int code)
{
    sc6(SYS_EXIT_GROUP, code, 0, 0, 0, 0, 0);
    for (;;) {
    }
}

static inline u64 sys_brk(u64 addr) { return (u64)sc6(SYS_BRK, (i64)addr, 0, 0, 0, 0, 0); }

static inline i64 sys_mmap(u64 addr, u64 len, i64 prot, i64 flags, i64 fd, i64 off)
{
    return sc6(SYS_MMAP, (i64)addr, (i64)len, prot, flags, fd, off);
}

static inline i64 sys_munmap(u64 addr, u64 len) { return sc6(SYS_MUNMAP, (i64)addr, (i64)len, 0, 0, 0, 0); }

static inline i64 sys_mprotect(u64 addr, u64 len, i64 prot)
{
    return sc6(SYS_MPROTECT, (i64)addr, (i64)len, prot, 0, 0, 0);
}

static inline i64 sys_getrandom(void *buf, size_t len, u32 flags)
{
    return sc6(SYS_GETRANDOM, (i64)buf, (i64)len, flags, 0, 0, 0);
}

static inline i64 sys_clock_gettime(i64 clk, void *ts)
{
    return sc6(SYS_CLOCK_GETTIME, clk, (i64)ts, 0, 0, 0, 0);
}

/* Set by _start: the envp array that follows argv. */
extern char **xiaoji_envp;

int main(int argc, char **argv);

/* Called from _start with the initial stack pointer. */
void __xiaoji_start_c(i64 *sp) __attribute__((noreturn));
void __xiaoji_start_c(i64 *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    xiaoji_envp = argv + argc + 1;
    int rc = main(argc, argv);
    sys_exit_group(rc);
}

char **xiaoji_envp;

__asm__(".text\n"
        ".global _start\n"
        "_start:\n"
        "  mov x29, #0\n"
        "  mov x0, sp\n"
        "  bl __xiaoji_start_c\n"
        "  brk #0\n");

/* Volatile byte accesses keep gcc from turning these loops into calls to
 * themselves or into NEON code. */
void *memcpy(void *dst, const void *src, size_t n)
{
    volatile u8 *d = dst;
    const volatile u8 *s = src;
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    volatile u8 *d = dst;
    for (size_t i = 0; i < n; i++)
        d[i] = (u8)c;
    return dst;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static inline void put(const char *s) { sys_write(1, s, strlen(s)); }

static inline void putu(u64 v)
{
    char buf[24];
    int i = (int)sizeof buf;
    buf[--i] = 0;
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    put(buf + i);
}

static inline void putd(i64 v)
{
    if (v < 0) {
        put("-");
        putu((u64)0 - (u64)v);
    } else {
        putu((u64)v);
    }
}

static inline void putx(u64 v)
{
    char buf[19];
    int i = (int)sizeof buf;
    buf[--i] = 0;
    do {
        buf[--i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v);
    buf[--i] = 'x';
    buf[--i] = '0';
    put(buf + i);
}

#endif
