/* goldfish-pipe (G1): /dev/goldfish_pipe and the renderControl handshake (docs/graphics.md, sections 1.4 and
 * 3.2). The steps are those of shared/qemupipe (qemu_pipe_open_ns: open O_RDWR|O_NONBLOCK, then write
 * "pipe:<service>\0") and of HostConnection (the opengles stream starts with a zero clientFlags word):
 *
 *   1. "opengles": rcGetRendererVersion (OP 10000, 8 bytes) and rcGetFBParam (OP 10007, 12 bytes) for
 *      each parameter; the replies are 4-byte ints.
 *   2. "GLProcessPipe": int32 100, then the reply is a uint64 process id (nonzero).
 *   3. An unknown service name is refused (EINVAL), and a read with no reply pending is EAGAIN.
 *
 * The builder VM has no goldfish device, so there is no native oracle. The expected output is
 * tests/expect/goldfish-pipe.out, which holds the host's values (vulkan-cereal stream-servers). */
#include "sys.h"

#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_READ 63
#define AT_FDCWD (-100)
#define O_RDWR 2
#define O_NONBLOCK 04000

static i64 sys_openat(const char *path, int flags) { return sc6(SYS_OPENAT, AT_FDCWD, (i64)path, flags, 0, 0, 0); }
static i64 sys_read(int fd, void *buf, size_t n) { return sc6(SYS_READ, fd, (i64)buf, (i64)n, 0, 0, 0); }
static i64 sys_close(int fd) { return sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0); }

static void put32(u8 *p, u32 v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (u8)(v >> (8 * i));
}

static u32 get32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static void line(const char *tag, i64 r)
{
    put(tag);
    put(" ");
    putd(r);
    put("\n");
}

/* One opengles request and its 4-byte reply. */
static void rc_call(int fd, const char *tag, const u8 *req, size_t len)
{
    u8 rep[4];
    i64 w = sys_write(fd, req, len);
    i64 r = sys_read(fd, rep, 4);
    put(tag);
    put(" write ");
    putd(w);
    put(" read ");
    putd(r);
    put(" reply ");
    putd((i64)(i32)get32(rep));
    put("\n");
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static const char opengles[] = "/dev/goldfish_pipe";
    u8 req[12];

    int fd = (int)sys_openat(opengles, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        line("open /dev/goldfish_pipe", fd);
        return 1;
    }
    /* The service name, with its NUL, and the clientFlags word (zero). */
    static const char svc[] = "pipe:opengles";
    line("name opengles", sys_write(fd, svc, sizeof svc));
    static const u8 flags[4] = {0, 0, 0, 0};
    line("clientFlags", sys_write(fd, flags, 4));

    /* rcGetRendererVersion: opcode 10000, total size 8. */
    put32(req, 10000);
    put32(req + 4, 8);
    rc_call(fd, "rcGetRendererVersion", req, 8);

    /* rcGetFBParam: opcode 10007, total size 12, then the parameter. */
    static const u32 params[] = {1, 2, 3, 4, 5, 6, 7, 9};
    static const char *const names[] = {"FB_WIDTH", "FB_HEIGHT", "FB_XDPI", "FB_YDPI", "FB_FPS",
                                        "FB_MIN_SWAP_INTERVAL", "FB_MAX_SWAP_INTERVAL", "unknown 9"};
    for (u32 i = 0; i < sizeof params / sizeof params[0]; i++) {
        put32(req, 10007);
        put32(req + 4, 12);
        put32(req + 8, params[i]);
        rc_call(fd, names[i], req, 12);
    }

    /* Nothing pending: the read is EAGAIN on a nonblocking pipe. */
    u8 none[4];
    line("read with no reply", sys_read(fd, none, 4));
    sys_close(fd);

    /* GLProcessPipe: int32 100 is answered with a uint64 process id. */
    fd = (int)sys_openat(opengles, O_RDWR | O_NONBLOCK);
    static const char proc[] = "pipe:GLProcessPipe";
    line("name GLProcessPipe", sys_write(fd, proc, sizeof proc));
    put32(req, 100);
    line("process request", sys_write(fd, req, 4));
    u8 id[8];
    i64 n = sys_read(fd, id, 8);
    line("process id bytes", n);
    int nonzero = 0;
    for (int i = 0; i < 8; i++)
        nonzero |= id[i] != 0;
    put("process id nonzero ");
    putd(nonzero);
    put("\n");
    sys_close(fd);

    /* An unknown service is refused. */
    fd = (int)sys_openat(opengles, O_RDWR | O_NONBLOCK);
    static const char bad[] = "pipe:nosuch";
    line("name nosuch", sys_write(fd, bad, sizeof bad));
    sys_close(fd);
    put("done\n");
    return 0;
}
