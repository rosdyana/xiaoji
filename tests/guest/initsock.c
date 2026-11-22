/* Init-made sockets (B4a): one program in two roles, run as services by tests/init/sockets.rc.
 * server: takes the socket init made for it (ANDROID_SOCKET_xiaoji), checks its name is
 * /dev/socket/xiaoji, listens, accepts one connection, reads "ping" and answers "pong".
 * client: connects to /dev/socket/xiaoji (retrying until the server listens), sends "ping",
 * and reads "pong". Only run under init (it is not an oracle case: tests/run.sh skips it). */
#include "sys.h"

#define SYS_SOCKET 198
#define SYS_CONNECT 203
#define SYS_GETSOCKNAME 204
#define SYS_LISTEN 201
#define SYS_ACCEPT 202
#define SYS_SENDTO 206
#define SYS_RECVFROM 207
#define SYS_CLOSE 57
#define SYS_NANOSLEEP 101
#define AF_UNIX 1
#define SOCK_STREAM 1

static int memeq(const void *a, const void *b, size_t n);
static int streq_s(const char *a, const char *b);

static const char *env_value(const char *key)
{
    size_t kl = strlen(key);
    for (char **e = xiaoji_envp; *e; e++)
        if (memeq(*e, key, kl) && (*e)[kl] == '=')
            return *e + kl + 1;
    return 0;
}

static int parse_int(const char *s)
{
    int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

static u32 mkaddr(u8 *sa, const char *path)
{
    for (int i = 0; i < 110; i++)
        sa[i] = 0;
    sa[0] = AF_UNIX;
    size_t n = strlen(path);
    for (size_t i = 0; i <= n; i++)
        sa[2 + i] = (u8)path[i];
    return (u32)(2 + n + 1);
}

static void nap_ms(i64 ms)
{
    i64 ts[2] = {ms / 1000, (ms % 1000) * 1000000};
    sc6(SYS_NANOSLEEP, (i64)ts, 0, 0, 0, 0, 0);
}

static int server(void)
{
    const char *v = env_value("ANDROID_SOCKET_xiaoji");
    if (!v)
        return 1;
    int fd = parse_int(v);
    u8 sa[110];
    u32 len = 110;
    sc6(SYS_GETSOCKNAME, fd, (i64)sa, (i64)&len, 0, 0, 0);
    const char *want = "/dev/socket/xiaoji";
    int named = len == 2 + strlen(want) + 1 && memeq(sa + 2, want, strlen(want) + 1);
    put(named ? "server: name ok\n" : "server: name bad\n");
    if (sc6(SYS_LISTEN, fd, 4, 0, 0, 0, 0) != 0)
        return 1;
    int c = (int)sc6(SYS_ACCEPT, fd, 0, 0, 0, 0, 0);
    if (c < 0)
        return 1;
    char buf[8];
    int got = sc6(SYS_RECVFROM, c, (i64)buf, 4, 0, 0, 0) == 4 && memeq(buf, "ping", 4);
    put(got ? "server: got ping\n" : "server: bad request\n");
    sc6(SYS_SENDTO, c, (i64)"pong", 4, 0, 0, 0);
    sc6(SYS_CLOSE, c, 0, 0, 0, 0, 0);
    return named && got ? 0 : 1;
}

static int client(void)
{
    u8 sa[110];
    u32 len = mkaddr(sa, "/dev/socket/xiaoji");
    int fd = (int)sc6(SYS_SOCKET, AF_UNIX, SOCK_STREAM, 0, 0, 0, 0);
    int i = 0;
    while (sc6(SYS_CONNECT, fd, (i64)sa, len, 0, 0, 0) != 0) {
        if (++i > 200)
            return 1;
        nap_ms(20);
    }
    sc6(SYS_SENDTO, fd, (i64)"ping", 4, 0, 0, 0);
    char buf[8];
    int got = sc6(SYS_RECVFROM, fd, (i64)buf, 4, 0, 0, 0) == 4 && memeq(buf, "pong", 4);
    if (got)
        put("client: got pong\n");
    sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return got ? 0 : 1;
}

static int memeq(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;
    for (size_t i = 0; i < n; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

static int streq_s(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int main(int argc, char **argv)
{
    if (argc > 1 && streq_s(argv[1], "server"))
        return server();
    return client();
}
