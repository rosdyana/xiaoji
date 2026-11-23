#ifndef XIAOJI_POWERSUPPLY_H
#define XIAOJI_POWERSUPPLY_H

/* The synthetic /sys/class/power_supply tree (B7): one battery and one mains supply, the entries
 * and values healthd's BatteryMonitor reads (system/core/healthd/BatteryMonitor.cpp). The battery
 * is the full, charged device on mains power. Static text, read only, like the selinuxfs files. */

#include <stddef.h>
#include <stdint.h>

typedef enum {
    PSFS_NONE,
    PSFS_DIR,
    PSFS_FILE,
} psfs_kind;

psfs_kind powersupply_kind(const char *guest);

/* The i-th child of a synthetic directory (in tree order). Returns 1 and sets name and is_dir, or 0. */
int powersupply_child(const char *dir, int i, const char **name, int *is_dir);

/* The contents of a PSFS_FILE (a malloc'd copy). Returns 0, or -1 for a path that is not a file. */
int powersupply_read(const char *guest, uint8_t **data, size_t *len);

#endif
