/* Properties (B2): the property service and the futex protocol of the property area,
 * from a guest program that implements the protocol itself (no bionic).
 *
 * The parent sets debug.xiaoji.wait to "start" through /dev/socket/property_service
 * (PROP_MSG_SETPROP2, as bionic's SocketWriter sends it), maps /dev/__properties__
 * read-only and shared, and forks. The child walks the trie to the property's record,
 * then waits with FUTEX_WAIT on the record's serial, which is a word of the shared
 * area, until the value is "done". The parent sleeps, sets "done" through the service,
 * and reaps the child. The engine's futex wake on the record's serial reaches the child.
 *
 * Output is the same on every run: the child prints nothing, and the parent prints
 * after each step. */
#include "sys.h"

#define SYS_SOCKET 198
#define SYS_CONNECT 203
#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_READ 63
#define SYS_NANOSLEEP 101
#define SYS_FUTEX 98
#define SYS_CLONE 220
#define SYS_WAIT4 260
#define SYS_FTRUNCATE 46
#define SYS_MPROTECT 226
#define SIGCHLD_N 17
#define FUTEX_WAIT 0
#define AREA_SIZE 0x40000
#define AREA_DATA 128

static const char prop_name[] = "debug.xiaoji.wait";
static const char service[] = "/dev/socket/property_service";
static const char area_path[] = "/dev/__properties__";

static u8 *area;

static u32 slen(const char *s)
{
    u32 n = 0;
    while (s[n])
        n++;
    return n;
}

static void copy(u8 *d, const u8 *s, u64 n)
{
    for (u64 i = 0; i < n; i++)
        d[i] = s[i];
}

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static u32 get32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

/* PROP_MSG_SETPROP2 through the service. Returns the service's reply (0 is success),
 * or a negative value if the connection or the exchange failed. */
static i64 set_prop(const char *n, const char *v)
{
    u8 msg[256];
    u32 nl = slen(n), vl = slen(v);
    put32(msg, 0x00020001);
    put32(msg + 4, nl);
    copy(msg + 8, (const u8 *)n, nl);
    put32(msg + 8 + nl, vl);
    copy(msg + 12 + nl, (const u8 *)v, vl);

    i64 s = sc6(SYS_SOCKET, 1 /* AF_UNIX */, 1 /* SOCK_STREAM */ | 0x80000 /* SOCK_CLOEXEC */, 0, 0, 0, 0);
    if (s < 0)
        return s;
    u8 sa[110];
    for (u32 i = 0; i < sizeof sa; i++)
        sa[i] = 0;
    sa[0] = 1;
    u32 plen = slen(service);
    copy(sa + 2, (const u8 *)service, plen + 1);
    i64 rc = sc6(SYS_CONNECT, s, (i64)sa, 2 + plen + 1, 0, 0, 0);
    if (rc < 0) {
        sc6(SYS_CLOSE, s, 0, 0, 0, 0, 0);
        return rc;
    }
    rc = sc6(SYS_WRITE, s, (i64)msg, 12 + nl + vl, 0, 0, 0);
    u8 reply[4];
    i64 got = sc6(SYS_READ, s, (i64)reply, 4, 0, 0, 0);
    sc6(SYS_CLOSE, s, 0, 0, 0, 0, 0);
    if (rc < 0 || got != 4)
        return -1;
    return (i64)get32(reply);
}

/* The record of a property, walked the way bionic's find_property does: a segment
 * per level, siblings by (length, bytes), the record in the node's prop field. Returns
 * the record's offset in the area, or 0. */
static u32 find_bt(u32 cur, const char *s, u32 n)
{
    for (;;) {
        u32 nl = get32(area + AREA_DATA + cur);
        const u8 *nm = area + AREA_DATA + cur + 20;
        int r;
        if (n < nl)
            r = -1;
        else if (n > nl)
            r = 1;
        else {
            r = 0;
            for (u32 i = 0; i < n; i++)
                if ((u8)s[i] != nm[i]) {
                    r = (u8)s[i] < nm[i] ? -1 : 1;
                    break;
                }
        }
        if (r == 0)
            return cur;
        u32 next = get32(area + AREA_DATA + cur + (r < 0 ? 8 : 12));
        if (!next)
            return 0;
        cur = next;
    }
}

static u32 find_record(const char *n)
{
    u32 cur = 0, p = 0, len = slen(n);
    for (;;) {
        u32 sub = p;
        while (sub < len && n[sub] != '.')
            sub++;
        if (sub == p)
            return 0;
        u32 root = get32(area + AREA_DATA + cur + 16);
        if (!root)
            return 0;
        cur = find_bt(root, n + p, sub - p);
        if (!cur)
            return 0;
        if (sub == len)
            break;
        p = sub + 1;
    }
    u32 pr = get32(area + AREA_DATA + cur + 4);
    return pr ? AREA_DATA + pr : 0;
}

static void nap_ms(u64 ms)
{
    i64 ts[2] = {(i64)(ms / 1000), (i64)((ms % 1000) * 1000000)};
    sc6(SYS_NANOSLEEP, (i64)ts, 0, 0, 0, 0, 0);
}

/* The child: wait on the record's serial until the value is "done". */
static i64 child(void)
{
    u32 rec = find_record(prop_name);
    if (!rec)
        return 2;
    for (int tries = 0; tries < 20; tries++) {
        u32 serial = get32(area + rec);
        u32 len = serial >> 24;
        if (len == 4 && !(serial & 1) && area[rec + 4] == 'd' && area[rec + 5] == 'o' &&
            area[rec + 6] == 'n' && area[rec + 7] == 'e')
            return 0;
        i64 ts[2] = {5, 0};
        sc6(SYS_FUTEX, (i64)(area + rec), FUTEX_WAIT, serial, (i64)ts, 0, 0);
    }
    return 3;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    i64 rc = set_prop(prop_name, "start");
    put("prop-wait: start set rc=");
    putd(rc);
    put("\n");

    i64 fd = sc6(SYS_OPENAT, -100, (i64)area_path, 0x80000 /* O_CLOEXEC */, 0, 0, 0);
    if (fd < 0) {
        put("prop-wait: open failed\n");
        return 1;
    }
    i64 m = sys_mmap(0, AREA_SIZE, 1 /* PROT_READ */, 1 /* MAP_SHARED */, fd, 0);
    if (m < 0) {
        put("prop-wait: mmap failed\n");
        return 1;
    }
    area = (u8 *)(u64)m;

    /* The area is read-only to guests: a writable shared view, mprotect to writable,
     * and ftruncate on the read-only fd are all refused (EACCES, EACCES, EINVAL). */
    i64 wm = sys_mmap(0, AREA_SIZE, 3 /* PROT_READ|PROT_WRITE */, 1 /* MAP_SHARED */, fd, 0);
    i64 mp = sc6(SYS_MPROTECT, (i64)area, AREA_SIZE, 3, 0, 0, 0);
    i64 ft = sc6(SYS_FTRUNCATE, fd, 0, 0, 0, 0, 0);
    put("prop-wait: writable mmap=");
    putd(wm);
    put(" mprotect=");
    putd(mp);
    put(" ftruncate=");
    putd(ft);
    put("\n");

    i64 pid = sc6(SYS_CLONE, SIGCHLD_N, 0, 0, 0, 0, 0);
    if (pid == 0)
        sys_exit_group((int)child());

    nap_ms(50);
    rc = set_prop(prop_name, "done");
    put("prop-wait: done set rc=");
    putd(rc);
    put("\n");

    i64 st = -1;
    i64 reaped = sc6(SYS_WAIT4, pid, (i64)&st, 0, 0, 0, 0);
    put("prop-wait: child exit=");
    putd((st >> 8) & 0xff);
    put(reaped == pid ? "\n" : " (not reaped)\n");

    /* A write into the read-only mapping: the child dies of SIGSEGV (11). */
    i64 pid2 = sc6(SYS_CLONE, SIGCHLD_N, 0, 0, 0, 0, 0);
    if (pid2 == 0) {
        volatile u8 *w = area;
        w[AREA_DATA] = 1;
        sys_exit_group(0);
    }
    i64 st2 = -1;
    sc6(SYS_WAIT4, pid2, (i64)&st2, 0, 0, 0, 0);
    put("prop-wait: child write signal=");
    putd(st2 & 0x7f);
    put("\n");
    return 0;
}
