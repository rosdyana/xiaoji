/* The synthetic selinuxfs at /sys/fs/selinux. See selinuxfs.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "selinuxfs.h"

#define STATUS_PAGE 4096
#define STATUS_BYTES 20 /* struct selinux_status_t: five u32 */
#define REQ_MAX 4096

/* The tree. Paths are absolute guest paths. A directory's children are the entries whose
 * path is the directory's path plus one component. */
static const struct {
    const char *path;
    selfs_kind kind;
    const char *text; /* SELFS_FILE contents */
} tree[] = {
    {"/sys/fs/selinux", SELFS_DIR, NULL},
    {"/sys/fs/selinux/status", SELFS_STATUS, NULL},
    {"/sys/fs/selinux/enforce", SELFS_FILE, "0\n"},
    {"/sys/fs/selinux/deny_unknown", SELFS_FILE, "1\n"},
    {"/sys/fs/selinux/reject_unknown", SELFS_FILE, "0\n"},
    {"/sys/fs/selinux/policyvers", SELFS_FILE, "30\n"},
    {"/sys/fs/selinux/checkreqprot", SELFS_FILE, "0\n"},
    {"/sys/fs/selinux/mls", SELFS_FILE, "0\n"},
    {"/sys/fs/selinux/access", SELFS_ACCESS, NULL},
    {"/sys/fs/selinux/context", SELFS_CONTEXT, NULL},
    {"/sys/fs/selinux/class", SELFS_DIR, NULL},
    {"/sys/fs/selinux/class/service_manager", SELFS_DIR, NULL},
    {"/sys/fs/selinux/class/service_manager/index", SELFS_FILE, "60\n"},
    /* Permission values are 1-based: libselinux maps value v to bit v-1. */
    {"/sys/fs/selinux/class/service_manager/perms", SELFS_DIR, NULL},
    {"/sys/fs/selinux/class/service_manager/perms/add", SELFS_FILE, "1\n"},
    {"/sys/fs/selinux/class/service_manager/perms/find", SELFS_FILE, "2\n"},
    {"/sys/fs/selinux/class/service_manager/perms/list", SELFS_FILE, "3\n"},
    /* hwservice_manager (the HIDL service manager's class, system/sepolicy private/access_vectors):
     * without it libselinux reports "Unknown class", and hwservicemanager denies its own add (so the
     * HIDL manager is not registered, and Watchdog's lookup of it fails). The index is the next free
     * one after service_manager's; the access decision is still allow-all. */
    {"/sys/fs/selinux/class/hwservice_manager", SELFS_DIR, NULL},
    {"/sys/fs/selinux/class/hwservice_manager/index", SELFS_FILE, "61\n"},
    {"/sys/fs/selinux/class/hwservice_manager/perms", SELFS_DIR, NULL},
    {"/sys/fs/selinux/class/hwservice_manager/perms/add", SELFS_FILE, "1\n"},
    {"/sys/fs/selinux/class/hwservice_manager/perms/find", SELFS_FILE, "2\n"},
    {"/sys/fs/selinux/class/hwservice_manager/perms/list", SELFS_FILE, "3\n"},
    /* keystore2 (B16): keystore2 checks its permissions against this class (the AOSP policy's class keystore2, index
     * 62 here). libselinux reads each perms file as the permission's number plus one (1..32: discover_class rejects 0
     * and stores value-1), not a mask, so the first permission is 1. The access decisions are all allowed
     * (selinuxfs_access). */
    {"/sys/fs/selinux/class/keystore2", SELFS_DIR, NULL},
    {"/sys/fs/selinux/class/keystore2/index", SELFS_FILE, "62\n"},
    {"/sys/fs/selinux/class/keystore2/perms", SELFS_DIR, NULL},
    {"/sys/fs/selinux/class/keystore2/perms/add_auth", SELFS_FILE, "1\n"},
    {"/sys/fs/selinux/class/keystore2/perms/change_password", SELFS_FILE, "2\n"},
    {"/sys/fs/selinux/class/keystore2/perms/change_user", SELFS_FILE, "3\n"},
    {"/sys/fs/selinux/class/keystore2/perms/clear_ns", SELFS_FILE, "4\n"},
    {"/sys/fs/selinux/class/keystore2/perms/clear_uid", SELFS_FILE, "5\n"},
    {"/sys/fs/selinux/class/keystore2/perms/delete_all_keys", SELFS_FILE, "6\n"},
    {"/sys/fs/selinux/class/keystore2/perms/early_boot_ended", SELFS_FILE, "7\n"},
    {"/sys/fs/selinux/class/keystore2/perms/get_attestation_key", SELFS_FILE, "8\n"},
    {"/sys/fs/selinux/class/keystore2/perms/get_auth_token", SELFS_FILE, "9\n"},
    {"/sys/fs/selinux/class/keystore2/perms/get_state", SELFS_FILE, "10\n"},
    {"/sys/fs/selinux/class/keystore2/perms/list", SELFS_FILE, "11\n"},
    {"/sys/fs/selinux/class/keystore2/perms/lock", SELFS_FILE, "12\n"},
    {"/sys/fs/selinux/class/keystore2/perms/pull_metrics", SELFS_FILE, "13\n"},
    {"/sys/fs/selinux/class/keystore2/perms/report_off_body", SELFS_FILE, "14\n"},
    {"/sys/fs/selinux/class/keystore2/perms/reset", SELFS_FILE, "15\n"},
    {"/sys/fs/selinux/class/keystore2/perms/unlock", SELFS_FILE, "16\n"},
};

#define NTREE (sizeof tree / sizeof tree[0])

static int find_entry(const char *path)
{
    for (size_t i = 0; i < NTREE; i++)
        if (!strcmp(tree[i].path, path))
            return (int)i;
    return -1;
}

selfs_kind selinuxfs_kind(const char *guest)
{
    int i = find_entry(guest);
    return i < 0 ? SELFS_NONE : tree[i].kind;
}

int selinuxfs_child(const char *dir, int i, const char **name, int *is_dir)
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
            *is_dir = tree[k].kind == SELFS_DIR;
            return 1;
        }
    }
    return 0;
}

int selinuxfs_read(const char *guest, uint8_t **data, size_t *len)
{
    int i = find_entry(guest);
    if (i < 0 || tree[i].kind != SELFS_FILE)
        return -1;
    size_t n = strlen(tree[i].text);
    uint8_t *d = malloc(n ? n : 1);
    if (!d)
        return -1;
    memcpy(d, tree[i].text, n);
    *data = d;
    *len = n;
    return 0;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

memfd_t *selinuxfs_status_memfd(void)
{
    static memfd_t *status;
    if (status)
        return status;
    memfd_t *m = memfd_new("selinux-status");
    if (!m)
        return NULL;
    if (memfd_truncate(m, STATUS_PAGE) < 0) {
        memfd_unref(m);
        return NULL;
    }
    /* struct selinux_status_t: version 1, sequence 2 (even: stable), enforcing 0,
     * policyload 0, deny_unknown 1. The rest of the page is zero. */
    uint8_t b[STATUS_BYTES] = {0};
    put_le32(b + 0, 1);
    put_le32(b + 4, 2);
    put_le32(b + 8, 0);
    put_le32(b + 12, 0);
    put_le32(b + 16, 1);
    if (memfd_pwrite(m, b, sizeof b, 0) != (int64_t)sizeof b) {
        memfd_unref(m);
        return NULL;
    }
    status = m;
    return status;
}

int selinuxfs_check_context(const char *ctx, size_t n)
{
    if (n < 2 || n > 4096 || ctx[n - 1] != 0)
        return -1;
    size_t len = n - 1, fields = 1, field_len = 0;
    for (size_t i = 0; i < len; i++) {
        if (ctx[i] == ':') {
            if (field_len == 0)
                return -1;
            fields++;
            field_len = 0;
        } else {
            field_len++;
        }
    }
    if (field_len == 0)
        return -1;
    return fields >= 4 && fields <= 5 ? 0 : -1;
}

int selinuxfs_access(const char *req, size_t n, uint8_t **reply, size_t *len)
{
    if (n == 0 || n > REQ_MAX)
        return -1;
    char buf[REQ_MAX + 1];
    memcpy(buf, req, n);
    buf[n] = 0;
    unsigned int wanted;
    if (sscanf(buf, "%*s %*s %*u %x", &wanted) != 1)
        return -1;
    /* allowed, decided, auditallow, auditdeny, seqno, flags. */
    char out[128];
    int k = snprintf(out, sizeof out, "%x %x %x %x %u %x\n", wanted, 0xffffffffu, 0u, 0u, 0u, 0u);
    if (k < 0 || (size_t)k >= sizeof out)
        return -1;
    uint8_t *d = malloc((size_t)k);
    if (!d)
        return -1;
    memcpy(d, out, (size_t)k);
    *reply = d;
    *len = (size_t)k;
    return 0;
}
