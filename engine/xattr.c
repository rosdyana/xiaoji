/* In-memory extended attributes (xattr.h). A hash table of (file, name, value) entries. */
#include <stdlib.h>
#include <string.h>

#include "linux.h"
#include "xattr.h"

#define XA_BUCKETS 1024

/* A file that has no stored SELinux label reads back this one, as a kernel with SELinux labels every new file
 * (the default type of the unlabelled objects). The value includes its NUL, as the kernel's does. restorecon
 * (android_platform.c, restorecon_sb) treats a failed label read as an error, so an unlabelled file would fail it. */
#define XA_SELINUX_NAME "security.selinux"
static const char xa_default_label[] = "u:object_r:unlabeled:s0";

typedef struct xa {
    char *file;
    char *name;
    uint8_t *val;
    size_t len;
    struct xa *next;
} xa_t;

static xa_t *bucket[XA_BUCKETS];

/* A copy of a string (strdup is POSIX, and the engine builds as strict C11). */
static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static uint32_t hash2(const char *a, const char *b)
{
    uint32_t h = 2166136261u;
    for (const char *p = a; *p; p++)
        h = (h ^ (uint8_t)*p) * 16777619u;
    h = (h ^ 0x2f) * 16777619u;
    for (const char *p = b; *p; p++)
        h = (h ^ (uint8_t)*p) * 16777619u;
    return h % XA_BUCKETS;
}

static xa_t *find(const char *file, const char *name)
{
    for (xa_t *e = bucket[hash2(file, name)]; e; e = e->next)
        if (!strcmp(e->file, file) && !strcmp(e->name, name))
            return e;
    return NULL;
}

/* The namespace check: a name is "user.", "trusted.", "security." or "system." followed by something. */
static int name_ok(const char *name)
{
    static const char *const ns[] = {"user.", "trusted.", "security.", "system."};
    for (size_t i = 0; i < sizeof ns / sizeof ns[0]; i++) {
        size_t n = strlen(ns[i]);
        if (!strncmp(name, ns[i], n) && name[n] != 0)
            return 1;
    }
    return 0;
}

int64_t xattr_set(const char *guest, const char *name, const uint8_t *val, size_t len, int flags)
{
    if (!name_ok(name))
        return -LE_NOTSUP;
    if (len > XATTR_VALUE_MAX)
        return -LE_2BIG;
    xa_t *e = find(guest, name);
    if (flags & XATTR_CREATE && e)
        return -LE_EXIST;
    if (flags & XATTR_REPLACE && !e)
        return -LE_NODATA;
    uint8_t *nv = malloc(len ? len : 1);
    if (!nv)
        return -LE_NOMEM;
    if (len)
        memcpy(nv, val, len);
    if (e) {
        free(e->val);
        e->val = nv;
        e->len = len;
        return 0;
    }
    e = calloc(1, sizeof *e);
    if (!e) {
        free(nv);
        return -LE_NOMEM;
    }
    e->file = dup_str(guest);
    e->name = dup_str(name);
    if (!e->file || !e->name) {
        free(e->file);
        free(e->name);
        free(e);
        free(nv);
        return -LE_NOMEM;
    }
    e->val = nv;
    e->len = len;
    uint32_t b = hash2(guest, name);
    e->next = bucket[b];
    bucket[b] = e;
    return 0;
}

int64_t xattr_get(const char *guest, const char *name, uint8_t *buf, size_t cap)
{
    if (!name_ok(name))
        return -LE_NOTSUP;
    xa_t *e = find(guest, name);
    if (!e) {
        if (strcmp(name, XA_SELINUX_NAME) != 0)
            return -LE_NODATA;
        size_t n = sizeof xa_default_label; /* with its NUL */
        if (cap == 0)
            return (int64_t)n;
        if (cap < n)
            return -LE_RANGE;
        memcpy(buf, xa_default_label, n);
        return (int64_t)n;
    }
    if (cap == 0)
        return (int64_t)e->len;
    if (cap < e->len)
        return -LE_RANGE;
    memcpy(buf, e->val, e->len);
    return (int64_t)e->len;
}

int64_t xattr_list(const char *guest, uint8_t *buf, size_t cap)
{
    size_t total = 0;
    for (size_t b = 0; b < XA_BUCKETS; b++)
        for (xa_t *e = bucket[b]; e; e = e->next) {
            if (strcmp(e->file, guest))
                continue;
            size_t n = strlen(e->name) + 1;
            if (cap && total + n <= cap)
                memcpy(buf + total, e->name, n);
            total += n;
        }
    /* The default label is listed too, unless the file has its own (the list is the names a get would answer). */
    if (!find(guest, XA_SELINUX_NAME)) {
        size_t n = sizeof XA_SELINUX_NAME; /* with its NUL */
        if (cap && total + n <= cap)
            memcpy(buf + total, XA_SELINUX_NAME, n);
        total += n;
    }
    if (cap && total > cap)
        return -LE_RANGE;
    return (int64_t)total;
}

int64_t xattr_remove(const char *guest, const char *name)
{
    if (!name_ok(name))
        return -LE_NOTSUP;
    xa_t **pp = &bucket[hash2(guest, name)];
    for (; *pp; pp = &(*pp)->next) {
        xa_t *e = *pp;
        if (!strcmp(e->file, guest) && !strcmp(e->name, name)) {
            *pp = e->next;
            free(e->file);
            free(e->name);
            free(e->val);
            free(e);
            return 0;
        }
    }
    return -LE_NODATA;
}
