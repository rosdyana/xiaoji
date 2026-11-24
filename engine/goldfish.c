/* The goldfish devices (goldfish.h).
 *
 * The pipe (G1). The guest opens /dev/goldfish_pipe O_RDWR|O_NONBLOCK and writes "pipe:<service>\0"
 * (qemu_pipe_open_ns). After that the fd is a byte stream: writes carry requests and reads return replies
 * (shared/qemupipe/qemu_pipe_guest.cpp; HostConnection.cpp for the framing).
 *   opengles:      the first 4 bytes are clientFlags (zero), then GL requests (gl_host.c, the generated decoders).
 *   GLProcessPipe: a write of int32 100 is answered with the process id, a uint64.
 *   refcount:      a write of a color-buffer handle (uint32); nothing is returned; closing the pipe releases the reference.
 *   qemud:sensors: framed messages (4 hex digits, then the payload), the qemud sensors protocol of sensors.c.
 *                  Its event batches are made when a read finds the time due (goldfish_read, goldfish_readable,
 *                  and the deadline that vfs_pipe_block and poll wait on), so no thread runs for them.
 * A reply is produced during the write that asks for it, so the guest's read finds it at once.
 *
 * The address space (G1). The guest's ranchu allocator (shared/GoldfishAddressSpace, the HostMemoryAllocator
 * subdevice) does: PING with metadata = the subdevice type (the first ping sets it); ALLOCATE_BLOCK {size,
 * offset, phys_addr}; PING with offset, size and metadata 1 (ALLOCATE_ID); mmap(fd, offset). Its free is
 * PING metadata 2 (UNALLOCATE_ID) then DEALLOCATE_BLOCK. The blocks are ranges of one sparse memfd, so a
 * mapping of a block sees the same bytes as the engine. Without shared slots there are no CLAIM_SHARED calls. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl_host.h"
#include "goldfish.h"
#include "fdobj.h"
#include "linux.h"
#include "mem.h"
#include "poll.h"
#include "sched.h"
#include "sensors.h"
#include "vfs.h"

/* ---------- the pipe ---------- */

typedef enum { SVC_NONE, SVC_OPENGLES, SVC_PROCESS, SVC_REFCOUNT, SVC_SENSORS } gf_service;

typedef struct {
    int kind; /* GF_PIPE; first, for goldfish_read and the others */
    gf_service svc;
    int flags_pending; /* opengles: the clientFlags word is not read yet */
    int broken;        /* a malformed or unknown request: the stream takes no more input */
    uint64_t puid;     /* GLProcessPipe: the id this pipe reports */
    sns_pipe_t sns;    /* qemud:sensors: the enabled sensors and the batch schedule */
    uint8_t *in;       /* request bytes not yet decoded */
    size_t inlen, incap;
    uint8_t *out;      /* reply bytes not yet read */
    size_t outoff, outlen, outcap;
    uint32_t rc_handles[64]; /* refcount: the color buffers this pipe holds a reference to */
    uint32_t rc_n;
} gfpipe_t;

static uint64_t next_puid = 1;

/* XIAOJI_GF_TRACE=1: one line per pipe open, request, reply and device call (stderr). A debug aid. */
static int gf_trace(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("XIAOJI_GF_TRACE") != NULL;
    return on;
}

/* ---------- the sync device ---------- */

/* goldfish_sync.h: QUEUE_WORK and SIGNAL take goldfish_sync_ioctl_info (24 bytes: two handles, then the fd out). */
#define GF_SYNC_IOC_TYPE '@'
#define GF_SYNC_IOC_QUEUE_WORK 0
#define GF_SYNC_IOC_SIGNAL 1
#define GF_SYNC_INFO_SIZE 24

typedef struct {
    int kind; /* GF_SYNC */
} gfsync_t;

/* ---------- the address space ---------- */

#define GF_AS_WINDOW (256ULL << 20) /* the sparse memfd every block is carved from */
#define GF_AS_PAGE 4096ULL
#define GF_AS_MAX_BLOCKS 4096
#define GF_AS_MAX_FREE 1024

/* The subdevice command numbers (ALLOCATE_ID, UNALLOCATE_ID) and the ioctl numbers: goldfish_address_space_android.impl. */
#define GF_AS_ALLOCATE_ID 1
#define GF_AS_UNALLOCATE_ID 2
#define GF_IOC_ALLOCATE_BLOCK 10
#define GF_IOC_DEALLOCATE_BLOCK 11
#define GF_IOC_PING 12
#define GF_IOC_TYPE 'G'
#define GF_AS_ALLOCATE_SIZE 24 /* struct goldfish_address_space_allocate_block: size, offset, phys_addr */
#define GF_AS_PING_SIZE 40     /* struct address_space_ping: offset, size, metadata, version, wait_fd, wait_flags, direction */

typedef struct {
    uint64_t off, size;
} gf_range;

typedef struct {
    int kind; /* GF_ADDRESS_SPACE */
    int64_t subdev; /* -1 until the first ping sets the subdevice type */
    memfd_t *mem;   /* the window; the object holds one reference */
    uint64_t top;   /* the first byte past the blocks carved so far */
    int nblocks;
    gf_range blocks[GF_AS_MAX_BLOCKS]; /* the live blocks (offset, size) */
    int nfree;
    gf_range freeq[GF_AS_MAX_FREE];    /* freed ranges, reused first-fit */
} gfas_t;

/* ---------- common ---------- */

gf_kind goldfish_device(const char *path)
{
    if (!strcmp(path, "/dev/goldfish_pipe") || !strcmp(path, "/dev/qemu_pipe"))
        return GF_PIPE;
    if (!strcmp(path, "/dev/goldfish_address_space"))
        return GF_ADDRESS_SPACE;
    if (!strcmp(path, "/dev/goldfish_sync"))
        return GF_SYNC;
    return GF_NONE;
}

void *goldfish_open(gf_kind kind)
{
    if (kind == GF_PIPE) {
        gfpipe_t *p = calloc(1, sizeof *p);
        if (p)
            p->kind = GF_PIPE;
        return p;
    }
    if (kind == GF_SYNC) {
        gfsync_t *s = calloc(1, sizeof *s);
        if (s)
            s->kind = GF_SYNC;
        return s;
    }
    if (kind == GF_ADDRESS_SPACE) {
        gfas_t *a = calloc(1, sizeof *a);
        if (!a)
            return NULL;
        a->kind = GF_ADDRESS_SPACE;
        a->subdev = -1;
        a->mem = memfd_new("goldfish-address-space");
        if (!a->mem || memfd_truncate(a->mem, GF_AS_WINDOW) != 0) {
            if (a->mem)
                memfd_unref(a->mem);
            free(a);
            return NULL;
        }
        return a;
    }
    return NULL;
}

void goldfish_free(void *o)
{
    if (!o)
        return;
    int kind = *(int *)o;
    if (kind == GF_PIPE) {
        gfpipe_t *p = o;
        if (p->svc == SVC_REFCOUNT && p->rc_n) {
            const gl_host_t *h = gl_host_current();
            for (uint32_t i = 0; i < p->rc_n; i++)
                h->rc->rcCloseColorBuffer(p, p->rc_handles[i]);
        }
        free(p->in);
        free(p->out);
        free(p);
    } else if (kind == GF_ADDRESS_SPACE) {
        gfas_t *a = o;
        memfd_unref(a->mem);
        free(a);
    } else {
        free(o);
    }
}

uint64_t goldfish_key(void *o)
{
    return (uint64_t)(uintptr_t)o;
}

static void sns_pump(gfpipe_t *p);

int goldfish_readable(void *o)
{
    if (*(int *)o != GF_PIPE)
        return 0;
    gfpipe_t *p = o;
    sns_pump(p);
    return p->outoff < p->outlen;
}

/* The time the next batch is due (host monotonic ns), when a sensor is enabled. The pipe has no other timed wait. */
int goldfish_deadline(void *o, uint64_t *dl)
{
    if (*(int *)o != GF_PIPE)
        return 0;
    gfpipe_t *p = o;
    if (p->svc != SVC_SENSORS)
        return 0;
    return sns_deadline(&p->sns, dl);
}

int64_t goldfish_read(void *o, void *buf, size_t n)
{
    if (*(int *)o != GF_PIPE)
        return -LE_INVAL;
    gfpipe_t *p = o;
    sns_pump(p);
    size_t avail = p->outlen - p->outoff;
    if (!avail) {
        if (gf_trace())
            fprintf(stderr, "xiaoji: gf: pipe read %u bytes: none pending (blocks)\n", (unsigned)n);
        return -LE_AGAIN;
    }
    if (n > avail)
        n = avail;
    memcpy(buf, p->out + p->outoff, n);
    p->outoff += n;
    if (p->outoff == p->outlen)
        p->outoff = p->outlen = 0;
    return (int64_t)n;
}

/* ---------- the pipe: requests and replies ---------- */

static int buf_append(uint8_t **b, size_t *len, size_t *cap, const void *src, size_t n)
{
    if (*len + n > *cap) {
        size_t nc = *cap ? *cap : 256;
        while (nc < *len + n)
            nc *= 2;
        uint8_t *nb = realloc(*b, nc);
        if (!nb)
            return -1;
        *b = nb;
        *cap = nc;
    }
    memcpy(*b + *len, src, n);
    *len += n;
    return 0;
}

/* Drops the first n request bytes. */
static void in_drop(gfpipe_t *p, size_t n)
{
    memmove(p->in, p->in + n, p->inlen - n);
    p->inlen -= n;
}

static void reply(gfpipe_t *p, const uint8_t *src, size_t n)
{
    if (buf_append(&p->out, &p->outlen, &p->outcap, src, n) != 0)
        p->broken = 1; /* out of memory: the stream stops */
}

/* The sensors stream makes its next batch when it is due and nothing else is waiting to be read. */
static void sns_pump(gfpipe_t *p)
{
    if (p->svc != SVC_SENSORS || p->outlen > p->outoff)
        return;
    char b[SNS_BATCH_MAX];
    size_t n = sns_batch(&p->sns, fdobj_now(), b, sizeof b);
    if (n)
        reply(p, (const uint8_t *)b, n);
}

static uint32_t rd32(const uint8_t *q)
{
    return (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
}

/* Decodes what the request bytes hold. A partial request waits for the rest. */
static void process(gfpipe_t *p)
{
    if (p->svc == SVC_OPENGLES) {
        if (p->flags_pending) {
            if (p->inlen < 4)
                return;
            in_drop(p, 4);
            p->flags_pending = 0;
        }
        while (!p->broken && p->inlen >= 8) {
            uint32_t total = rd32(p->in + 4);
            if (total < 8) {
                p->broken = 1;
                break;
            }
            if (p->inlen < total)
                return;
            gl_reply rep = {0};
            if (gf_trace())
                fprintf(stderr, "xiaoji: gf: opengles request op %u, %u bytes\n", (unsigned)rd32(p->in), (unsigned)total);
            if (gl_host_decode(gl_host_current(), p, rd32(p->in), p->in, total, &rep) != 0) {
                fprintf(stderr, "xiaoji: goldfish: opengles: opcode %u is not decoded (packet %u bytes)\n",
                        (unsigned)rd32(p->in), (unsigned)total);
                free(rep.buf);
                p->broken = 1;
                break;
            }
            in_drop(p, total);
            if (rep.len)
                reply(p, rep.buf, rep.len);
            free(rep.buf);
        }
    } else if (p->svc == SVC_PROCESS) {
        if (gf_trace() && p->inlen)
            fprintf(stderr, "xiaoji: gf: GLProcessPipe %u bytes\n", (unsigned)p->inlen);
        while (p->inlen >= 4) {
            uint32_t v = rd32(p->in);
            in_drop(p, 4);
            if (v == 100) {
                uint8_t id[8];
                for (int i = 0; i < 8; i++)
                    id[i] = (uint8_t)(p->puid >> (8 * i));
                reply(p, id, 8);
            }
        }
    } else if (p->svc == SVC_SENSORS) {
        /* Frames: 4 hex digits of length, then the payload. A malformed header ends the stream. */
        while (p->inlen >= 4) {
            size_t len = 0;
            for (int i = 0; i < 4; i++) {
                char c = (char)p->in[i];
                int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (d < 0) {
                    p->broken = 1;
                    return;
                }
                len = len * 16 + (size_t)d;
            }
            if (p->inlen < 4 + len)
                return;
            char rb[64];
            size_t rl = sns_command(&p->sns, (const char *)p->in + 4, len, fdobj_now(), rb, sizeof rb);
            in_drop(p, 4 + len);
            if (rl)
                reply(p, (const uint8_t *)rb, rl);
        }
    } else {
        /* refcount: each write is a color-buffer handle (uint32), and this pipe holds one reference to it. The
         * references are released when the pipe closes (goldfish_free), as the host's refcount pipe does. */
        while (p->inlen >= 4) {
            if (p->rc_n < sizeof p->rc_handles / sizeof p->rc_handles[0])
                p->rc_handles[p->rc_n++] = rd32(p->in);
            in_drop(p, 4);
        }
    }
}

/* "pipe:<name>\0" at the start of buf: the service. Returns the length taken, or -1. */
static int64_t name_service(gfpipe_t *p, const uint8_t *buf, size_t n)
{
    size_t z = 0;
    while (z < n && buf[z])
        z++;
    if (z == n || n < 6 || memcmp(buf, "pipe:", 5) != 0)
        return -1;
    const char *name = (const char *)buf + 5;
    size_t nl = z - 5;
    if (nl == 8 && !memcmp(name, "opengles", 8)) {
        p->svc = SVC_OPENGLES;
        p->flags_pending = 1;
    } else if (nl == 13 && !memcmp(name, "GLProcessPipe", 13)) {
        p->svc = SVC_PROCESS;
        p->puid = next_puid++;
    } else if (nl == 8 && !memcmp(name, "refcount", 8)) {
        p->svc = SVC_REFCOUNT;
    } else if (nl == 13 && !memcmp(name, "qemud:sensors", 13)) {
        p->svc = SVC_SENSORS;
        sns_pipe_init(&p->sns);
    } else {
        return -1;
    }
    return (int64_t)z + 1;
}

int64_t goldfish_write(void *o, const void *buf, size_t n)
{
    if (*(int *)o != GF_PIPE)
        return -LE_INVAL;
    gfpipe_t *p = o;
    const uint8_t *s = buf;
    size_t i = 0;
    if (p->broken)
        return -LE_INVAL;
    if (p->svc == SVC_NONE) {
        int64_t taken = name_service(p, s, n);
        if (taken < 0)
            return -LE_INVAL;
        i = (size_t)taken;
    }
    if (gf_trace())
        fprintf(stderr, "xiaoji: gf: pipe write %u bytes (service %d, %u pending reply bytes)\n",
                (unsigned)n, (int)p->svc, (unsigned)(p->outlen - p->outoff));
    if (n > i && buf_append(&p->in, &p->inlen, &p->incap, s + i, n - i) != 0)
        return -LE_NOMEM;
    process(p);
    if (p->outlen > p->outoff || p->svc == SVC_SENSORS) {
        /* A reader blocked on an empty reply may go on. A sensor command may change when a batch is due
         * (set and set-delay), so a waiter with a deadline is woken as well. */
        sched_wake_key(goldfish_key(p));
        poll_notify();
    }
    return (int64_t)n;
}

/* ---------- the address space: blocks and ioctls ---------- */

static int as_alloc(gfas_t *a, uint64_t size, uint64_t *off)
{
    for (int i = 0; i < a->nfree; i++) {
        if (a->freeq[i].size >= size) {
            *off = a->freeq[i].off;
            a->freeq[i].off += size;
            a->freeq[i].size -= size;
            if (a->freeq[i].size == 0)
                a->freeq[i] = a->freeq[--a->nfree];
            return 0;
        }
    }
    if (a->top + size > GF_AS_WINDOW)
        return -1;
    *off = a->top;
    a->top += size;
    return 0;
}

static int as_find(gfas_t *a, uint64_t off)
{
    for (int i = 0; i < a->nblocks; i++)
        if (a->blocks[i].off == off)
            return i;
    return -1;
}

static void put_le(uint8_t *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_le(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static int64_t as_allocate(gfas_t *a, uint64_t arg)
{
    uint8_t b[GF_AS_ALLOCATE_SIZE];
    if (mem_copy_out(b, arg, sizeof b) < 0)
        return -LE_FAULT;
    uint64_t size = (get_le(b, 8) + GF_AS_PAGE - 1) & ~(GF_AS_PAGE - 1);
    if (!size || a->nblocks >= GF_AS_MAX_BLOCKS)
        return -LE_INVAL;
    uint64_t off;
    if (as_alloc(a, size, &off) != 0)
        return -LE_NOMEM;
    a->blocks[a->nblocks].off = off;
    a->blocks[a->nblocks].size = size;
    a->nblocks++;
    /* The kernel fills in the rounded size, the offset and the physical address (none here). */
    put_le(b, size, 8);
    put_le(b + 8, off, 8);
    put_le(b + 16, 0, 8);
    return mem_copy_in(arg, b, sizeof b) < 0 ? -LE_FAULT : 0;
}

static int64_t as_deallocate(gfas_t *a, uint64_t arg)
{
    uint8_t b[8];
    if (mem_copy_out(b, arg, sizeof b) < 0)
        return -LE_FAULT;
    int i = as_find(a, get_le(b, 8));
    if (i < 0)
        return -LE_INVAL;
    if (a->nfree < GF_AS_MAX_FREE)
        a->freeq[a->nfree++] = a->blocks[i];
    a->blocks[i] = a->blocks[--a->nblocks];
    return 0;
}

static int64_t as_ping(gfas_t *a, uint64_t arg)
{
    uint8_t b[GF_AS_PING_SIZE];
    if (mem_copy_out(b, arg, sizeof b) < 0)
        return -LE_FAULT;
    uint64_t md = get_le(b + 16, 8);
    if (a->subdev < 0) {
        /* The first ping sets the subdevice type; metadata comes back as the type. */
        a->subdev = (int64_t)md;
        return mem_copy_in(arg + 16, b + 16, 8) < 0 ? -LE_FAULT : 0;
    }
    /* A host command: the result is written to metadata, 0 for success. ALLOCATE_ID needs a block at the
     * offset given; UNALLOCATE_ID is always accepted. */
    int64_t res = -LE_INVAL;
    if (md == GF_AS_ALLOCATE_ID && as_find(a, get_le(b, 8)) >= 0)
        res = 0;
    else if (md == GF_AS_UNALLOCATE_ID)
        res = 0;
    put_le(b + 16, (uint64_t)res, 8);
    return mem_copy_in(arg + 16, b + 16, 8) < 0 ? -LE_FAULT : 0;
}

static int64_t sync_ioctl(uint32_t cmd, uint64_t arg)
{
    if (gf_trace())
        fprintf(stderr, "xiaoji: gf: sync ioctl %u\n", (unsigned)(cmd & 0xff));
    if (((cmd >> 8) & 0xff) != GF_SYNC_IOC_TYPE || ((cmd >> 16) & 0x3fff) != GF_SYNC_INFO_SIZE)
        return -LE_NOTTY;
    uint8_t b[GF_SYNC_INFO_SIZE];
    if (mem_copy_out(b, arg, sizeof b) < 0)
        return -LE_FAULT;
    switch (cmd & 0xff) {
    case GF_SYNC_IOC_QUEUE_WORK: {
        /* The work is done when it is queued (the null backend), so the fence is signalled. */
        void *e = efd_new(1, 0);
        if (!e)
            return -LE_NOMEM;
        int64_t fd = vfs_obj_new(VFS_EVENTFD, e, 0, LO_RDWR);
        if (fd < 0)
            return fd;
        put_le(b + 16, (uint64_t)fd, 4);
        return mem_copy_in(arg, b, sizeof b) < 0 ? -LE_FAULT : 0;
    }
    case GF_SYNC_IOC_SIGNAL:
        return 0;
    default:
        return -LE_NOTTY;
    }
}

int64_t goldfish_ioctl(void *o, uint32_t cmd, uint64_t arg)
{
    if (*(int *)o == GF_SYNC)
        return sync_ioctl(cmd, arg);
    if (*(int *)o != GF_ADDRESS_SPACE)
        return -LE_NOTTY;
    gfas_t *a = o;
    if (gf_trace())
        fprintf(stderr, "xiaoji: gf: address space ioctl %u\n", (unsigned)(cmd & 0xff));
    if (((cmd >> 8) & 0xff) != GF_IOC_TYPE)
        return -LE_NOTTY;
    uint32_t size = (cmd >> 16) & 0x3fff;
    switch (cmd & 0xff) {
    case GF_IOC_ALLOCATE_BLOCK:
        return size == GF_AS_ALLOCATE_SIZE ? as_allocate(a, arg) : -LE_NOTTY;
    case GF_IOC_DEALLOCATE_BLOCK:
        return size == 8 ? as_deallocate(a, arg) : -LE_NOTTY;
    case GF_IOC_PING:
        return size == GF_AS_PING_SIZE ? as_ping(a, arg) : -LE_NOTTY;
    default:
        return -LE_NOTTY; /* CLAIM_SHARED and UNCLAIM_SHARED: the shared-slot allocator is not advertised */
    }
}

memfd_t *goldfish_as_memfd(void *o)
{
    if (*(int *)o != GF_ADDRESS_SPACE)
        return NULL;
    gfas_t *a = o;
    memfd_ref(a->mem);
    return a->mem;
}
