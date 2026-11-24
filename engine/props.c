/* Android system properties: the property area, the property service, and the
 * engine's values. See props.h.
 *
 * The area layout and the update protocol are bionic's (system_properties/prop_area.cpp,
 * prop_info.cpp and system_properties.cpp, android-13.0.0_r1). The engine writes the
 * area as init does, through a shadow buffer that is copied into one memfd after
 * each change. A guest reads that memfd through its mapping, so it sees the change as
 * soon as the engine returns. Each word is kept little-endian, as in the file.
 *
 * File offsets (what a guest sees, from the start of /dev/__properties__):
 *   0    bytes_used   (data bytes allocated so far, counted from data[0])
 *   4    serial       (area serial, futex-waited by bionic)
 *   8    magic        0x504f5250
 *   12   version      0xfc6ed0ab
 *   16   reserved[28]
 *   128  data[]       the trie root (prop_bt at data 0), then the dirty backup
 *                     area (data 20, 92 bytes), then allocations
 * A prop_bt has namelen, prop, left, right, children (20 bytes), then the segment
 * name and a NUL. A prop_info has serial (4 bytes), then value[92] or, for a long
 * value, error_message[56] and an offset, then the full name and a NUL. The offsets
 * stored in the trie are relative to data[0]. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl_display.h"
#include "init.h"
#include "memfd.h"
#include "props.h"
#include "sched.h"
#include "vfs.h"

/* bionic's status codes (sys/_system_properties.h). */
#define PROP_SUCCESS 0u
#define PROP_ERROR_READ_CMD 0x0004u
#define PROP_ERROR_READ_DATA 0x0008u
#define PROP_ERROR_READ_ONLY_PROPERTY 0x000Bu
#define PROP_ERROR_INVALID_NAME 0x0010u
#define PROP_ERROR_INVALID_VALUE 0x0014u
#define PROP_ERROR_INVALID_CMD 0x001Bu
#define PROP_ERROR_SET_FAILED 0x0024u

/* bionic's message commands (sys/_system_properties.h). */
#define PROP_MSG_SETPROP 1u
#define PROP_MSG_SETPROP2 0x00020001u

#define PROP_NAME_MAX 32
#define PROP_VALUE_MAX 92

#define AREA_SIZE (256u * 1024u) /* fixed: a process maps it once, so it cannot grow */
#define AREA_HDR 128u
#define AREA_DATA (AREA_SIZE - AREA_HDR)
#define AREA_MAGIC 0x504f5250u
#define AREA_VERSION 0xfc6ed0abu

#define BT_SIZE 20u   /* prop_bt: namelen, prop, left, right, children */
#define PI_SIZE 96u   /* prop_info: serial, value[92] */
#define PI_LFLAG 0x10000u /* kLongFlag in the serial of a long record */
#define PI_LONG_OFF 60u   /* long record: the offset of the value, after error_message[56] */
#define DIRTY_BACKUP_DATA BT_SIZE /* dirty_backup_area() is data + sizeof(prop_bt) */

static const char long_legacy_error[] = "Must use __system_property_read_callback() to read";

/* Area state. A is the shadow, area_memfd holds the same bytes for the guests. */
static uint8_t *A;
static memfd_t *area_memfd;

static uint32_t rd32(uint32_t fo)
{
    const uint8_t *p = A + fo;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void wr32(uint32_t fo, uint32_t v)
{
    uint8_t *p = A + fo;
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* File offset of a data offset. */
static uint32_t F(uint32_t d)
{
    return AREA_HDR + d;
}

/* Wakes the futex words at file offset fo of the area (bionic's __futex_wake). The
 * key is the file offset and the object is the memfd, as mem_futex_key gives for a
 * word in a shared mapping of it. */
static void wake_at(uint32_t fo)
{
    sched_futex_wake((uint64_t)fo, (uint64_t)(uintptr_t)area_memfd, (uint64_t)INT32_MAX, 0xFFFFFFFFu);
}

static void area_flush(void)
{
    if (memfd_pwrite(area_memfd, A, AREA_SIZE, 0) != (int64_t)AREA_SIZE)
        fprintf(stderr, "xiaoji: property area: write failed\n");
}

/* prop_area::allocate_obj: returns the data offset, or 0 when the area is full. */
static uint32_t alloc_obj(size_t size)
{
    size_t aligned = (size + 3) & ~(size_t)3;
    uint32_t used = rd32(0);
    if ((size_t)used + aligned > AREA_DATA)
        return 0;
    wr32(0, used + (uint32_t)aligned);
    return used;
}

static int cmp_name(const char *a, size_t an, const char *b, size_t bn)
{
    if (an < bn)
        return -1;
    if (an > bn)
        return 1;
    return memcmp(a, b, an);
}

static uint32_t bt_namelen(uint32_t d) { return rd32(F(d)); }
static uint32_t bt_prop(uint32_t d) { return rd32(F(d) + 4); }
static uint32_t bt_left(uint32_t d) { return rd32(F(d) + 8); }
static uint32_t bt_right(uint32_t d) { return rd32(F(d) + 12); }
static uint32_t bt_children(uint32_t d) { return rd32(F(d) + 16); }
static const char *bt_name(uint32_t d) { return (const char *)A + F(d) + BT_SIZE; }

/* prop_area::new_prop_bt. Returns the data offset, or 0. */
static uint32_t new_bt(const char *name, size_t n)
{
    uint32_t d = alloc_obj(BT_SIZE + n + 1);
    if (!d)
        return 0;
    wr32(F(d), (uint32_t)n);
    wr32(F(d) + 4, 0);
    wr32(F(d) + 8, 0);
    wr32(F(d) + 12, 0);
    wr32(F(d) + 16, 0);
    memcpy(A + F(d) + BT_SIZE, name, n);
    A[F(d) + BT_SIZE + n] = 0;
    return d;
}

/* prop_area::find_prop_bt: finds the node for one segment in the sibling list that
 * starts at cur, adding it when alloc is set. Returns the data offset, or 0. */
static uint32_t find_bt(uint32_t cur, const char *name, size_t n, int alloc)
{
    for (;;) {
        int r = cmp_name(name, n, bt_name(cur), bt_namelen(cur));
        if (r == 0)
            return cur;
        if (r < 0) {
            uint32_t l = bt_left(cur);
            if (l) {
                cur = l;
                continue;
            }
            if (!alloc)
                return 0;
            uint32_t nb = new_bt(name, n);
            if (nb)
                wr32(F(cur) + 8, nb);
            return nb;
        }
        uint32_t rr = bt_right(cur);
        if (rr) {
            cur = rr;
            continue;
        }
        if (!alloc)
            return 0;
        uint32_t nb = new_bt(name, n);
        if (nb)
            wr32(F(cur) + 12, nb);
        return nb;
    }
}

/* prop_info record for a value: short (inline) or long (error_message, then the value
 * stored after the record). Returns the data offset, or 0. */
static uint32_t new_pi(const char *name, size_t n, const char *value, size_t vlen)
{
    uint32_t d = alloc_obj(PI_SIZE + n + 1);
    if (!d)
        return 0;
    if (vlen >= PROP_VALUE_MAX) {
        uint32_t loff = alloc_obj(vlen + 1);
        if (!loff)
            return 0;
        memcpy(A + F(loff), value, vlen);
        A[F(loff) + vlen] = 0;
        size_t elen = sizeof long_legacy_error - 1;
        wr32(F(d), ((uint32_t)elen << 24) | PI_LFLAG);
        memcpy(A + F(d) + 4, long_legacy_error, sizeof long_legacy_error);
        wr32(F(d) + PI_LONG_OFF, loff - d);
    } else {
        wr32(F(d), (uint32_t)vlen << 24);
        memcpy(A + F(d) + 4, value, vlen);
        A[F(d) + 4 + vlen] = 0;
    }
    memcpy(A + F(d) + PI_SIZE, name, n);
    A[F(d) + PI_SIZE + n] = 0;
    return d;
}

static int is_long(uint32_t d)
{
    return (rd32(F(d)) & PI_LFLAG) != 0;
}

/* prop_area::find_property. Walks the trie one dot-separated segment at a time. With
 * alloc, missing nodes and the record are added. Returns the prop_info's data
 * offset, or 0. */
static uint32_t find_property(const char *name, size_t nlen, const char *value, size_t vlen, int alloc)
{
    uint32_t cur = 0; /* the root */
    const char *p = name, *end = name + nlen;
    for (;;) {
        const char *dot = memchr(p, '.', (size_t)(end - p));
        int want_subtree = dot != NULL;
        size_t sub = want_subtree ? (size_t)(dot - p) : (size_t)(end - p);
        if (!sub)
            return 0;
        uint32_t root = bt_children(cur);
        if (!root && alloc) {
            root = new_bt(p, sub);
            if (root)
                wr32(F(cur) + 16, root);
        }
        if (!root)
            return 0;
        cur = find_bt(root, p, sub, alloc);
        if (!cur)
            return 0;
        if (!want_subtree)
            break;
        p = dot + 1;
    }
    uint32_t pr = bt_prop(cur);
    if (pr)
        return pr;
    if (!alloc)
        return 0;
    uint32_t d = new_pi(name, nlen, value, vlen);
    if (d)
        wr32(F(cur) + 4, d);
    return d;
}

static uint32_t lookup(const char *name, size_t nlen)
{
    return find_property(name, nlen, NULL, 0, 0);
}

static void bump_area_serial(void)
{
    wr32(4, rd32(4) + 1);
    wake_at(4);
}

/* SystemProperties::Update: the writer's sequence, with the dirty bit and the backup
 * copy, so a reader that races with it sees a consistent value. */
static void update_pi(uint32_t d, const char *value, size_t len)
{
    uint32_t fo = F(d);
    uint32_t serial = rd32(fo);
    uint32_t old_len = serial >> 24;
    memcpy(A + F(DIRTY_BACKUP_DATA), A + fo + 4, old_len + 1);
    serial |= 1;
    wr32(fo, serial);
    memcpy(A + fo + 4, value, len);
    A[fo + 4 + len] = 0;
    wr32(fo, ((uint32_t)len << 24) | ((serial + 1) & 0xffffffu));
    wake_at(fo);
    bump_area_serial();
}

/* init's IsLegalPropertyName: alphanumerics and '.', '-', '@', ':', '_', with no empty
 * segment and no leading or trailing dot. */
static int legal_name(const char *n, size_t len)
{
    if (len < 1 || n[0] == '.' || n[len - 1] == '.')
        return 0;
    for (size_t i = 0; i < len; i++) {
        char c = n[i];
        if (c == '.') {
            if (n[i - 1] == '.')
                return 0;
            continue;
        }
        if (c == '_' || c == '-' || c == '@' || c == ':')
            continue;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            continue;
        return 0;
    }
    return 1;
}

static int is_ro(const char *name, size_t n)
{
    return n >= 3 && !memcmp(name, "ro.", 3);
}

/* Sets a property from the engine or a file, bypassing the service's rules. Existing
 * records are updated in place. A long record cannot be updated, so it is an error. */
static int put(const char *name, size_t n, const char *value, size_t vlen)
{
    if (vlen >= PROP_VALUE_MAX && !is_ro(name, n))
        return -1;
    uint32_t d = lookup(name, n);
    if (d) {
        if (is_long(d) || vlen >= PROP_VALUE_MAX)
            return -1;
        update_pi(d, value, vlen);
        return 0;
    }
    if (!find_property(name, n, value, vlen, 1))
        return -1;
    bump_area_serial();
    return 0;
}

/* A line of a build.prop file: key=value, with '#' comments and blank lines skipped.
 * ro.* keeps its first value; any other key takes the last one. */
static void file_line(const char *line, size_t len)
{
    if (len && line[len - 1] == '\r')
        len--;
    if (!len || line[0] == '#')
        return;
    const char *eq = memchr(line, '=', len);
    if (!eq)
        return;
    size_t nlen = (size_t)(eq - line), vlen = len - nlen - 1;
    const char *value = eq + 1;
    if (!legal_name(line, nlen))
        return;
    if (is_ro(line, nlen)) {
        if (lookup(line, nlen))
            return; /* first value wins */
    } else if (vlen >= PROP_VALUE_MAX) {
        return; /* init refuses a long value for a non-ro property */
    }
    put(line, nlen, value, vlen);
}

static void load_file(const char *path)
{
    uint8_t *data = NULL;
    size_t len = 0;
    char canon[VFS_PATH_MAX];
    if (vfs_load_file(path, &data, &len, canon, sizeof canon) != 0)
        return;
    size_t i = 0;
    while (i < len) {
        size_t e = i;
        while (e < len && data[e] != '\n')
            e++;
        file_line((const char *)data + i, e - i);
        i = e + 1;
    }
    free(data);
}

static const char *const build_props[] = {
    "/system/etc/prop.default",
    "/system/build.prop",
    "/system_ext/etc/build.prop",
    "/vendor/default.prop",
    "/vendor/build.prop",
    "/odm/etc/build.prop",
    "/product/etc/build.prop",
};

/* The values the engine sets itself (an override, so they win over the files). */
static const char *const engine_props[][2] = {
    {"ro.hardware", "xiaoji"},
    {"ro.boot.hardware", "xiaoji"},
    {"ro.kernel.qemu", "0"},
    {"ro.debuggable", "1"},
    {"ro.secure", "0"},
    {"ro.adb.secure", "0"},
    {"ro.zygote", "zygote64"},
    {"ro.xiaoji.engine", "b2"},
    {"ro.property_service.version", "2"}, /* the client uses protocol 2 only at 2 or more */
    /* APEX: the rootfs shows APEXes as plain directories under /apex (a flattened build), and xiaoji
     * installs no APEX updates. With this, ApexManager uses ApexManagerFlattenedApex, which does not
     * wait for apexservice (vendor/build.prop says true, and an override is needed to win). */
    {"ro.apex.updatable", "false"},
    /* Graphics (docs/graphics.md, A2): the engine plays the emulator host. EGL is the goldfish
     * libEGL_emulation (encoders over /dev/goldfish_pipe "pipe:opengles"); the composer is
     * hwcomposer.ranchu (HostComposer); gralloc is the ranchu allocator/mapper (goldfish_address_space).
     * No ro.boot.qemu.gltransport*: the guest uses the pipe transport. No Vulkan (ro.hardware.vulkan unset). */
    {"ro.hardware.egl", "emulation"},
    {"ro.hardware.hwcomposer", "ranchu"},
    {"ro.hardware.gralloc", "ranchu"},
    {"ro.opengles.version", "196608"},
    {"debug.renderengine.backend", "gles"},
    /* The display's density (gl_display.h): 240 dpi, with FB_XDPI the host reports the same. */
    {"ro.sf.lcd_density", XJ_FB_DPI_STR},
};

int props_init(const char *const *overrides, int noverrides)
{
    area_memfd = memfd_new("properties");
    if (!area_memfd)
        return -1;
    if (memfd_truncate(area_memfd, AREA_SIZE) != 0)
        return -1;
    A = calloc(1, AREA_SIZE);
    if (!A)
        return -1;
    /* prop_area's constructor: bytes_used starts after the root and the backup area. */
    wr32(0, BT_SIZE + ((PROP_VALUE_MAX + 3) & ~3u));
    wr32(4, 0);
    wr32(8, AREA_MAGIC);
    wr32(12, AREA_VERSION);

    for (size_t i = 0; i < sizeof build_props / sizeof build_props[0]; i++)
        load_file(build_props[i]);
    for (size_t i = 0; i < sizeof engine_props / sizeof engine_props[0]; i++) {
        const char *k = engine_props[i][0], *v = engine_props[i][1];
        put(k, strlen(k), v, strlen(v));
    }
    for (int i = 0; i < noverrides; i++) {
        const char *eq = strchr(overrides[i], '=');
        if (!eq) {
            fprintf(stderr, "xiaoji: --prop %s: expected K=V\n", overrides[i]);
            continue;
        }
        size_t nlen = (size_t)(eq - overrides[i]);
        if (!legal_name(overrides[i], nlen) || put(overrides[i], nlen, eq + 1, strlen(eq + 1)) != 0)
            fprintf(stderr, "xiaoji: --prop %s: not set (bad name, or a long or existing long value)\n",
                    overrides[i]);
    }
    area_flush();
    return 0;
}

memfd_t *props_memfd(void)
{
    return area_memfd;
}

uint32_t props_set(const char *name, size_t n, const char *value, size_t vlen)
{
    if (!area_memfd)
        return PROP_ERROR_SET_FAILED;
    if (!legal_name(name, n))
        return PROP_ERROR_INVALID_NAME;
    int ro = is_ro(name, n);
    if (vlen >= PROP_VALUE_MAX && !ro)
        return PROP_ERROR_INVALID_VALUE;
    if (n >= 4 && !memcmp(name, "ctl.", 4)) {
        /* init acts on these (start, stop, restart a service), between slices. Without init
         * they are logged and ignored. */
        if (init_active()) {
            init_ctl_request(name, n, value, vlen);
            return PROP_SUCCESS;
        }
        fprintf(stderr, "xiaoji: property service: %.*s=%.*s (ctl: no init)\n", (int)n, name,
                (int)vlen, value);
        return PROP_SUCCESS;
    }
    uint32_t d = lookup(name, n);
    if (d) {
        /* ro.* can be set once. */
        if (ro)
            return PROP_ERROR_READ_ONLY_PROPERTY;
        if (is_long(d))
            return PROP_ERROR_INVALID_VALUE;
        update_pi(d, value, vlen);
    } else {
        if (!find_property(name, n, value, vlen, 1))
            return PROP_ERROR_SET_FAILED;
        bump_area_serial();
    }
    area_flush();
    init_prop_changed(name, n, value, vlen);
    return PROP_SUCCESS;
}

/* ---- the property service endpoint (PROP_MSG_SETPROP2 and the legacy message) ---- */

#define PROP_MSG_MAX_STR (64u * 1024u)

typedef struct {
    uint8_t *in;
    size_t nin, capin;
    uint8_t *out;
    size_t nout, outpos, capout;
    int done; /* the legacy message ends the connection's input */
} pconn;

/* strnlen is POSIX, not C11: the length of s, at most max. */
static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int buf_grow(uint8_t **b, size_t *cap, size_t need)
{
    if (need <= *cap)
        return 0;
    size_t nc = *cap ? *cap : 256;
    while (nc < need)
        nc *= 2;
    uint8_t *nb = realloc(*b, nc);
    if (!nb)
        return -1;
    *b = nb;
    *cap = nc;
    return 0;
}

static void reply(pconn *c, uint32_t code)
{
    if (buf_grow(&c->out, &c->capout, c->nout + 4) != 0)
        return;
    c->out[c->nout] = (uint8_t)code;
    c->out[c->nout + 1] = (uint8_t)(code >> 8);
    c->out[c->nout + 2] = (uint8_t)(code >> 16);
    c->out[c->nout + 3] = (uint8_t)(code >> 24);
    c->nout += 4;
}

static void consume(pconn *c, size_t n)
{
    memmove(c->in, c->in + n, c->nin - n);
    c->nin -= n;
}

/* Parses as many whole messages as the input holds. A bad or unknown command gets an
 * error reply and ends the input, as init does. */
static void parse(pconn *c)
{
    while (!c->done) {
        if (c->nin < 4)
            return;
        uint32_t cmd = le32(c->in);
        if (cmd == PROP_MSG_SETPROP2) {
            if (c->nin < 8)
                return;
            uint32_t nl = le32(c->in + 4);
            if (nl > PROP_MSG_MAX_STR) {
                reply(c, PROP_ERROR_READ_DATA);
                c->done = 1;
                return;
            }
            if (c->nin < 8 + (size_t)nl + 4)
                return;
            uint32_t vl = le32(c->in + 8 + nl);
            if (vl > PROP_MSG_MAX_STR) {
                reply(c, PROP_ERROR_READ_DATA);
                c->done = 1;
                return;
            }
            if (c->nin < 12 + (size_t)nl + vl)
                return;
            uint32_t code = props_set((const char *)c->in + 8, nl, (const char *)c->in + 12 + nl, vl);
            reply(c, code);
            consume(c, 12 + (size_t)nl + vl);
        } else if (cmd == PROP_MSG_SETPROP) {
            /* struct prop_msg: cmd, name[32], value[92]. No reply; init closes the socket. */
            if (c->nin < 4 + PROP_NAME_MAX + PROP_VALUE_MAX)
                return;
            const char *name = (const char *)c->in + 4, *value = (const char *)c->in + 4 + PROP_NAME_MAX;
            size_t nl = bounded_len(name, PROP_NAME_MAX), vl = bounded_len(value, PROP_VALUE_MAX);
            props_set(name, nl, value, vl);
            c->done = 1;
        } else {
            reply(c, PROP_ERROR_INVALID_CMD);
            c->done = 1;
        }
    }
}

static void *conn_connect(void)
{
    return calloc(1, sizeof(pconn));
}

static int64_t conn_write(void *conn, const uint8_t *buf, size_t n)
{
    pconn *c = conn;
    if (c->done)
        return (int64_t)n; /* input after the end is discarded */
    if (buf_grow(&c->in, &c->capin, c->nin + n) != 0)
        return -LE_NOMEM;
    memcpy(c->in + c->nin, buf, n);
    c->nin += n;
    parse(c);
    return (int64_t)n;
}

static int64_t conn_read(void *conn, uint8_t *buf, size_t n)
{
    pconn *c = conn;
    size_t avail = c->nout - c->outpos;
    if (n > avail)
        n = avail;
    memcpy(buf, c->out + c->outpos, n);
    c->outpos += n;
    return (int64_t)n;
}

static void conn_close(void *conn)
{
    pconn *c = conn;
    free(c->in);
    free(c->out);
    free(c);
}

static const sock_ops service_ops = {conn_connect, conn_write, conn_read, conn_close};

const sock_ops *props_service_ops(void)
{
    return &service_ops;
}
