/* Virtual evdev devices (evdev.h). The codes, the ioctl numbers and the struct layouts are the uapi's
 * (linux/input.h, linux/input-event-codes.h; arm64 uses the asm-generic ioctl encoding). */
#include "evdev.h"
#include "signals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "host.h"
#include "linux.h"
#include "mem.h"

/* ---- uapi constants ---- */
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_MSC 0x04
#define EV_SW 0x05
#define EV_LED 0x11
#define EV_SND 0x12
#define EV_REP 0x14
#define EV_FF 0x15
#define EV_PWR 0x16
#define EV_FF_STATUS 0x17
#define EV_MAX 0x1f
#define SYN_REPORT 0
#define SYN_DROPPED 3
#define INPUT_PROP_DIRECT 0x01
#define INPUT_PROP_MAX 0x1f
#define KEY_MAX 0x2ff
#define KEY_BTN_TOUCH 0x14a
#define REL_MAX 0x0f
#define ABS_MAX 0x3f
#define ABS_MT_SLOT 0x2f
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define ABS_MT_PRESSURE 0x3a
#define ABS_MT_TRACKING_ID 0x39
#define SW_MAX 0x0f
#define LED_MAX 0x0f
#define SND_MAX 0x07
#define MSC_MAX 0x07
#define REP_MAX 0x01
#define FF_MAX 0x7f
#define BUS_VIRTUAL 0x06
#define CLOCK_REALTIME_ID 0
#define CLOCK_MONOTONIC_ID 1
#define CLOCK_BOOTTIME_ID 7

/* ioctl encoding: dir in bits 30..31 (1 write, 2 read), size in 16..29, type in 8..15, nr in 0..7. */
#define IOC_WRITE 1u
#define IOC_READ 2u
#define EV_IOC_TYPE 'E'

#define BIT_BYTES 96 /* KEY_MAX + 1 bits, in whole longs: the largest bitmap */
#define EV_TYPES (EV_MAX + 1)
#define RING_CAP 256
#define EV_BYTES 24 /* sizeof(struct input_event) on arm64: timeval (2 x 8), type (2), code (2), value (4) */
#define MAX_OPEN 64

typedef struct {
    int32_t value, minimum, maximum, fuzz, flat, resolution;
} absinfo_t;

typedef struct vdev {
    const char *name;
    const char *phys;
    uint16_t vendor, product, version;
    uint8_t bits[EV_TYPES][BIT_BYTES]; /* the codes the device has, per type (EV_KEY: keys, EV_ABS: axes) */
    uint8_t keystate[BIT_BYTES];       /* EVIOCGKEY: the keys held down */
    uint8_t props[BIT_BYTES];          /* EVIOCGPROP: INPUT_PROP_* */
    absinfo_t abs[ABS_MAX + 1];
    uint32_t rep[2];                   /* EVIOCGREP / EVIOCSREP: delay and period, ms */
    struct evfd *grab;                 /* the fd that holds EVIOCGRAB, or NULL */
} vdev;

/* One open of a device: a queue of whole struct input_event records, each stamped in the fd's clock. */
typedef struct evfd {
    int dev;
    uint32_t clock;
    uint8_t ring[RING_CAP][EV_BYTES];
    unsigned head, count, complete; /* complete: the SYN_REPORT records queued */
    int overflow;                   /* an event was dropped: SYN_DROPPED goes in before the next one */
} evfd;

static evfd *opened[MAX_OPEN];
static vdev devs[EVDEV_COUNT];
static int devs_ready;
static int origin_set;
static uint64_t origin_ns __attribute__((unused)); /* read by the script only (the browser has none) */

static void bit_set(uint8_t *b, unsigned n) { b[n >> 3] |= (uint8_t)(1u << (n & 7)); }
static void bit_clear(uint8_t *b, unsigned n) { b[n >> 3] &= (uint8_t)~(1u << (n & 7)); }
static int bit_test(const uint8_t *b, unsigned n) { return (b[n >> 3] >> (n & 7)) & 1; }

static void put_le(uint8_t *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void devs_init(void)
{
    if (devs_ready)
        return;
    devs_ready = 1;

    /* The touchscreen: protocol B multi-touch, direct, the display's size. */
    vdev *t = &devs[EVDEV_TOUCH];
    t->name = "xiaoji_touchscreen";
    t->phys = "xiaoji/input0";
    t->vendor = 0x0001;
    t->product = 0x0001;
    t->version = 0x0001;
    bit_set(t->bits[EV_SYN], SYN_REPORT);
    bit_set(t->bits[EV_SYN], SYN_DROPPED);
    bit_set(t->bits[EV_KEY], KEY_BTN_TOUCH);
    bit_set(t->bits[EV_ABS], ABS_MT_SLOT);
    bit_set(t->bits[EV_ABS], ABS_MT_TRACKING_ID);
    bit_set(t->bits[EV_ABS], ABS_MT_POSITION_X);
    bit_set(t->bits[EV_ABS], ABS_MT_POSITION_Y);
    bit_set(t->bits[EV_ABS], ABS_MT_PRESSURE);
    bit_set(t->props, INPUT_PROP_DIRECT);
    t->abs[ABS_MT_SLOT] = (absinfo_t){0, 0, 9, 0, 0, 0};
    t->abs[ABS_MT_TRACKING_ID] = (absinfo_t){0, 0, 65535, 0, 0, 0};
    t->abs[ABS_MT_POSITION_X] = (absinfo_t){0, 0, XJ_DISPLAY_W - 1, 0, 0, 0};
    t->abs[ABS_MT_POSITION_Y] = (absinfo_t){0, 0, XJ_DISPLAY_H - 1, 0, 0, 0};
    t->abs[ABS_MT_PRESSURE] = (absinfo_t){0, 0, 255, 0, 0, 0};

    /* The keyboard: a phone's keys (letters, digits, punctuation, Enter, Backspace, Tab, Space, arrows, Home, End,
     * Page Up and Down, Delete, Caps Lock, the modifiers), Back and Homepage (the on-screen buttons), and EV_REP. */
    vdev *k = &devs[EVDEV_KEYBOARD];
    k->name = "xiaoji_keyboard";
    k->phys = "xiaoji/input1";
    k->vendor = 0x0001;
    k->product = 0x0002;
    k->version = 0x0001;
    static const uint16_t keys[] = {
        1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  /* Esc 1..0 - = Backspace Tab */
        16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,       /* Q..P [ ] Enter LCtrl */
        30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,       /* A..L ; ' ` LShift \ */
        44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  56,  57,  58,       /* Z..M , . / RShift LAlt Space CapsLock */
        97,  100, 102, 103, 104, 105, 106, 107, 108, 109, 111, 125, 139, 158, 172, /* RCtrl RAlt Home Up PgUp Left Right End Down PgDn Del LMeta Menu Back Homepage */
    };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        bit_set(k->bits[EV_KEY], keys[i]);
    bit_set(k->bits[EV_SYN], SYN_REPORT);
    bit_set(k->bits[EV_SYN], SYN_DROPPED);
    bit_set(k->bits[EV_REP], 0); /* REP_DELAY */
    bit_set(k->bits[EV_REP], 1); /* REP_PERIOD */
    k->rep[0] = 250; /* the kernel's defaults: 250 ms delay, 33 ms period */
    k->rep[1] = 33;
}

/* The maxbit of a type's bitmap (the kernel's *_MAX for it). */
static unsigned type_maxbit(unsigned ev)
{
    switch (ev) {
    case EV_SYN: return 7;
    case EV_KEY: return KEY_MAX;
    case EV_REL: return REL_MAX;
    case EV_ABS: return ABS_MAX;
    case EV_MSC: return MSC_MAX;
    case EV_SW: return SW_MAX;
    case EV_LED: return LED_MAX;
    case EV_SND: return SND_MAX;
    case EV_REP: return REP_MAX;
    case EV_FF: return FF_MAX;
    case EV_FF_STATUS: return 1;
    default: return 0;
    }
}

/* ---- names ---- */
int evdev_index(const char *guest)
{
    if (!strcmp(guest, "/dev/input/event0")) return EVDEV_TOUCH;
    if (!strcmp(guest, "/dev/input/event1")) return EVDEV_KEYBOARD;
    return -1;
}

int evdev_is_dir(const char *guest)
{
    return !strcmp(guest, EVDEV_DIR);
}

/* The configuration file EventHub reads for the touchscreen, by its canonical name (/vendor/usr/idc/<name>.idc). It is a
 * synthetic file: its text is made at open (vfs.c), as powersupply's are. */
int evdev_file(const char *guest)
{
    return !strcmp(guest, "/vendor/usr/idc/xiaoji_touchscreen.idc");
}

int evdev_file_read(const char *guest, uint8_t **data, size_t *len)
{
    static const char text[] =
        "# The virtual touchscreen (engine/evdev.c, B16): a direct touchscreen, oriented with the display.\n"
        "touch.deviceType = touchScreen\n"
        "touch.orientationAware = 1\n";
    (void)guest;
    size_t n = sizeof text - 1;
    uint8_t *d = malloc(n);
    if (!d)
        return -1;
    memcpy(d, text, n);
    *data = d;
    *len = n;
    return 0;
}

int evdev_child(const char *dir, int idx, const char **name, int *is_dir)
{
    static const char *names[EVDEV_COUNT] = {"event0", "event1"};
    if (!evdev_is_dir(dir) || idx < 0 || idx >= EVDEV_COUNT)
        return 0;
    *name = names[idx];
    *is_dir = 0;
    return 1;
}

/* ---- the source: the page's ring (browser) or the test script (native, Node) ---- */
#ifdef XIAOJI_WEB
/* web/input.js: copies the next event of the page's ring into out (dev, type, code, value) and returns 1, or returns 0
 * when the ring is empty. */
extern int xiaoji_input_pop(int32_t *out);
static int source_next(int32_t *out) { return xiaoji_input_pop(out); }
/* The browser has no test script: --input-script is refused (main.c reports the file as unreadable). */
int evdev_script_load(const char *path)
{
    (void)path;
    return -1;
}
#else
typedef struct {
    uint64_t at_ns; /* from the origin */
    int32_t f[4];   /* dev, type, code, value */
} script_ev;
static script_ev *script;
static size_t nscript, script_pos;
static int script_tried; /* the script was loaded (--input-script), or the environment was looked at */

int evdev_script_load(const char *path)
{
    script_tried = 1;
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;
    size_t cap = 0;
    char line[256];
    while (fgets(line, sizeof line, fp)) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        unsigned long long ms;
        int dev, type, code, value;
        if (sscanf(line, "%llu %d %d %d %d", &ms, &dev, &type, &code, &value) != 5)
            continue;
        if (nscript == cap) {
            size_t ncap = cap ? cap * 2 : 256;
            script_ev *n = realloc(script, ncap * sizeof *n);
            if (!n) {
                fclose(fp);
                return -1;
            }
            script = n;
            cap = ncap;
        }
        script[nscript].at_ns = ms * 1000000ull;
        script[nscript].f[0] = dev;
        script[nscript].f[1] = type;
        script[nscript].f[2] = code;
        script[nscript].f[3] = value;
        nscript++;
    }
    fclose(fp);
    return 0;
}

static void script_from_env(void)
{
    const char *p;
    if (script_tried)
        return;
    script_tried = 1;
    p = getenv("XIAOJI_INPUT_SCRIPT"); /* a run of tests/run.sh (the evdev case) */
    if (p && *p)
        (void)evdev_script_load(p);
}

static int source_next(int32_t *out)
{
    if (script_pos >= nscript || !origin_set)
        return 0;
    if (host_monotonic_ns() < origin_ns + script[script_pos].at_ns)
        return 0;
    memcpy(out, script[script_pos].f, sizeof script[script_pos].f);
    script_pos++;
    return 1;
}
#endif

/* ---- queues ---- */
static void stamp(uint8_t *ev, uint64_t ns, uint16_t type, uint16_t code, int32_t value)
{
    put_le(ev, ns / 1000000000ull, 8);
    put_le(ev + 8, (ns % 1000000000ull) / 1000, 8);
    put_le(ev + 16, type, 2);
    put_le(ev + 18, code, 2);
    put_le(ev + 20, (uint32_t)value, 4);
}

static int ring_put(evfd *e, uint16_t type, uint16_t code, int32_t value, uint64_t ns)
{
    if (e->count >= RING_CAP)
        return -1;
    unsigned slot = (e->head + e->count) % RING_CAP;
    stamp(e->ring[slot], ns, type, code, value);
    e->count++;
    if (type == EV_SYN && code == SYN_REPORT)
        e->complete++;
    return 0;
}

static void deliver(evfd *e, uint16_t type, uint16_t code, int32_t value, uint64_t mono, uint64_t real)
{
    uint64_t ns = e->clock == CLOCK_REALTIME_ID ? real : mono;
    if (e->overflow && RING_CAP - e->count >= 2) { /* the client resyncs on SYN_DROPPED (as the kernel's) */
        ring_put(e, EV_SYN, SYN_DROPPED, 0, ns);
        ring_put(e, EV_SYN, SYN_REPORT, 0, ns);
        e->overflow = 0;
    }
    if (ring_put(e, type, code, value, ns) < 0)
        e->overflow = 1;
}

void evdev_pump(void)
{
    devs_init();
    int32_t s[4];
    while (source_next(s)) {
        if (s[0] < 0 || s[0] >= EVDEV_COUNT || s[1] < 0 || s[1] > EV_MAX || s[2] < 0 || s[2] >= BIT_BYTES * 8)
            continue;
        vdev *d = &devs[s[0]];
        if (s[1] != EV_SYN && !bit_test(d->bits[s[1]], (unsigned)s[2]))
            continue; /* a code the device doesn't have is dropped, as the kernel's input core does */
        if (s[1] == EV_ABS) /* the axis's current value (EVIOCGABS reports it; EventHub reads it as getAbsoluteAxisValue) */
            d->abs[s[2]].value = s[3];
        if (s[1] == EV_KEY && s[3] == 1)
            bit_set(d->keystate, (unsigned)s[2]);
        else if (s[1] == EV_KEY && s[3] == 0)
            bit_clear(d->keystate, (unsigned)s[2]);
        /* EV_KEY value 2 is a repeat: the state is unchanged. */
        uint64_t mono = host_monotonic_ns(), real = host_realtime_ns();
        int nopen = 0;
        for (int i = 0; i < MAX_OPEN; i++) {
            evfd *e = opened[i];
            if (!e || e->dev != s[0])
                continue;
            if (d->grab && d->grab != e)
                continue;
            deliver(e, (uint16_t)s[1], (uint16_t)s[2], s[3], mono, real);
            nopen++;
        }
        /* --debug-faults: the first events from the source, and how many open fds took each (B17). */
        static int noted;
        if (sig_debug_faults() && noted < 64) {
            noted++;
            fprintf(stderr, "xiaoji: evdev: event%d type %d code %d value %d to %d open fd(s)\n", s[0], s[1], s[2], s[3], nopen);
        }
    }
}

/* ---- open, close, read, readiness ---- */
void *evdev_open(int idx)
{
    devs_init();
#ifndef XIAOJI_WEB
    script_from_env();
#endif
    if (idx < 0 || idx >= EVDEV_COUNT)
        return NULL;
    int slot = -1;
    for (int i = 0; i < MAX_OPEN; i++)
        if (!opened[i]) {
            slot = i;
            break;
        }
    if (slot < 0)
        return NULL;
    evfd *e = calloc(1, sizeof *e);
    if (!e)
        return NULL;
    e->dev = idx;
    e->clock = CLOCK_REALTIME_ID; /* the kernel's default; EventHub sets CLOCK_MONOTONIC with EVIOCSCLOCKID */
    opened[slot] = e;
    if (!origin_set) { /* the test script's times run from the first open */
        origin_set = 1;
        origin_ns = host_monotonic_ns();
    }
    return e;
}

void evdev_free(void *o)
{
    evfd *e = o;
    if (!e)
        return;
    for (int i = 0; i < EVDEV_COUNT; i++)
        if (devs[i].grab == e)
            devs[i].grab = NULL;
    for (int i = 0; i < MAX_OPEN; i++)
        if (opened[i] == e)
            opened[i] = NULL;
    free(e);
}

int evdev_readable(void *o)
{
    evdev_pump();
    return ((evfd *)o)->complete > 0;
}

uint64_t evdev_recheck_interval_ns(void)
{
    return 2000000ull; /* 2 ms: a reader that waits looks again this often */
}

int64_t evdev_read(void *o, void *buf, size_t n)
{
    evfd *e = o;
    if (n < EV_BYTES)
        return -LE_INVAL;
    evdev_pump();
    size_t out = 0;
    uint8_t *dst = buf;
    /* Whole events, up to the last complete packet: a partial packet stays queued. */
    while (e->complete > 0 && out + EV_BYTES <= n) {
        uint8_t *ev = e->ring[e->head];
        memcpy(dst + out, ev, EV_BYTES);
        out += EV_BYTES;
        if (ev[16] == EV_SYN && ev[18] == SYN_REPORT)
            e->complete--;
        e->head = (e->head + 1) % RING_CAP;
        e->count--;
    }
    if (out == 0)
        return -LE_AGAIN;
    return (int64_t)out;
}

/* ---- ioctls ---- */
static int64_t out_bytes(uint64_t arg, const void *src, size_t have, size_t maxlen)
{
    size_t m = have < maxlen ? have : maxlen;
    if (mem_copy_in(arg, src, m) < 0)
        return -LE_FAULT;
    return (int64_t)m;
}

/* A fixed-size struct (EVIOCGVERSION, EVIOCGID, EVIOCGREP, EVIOCGABS): copied, and the ioctl returns 0 (the kernel's
 * copy_to_user, which EventHub checks for zero). */
static int64_t out_fixed(uint64_t arg, const void *src, size_t n)
{
    return mem_copy_in(arg, src, n) < 0 ? -LE_FAULT : 0;
}

/* A string and its NUL, truncated to maxlen (the kernel's str_to_user). No string: ENOENT. */
static int64_t out_str(uint64_t arg, const char *s, size_t maxlen)
{
    if (!s || !s[0])
        return -LE_NOENT;
    return out_bytes(arg, s, strlen(s) + 1, maxlen);
}

/* A bitmap of maxbit bits: its longs, truncated to maxlen (the kernel's bits_to_user). */
static int64_t out_bits(uint64_t arg, const uint8_t *bits, unsigned maxbit, size_t maxlen)
{
    size_t bytes = ((maxbit + 63) / 64) * 8;
    return out_bytes(arg, bits, bytes, maxlen);
}

/* EVIOCGBIT(0): the types the device has (EV_REP for the keyboard), EV_MAX bits. */
static int64_t out_types(uint64_t arg, vdev *d, size_t size)
{
    uint8_t types[BIT_BYTES] = {0};
    for (unsigned t = 0; t < EV_TYPES; t++) {
        for (unsigned i = 0; i < BIT_BYTES; i++)
            if (d->bits[t][i]) {
                bit_set(types, t);
                break;
            }
    }
    if (d == &devs[EVDEV_KEYBOARD])
        bit_set(types, EV_REP);
    return out_bits(arg, types, EV_MAX, size);
}

int64_t evdev_ioctl(void *o, uint32_t cmd, uint64_t arg)
{
    evfd *e = o;
    devs_init();
    vdev *d = &devs[e->dev];
    uint32_t dir = (cmd >> 30) & 3u, type = (cmd >> 8) & 0xffu, nr = cmd & 0xffu, size = (cmd >> 16) & 0x3fffu;
    if (type != EV_IOC_TYPE)
        return -LE_NOTTY;
    if (dir == IOC_READ) {
        if (nr == 0x01) { /* EVIOCGVERSION: EV_VERSION */
            uint8_t b[4];
            put_le(b, 0x010001, 4);
            return out_fixed(arg, b, 4);
        }
        if (nr == 0x02) { /* EVIOCGID: struct input_id { bustype, vendor, product, version } */
            uint8_t b[8];
            put_le(b, BUS_VIRTUAL, 2);
            put_le(b + 2, d->vendor, 2);
            put_le(b + 4, d->product, 2);
            put_le(b + 6, d->version, 2);
            return out_fixed(arg, b, 8);
        }
        if (nr == 0x03) { /* EVIOCGREP */
            uint8_t b[8];
            put_le(b, d->rep[0], 4);
            put_le(b + 4, d->rep[1], 4);
            return out_fixed(arg, b, 8);
        }
        if (nr == 0x06) return out_str(arg, d->name, size); /* EVIOCGNAME */
        if (nr == 0x07) return out_str(arg, d->phys, size); /* EVIOCGPHYS */
        if (nr == 0x08) return out_str(arg, NULL, size);    /* EVIOCGUNIQ: none */
        if (nr == 0x09) return out_bits(arg, d->props, INPUT_PROP_MAX, size); /* EVIOCGPROP */
        if (nr == 0x18) return out_bits(arg, d->keystate, KEY_MAX, size);     /* EVIOCGKEY */
        if (nr == 0x19 || nr == 0x1a || nr == 0x1b) { /* EVIOCGLED, EVIOCGSND, EVIOCGSW: none set */
            uint8_t z[BIT_BYTES] = {0};
            unsigned max = nr == 0x19 ? LED_MAX : nr == 0x1a ? SND_MAX : SW_MAX;
            return out_bits(arg, z, max, size);
        }
        if (nr >= 0x20 && nr < 0x20 + EV_TYPES) { /* EVIOCGBIT(ev, len) */
            unsigned ev = nr - 0x20;
            if (ev == 0)
                return out_types(arg, d, size);
            return out_bits(arg, d->bits[ev], type_maxbit(ev), size);
        }
        if (nr >= 0x40 && nr <= 0x40 + ABS_MAX) { /* EVIOCGABS(abs): struct input_absinfo, six __s32 */
            unsigned a = nr - 0x40;
            if (!bit_test(d->bits[EV_ABS], a))
                return -LE_INVAL;
            uint8_t b[24];
            put_le(b, (uint32_t)d->abs[a].value, 4);
            put_le(b + 4, (uint32_t)d->abs[a].minimum, 4);
            put_le(b + 8, (uint32_t)d->abs[a].maximum, 4);
            put_le(b + 12, (uint32_t)d->abs[a].fuzz, 4);
            put_le(b + 16, (uint32_t)d->abs[a].flat, 4);
            put_le(b + 20, (uint32_t)d->abs[a].resolution, 4);
            return out_fixed(arg, b, 24);
        }
        return -LE_NOTTY;
    }
    if (dir == IOC_WRITE) {
        if (nr == 0x03) { /* EVIOCSREP: the repeat delay and period */
            uint8_t b[8];
            if (mem_copy_out(b, arg, 8) < 0)
                return -LE_FAULT;
            d->rep[0] = get_le32(b);
            d->rep[1] = get_le32(b + 4);
            return 0;
        }
        if (nr == 0x90) { /* EVIOCGRAB: an int, 1 takes the device, 0 gives it back */
            uint8_t b[4];
            if (mem_copy_out(b, arg, 4) < 0)
                return -LE_FAULT;
            if (get_le32(b)) {
                if (d->grab && d->grab != e)
                    return -LE_BUSY;
                d->grab = e;
                return 0;
            }
            if (d->grab != e)
                return -LE_INVAL;
            d->grab = NULL;
            return 0;
        }
        if (nr == 0xa0) { /* EVIOCSCLOCKID: an int, CLOCK_REALTIME, CLOCK_MONOTONIC or CLOCK_BOOTTIME */
            uint8_t b[4];
            if (mem_copy_out(b, arg, 4) < 0)
                return -LE_FAULT;
            uint32_t c = get_le32(b);
            if (c != CLOCK_REALTIME_ID && c != CLOCK_MONOTONIC_ID && c != CLOCK_BOOTTIME_ID)
                return -LE_INVAL;
            e->clock = c;
            return 0;
        }
    }
    /* Not implemented (ENOTTY): EVIOCGKEYCODE, EVIOCSKEYCODE, EVIOCGMTSLOTS, EVIOCSABS, EVIOCSFF, EVIOCREVOKE. */
    return -LE_NOTTY;
}
