/* bpfmap (B15): the maps-only bpf(2) subset (engine/bpf.c) against the native kernel of the builder VM, run as
 * root there (CAP_BPF, and the bpffs at /sys/fs/bpf). One program, output compared with native.
 *
 * It prints only errnos and values: never a map id, a fd number or a pid. The keys that GET_NEXT_KEY returns are
 * sorted before they are printed (the hash order is the kernel's). Pins are /sys/fs/bpf/xj-bpfmap-*, unlinked at
 * the start and at the end, so a rerun does not hit EEXIST.
 *
 * Not compared with native (the native kernel has more than the engine does, so their errnos differ): BPF_PROG_LOAD
 * (the engine gives EPERM, native does its own checks), the per-CPU and LRU types, and the bpffs listing. */
#include <stdint.h>
#include "sys.h"

#define SYS_UNLINKAT 35
#define SYS_MKDIRAT 34
#define SYS_FACCESSAT 48
#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_NEWFSTATAT 79
#define AT_REMOVEDIR_ 0x200
#define S_IFMT_ 0170000u
#define S_IFDIR_ 0040000u
#define S_IFREG_ 0100000u
#define SYS_CLONE 220
#define SYS_WAIT4 260
#define SYS_BPF 280
#define SIGCHLD_ 17
#define AT_FDCWD_ (-100)

#define CMD_MAP_CREATE 0
#define CMD_LOOKUP 1
#define CMD_UPDATE 2
#define CMD_DELETE 3
#define CMD_NEXT 4
#define CMD_OBJ_PIN 6
#define CMD_OBJ_GET 7
#define CMD_INFO 15

#define T_HASH 1
#define T_ARRAY 2

#define BPF_ANY 0u
#define F_RDONLY 8u
#define F_WRONLY 16u

#define E_PERM 1
#define E_NOENT 2
#define E_ACCES 13
#define E_EXIST 17
#define E_NOTDIR 20
#define E_INVAL 22
#define E_2BIG 7

static void put32(u8 *p, u32 v) { memcpy(p, &v, 4); }
static void put64(u8 *p, u64 v) { memcpy(p, &v, 8); }
static u32 get32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static u64 get64(const u8 *p) { u64 v; memcpy(&v, p, 8); return v; }

static long bpf(int cmd, const void *attr, u32 size)
{
    return sc6(SYS_BPF, cmd, (i64)(uintptr_t)attr, size, 0, 0, 0);
}

/* The errno of a failed call (0 for a call that succeeded). */
static int err_of(long r) { return r < 0 ? (int)-r : 0; }

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

static void unlink_pin(const char *path)
{
    sc6(SYS_UNLINKAT, AT_FDCWD_, (i64)(uintptr_t)path, 0, 0, 0, 0);
}

static long at_mkdir(const char *path, u32 mode)
{
    return sc6(SYS_MKDIRAT, AT_FDCWD_, (i64)(uintptr_t)path, mode, 0, 0, 0);
}

static long at_unlink(const char *path, int flags)
{
    return sc6(SYS_UNLINKAT, AT_FDCWD_, (i64)(uintptr_t)path, flags, 0, 0, 0);
}

static long at_access(const char *path)
{
    return sc6(SYS_FACCESSAT, AT_FDCWD_, (i64)(uintptr_t)path, 0, 0, 0, 0);
}

static long at_open(const char *path, u32 flags, u32 mode)
{
    return sc6(SYS_OPENAT, AT_FDCWD_, (i64)(uintptr_t)path, flags, mode, 0, 0);
}

/* The st_mode of a path (errno when it fails): the file type and the permission bits, printed by the caller. */
static long at_mode(const char *path)
{
    u8 st[128];
    memset(st, 0, sizeof st);
    long r = sc6(SYS_NEWFSTATAT, AT_FDCWD_, (i64)(uintptr_t)path, (i64)(uintptr_t)st, 0, 0, 0);
    if (r < 0)
        return r;
    u32 mode = 0;
    memcpy(&mode, st + 16, 4);
    return (long)mode;
}

static void put_mode(const char *what, const char *path)
{
    long m = at_mode(path);
    put(what);
    put(": ");
    if (m < 0) {
        put("errno ");
        putd(-m);
    } else {
        put(((u32)m & S_IFMT_) == S_IFDIR_ ? "dir " : ((u32)m & S_IFMT_) == S_IFREG_ ? "reg " : "other ");
        putx((u32)m & 07777u);
    }
    put("\n");
}

static long map_create(u32 type, u32 key, u32 value, u32 max, u32 flags, const char *name)
{
    u8 a[128];
    memset(a, 0, sizeof a);
    put32(a + 0, type);
    put32(a + 4, key);
    put32(a + 8, value);
    put32(a + 12, max);
    put32(a + 16, flags);
    size_t n = strlen(name);
    if (n > 15)
        n = 15;
    memcpy(a + 28, name, n);
    return bpf(CMD_MAP_CREATE, a, 80);
}

/* One element command. For GET_NEXT_KEY, val receives the next key; key may be NULL (the first element). */
static long elem(int cmd, int fd, const void *key, void *val, u64 flags)
{
    u8 a[32];
    memset(a, 0, sizeof a);
    put32(a + 0, (u32)fd);
    put64(a + 8, (u64)(uintptr_t)key);
    put64(a + 16, (u64)(uintptr_t)val);
    put64(a + 24, flags);
    return bpf(cmd, a, 32);
}

static long upd(int fd, u64 k, u64 v, u64 flags)
{
    return elem(CMD_UPDATE, fd, &k, &v, flags);
}

static long lookup64(int fd, u64 k, u64 *v)
{
    return elem(CMD_LOOKUP, fd, &k, v, 0);
}

static long obj_pin(int fd, const char *path)
{
    u8 a[20];
    memset(a, 0, sizeof a);
    put64(a + 0, (u64)(uintptr_t)path);
    put32(a + 8, (u32)fd);
    return bpf(CMD_OBJ_PIN, a, 20);
}

static long obj_get(const char *path, u32 flags)
{
    u8 a[20];
    memset(a, 0, sizeof a);
    put64(a + 0, (u64)(uintptr_t)path);
    put32(a + 12, flags);
    return bpf(CMD_OBJ_GET, a, 20);
}

static long map_info(int fd, u8 *buf, u32 len)
{
    u8 a[16];
    memset(a, 0, sizeof a);
    put32(a + 0, (u32)fd);
    put32(a + 4, len);
    put64(a + 8, (u64)(uintptr_t)buf);
    return bpf(CMD_INFO, a, 16);
}

/* The keys of a hash map (u64 keys), sorted, into out. Returns the count. */
static int hash_keys(int fd, u64 *out, int cap)
{
    int n = 0;
    u64 key = 0, next = 0;
    long r = elem(CMD_NEXT, fd, NULL, &next, 0);
    while (r == 0 && n < cap) {
        out[n++] = next;
        key = next;
        r = elem(CMD_NEXT, fd, &key, &next, 0);
    }
    for (int i = 1; i < n; i++) {
        u64 v = out[i];
        int j = i - 1;
        while (j >= 0 && out[j] > v) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = v;
    }
    return n;
}

static void print_keys(const char *what, int fd)
{
    u64 keys[16];
    int n = hash_keys(fd, keys, 16);
    put(what);
    put(":");
    for (int i = 0; i < n; i++) {
        put(" ");
        putu(keys[i]);
    }
    put("\n");
}

/* The child of a fork: it gets the pin by path and looks up and writes through its own fd. */
static void child_side(void)
{
    int fd = (int)obj_get("/sys/fs/bpf/xj-bpfmap-h", 0);
    u64 v = 0;
    if (fd < 0) {
        report("child get", fd);
        sys_exit_group(1);
    }
    long r = lookup64(fd, 2, &v);
    put("child lookup 2: ");
    if (r) {
        put("errno ");
        putd(-r);
    } else {
        putu(v);
    }
    put("\n");
    report("child update 5", upd(fd, 5, 50, BPF_ANY));
    sys_exit_group(0);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static const char *const pins[] = {"/sys/fs/bpf/xj-bpfmap-h", "/sys/fs/bpf/xj-bpfmap-a", "/sys/fs/bpf/xj-bpfmap-r"};
    for (int i = 0; i < 3; i++)
        unlink_pin(pins[i]);

    /* A hash map: create, update, lookup, delete, get_next_key. */
    long h = map_create(T_HASH, 8, 8, 4, 0, "xjh");
    report("create hash", h);
    int hfd = (int)h;
    report("insert 3", upd(hfd, 3, 30, BPF_ANY));
    report("insert 1", upd(hfd, 1, 10, BPF_ANY));
    report("insert 2", upd(hfd, 2, 20, BPF_ANY));
    u64 v = 0;
    report("lookup 2", lookup64(hfd, 2, &v));
    put("value 2: ");
    putu(v);
    put("\n");
    report("lookup 9", lookup64(hfd, 9, &v));
    report("noexist 1", upd(hfd, 1, 11, 1));
    report("exist 9", upd(hfd, 9, 90, 2));
    report("flags 3", upd(hfd, 1, 11, 3));
    report("insert 4", upd(hfd, 4, 40, BPF_ANY));
    report("insert 5 full", upd(hfd, 5, 50, BPF_ANY));
    report("delete 3", elem(CMD_DELETE, hfd, &(u64){3}, NULL, 0));
    report("delete 3 again", elem(CMD_DELETE, hfd, &(u64){3}, NULL, 0));
    print_keys("keys after delete", hfd);
    report("lookup 3 after delete", lookup64(hfd, 3, &v));

    /* Pin, and the errors of the pin and the get. */
    report("pin", obj_pin(hfd, pins[0]));
    report("pin again", obj_pin(hfd, pins[0]));
    report("pin under missing dir", obj_pin(hfd, "/sys/fs/bpf/xj-nodir/x"));
    report("pin outside bpffs", obj_pin(hfd, "/tmp/xj-bpfmap-x"));
    report("get missing", obj_get("/sys/fs/bpf/xj-missing", 0));
    report("get the directory", obj_get("/sys/fs/bpf", 0));
    report("get rdonly and wronly", obj_get(pins[0], F_RDONLY | F_WRONLY));

    /* A fork gets the same map by the pin, and writes it. */
    long pid = sc6(SYS_CLONE, SIGCHLD_, 0, 0, 0, 0, 0);
    if (pid == 0)
        child_side();
    int status = 0;
    sc6(SYS_WAIT4, pid, (i64)(uintptr_t)&status, 0, 0, 0, 0);
    put("child exit: ");
    putd((status >> 8) & 0xff);
    put("\n");
    report("parent lookup 5 after child", lookup64(hfd, 5, &v));
    put("value 5: ");
    putu(v);
    put("\n");
    print_keys("keys after child", hfd);

    /* Access modes of the get: read only, write only. */
    long rfd = obj_get(pins[0], F_RDONLY);
    report("get rdonly", rfd);
    if (rfd >= 0) {
        report("rdonly update", upd((int)rfd, 2, 99, BPF_ANY));
        report("rdonly lookup", lookup64((int)rfd, 2, &v));
    }
    long wfd = obj_get(pins[0], F_WRONLY);
    report("get wronly", wfd);
    if (wfd >= 0) {
        report("wronly update", upd((int)wfd, 2, 21, BPF_ANY));
        report("wronly lookup", lookup64((int)wfd, 2, &v));
        report("wronly lookup check", lookup64(hfd, 2, &v));
        put("value 2 after wronly: ");
        putu(v);
        put("\n");
    }

    /* Info by fd. */
    u8 info[88];
    memset(info, 0, sizeof info);
    long ir = map_info(hfd, info, 88);
    report("info", ir);
    if (ir == 0) {
        put("info: type ");
        putu(get32(info + 0));
        put(" key ");
        putu(get32(info + 8));
        put(" value ");
        putu(get32(info + 12));
        put(" max ");
        putu(get32(info + 16));
        put("\n");
    }

    /* An array map: elements always exist, and the last index has no next key. */
    long a = map_create(T_ARRAY, 4, 8, 3, 0, "xja");
    report("create array", a);
    int afd = (int)a;
    u32 idx = 0;
    u64 av = 100;
    report("array update 0", elem(CMD_UPDATE, afd, &idx, &av, 0));
    idx = 2;
    av = 300;
    report("array update 2", elem(CMD_UPDATE, afd, &idx, &av, 0));
    idx = 3;
    av = 1;
    report("array update 3", elem(CMD_UPDATE, afd, &idx, &av, 0));
    idx = 1;
    av = 0;
    report("array lookup 1", elem(CMD_LOOKUP, afd, &idx, &av, 0));
    put("array value 1: ");
    putu(av);
    put("\n");
    idx = 5;
    report("array lookup 5", elem(CMD_LOOKUP, afd, &idx, &av, 0));
    idx = 0;
    report("array delete 0", elem(CMD_DELETE, afd, &idx, NULL, 0));
    idx = 0;
    report("array noexist 0", elem(CMD_UPDATE, afd, &idx, &av, 1));
    u32 nk = 0;
    report("array next from none", elem(CMD_NEXT, afd, NULL, &nk, 0));
    put("array next: ");
    putu(nk);
    put("\n");
    u32 cur = nk;
    report("array next from 0", elem(CMD_NEXT, afd, &cur, &nk, 0));
    cur = nk;
    report("array next from 1", elem(CMD_NEXT, afd, &cur, &nk, 0));
    cur = nk;
    report("array next from 2 (last)", elem(CMD_NEXT, afd, &cur, &nk, 0));

    /* A read-only map: the map's own flag refuses the update through its fd, as native does. */
    long r = map_create(T_HASH, 8, 8, 2, F_RDONLY, "xjr");
    report("create rdonly map", r);
    if (r >= 0) {
        report("map rdonly update", upd((int)r, 1, 1, BPF_ANY));
        report("map rdonly lookup missing", lookup64((int)r, 1, &v));
    }

    /* Errors of the commands and of the create. */
    u8 junk[16];
    memset(junk, 0, sizeof junk);
    report("unknown command", bpf(99, junk, 16));
    report("create type 0", map_create(0, 8, 8, 4, 0, "x"));
    report("create hash key 0", map_create(T_HASH, 0, 8, 4, 0, "x"));
    report("create array key 8", map_create(T_ARRAY, 8, 8, 4, 0, "x"));
    report("create hash bad flags", map_create(T_HASH, 8, 8, 4, 0x80000000u, "x"));

    /* The bpffs as a tree: directories, pins inside them, stat, access, open, unlink and rmdir. */
    const char *dir = "/sys/fs/bpf/xj-bpfmap-d";
    const char *dpin = "/sys/fs/bpf/xj-bpfmap-d/p";
    unlink_pin(dpin);
    at_unlink(dir, AT_REMOVEDIR_);
    report("mkdir", at_mkdir(dir, 0755));
    report("mkdir again", at_mkdir(dir, 0755));
    report("mkdir under missing dir", at_mkdir("/sys/fs/bpf/xj-nodir2/x", 0755));
    put_mode("stat dir", dir);
    long dh = map_create(T_HASH, 8, 8, 4, 0, "xjd");
    report("create map for dir pin", dh);
    report("pin inside dir", obj_pin((int)dh, dpin));
    put_mode("stat pin", dpin);
    report("access pin", at_access(dpin));
    report("access missing", at_access("/sys/fs/bpf/xj-d-missing"));
    long of = at_open(dpin, 0, 0);
    if (of >= 0) {
        report("open pin", 0);
        sc6(SYS_CLOSE, of, 0, 0, 0, 0, 0);
    } else {
        report("open pin", of);
    }
    report("open missing", at_open("/sys/fs/bpf/xj-d-missing", 0, 0));
    report("open missing creat", at_open("/sys/fs/bpf/xj-d-new", 0100 | 1, 0644));
    report("unlink dir", at_unlink(dir, 0));
    report("rmdir non-empty", at_unlink(dir, AT_REMOVEDIR_));
    report("rmdir bpffs root", at_unlink("/sys/fs/bpf", AT_REMOVEDIR_));
    report("unlink pin", at_unlink(dpin, 0));
    report("get after unlink", obj_get(dpin, 0));
    report("rmdir empty", at_unlink(dir, AT_REMOVEDIR_));
    put_mode("stat dir after rmdir", dir);

    /* Clean up: the pins go, and the maps close with the process. */
    for (int i = 0; i < 3; i++)
        unlink_pin(pins[i]);
    sys_exit_group(0);
}
