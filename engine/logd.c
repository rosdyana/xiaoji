/* The logd sink (logd.h). A datagram is liblog's log_header (struct
 * android_log_header_t: id, tid, realtime sec and nsec, 11 bytes packed), then for the
 * text buffers the priority (1 byte), the tag and the message, each NUL-terminated.
 * The binary buffers (events, stats, security, kernel) are not decoded. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "logd.h"
#include "proc.h"
#include "sched.h"

#define LOGD_HDR_LEN 11
/* log_id values from liblog's log.h: MAIN, RADIO, SYSTEM and CRASH are text. */
#define LOG_ID_MAIN 0
#define LOG_ID_RADIO 1
#define LOG_ID_SYSTEM 3
#define LOG_ID_CRASH 4

static int print_on;

void logd_set_print(int on)
{
    print_on = on != 0;
}

/* Android's priority letters (ANDROID_LOG_VERBOSE is 2). */
static char prio_letter(unsigned p)
{
    switch (p) {
    case 2: return 'V';
    case 3: return 'D';
    case 4: return 'I';
    case 5: return 'W';
    case 6: return 'E';
    case 7: return 'F';
    default: return '?';
    }
}

static int text_id(unsigned id)
{
    return id == LOG_ID_MAIN || id == LOG_ID_RADIO || id == LOG_ID_SYSTEM || id == LOG_ID_CRASH;
}

/* Returns the index of the first NUL in buf[from, n), or n if there is none. */
static size_t find_nul(const uint8_t *buf, size_t from, size_t n)
{
    size_t i = from;
    while (i < n && buf[i])
        i++;
    return i;
}

static void print_datagram(const uint8_t *buf, size_t n)
{
    if (n < LOGD_HDR_LEN + 1 || !text_id(buf[0]))
        return;
    unsigned prio = buf[LOGD_HDR_LEN];
    size_t tag_end = find_nul(buf, LOGD_HDR_LEN + 1, n);
    size_t msg_start = tag_end < n ? tag_end + 1 : n;
    size_t msg_end = find_nul(buf, msg_start, n);
    /* The sender is the calling process: the datagram carries no pid. */
    proc_t *p = proc_cur();
    /* XIAOJI_STAMP=1: each line starts with the time since the first line and the instructions run
     * so far, so a milestone's cost can be read from the log. */
    static int stamp = -1;
    static uint64_t t0;
    if (stamp < 0) {
#ifdef XIAOJI_WEB
        /* The browser build always stamps: the page's milestone strip reads the counts (W1). */
        stamp = 1;
#else
        const char *e = getenv("XIAOJI_STAMP");
        stamp = e && e[0] == '1';
#endif
        t0 = host_monotonic_ns();
    }
    if (stamp)
        fprintf(stderr, "[%llu ms %llu insn] ", (unsigned long long)((host_monotonic_ns() - t0) / 1000000ull),
                (unsigned long long)sched_icount());
    fprintf(stderr, "%c/%.*s(%d): %.*s\n", prio_letter(prio), (int)(tag_end - (LOGD_HDR_LEN + 1)),
            (const char *)buf + LOGD_HDR_LEN + 1, p ? p->pid : 0, (int)(msg_end - msg_start),
            (const char *)buf + msg_start);
    stats_log_hook((const char *)buf + msg_start);
}

static void *logd_connect(void)
{
    /* Any non-NULL token: the endpoint keeps no state per connection. */
    return calloc(1, 1);
}

static int64_t logd_write(void *conn, const uint8_t *buf, size_t n)
{
    (void)conn;
    if (print_on)
        print_datagram(buf, n);
    return (int64_t)n;
}

/* Nothing is ever queued for a logd client. */
static int64_t logd_read(void *conn, uint8_t *buf, size_t n)
{
    (void)conn;
    (void)buf;
    (void)n;
    return 0;
}

static void logd_close(void *conn)
{
    free(conn);
}

static const sock_ops logd_table = {logd_connect, logd_write, logd_read, logd_close};

const sock_ops *logd_ops(void)
{
    return &logd_table;
}
