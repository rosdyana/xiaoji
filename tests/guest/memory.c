/* Memory: brk grow and shrink, anonymous mmap with a pattern, mprotect to
 * read-only, munmap, a page-boundary-crossing access, and exclusive
 * load/store through an atomic add. Addresses are never printed (native runs
 * randomise them). */
#include "sys.h"

#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20

#define MB (1u << 20)

static u32 counter;

static void report(const char *label, int ok)
{
    put(label);
    put(ok ? "=ok\n" : "=FAIL\n");
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int fails = 0;

    /* brk: grow by 64 KiB, fill, check, shrink back. */
    u64 cur = sys_brk(0);
    u64 want = cur + 65536;
    u64 got = sys_brk(want);
    int ok = got == want;
    report("brk_grow", ok);
    fails += !ok;
    if (ok) {
        volatile u8 *p = (volatile u8 *)cur;
        for (u32 i = 0; i < 65536; i++)
            p[i] = (u8)(i * 3 + 1);
        u64 s = 0;
        for (u32 i = 0; i < 65536; i++)
            s += p[i];
        put("brk_sum=");
        putu(s);
        put("\n");
    }
    u64 shrunk = sys_brk(cur);
    ok = shrunk == cur;
    report("brk_shrink", ok);
    fails += !ok;

    /* mmap 1 MiB RW, write a pattern, sum it. */
    i64 m = sys_mmap(0, MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ok = m > 0;
    report("mmap", ok);
    fails += !ok;
    if (ok) {
        volatile u8 *mp = (volatile u8 *)(u64)m;
        for (u32 i = 0; i < MB; i++)
            mp[i] = (u8)(i ^ (i >> 8));
        u64 ms = 0;
        for (u32 i = 0; i < MB; i++)
            ms += mp[i];
        put("mmap_sum=");
        putu(ms);
        put("\n");

        /* mprotect to read-only, then read. */
        i64 r = sys_mprotect((u64)m, MB, PROT_READ);
        ok = r == 0;
        report("mprotect_ro", ok);
        fails += !ok;
        put("ro_read=");
        putu(mp[12345]);
        put("\n");

        /* munmap. */
        r = sys_munmap((u64)m, MB);
        ok = r == 0;
        report("munmap", ok);
        fails += !ok;
    }

    /* 8-byte access that crosses a 4 KiB page boundary. */
    i64 b = sys_mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ok = b > 0;
    if (ok) {
        volatile u64 *cross = (volatile u64 *)((u64)b + 4092);
        *cross = 0x1122334455667788ull;
        u64 back = *cross;
        ok = back == 0x1122334455667788ull;
        put("cross_read=");
        putx(back);
        put("\n");
        u8 *raw = (u8 *)(u64)b;
        ok = ok && raw[4092] == 0x88 && raw[4099] == 0x11;
    }
    report("page_cross", ok);
    fails += !ok;

    /* Exclusive load/store: atomic add compiles to LDAXR/STLXR loops here. */
    u32 old = 0;
    for (u32 i = 0; i < 100; i++)
        old = __atomic_fetch_add(&counter, 5u, __ATOMIC_SEQ_CST);
    put("atomic_counter=");
    putu(counter);
    put(" last_old=");
    putu(old);
    put("\n");
    ok = counter == 500 && old == 495;
    report("exclusive", ok);
    fails += !ok;

    put("failures=");
    putu((u64)fails);
    put("\n");
    return fails;
}
