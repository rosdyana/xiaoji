/* The synthetic /sys/class/power_supply tree (powersupply.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "powersupply.h"

/* The paths and the values. The values follow the sysfs formats the kernel uses: voltages and currents
 * in micro units, charge in micro-ampere-hours, temp in tenths of a degree Celsius (250 is 25.0 C). */
static const struct {
    const char *path;
    psfs_kind kind;
    const char *text; /* PSFS_FILE contents */
} tree[] = {
    {"/sys/class", PSFS_DIR, NULL},
    {"/sys/class/power_supply", PSFS_DIR, NULL},
    {"/sys/class/power_supply/battery", PSFS_DIR, NULL},
    {"/sys/class/power_supply/battery/type", PSFS_FILE, "Battery\n"},
    {"/sys/class/power_supply/battery/status", PSFS_FILE, "Full\n"},
    {"/sys/class/power_supply/battery/capacity_level", PSFS_FILE, "Full\n"},
    {"/sys/class/power_supply/battery/health", PSFS_FILE, "Good\n"},
    {"/sys/class/power_supply/battery/present", PSFS_FILE, "1\n"},
    {"/sys/class/power_supply/battery/capacity", PSFS_FILE, "100\n"},
    {"/sys/class/power_supply/battery/technology", PSFS_FILE, "Li-ion\n"},
    {"/sys/class/power_supply/battery/voltage_now", PSFS_FILE, "4200000\n"},
    {"/sys/class/power_supply/battery/current_now", PSFS_FILE, "0\n"},
    {"/sys/class/power_supply/battery/charge_full", PSFS_FILE, "5000000\n"},
    {"/sys/class/power_supply/battery/charge_full_design", PSFS_FILE, "5000000\n"},
    {"/sys/class/power_supply/battery/cycle_count", PSFS_FILE, "0\n"},
    {"/sys/class/power_supply/battery/temp", PSFS_FILE, "250\n"},
    {"/sys/class/power_supply/ac", PSFS_DIR, NULL},
    {"/sys/class/power_supply/ac/type", PSFS_FILE, "Mains\n"},
    {"/sys/class/power_supply/ac/online", PSFS_FILE, "1\n"},
};

#define NTREE (sizeof tree / sizeof tree[0])

static int find_entry(const char *guest)
{
    for (size_t i = 0; i < NTREE; i++)
        if (!strcmp(tree[i].path, guest))
            return (int)i;
    return -1;
}

psfs_kind powersupply_kind(const char *guest)
{
    int i = find_entry(guest);
    return i < 0 ? PSFS_NONE : tree[i].kind;
}

int powersupply_child(const char *dir, int i, const char **name, int *is_dir)
{
    size_t dl = strlen(dir);
    int seen = 0;
    for (size_t k = 0; k < NTREE; k++) {
        const char *p = tree[k].path;
        if (strncmp(p, dir, dl) != 0 || p[dl] != '/')
            continue;
        const char *rest = p + dl + 1;
        if (strchr(rest, '/'))
            continue;
        if (seen++ == i) {
            *name = rest;
            *is_dir = tree[k].kind == PSFS_DIR;
            return 1;
        }
    }
    return 0;
}

/* XIAOJI_PS_TRACE=1: one line per file opened (stderr), a debug aid like XIAOJI_GF_TRACE. */
int powersupply_read(const char *guest, uint8_t **data, size_t *len)
{
    int i = find_entry(guest);
    if (i < 0 || tree[i].kind != PSFS_FILE)
        return -1;
    if (getenv("XIAOJI_PS_TRACE"))
        fprintf(stderr, "xiaoji: power_supply: open %s\n", guest);
    size_t n = strlen(tree[i].text);
    uint8_t *d = malloc(n ? n : 1);
    if (!d)
        return -1;
    memcpy(d, tree[i].text, n);
    *data = d;
    *len = n;
    return 0;
}
