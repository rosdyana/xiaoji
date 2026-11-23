/* The goldfish qemud "sensors" service (B7), from a guest program that speaks it as the sensors HAL does
 * (device/generic/goldfish/sensors/multihal_sensors.cpp, multihal_sensors_qemu.cpp, and qemud.cpp for the
 * framing: 4 lowercase hex digits of length, then the payload).
 *
 *   1. open /dev/goldfish_pipe O_RDWR (blocking, as qemu_pipe_open_ns does) and write "pipe:qemud:sensors\0".
 *   2. "time:<ns>" (no reply), then "list-sensors": a framed decimal mask. The advertised handles are
 *      0, 1, 2, 3, 5 and 6 (accelerometer, gyroscope, magnetic field, orientation, proximity, light): 111.
 *   3. Unknown commands and an unadvertised sensor are ignored, so nothing is readable.
 *   4. "set-delay:20" and "set:acceleration:1": the stream becomes readable, and ppoll waits for it (the HAL's
 *      epoll does the same). Three batches are read; each is an "acceleration:x:y:z" line with the static
 *      values (0, 9.81, 0) and a "sync:<us>" line.
 *   5. "set:acceleration:0": nothing more is readable.
 *
 * Only format and protocol facts are printed. No timestamp, and no count that depends on timing, is printed.
 * The builder VM has no goldfish device, so the expectation is tests/expect/sensors.out, from the engine's
 * values (sensors.c). */
#include <stdint.h>
#include "sys.h"

#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_READ 63
#define SYS_PPOLL 73
#define AT_FDCWD (-100)
#define O_RDWR 2
#define POLLIN 1

static i64 sys_openat(const char *path, int flags) { return sc6(SYS_OPENAT, AT_FDCWD, (i64)path, flags, 0, 0, 0); }
static i64 sys_read(int fd, void *buf, size_t n) { return sc6(SYS_READ, fd, (i64)buf, (i64)n, 0, 0, 0); }
static i64 sys_close(int fd) { return sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0); }

struct pollfd_ {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

/* ppoll with a relative timeout in ms; returns the ready count, 0 on timeout, or -errno. */
static i64 wait_readable(int fd, i64 ms)
{
    struct pollfd_ p = {fd, POLLIN, 0};
    i64 ts[2] = {ms / 1000, (ms % 1000) * 1000000};
    i64 r = sc6(SYS_PPOLL, (i64)&p, 1, (i64)ts, 0, 8, 0);
    return r < 0 ? r : (i64)(p.revents & POLLIN ? 1 : 0);
}

static int read_full(int fd, u8 *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        i64 r = sys_read(fd, buf + got, n - got);
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const char *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        i64 w = sys_write(fd, buf + done, n - done);
        if (w <= 0)
            return -1;
        done += (size_t)w;
    }
    return 0;
}

/* One framed message: 4 hex digits of the payload length (lowercase, as "%04x"), then the payload. */
static int send_frame(int fd, const char *msg)
{
    size_t n = 0;
    while (msg[n])
        n++;
    static const char hex[] = "0123456789abcdef";
    char hdr[4] = {hex[(n >> 12) & 15], hex[(n >> 8) & 15], hex[(n >> 4) & 15], hex[n & 15]};
    if (write_full(fd, hdr, 4) != 0)
        return -1;
    return write_full(fd, msg, n);
}

/* Reads one framed message into buf (NUL terminated). Returns its length, or -1. */
static int recv_frame(int fd, char *buf, size_t cap)
{
    u8 hdr[4];
    if (read_full(fd, hdr, 4) != 0)
        return -1;
    size_t n = 0;
    for (int i = 0; i < 4; i++) {
        u8 c = hdr[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0)
            return -1;
        n = n * 16 + (size_t)d;
    }
    if (n + 1 > cap)
        return -1;
    if (read_full(fd, (u8 *)buf, n) != 0)
        return -1;
    buf[n] = 0;
    return (int)n;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int starts(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s != *prefix)
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

static int is_digits(const char *s)
{
    if (!*s)
        return 0;
    for (; *s; s++)
        if (*s < '0' || *s > '9')
            return 0;
    return 1;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    static const char dev[] = "/dev/goldfish_pipe";
    static const char svc[] = "pipe:qemud:sensors";

    int fd = (int)sys_openat(dev, O_RDWR);
    if (fd < 0) {
        putd(fd);
        put("\nopen failed\n");
        return 1;
    }
    /* The service name with its NUL (qemu_pipe_open_ns writes strlen + 1 bytes). */
    if (write_full(fd, svc, sizeof svc) != 0) {
        put("name write failed\n");
        return 1;
    }
    put("open ok\n");

    if (send_frame(fd, "time:1000000000") != 0) {
        put("time send failed\n");
        return 1;
    }
    if (send_frame(fd, "list-sensors") != 0) {
        put("list send failed\n");
        return 1;
    }
    char msg[256];
    if (wait_readable(fd, 1000) != 1) {
        put("list reply not readable\n");
        return 1;
    }
    if (recv_frame(fd, msg, sizeof msg) < 0) {
        put("list reply malformed\n");
        return 1;
    }
    put("mask ");
    put(msg);
    put(streq(msg, "111") ? " ok\n" : " unexpected\n");

    /* Before any enable, nothing is readable: an unknown command, an unadvertised sensor and a
     * disabled sensor all leave the stream quiet. */
    if (send_frame(fd, "nosuch:command") != 0 || send_frame(fd, "set:temperature:1") != 0) {
        put("send failed\n");
        return 1;
    }
    put("quiet before enable ");
    put(wait_readable(fd, 0) == 0 ? "ok\n" : "failed\n");

    if (send_frame(fd, "set-delay:20") != 0 || send_frame(fd, "set:acceleration:1") != 0) {
        put("enable send failed\n");
        return 1;
    }
    for (int b = 0; b < 3; b++) {
        if (wait_readable(fd, 2000) != 1) {
            put("batch not readable\n");
            return 1;
        }
        /* A batch: the acceleration line, then sync. */
        if (recv_frame(fd, msg, sizeof msg) < 0) {
            put("acceleration malformed\n");
            return 1;
        }
        put("batch ");
        putd(b);
        put(" ");
        put(msg);
        put(streq(msg, "acceleration:0.000000:9.810000:0.000000") ? " ok\n" : " unexpected\n");
        if (recv_frame(fd, msg, sizeof msg) < 0) {
            put("sync malformed\n");
            return 1;
        }
        int sync_ok = starts(msg, "sync:") && is_digits(msg + 5);
        put("sync ");
        put(sync_ok ? "ok\n" : "unexpected\n");
    }

    if (send_frame(fd, "set:acceleration:0") != 0) {
        put("disable send failed\n");
        return 1;
    }
    /* Anything the stream still holds is read out, then nothing comes. The loop is bounded. */
    int drained = 0;
    for (int i = 0; i < 4 && !drained; i++) {
        if (wait_readable(fd, 100) == 1) {
            if (recv_frame(fd, msg, sizeof msg) < 0)
                break;
        } else {
            drained = 1;
        }
    }
    put("quiet after disable ");
    put(drained ? "ok\n" : "failed\n");

    sys_close(fd);
    put("done\n");
    return 0;
}
