#ifndef XIAOJI_SENSORS_H
#define XIAOJI_SENSORS_H

/* The sensors of the goldfish qemud "sensors" service (B7), and the data behind it.
 *
 * The protocol is the one the guest's sensors HAL speaks (device/generic/goldfish/sensors,
 * multihal_sensors.cpp and multihal_sensors_qemu.cpp; the framing is qemud.cpp): every message
 * is a 4-digit lowercase hex length and the payload. The guest sends "time:<ns>", "list-sensors",
 * "set:<name>:<0|1>" and "set-delay:<ms>". The engine answers "list-sensors" with the decimal
 * mask of the sensors it has (bit = handle), and then sends one batch of event lines per delay
 * while sensors are enabled, each batch ending with "sync:<us>".
 *
 * The data source is sensors_set(). The values are static for now (a device at rest, in portrait,
 * with gravity on +Y). A browser bridge (DeviceMotion, DeviceOrientation) will call sensors_set.
 *
 * Everything here is plain state. The caller passes the host monotonic time in ns, so the pipe
 * (goldfish.c) decides when a batch is due and waits on the same clock as timerfd. */

#include <stddef.h>
#include <stdint.h>

/* Handles: the index in the goldfish qemud name table (sensor_list.cpp, kQemuSensorName). */
#define SNS_COUNT 17
#define SNS_ACCELEROMETER 0
#define SNS_GYROSCOPE 1
#define SNS_MAGNETIC_FIELD 2
#define SNS_ORIENTATION 3
#define SNS_PROXIMITY 5
#define SNS_LIGHT 6

/* The handles the engine advertises (bit = handle): accelerometer, gyroscope, magnetic field,
 * orientation, proximity and light. */
#define SNS_ADVERTISED ((1u << SNS_ACCELEROMETER) | (1u << SNS_GYROSCOPE) | (1u << SNS_MAGNETIC_FIELD) | \
                        (1u << SNS_ORIENTATION) | (1u << SNS_PROXIMITY) | (1u << SNS_LIGHT))

/* The data source. n is 3 for a vector (accelerometer, gyroscope, magnetic field, orientation) and
 * 1 for a scalar (proximity, light). Another handle, an unadvertised one, or another n is ignored. */
void sensors_set(int handle, const float *values, int n);

/* The default batch period (SENSOR_DELAY_NORMAL, 200 ms), used until the guest sends set-delay. */
#define SNS_DEFAULT_DELAY_MS 200u
/* The largest batch the engine writes in one go (sns_batch's cap). */
#define SNS_BATCH_MAX 1024

typedef struct {
    uint32_t enabled;  /* bit per handle the guest has switched on */
    uint32_t delay_ms; /* the batch period */
    int armed;         /* 1 while a batch is scheduled (some sensor is enabled) */
    uint64_t next;     /* host monotonic ns of the next batch, when armed */
} sns_pipe_t;

void sns_pipe_init(sns_pipe_t *s);

/* Handles one command (the payload of a framed message, not NUL terminated). Writes the framed reply
 * to reply (cap bytes) and returns its length, or 0 when there is no reply. Unknown commands and
 * malformed arguments are ignored: the HAL aborts when a send fails, so none of them is an error. */
size_t sns_command(sns_pipe_t *s, const char *cmd, size_t n, uint64_t now, char *reply, size_t cap);

/* 1 when a batch is scheduled, with its time in *dl (host monotonic ns); 0 when no sensor is enabled. */
int sns_deadline(const sns_pipe_t *s, uint64_t *dl);

/* When a batch is due (armed and now >= next), writes it to buf as framed messages (one per enabled
 * sensor, then "sync:<us>") and returns its length. Then next = now + delay: a batch that is overdue
 * (nobody read) is sent once, not caught up. Returns 0 when nothing is due. */
size_t sns_batch(sns_pipe_t *s, uint64_t now, char *buf, size_t cap);

#endif
