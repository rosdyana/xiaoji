/* bpfpin (B15): the gpu_mem pin as libmeminfo reads it, run by an init service as uid system with the graphics group
 * (tests/init/bpf.rc). The root oracle cannot check the access checks of the pin (root bypasses them), so this one is
 * engine only: tests/expect/init-bpf.out is its expected text, from the DAC rules of the pin (mode 0640, gid 1003).
 *
 * It prints the owner and mode of the pin, the uid and the groups, then: a get with the read-write flags (EACCES: the
 * group has no write bit), a get with BPF_F_RDONLY (ok), the info of the map, a lookup of a missing key (ENOENT), and an
 * update through the read-only fd (EPERM). Nothing else. */
#include <stdint.h>
#include "sys.h"

#define SYS_NEWFSTATAT 79
#define SYS_GETUID 174
#define SYS_GETGROUPS 158
#define SYS_BPF 280
#define AT_FDCWD_ (-100)
#define PIN "/sys/fs/bpf/map_gpu_mem_gpu_mem_total_map"

static void put32(u8 *p, u32 v) { memcpy(p, &v, 4); }
static void put64(u8 *p, u64 v) { memcpy(p, &v, 8); }
static u32 get32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }

static long bpf(int cmd, const void *attr, u32 size)
{
    return sc6(SYS_BPF, cmd, (i64)(uintptr_t)attr, size, 0, 0, 0);
}

static void report(const char *what, long r)
{
    put(what);
    put(": ");
    if (r < 0) {
        put("errno ");
        putd(-r);
    } else {
        put("ok");
    }
    put("\n");
}

static long obj_get(u32 flags)
{
    u8 a[20];
    memset(a, 0, sizeof a);
    put64(a + 0, (u64)(uintptr_t)PIN);
    put32(a + 12, flags);
    return bpf(7, a, 20);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    put("uid ");
    putd(sc6(SYS_GETUID, 0, 0, 0, 0, 0, 0));
    put("\n");
    u32 gids[32];
    long ng = sc6(SYS_GETGROUPS, 32, (i64)(uintptr_t)gids, 0, 0, 0, 0);
    int has_graphics = 0;
    for (long i = 0; i < ng; i++)
        has_graphics |= gids[i] == 1003;
    put("graphics group: ");
    put(has_graphics ? "yes\n" : "no\n");

    u8 st[128];
    memset(st, 0, sizeof st);
    long r = sc6(SYS_NEWFSTATAT, AT_FDCWD_, (i64)(uintptr_t)PIN, (i64)(uintptr_t)st, 0, 0, 0);
    if (r == 0) {
        u32 mode = get32(st + 16), uid = get32(st + 24), gid = get32(st + 28);
        put("stat: ");
        putx(mode & 0777u);
        put(" uid ");
        putd(uid);
        put(" gid ");
        putd(gid);
        put("\n");
    } else {
        report("stat", r);
    }

    report("get read-write", obj_get(0));
    long fd = obj_get(8);
    report("get rdonly", fd);
    if (fd >= 0) {
        u8 info[88];
        memset(info, 0, sizeof info);
        u8 a[16];
        memset(a, 0, sizeof a);
        put32(a + 0, (u32)fd);
        put32(a + 4, sizeof info);
        put64(a + 8, (u64)(uintptr_t)info);
        report("info", bpf(15, a, 16));
        put("info: type ");
        putd(get32(info + 0));
        put(" key ");
        putd(get32(info + 8));
        put(" value ");
        putd(get32(info + 12));
        put(" max ");
        putd(get32(info + 16));
        put("\n");

        u64 key = 1, val = 0;
        u8 e[32];
        memset(e, 0, sizeof e);
        put32(e + 0, (u32)fd);
        put64(e + 8, (u64)(uintptr_t)&key);
        put64(e + 16, (u64)(uintptr_t)&val);
        report("lookup 1", bpf(1, e, 32));
        report("update rdonly", bpf(2, e, 32));
    }
    sys_exit_group(0);
}
