/* The goldfish qemud "sensors" protocol and its data (sensors.h). */
#include <stdio.h>
#include <string.h>

#include "sensors.h"

/* sensor_list.cpp, kQemuSensorName: the name each handle has in "set:<name>:<0|1>". */
static const char *const sns_name[SNS_COUNT] = {
    "acceleration",
    "gyroscope",
    "magnetic-field",
    "orientation",
    "temperature",
    "proximity",
    "light",
    "pressure",
    "humidity",
    "magnetic-field-uncalibrated",
    "gyroscope-uncalibrated",
    "hinge-angle0",
    "hinge-angle1",
    "hinge-angle2",
    "heart-rate",
    "rgbc-light",
    "wrist-tilt",
};

/* The event prefix multihal_sensors_qemu.cpp parses for each advertised handle. It differs from the
 * set name for the magnetic field ("magnetic:x:y:z" is the event, "magnetic-field" the name). */
static const char *sns_event(int h)
{
    switch (h) {
    case SNS_ACCELEROMETER: return "acceleration";
    case SNS_GYROSCOPE: return "gyroscope";
    case SNS_MAGNETIC_FIELD: return "magnetic";
    case SNS_ORIENTATION: return "orientation";
    case SNS_PROXIMITY: return "proximity";
    case SNS_LIGHT: return "light";
    default: return NULL;
    }
}

/* The values. Vectors have 3 components; the scalars have 1. A value of n 0 is a sensor the engine
 * does not have, so sensors_set refuses it. The numbers are the static device described in sensors.h:
 * at rest, portrait, gravity 9.81 m/s^2 on +Y (acceleration), no rotation, no field change; the
 * magnetic field is a plausible mid-latitude value in microtesla (an engine choice, not measured);
 * proximity 1.0 is "far" (the HAL's maxRange for it); light 300 lux. */
static float sns_value[SNS_COUNT][3] = {
    [SNS_ACCELEROMETER] = {0.0f, 9.81f, 0.0f},
    [SNS_GYROSCOPE] = {0.0f, 0.0f, 0.0f},
    [SNS_MAGNETIC_FIELD] = {20.0f, 0.0f, -40.0f},
    [SNS_ORIENTATION] = {0.0f, 0.0f, 0.0f},
    [SNS_PROXIMITY] = {1.0f, 0.0f, 0.0f},
    [SNS_LIGHT] = {300.0f, 0.0f, 0.0f},
};

static const int sns_nvalues[SNS_COUNT] = {
    [SNS_ACCELEROMETER] = 3,
    [SNS_GYROSCOPE] = 3,
    [SNS_MAGNETIC_FIELD] = 3,
    [SNS_ORIENTATION] = 3,
    [SNS_PROXIMITY] = 1,
    [SNS_LIGHT] = 1,
};

void sensors_set(int handle, const float *values, int n)
{
    if (!values || handle < 0 || handle >= SNS_COUNT || n != sns_nvalues[handle])
        return;
    for (int i = 0; i < n; i++)
        sns_value[handle][i] = values[i];
}

void sns_pipe_init(sns_pipe_t *s)
{
    memset(s, 0, sizeof *s);
    s->delay_ms = SNS_DEFAULT_DELAY_MS;
}

/* Writes "%04x" and the payload (the qemud framing) at dst, returns the bytes written or 0 if cap is short. */
static size_t frame(char *dst, size_t cap, const char *msg, size_t n)
{
    if (n > 0xffff || cap < n + 4)
        return 0;
    int h = snprintf(dst, 5, "%04x", (unsigned)n);
    if (h != 4)
        return 0;
    memcpy(dst + 4, msg, n);
    return n + 4;
}

/* Decimal digits only, 1 to 9 of them. Returns 0 on anything else. */
static int parse_uint(const char *p, size_t n, uint32_t *out)
{
    if (n == 0 || n > 9)
        return 0;
    uint32_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9')
            return 0;
        v = v * 10 + (uint32_t)(p[i] - '0');
    }
    *out = v;
    return 1;
}

/* The handle named by a set name, or -1. Only advertised handles count. */
static int handle_of(const char *name, size_t n)
{
    for (int h = 0; h < SNS_COUNT; h++) {
        if (!((SNS_ADVERTISED >> h) & 1u))
            continue;
        if (strlen(sns_name[h]) == n && !memcmp(sns_name[h], name, n))
            return h;
    }
    return -1;
}

static void update_armed(sns_pipe_t *s, uint64_t now)
{
    if (s->enabled) {
        if (!s->armed) {
            s->armed = 1;
            s->next = now; /* the first batch goes out at once */
        }
    } else {
        s->armed = 0;
        s->next = 0;
    }
}

size_t sns_command(sns_pipe_t *s, const char *cmd, size_t n, uint64_t now, char *reply, size_t cap)
{
    if (n == 0 || n >= 128)
        return 0;
    char c[128];
    memcpy(c, cmd, n);
    c[n] = 0;

    if (!strcmp(c, "list-sensors")) {
        char txt[16];
        int tl = snprintf(txt, sizeof txt, "%u", (unsigned)SNS_ADVERTISED);
        if (tl <= 0)
            return 0;
        return frame(reply, cap, txt, (size_t)tl);
    }
    if (!strncmp(c, "time:", 5))
        return 0; /* the guest's clock: not needed, the engine keeps its own */
    if (!strncmp(c, "set-delay:", 10)) {
        uint32_t ms;
        if (parse_uint(c + 10, n - 10, &ms) && ms >= 1 && ms <= 60000) {
            s->delay_ms = ms;
        }
        return 0;
    }
    if (!strncmp(c, "set:", 4)) {
        const char *name = c + 4;
        const char *colon = strchr(name, ':');
        if (!colon)
            return 0;
        int h = handle_of(name, (size_t)(colon - name));
        const char *val = colon + 1;
        if (h < 0 || !(val[0] == '0' || val[0] == '1') || val[1] != 0)
            return 0;
        if (val[0] == '1')
            s->enabled |= 1u << h;
        else
            s->enabled &= ~(1u << h);
        update_armed(s, now);
        return 0;
    }
    return 0; /* unknown: ignored */
}

int sns_deadline(const sns_pipe_t *s, uint64_t *dl)
{
    if (!s->armed)
        return 0;
    *dl = s->next;
    return 1;
}

size_t sns_batch(sns_pipe_t *s, uint64_t now, char *buf, size_t cap)
{
    if (!s->armed || now < s->next)
        return 0;
    size_t used = 0;
    char txt[128];
    for (int h = 0; h < SNS_COUNT; h++) {
        if (!((s->enabled >> h) & 1u) || !((SNS_ADVERTISED >> h) & 1u))
            continue;
        const char *ev = sns_event(h);
        int tl;
        if (sns_nvalues[h] == 3)
            tl = snprintf(txt, sizeof txt, "%s:%f:%f:%f", ev, sns_value[h][0], sns_value[h][1], sns_value[h][2]);
        else
            tl = snprintf(txt, sizeof txt, "%s:%f", ev, sns_value[h][0]);
        if (tl <= 0 || (size_t)tl >= sizeof txt)
            return 0;
        size_t w = frame(buf + used, cap - used, txt, (size_t)tl);
        if (!w)
            return 0;
        used += w;
    }
    int tl = snprintf(txt, sizeof txt, "sync:%llu", (unsigned long long)(now / 1000));
    size_t w = tl > 0 ? frame(buf + used, cap - used, txt, (size_t)tl) : 0;
    if (!w)
        return 0;
    used += w;
    s->next = now + (uint64_t)s->delay_ms * 1000000u;
    return used;
}
