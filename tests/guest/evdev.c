/* The virtual input devices (B16, engine/evdev.c), from a freestanding guest program. The engine runs it with the
 * script tests/input/evdev.script (XIAOJI_INPUT_SCRIPT). There is no native oracle for /dev/input in the builder VM, so the
 * expectation is tests/expect/evdev.out, derived from the uapi (linux/input.h, linux/input-event-codes.h) and the script.
 *
 * It checks: the directory /dev/input lists event0 and event1 and the nodes are character devices 13:64 and 13:65; the
 * ioctls EventHub and getevent use (VERSION, ID, NAME with truncation, PHYS, UNIQ (none), PROP, EVIOCGBIT for the types and
 * for the codes, ABS, KEY, REP and SREP, GRAB and its EBUSY, SCLOCKID and its EINVAL); the reads (whole events, EINVAL for a
 * short buffer, EAGAIN when drained, the keyboard's events in their own queue); and an epoll wait for the keyboard's event
 * at 300 ms. */
#include <stdint.h>
#include "sys.h"

#define SYS_IOCTL 29
#define SYS_INOTIFY_INIT1 26
#define SYS_INOTIFY_ADD_WATCH 27
#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_GETDENTS64 61
#define SYS_READ 63
#define SYS_FSTAT 80
#define SYS_EPOLL_CREATE1 20
#define SYS_EPOLL_CTL 21
#define SYS_EPOLL_PWAIT 22
#define AT_FDCWD (-100)
#define O_RDWR_V 2
#define O_NONBLOCK_V 04000
#define O_DIRECTORY_V 040000
#define O_CLOEXEC_V 02000000
#define S_IFMT_V 0170000
#define S_IFCHR_V 0020000
#define EPOLLIN_V 1u
#define EPOLL_CTL_ADD_V 1

#define IOC(dir, type, nr, size) (((u32)(dir) << 30) | ((u32)(size) << 16) | ((u32)(type) << 8) | (u32)(nr))
#define IOR(nr, size) IOC(2, 'E', (nr), (size))
#define IOW(nr, size) IOC(1, 'E', (nr), (size))
#define EVIOCGVERSION IOR(0x01, 4)
#define EVIOCGID IOR(0x02, 8)
#define EVIOCGREP IOR(0x03, 8)
#define EVIOCSREP IOW(0x03, 8)
#define EVIOCGNAME(len) IOR(0x06, (len))
#define EVIOCGPHYS(len) IOR(0x07, (len))
#define EVIOCGUNIQ(len) IOR(0x08, (len))
#define EVIOCGPROP(len) IOR(0x09, (len))
#define EVIOCGKEY(len) IOR(0x18, (len))
#define EVIOCGBIT(ev, len) IOR(0x20 + (ev), (len))
#define EVIOCGABS(a) IOR(0x40 + (a), 24)
#define EVIOCGRAB IOW(0x90, 4)
#define EVIOCSCLOCKID IOW(0xa0, 4)

#define EV_KEY_V 1
#define EV_ABS_V 3
#define EV_REP_V 0x14
#define KEY_A_V 30
#define KEY_BACK_V 158
#define KEY_F1_V 59
#define BTN_TOUCH_V 0x14a
#define ABS_X_V 0
#define ABS_MT_SLOT_V 0x2f
#define ABS_MT_POSITION_X_V 0x35
#define ABS_MT_POSITION_Y_V 0x36
#define ABS_MT_PRESSURE_V 0x3a

typedef struct {
    i64 sec, usec;
    uint16_t type, code;
    i32 value;
} ev_rec;

static i64 openat(const char *p, int fl) { return sc6(SYS_OPENAT, AT_FDCWD, (i64)p, fl, 0, 0, 0); }
static i64 ioctl_(int fd, u32 cmd, void *arg) { return sc6(SYS_IOCTL, fd, (i64)cmd, (i64)arg, 0, 0, 0); }
static i64 rd(int fd, void *b, u64 n) { return sc6(SYS_READ, fd, (i64)b, (i64)n, 0, 0, 0); }
static void line(const char *label, i64 v)
{
    put(label);
    put(" ");
    putd(v);
    put("\n");
}
static int bit(const u8 *b, unsigned n) { return (b[n >> 3] >> (n & 7)) & 1; }
static u32 get32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static void print_events(const char *who, const ev_rec *e, i64 n)
{
    for (i64 i = 0; i < n; i++) {
        put(who);
        put(" type=");
        putd(e[i].type);
        put(" code=");
        putd(e[i].code);
        put(" value=");
        putd(e[i].value);
        put("\n");
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    /* The first open starts the script's clock: both devices are open before any read. */
    i64 t = openat("/dev/input/event0", O_RDWR_V | O_NONBLOCK_V | O_CLOEXEC_V);
    i64 k = openat("/dev/input/event1", O_RDWR_V | O_NONBLOCK_V | O_CLOEXEC_V);
    line("open touch", t >= 0);
    line("open keyboard", k >= 0);

    /* The directory and the nodes. */
    i64 dfd = openat("/dev/input", O_DIRECTORY_V);
    u8 dbuf[512];
    i64 dn = sc6(SYS_GETDENTS64, dfd, (i64)dbuf, sizeof dbuf, 0, 0, 0);
    for (i64 off = 0; off < dn;) {
        uint16_t reclen = (uint16_t)(dbuf[off + 16] | dbuf[off + 17] << 8);
        const char *name = (const char *)(dbuf + off + 19);
        if (!(name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))) {
            put("entry ");
            put(name);
            put("\n");
        }
        off += reclen;
    }
    sc6(SYS_CLOSE, dfd, 0, 0, 0, 0, 0);
    /* EventHub watches /dev/input (B9): the watch on the directory succeeds. */
    i64 ifd = sc6(SYS_INOTIFY_INIT1, O_CLOEXEC_V, 0, 0, 0, 0, 0);
    i64 wd = sc6(SYS_INOTIFY_ADD_WATCH, ifd, (i64)"/dev/input", 0x100 | 0x200, 0, 0, 0);
    line("inotify watch /dev/input", wd >= 1);
    sc6(SYS_CLOSE, ifd, 0, 0, 0, 0, 0);
    u8 st[128];
    sc6(SYS_FSTAT, t, (i64)st, 0, 0, 0, 0);
    u32 mode = get32(st + 16);
    u64 rdev = (u64)get32(st + 32) | (u64)get32(st + 36) << 32;
    line("touch is chr", (mode & S_IFMT_V) == S_IFCHR_V);
    line("touch major", (rdev >> 8) & 0xfff);
    line("touch minor", rdev & 0xff);

    /* The ioctls: the identity of each device. */
    u32 ver = 0;
    line("version ret", ioctl_((int)t, EVIOCGVERSION, &ver));
    line("version", ver);
    u8 id[8];
    line("id ret", ioctl_((int)t, EVIOCGID, id));
    line("id bus", id[0] | id[1] << 8);
    line("id vendor", id[2] | id[3] << 8);
    line("id product", id[4] | id[5] << 8);
    line("id version", id[6] | id[7] << 8);
    char name[64];
    i64 r = ioctl_((int)t, EVIOCGNAME(sizeof name), name);
    line("name touch ret", r);
    put("name touch ");
    put(name);
    put("\n");
    for (int i = 0; i < 64; i++)
        name[i] = 0;
    r = ioctl_((int)k, EVIOCGNAME(8), name); /* truncated to 8 bytes, with no NUL (the kernel's str_to_user) */
    line("name keyboard ret", r);
    put("name keyboard ");
    put(name);
    put("\n");
    char phys[64];
    r = ioctl_((int)t, EVIOCGPHYS(sizeof phys), phys);
    line("phys touch ret", r);
    put("phys touch ");
    put(phys);
    put("\n");
    char uniq[64];
    line("uniq touch", ioctl_((int)t, EVIOCGUNIQ(sizeof uniq), uniq));
    u8 prop[4];
    line("prop touch ret", ioctl_((int)t, EVIOCGPROP(sizeof prop), prop));
    line("prop touch direct", prop[0]);
    line("prop keyboard", ioctl_((int)k, EVIOCGPROP(sizeof prop), prop));
    line("prop keyboard byte", prop[0]);

    /* The types: EV_SYN, EV_KEY, EV_ABS (EV_REP for the keyboard). */
    u8 types[4];
    line("types touch ret", ioctl_((int)t, EVIOCGBIT(0, sizeof types), types));
    line("touch has abs", bit(types, EV_ABS_V));
    line("touch has rep", bit(types, EV_REP_V));
    line("types keyboard ret", ioctl_((int)k, EVIOCGBIT(0, sizeof types), types));
    line("keyboard has abs", bit(types, EV_ABS_V));
    line("keyboard has rep", bit(types, EV_REP_V));

    /* The codes: the touchscreen's axes and button, the keyboard's keys. */
    u8 abs[8];
    ioctl_((int)t, EVIOCGBIT(EV_ABS_V, sizeof abs), abs);
    line("touch abs slot", bit(abs, ABS_MT_SLOT_V));
    line("touch abs pressure", bit(abs, ABS_MT_PRESSURE_V));
    line("touch abs x (ABS_X)", bit(abs, ABS_X_V));
    u8 keys[96];
    ioctl_((int)t, EVIOCGBIT(EV_KEY_V, sizeof keys), keys);
    line("touch btn_touch", bit(keys, BTN_TOUCH_V));
    line("touch key a", bit(keys, KEY_A_V));
    ioctl_((int)k, EVIOCGBIT(EV_KEY_V, sizeof keys), keys);
    line("keyboard key a", bit(keys, KEY_A_V));
    line("keyboard key back", bit(keys, KEY_BACK_V));
    line("keyboard key f1", bit(keys, KEY_F1_V));
    line("keyboard btn_touch", bit(keys, BTN_TOUCH_V));

    /* The axes: the ranges are the display's (720 x 1280) and the slots. */
    struct { i32 value, min, max, fuzz, flat, res; } ai;
    line("abs x ret", ioctl_((int)t, EVIOCGABS(ABS_MT_POSITION_X_V), &ai));
    line("abs x max", ai.max);
    line("abs x min", ai.min);
    line("abs y max", (ioctl_((int)t, EVIOCGABS(ABS_MT_POSITION_Y_V), &ai), ai.max));
    line("abs slot max", (ioctl_((int)t, EVIOCGABS(ABS_MT_SLOT_V), &ai), ai.max));
    line("abs x of keyboard", ioctl_((int)k, EVIOCGABS(ABS_MT_POSITION_X_V), &ai));
    line("abs not set (ABS_X)", ioctl_((int)t, EVIOCGABS(ABS_X_V), &ai));

    /* The key state and the repeat. */
    u32 rep[2] = {100, 50};
    line("srep ret", ioctl_((int)k, EVIOCSREP, rep));
    u32 got[2] = {0, 0};
    line("grep ret", ioctl_((int)k, EVIOCGREP, got));
    line("grep delay", got[0]);
    line("grep period", got[1]);

    /* The clock: EVIOCSCLOCKID takes CLOCK_REALTIME (0), CLOCK_MONOTONIC (1), CLOCK_BOOTTIME (7). */
    int clk = 5;
    line("sclockid 5", ioctl_((int)t, EVIOCSCLOCKID, &clk));
    clk = 1;
    line("sclockid monotonic", ioctl_((int)t, EVIOCSCLOCKID, &clk));

    /* The reads: the touchscreen's frame (7 events: four ABS, BTN_TOUCH, SYN_REPORT), then the keyboard's (4). */
    ev_rec ev[16];
    i64 n = rd((int)t, ev, sizeof ev);
    line("read touch bytes", n);
    print_events("touch", ev, n / 24);
    line("read short buffer", rd((int)t, ev, 23));
    line("read drained", rd((int)t, ev, sizeof ev));
    n = rd((int)k, ev, sizeof ev);
    line("read keyboard bytes", n);
    print_events("keyboard", ev, n / 24);

    /* The keyboard's state after the tap: A is up. */
    ioctl_((int)k, EVIOCGKEY(sizeof keys), keys);
    line("key state a", bit(keys, KEY_A_V));

    /* EVIOCGRAB: one fd holds the touchscreen, a second open cannot take it, and it is given back. */
    int one = 1, zero = 0;
    line("grab", ioctl_((int)t, EVIOCGRAB, &one));
    i64 t2 = openat("/dev/input/event0", O_RDWR_V | O_NONBLOCK_V | O_CLOEXEC_V);
    line("grab by second fd", ioctl_((int)t2, EVIOCGRAB, &one));
    line("ungrab", ioctl_((int)t, EVIOCGRAB, &zero));
    sc6(SYS_CLOSE, t2, 0, 0, 0, 0, 0);

    /* The keyboard's Back key at 300 ms: epoll on the keyboard, then its read. */
    i64 ep = sc6(SYS_EPOLL_CREATE1, O_CLOEXEC_V, 0, 0, 0, 0, 0);
    struct { u32 events; u32 pad; u64 data; } ee = {EPOLLIN_V, 0, (u64)k};
    line("epoll_ctl", sc6(SYS_EPOLL_CTL, ep, EPOLL_CTL_ADD_V, k, (i64)&ee, 0, 0));
    struct { u32 events; u32 pad; u64 data; } out[4];
    line("epoll_pwait", sc6(SYS_EPOLL_PWAIT, ep, (i64)out, 4, 3000, 0, 8));
    line("epoll events", out[0].events);
    n = rd((int)k, ev, sizeof ev);
    line("read back bytes", n);
    print_events("keyboard", ev, n / 24);

    sc6(SYS_CLOSE, t, 0, 0, 0, 0, 0);
    sc6(SYS_CLOSE, k, 0, 0, 0, 0, 0);
    sc6(SYS_CLOSE, ep, 0, 0, 0, 0, 0);
    sys_exit_group(0);
}
