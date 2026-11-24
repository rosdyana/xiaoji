#ifndef XIAOJI_EVDEV_H
#define XIAOJI_EVDEV_H

/* Virtual evdev input devices (docs/input.md, I1).
 *
 *   /dev/input/event0  a multi-touch touchscreen (protocol B, INPUT_PROP_DIRECT, the display's size)
 *   /dev/input/event1  a keyboard (the keys a phone's keyboard has, EV_REP, no LEDs)
 *
 * The ioctls answer as the kernel's evdev does (uapi linux/input.h); the ones EventHub and getevent use are implemented,
 * the rest answer ENOTTY (docs/input.md). A read returns struct input_event (24 bytes on arm64) in whole events, ending
 * at a SYN_REPORT. Events come from a source (evdev_source): a --input-script file (native and Node), or the page's
 * ring buffer in the browser (XIAOJI_WEB, web/input.js). evdev_pump moves them into the open devices.
 */

#include <stddef.h>
#include <stdint.h>

#define EVDEV_COUNT 2
#define EVDEV_TOUCH 0    /* /dev/input/event0 */
#define EVDEV_KEYBOARD 1 /* /dev/input/event1 */
#define EVDEV_DIR "/dev/input"

/* The guest path of device idx's node: its index, or -1. The directory /dev/input: 1 or 0. */
int evdev_index(const char *guest);
int evdev_is_dir(const char *guest);
/* The configuration file of the touchscreen (EventHub looks it up by name): evdev_file names it, evdev_file_read gives its
 * text (malloc'd; the caller owns it). */
int evdev_file(const char *guest);
int evdev_file_read(const char *guest, uint8_t **data, size_t *len);

/* The idx-th child of a directory for getdents (the chain in vfs.c syn_getdents): 1 with the name, or 0 when there is none
 * (the nodes are not directories). */
int evdev_child(const char *dir, int idx, const char **name, int *is_dir);

/* An open of device idx (never fails except for memory). NULL: out of memory. */
void *evdev_open(int idx);
void evdev_free(void *o);
/* read: whole events only. -LE_INVAL for n < 24, -LE_AGAIN when no complete packet is queued, else the bytes read. */
int64_t evdev_read(void *o, void *buf, size_t n);
/* Readiness: 1 when a complete packet (SYN_REPORT) is queued. */
int evdev_readable(void *o);
/* A blocked reader's wait: the interval, in ns, after which it looks again (the source may have events at any time). */
uint64_t evdev_recheck_interval_ns(void);
/* The ioctls on the fd (sysfs.c routes here). Returns the ioctl result or -LE_*. */
int64_t evdev_ioctl(void *o, uint32_t cmd, uint64_t arg);
/* Moves the source's due events into the open devices. Called by every read, readiness check and wait. */
void evdev_pump(void);
/* The native / Node test source: a --input-script file, one event per line: "<ms> <dev> <type> <code> <value>". ms is
 * from the first open of a device. Returns 0 on success, -1 if the file can't be read. */
int evdev_script_load(const char *path);

#endif
