/* Synthetic cgroups (B11). See cgroupfs.h and docs/boot.md B11.
 *
 * The mounts come from the guest's cgroups.json, as Android's init (system/core/libprocessgroup/setup/cgroup_map_write.cpp)
 * reads it: each "Cgroups" entry is a v1 controller mounted at its Path, and "Cgroups2" is the v2 root with its own
 * controllers. Each mount is a tree of directories (mkdir and rmdir make them). Files in a directory are the ones the
 * table below lists for the mount's kind, so a missing file is ENOENT as on Linux.
 *
 * Membership: each process records, per mount, the directory it is in (proc_t.cg; 0 is the mount root). It is copied at
 * fork and cleared at exit. cgroup.procs and tasks list the live member processes, and writing a pid moves it. Tasks list
 * processes, not threads (a documented simplification). Other writable files store their value and read it back. Nothing
 * is scheduled, limited or frozen. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cgroupfs.h"
#include "linux.h"
#include "memfd.h"
#include "proc.h"
#include "vfs.h"

/* cgrouprc.h's controller flags (system/core/libprocessgroup/cgrouprc/include/android/cgrouprc.h). */
#define CGROUPRC_FLAG_MOUNTED 0x1
#define CGROUPRC_FLAG_OPTIONAL 0x4

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Kinds of mount: the bits a file's table entry lists. */
enum { K_CPU = 1, K_CPUSET = 2, K_BLKIO = 4, K_MEMORY = 8, K_V2 = 16, K_V1OTHER = 32 };
#define K_V1 (K_CPU | K_CPUSET | K_BLKIO | K_MEMORY | K_V1OTHER)
#define K_ALL (K_V1 | K_V2)

enum ftype { FT_PROCS, FT_EVENTS, FT_STORE, FT_RO };

typedef struct {
    const char *name;
    unsigned kinds;
    int type;
    const char *def; /* the value a read gives before any write (FT_STORE, FT_RO) */
} cg_file;

/* The files a directory has. Only the ones the platform code reads or writes (libprocessgroup, ActivityManager, lmkd,
 * SurfaceFlinger, see docs/boot.md B11), with defaults that describe an idle system. */
static const cg_file files[] = {
    {"cgroup.procs", K_ALL, FT_PROCS, ""},
    {"tasks", K_V1, FT_PROCS, ""},
    {"cgroup.threads", K_V2, FT_PROCS, ""},
    {"cgroup.events", K_V2, FT_EVENTS, ""},
    {"cgroup.freeze", K_V2, FT_STORE, "0\n"},
    {"cgroup.controllers", K_V2, FT_RO, "cpu memory freezer\n"},
    {"cgroup.subtree_control", K_V2, FT_STORE, ""},
    {"cgroup.event_control", K_MEMORY, FT_STORE, ""},
    {"cpu.uclamp.min", K_CPU | K_V2, FT_STORE, "0\n"},
    {"cpu.uclamp.max", K_CPU | K_V2, FT_STORE, "1024\n"},
    {"cpu.uclamp.latency_sensitive", K_CPU | K_V2, FT_STORE, "0\n"},
    {"cpu.shares", K_CPU, FT_STORE, "1024\n"},
    {"cpus", K_CPUSET, FT_STORE, "0\n"},
    {"mems", K_CPUSET, FT_STORE, "0\n"},
    {"memory.stat", K_MEMORY, FT_RO, "cache 0\nrss 0\n"},
    {"memory.usage_in_bytes", K_MEMORY, FT_RO, "0\n"},
    {"memory.memsw.usage_in_bytes", K_MEMORY, FT_RO, "0\n"},
    {"memory.limit_in_bytes", K_MEMORY, FT_STORE, "9223372036854771712\n"},
    {"memory.soft_limit_in_bytes", K_MEMORY, FT_STORE, "9223372036854771712\n"},
    {"memory.swappiness", K_MEMORY, FT_STORE, "60\n"},
    {"memory.pressure_level", K_MEMORY, FT_STORE, ""},
};
#define NFILES ((int)(sizeof files / sizeof files[0]))

typedef struct cg_attr {
    int file;
    char *val;
    size_t len;
    struct cg_attr *next;
} cg_attr;

typedef struct {
    int used, mount, parent;
    char name[40];
    cg_attr *attrs;
} cg_node;

typedef struct {
    char path[64];
    unsigned kinds;
    int v2;
} cg_mount;

/* One controller of cgroup.rc (the records libprocessgroup reads). */
typedef struct {
    char name[16];
    char path[32];
    uint32_t version, flags;
    int mount;
} cg_ctl;

#define MAX_CTLS 16
#define CG_MAX_NAME 40

static int enabled;
static int nmounts, nctls;
static cg_mount mounts[CG_MAX_MOUNTS];
static cg_ctl ctls[MAX_CTLS];
static cg_node nodes[CG_MAX_NODES];
static memfd_t *rc_mfd;

/* ---------- a minimal JSON reader (cgroups.json only: objects, arrays, strings, booleans) ---------- */

typedef struct jv {
    char type; /* 'o' object, 'a' array, 's' string, 'b' boolean, 'n' other */
    char *str;
    int b;
    char *key;
    struct jv *child, *next;
} jv;

static const char *jp, *jend;

static void jws(void)
{
    while (jp < jend && (*jp == ' ' || *jp == '\t' || *jp == '\n' || *jp == '\r'))
        jp++;
}

static char *jstring(void)
{
    size_t cap = 32, n = 0;
    char *s = malloc(cap);
    if (!s)
        return NULL;
    jp++; /* the opening quote */
    while (jp < jend && *jp != '"') {
        char c = *jp++;
        if (c == '\\' && jp < jend) {
            c = *jp++;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
        }
        if (n + 2 > cap) {
            char *t = realloc(s, cap *= 2);
            if (!t) {
                free(s);
                return NULL;
            }
            s = t;
        }
        s[n++] = c;
    }
    if (jp >= jend)
        return NULL;
    jp++; /* the closing quote */
    s[n] = 0;
    return s;
}

static jv *jparse(int depth)
{
    jws();
    if (jp >= jend || depth > 16)
        return NULL;
    jv *v = calloc(1, sizeof *v);
    if (!v)
        return NULL;
    char c = *jp;
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        v->type = c == '{' ? 'o' : 'a';
        jp++;
        jws();
        if (jp < jend && *jp == close) {
            jp++;
            return v;
        }
        jv **tail = &v->child;
        for (;;) {
            char *key = NULL;
            jws();
            if (c == '{') {
                if (jp >= jend || *jp != '"' || !(key = jstring()))
                    return NULL;
                jws();
                if (jp >= jend || *jp != ':')
                    return NULL;
                jp++;
            }
            jv *item = jparse(depth + 1);
            if (!item)
                return NULL;
            item->key = key;
            *tail = item;
            tail = &item->next;
            jws();
            if (jp < jend && *jp == ',') {
                jp++;
                continue;
            }
            if (jp < jend && *jp == close) {
                jp++;
                return v;
            }
            return NULL;
        }
    }
    if (c == '"') {
        v->type = 's';
        v->str = jstring();
        return v->str ? v : NULL;
    }
    if (jend - jp >= 4 && !strncmp(jp, "true", 4)) {
        v->type = 'b', v->b = 1, jp += 4;
        return v;
    }
    if (jend - jp >= 5 && !strncmp(jp, "false", 5)) {
        v->type = 'b', v->b = 0, jp += 5;
        return v;
    }
    if (jend - jp >= 4 && !strncmp(jp, "null", 4)) {
        v->type = 'n', jp += 4;
        return v;
    }
    /* A number (not used by cgroups.json): skipped. */
    const char *s = jp;
    while (jp < jend && ((*jp >= '0' && *jp <= '9') || *jp == '-' || *jp == '+' || *jp == '.' || *jp == 'e' || *jp == 'E'))
        jp++;
    if (jp == s)
        return NULL;
    v->type = 'n';
    return v;
}

static jv *jget(const jv *obj, const char *key)
{
    if (!obj || obj->type != 'o')
        return NULL;
    for (jv *c = obj->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key))
            return c;
    return NULL;
}

static const char *jgets(const jv *obj, const char *key)
{
    jv *v = jget(obj, key);
    return v && v->type == 's' ? v->str : "";
}

static int jgetb(const jv *obj, const char *key)
{
    jv *v = jget(obj, key);
    return v && v->type == 'b' && v->b;
}

/* ---------- mounts and controllers ---------- */

static int mount_add(const char *path, unsigned kinds, int v2)
{
    for (int m = 0; m < nmounts; m++)
        if (!strcmp(mounts[m].path, path)) {
            mounts[m].kinds |= kinds;
            return m;
        }
    if (nmounts >= CG_MAX_MOUNTS || strlen(path) >= sizeof mounts[0].path)
        return -1;
    int m = nmounts++;
    snprintf(mounts[m].path, sizeof mounts[m].path, "%s", path);
    mounts[m].kinds = kinds;
    mounts[m].v2 = v2;
    /* The mount root is node m. */
    memset(&nodes[m], 0, sizeof nodes[m]);
    nodes[m].used = 1;
    nodes[m].mount = m;
    nodes[m].parent = -1;
    return m;
}

static unsigned v1_kind(const char *name)
{
    if (!strcmp(name, "cpu")) return K_CPU;
    if (!strcmp(name, "cpuset")) return K_CPUSET;
    if (!strcmp(name, "blkio")) return K_BLKIO;
    if (!strcmp(name, "memory")) return K_MEMORY;
    return K_V1OTHER;
}

/* Adds (or replaces, as libprocessgroup's map does) one controller record. */
static void ctl_add(const char *name, const char *path, uint32_t version, uint32_t flags, int mount)
{
    cg_ctl *c = NULL;
    for (int i = 0; i < nctls; i++)
        if (!strcmp(ctls[i].name, name))
            c = &ctls[i];
    if (!c) {
        if (nctls >= MAX_CTLS)
            return;
        c = &ctls[nctls++];
    }
    snprintf(c->name, sizeof c->name, "%s", name);
    snprintf(c->path, sizeof c->path, "%s", path);
    c->version = version;
    c->flags = flags;
    c->mount = mount;
}

/* Reads one cgroups.json document (a missing file is not an error). */
static void load_desc(const char *guest)
{
    uint8_t *data = NULL;
    size_t len = 0;
    char canon[VFS_PATH_MAX];
    if (vfs_load_file(guest, &data, &len, canon, sizeof canon) != 0)
        return;
    jp = (const char *)data;
    jend = jp + len;
    jv *root = jparse(0);
    if (!root || root->type != 'o') {
        fprintf(stderr, "xiaoji: cgroups: %s is not a JSON object\n", guest);
        free(data);
        return;
    }
    jv *v1 = jget(root, "Cgroups");
    for (jv *e = v1 ? v1->child : NULL; e; e = e->next) {
        const char *name = jgets(e, "Controller"), *path = jgets(e, "Path");
        if (!*name || !*path)
            continue;
        unsigned kind = v1_kind(name);
        int m = mount_add(path, kind, 0);
        if (m < 0)
            continue;
        uint32_t flags = CGROUPRC_FLAG_MOUNTED | (jgetb(e, "Optional") ? CGROUPRC_FLAG_OPTIONAL : 0);
        ctl_add(name, path, 1, flags, m);
    }
    jv *v2 = jget(root, "Cgroups2");
    if (v2) {
        const char *rootp = jgets(v2, "Path");
        if (*rootp) {
            int m = mount_add(rootp, K_V2, 1);
            if (m >= 0) {
                ctl_add("cgroup2", rootp, 2, CGROUPRC_FLAG_MOUNTED, m);
                jv *cs = jget(v2, "Controllers");
                for (jv *e = cs ? cs->child : NULL; e; e = e->next) {
                    const char *name = jgets(e, "Controller"), *sub = jgets(e, "Path");
                    if (!*name)
                        continue;
                    /* A controller at a sub-path of the root would need its own directory; only "." is used here. */
                    if (strcmp(sub, ".") && *sub)
                        fprintf(stderr, "xiaoji: cgroups: %s at %s/%s is not a root controller (not mounted)\n", name, rootp, sub);
                    else
                        ctl_add(name, rootp, 2, CGROUPRC_FLAG_MOUNTED, m);
                }
            }
        }
    }
    free(data);
}

/* The cgroup.rc image (cgroup_map_write.cpp's WriteRcFile): a CgroupFile header (version 1, count) and one
 * CgroupController record (version, flags, name[16], path[32]) per controller, in name order. */
static void build_rc(void)
{
    /* Sort the records by name (std::map's order). */
    int order[MAX_CTLS];
    for (int i = 0; i < nctls; i++)
        order[i] = i;
    for (int i = 1; i < nctls; i++)
        for (int j = i; j > 0 && strcmp(ctls[order[j - 1]].name, ctls[order[j]].name) > 0; j--) {
            int t = order[j - 1];
            order[j - 1] = order[j];
            order[j] = t;
        }
    size_t size = 8 + (size_t)nctls * 56;
    uint8_t *img = calloc(1, size);
    if (!img)
        return;
    put32(img, 1);
    put32(img + 4, (uint32_t)nctls);
    for (int i = 0; i < nctls; i++) {
        const cg_ctl *c = &ctls[order[i]];
        uint8_t *r = img + 8 + (size_t)i * 56;
        put32(r, c->version);
        put32(r + 4, c->flags);
        memcpy(r + 8, c->name, sizeof c->name - 1);
        memcpy(r + 24, c->path, sizeof c->path - 1);
    }
    rc_mfd = memfd_new("cgroup.rc");
    if (!rc_mfd || memfd_truncate(rc_mfd, size) < 0 || memfd_pwrite(rc_mfd, img, size, 0) != (int64_t)size) {
        free(img);
        if (rc_mfd)
            memfd_unref(rc_mfd);
        rc_mfd = NULL;
        return;
    }
    free(img);
}

int cgroupfs_init(void)
{
    if (enabled)
        return 0;
    load_desc("/system/etc/cgroups.json");
    load_desc("/vendor/etc/cgroups.json");
    if (nctls == 0)
        return 0;
    build_rc();
    enabled = rc_mfd != NULL;
    return 0;
}

/* ---------- paths ---------- */

/* The mount whose tree guest lies in (its root, or below), and the path under it ("" or "/a/b"). -1 if none. */
static int find_mount(const char *guest, const char **rest)
{
    int best = -1;
    size_t best_len = 0;
    for (int m = 0; m < nmounts; m++) {
        size_t n = strlen(mounts[m].path);
        if (strncmp(guest, mounts[m].path, n) == 0 && (guest[n] == 0 || guest[n] == '/') && n >= best_len) {
            best = m;
            best_len = n;
        }
    }
    if (best >= 0 && rest)
        *rest = guest + best_len;
    return best;
}

static int find_child(int node, const char *name)
{
    for (int i = 0; i < CG_MAX_NODES; i++)
        if (nodes[i].used && i != node && nodes[i].parent == node && !strcmp(nodes[i].name, name))
            return i;
    return -1;
}

static int find_file(int mount, const char *name)
{
    for (int i = 0; i < NFILES; i++)
        if ((files[i].kinds & mounts[mount].kinds) && !strcmp(files[i].name, name))
            return i;
    return -1;
}

/* Walks the path under a mount. Returns CG_DIR with *node, CG_FILE with *node (its directory) and *file, or CG_NONE. */
static cg_kind walk(int mount, const char *rest, int *node, int *file)
{
    int cur = mount;
    const char *p = rest;
    while (*p == '/')
        p++;
    if (!*p) {
        *node = cur;
        return CG_DIR;
    }
    for (;;) {
        char comp[CG_MAX_NAME];
        const char *slash = strchr(p, '/');
        size_t n = slash ? (size_t)(slash - p) : strlen(p);
        if (n == 0 || n >= sizeof comp)
            return CG_NONE;
        memcpy(comp, p, n);
        comp[n] = 0;
        int last = !slash || !slash[1];
        int child = find_child(cur, comp);
        if (last) {
            if (child >= 0) {
                *node = child;
                return CG_DIR;
            }
            int f = find_file(mount, comp);
            if (f >= 0) {
                *node = cur;
                *file = f;
                return CG_FILE;
            }
            return CG_NONE;
        }
        if (child < 0)
            return CG_NONE;
        cur = child;
        p = slash + 1;
    }
}

/* Resolves guest to a mount, and for a directory or file the node (and the file index). */
static cg_kind locate(const char *guest, int *mount, int *node, int *file)
{
    const char *rest;
    int m = find_mount(guest, &rest);
    if (m < 0)
        return CG_NONE;
    *mount = m;
    return walk(m, rest, node, file);
}

int cgroupfs_under(const char *guest)
{
    return enabled && find_mount(guest, NULL) >= 0;
}

cg_kind cgroupfs_lookup(const char *guest)
{
    if (!enabled)
        return CG_NONE;
    if (!strcmp(guest, "/dev/cgroup_info"))
        return CG_DIR;
    if (!strcmp(guest, "/dev/cgroup_info/cgroup.rc"))
        return CG_RC;
    int m, node = 0, file = -1;
    return locate(guest, &m, &node, &file);
}

memfd_t *cgroupfs_rc_memfd(void) { return enabled ? rc_mfd : NULL; }

/* ---------- membership and contents ---------- */

static int member_of(const proc_t *p, int mount)
{
    return (p->cg[mount] ? p->cg[mount] : mount);
}

static int populated(int mount, int node)
{
    for (proc_t *p = proc_first(); p; p = proc_next(p))
        if (!p->exited && member_of(p, mount) == node)
            return 1;
    return 0;
}

typedef struct {
    char *s;
    size_t n, cap;
} sbuf;

static void sb_add(sbuf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 128;
        while (cap < b->n + n + 1)
            cap *= 2;
        char *t = realloc(b->s, cap);
        if (!t)
            return;
        b->s = t;
        b->cap = cap;
    }
    if (b->s) {
        memcpy(b->s + b->n, s, n);
        b->n += n;
        b->s[b->n] = 0;
    }
}

static const char *attr_get(int node, int file, size_t *len)
{
    for (cg_attr *a = nodes[node].attrs; a; a = a->next)
        if (a->file == file) {
            *len = a->len;
            return a->val;
        }
    return NULL;
}

int cgroupfs_read(const char *guest, uint8_t **data, size_t *len)
{
    int m, node = 0, file = -1;
    if (locate(guest, &m, &node, &file) != CG_FILE)
        return -1;
    sbuf b = {0};
    const cg_file *f = &files[file];
    if (f->type == FT_PROCS) {
        for (proc_t *p = proc_first(); p; p = proc_next(p))
            if (!p->exited && member_of(p, m) == node) {
                char line[16];
                int k = snprintf(line, sizeof line, "%d\n", p->pid);
                sb_add(&b, line, (size_t)k);
            }
    } else if (f->type == FT_EVENTS) {
        char line[64];
        int k = snprintf(line, sizeof line, "populated %d\nfrozen 0\n", populated(m, node));
        sb_add(&b, line, (size_t)k);
    } else {
        size_t n = 0;
        const char *v = attr_get(node, file, &n);
        if (!v) {
            v = f->def;
            n = strlen(v);
        }
        sb_add(&b, v, n);
    }
    if (!b.s) {
        b.s = malloc(1);
        if (!b.s)
            return -1;
        b.s[0] = 0;
        b.cap = 1;
    }
    *data = (uint8_t *)b.s;
    *len = b.n;
    return 0;
}

int64_t cgroupfs_write(const char *guest, const uint8_t *buf, size_t n)
{
    int m, node = 0, file = -1;
    if (locate(guest, &m, &node, &file) != CG_FILE)
        return -LE_NOENT;
    const cg_file *f = &files[file];
    if (f->type == FT_EVENTS || f->type == FT_RO)
        return -LE_PERM;
    if (f->type == FT_PROCS) {
        /* One pid per line (or separated by spaces). 0 is the writing process. */
        size_t i = 0;
        while (i < n) {
            while (i < n && (buf[i] < '0' || buf[i] > '9'))
                i++;
            if (i >= n)
                break;
            long pid = 0;
            while (i < n && buf[i] >= '0' && buf[i] <= '9')
                pid = pid * 10 + (buf[i++] - '0');
            proc_t *p = pid == 0 ? proc_cur() : proc_find((int)pid);
            if (!p || p->exited)
                return -LE_SRCH;
            p->cg[m] = node == m ? 0 : node;
        }
        return (int64_t)n;
    }
    /* FT_STORE: the value replaces the last one written. */
    cg_attr *a = NULL;
    for (cg_attr *x = nodes[node].attrs; x; x = x->next)
        if (x->file == file)
            a = x;
    if (!a) {
        a = calloc(1, sizeof *a);
        if (!a)
            return -LE_NOMEM;
        a->file = file;
        a->next = nodes[node].attrs;
        nodes[node].attrs = a;
    }
    char *v = malloc(n + 1);
    if (!v)
        return -LE_NOMEM;
    memcpy(v, buf, n);
    v[n] = 0;
    free(a->val);
    a->val = v;
    a->len = n;
    return (int64_t)n;
}

/* ---------- directories ---------- */

int cgroupfs_mkdir(const char *guest)
{
    int m, node = 0, file = -1;
    if (cgroupfs_lookup(guest) != CG_NONE)
        return -LE_EXIST;
    /* The parent is the guest path without its last component. */
    char parent[VFS_PATH_MAX];
    snprintf(parent, sizeof parent, "%s", guest);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent)
        return -LE_NOENT;
    const char *name = slash + 1;
    if (!*name || strlen(name) >= CG_MAX_NAME)
        return -LE_INVAL;
    *slash = 0;
    if (locate(parent, &m, &node, &file) != CG_DIR)
        return -LE_NOENT;
    int slot = -1;
    for (int i = nmounts; i < CG_MAX_NODES; i++)
        if (!nodes[i].used) {
            slot = i;
            break;
        }
    if (slot < 0)
        return -LE_NOMEM;
    memset(&nodes[slot], 0, sizeof nodes[slot]);
    nodes[slot].used = 1;
    nodes[slot].mount = m;
    nodes[slot].parent = node;
    snprintf(nodes[slot].name, sizeof nodes[slot].name, "%s", name);
    return 0;
}

int cgroupfs_rmdir(const char *guest)
{
    int m, node = 0, file = -1;
    if (locate(guest, &m, &node, &file) != CG_DIR)
        return -LE_NOENT;
    if (node < nmounts)
        return -LE_BUSY; /* a mount point */
    for (int i = 0; i < CG_MAX_NODES; i++)
        if (nodes[i].used && nodes[i].parent == node)
            return -LE_BUSY;
    if (populated(m, node))
        return -LE_BUSY;
    for (cg_attr *a = nodes[node].attrs; a;) {
        cg_attr *next = a->next;
        free(a->val);
        free(a);
        a = next;
    }
    memset(&nodes[node], 0, sizeof nodes[node]);
    return 0;
}

int cgroupfs_child(const char *guest, int index, const char **name, int *is_dir)
{
    if (!enabled)
        return 0;
    if (!strcmp(guest, "/dev/cgroup_info")) {
        if (index != 0)
            return 0;
        *name = "cgroup.rc";
        *is_dir = 0;
        return 1;
    }
    int m, node = 0, file = -1;
    if (locate(guest, &m, &node, &file) != CG_DIR)
        return 0;
    int i = index;
    for (int j = 0; j < CG_MAX_NODES; j++)
        if (nodes[j].used && j != node && nodes[j].parent == node) {
            if (i-- == 0) {
                *name = nodes[j].name;
                *is_dir = 1;
                return 1;
            }
        }
    for (int f = 0; f < NFILES; f++)
        if (files[f].kinds & mounts[m].kinds) {
            if (i-- == 0) {
                *name = files[f].name;
                *is_dir = 0;
                return 1;
            }
        }
    return 0;
}

/* ---------- process lifetime and /proc/<pid>/cgroup ---------- */

void cgroupfs_proc_exit(proc_t *p)
{
    memset(p->cg, 0, sizeof p->cg); /* p->exited is already set (sched.c): an exited process is in no cgroup */
}

/* The path of a node within its mount ("/" for the root). */
static void node_path(int node, sbuf *b)
{
    if (node < nmounts) {
        sb_add(b, "/", 1);
        return;
    }
    char parts[CG_MAX_NODES / 8][CG_MAX_NAME];
    int depth = 0;
    for (int cur = node; cur >= nmounts && depth < CG_MAX_NODES / 8; cur = nodes[cur].parent) {
        snprintf(parts[depth++], CG_MAX_NAME, "%s", nodes[cur].name);
    }
    for (int i = depth - 1; i >= 0; i--) {
        sb_add(b, "/", 1);
        sb_add(b, parts[i], strlen(parts[i]));
    }
}

int cgroupfs_proc_text(int pid, uint8_t **data, size_t *len)
{
    proc_t *p = proc_find(pid);
    if (!p || p->exited)
        return -1;
    sbuf b = {0};
    for (int m = 0; m < nmounts; m++) {
        int node = member_of(p, m);
        if (mounts[m].v2) {
            sb_add(&b, "0::", 3);
            node_path(node, &b);
            sb_add(&b, "\n", 1);
            continue;
        }
        for (int c = 0; c < nctls; c++) {
            if (ctls[c].mount != m || ctls[c].version != 1)
                continue;
            char head[64];
            int k = snprintf(head, sizeof head, "%d:%s:", c + 2, ctls[c].name);
            sb_add(&b, head, (size_t)k);
            node_path(node, &b);
            sb_add(&b, "\n", 1);
        }
    }
    if (!b.s) {
        b.s = malloc(1);
        if (!b.s)
            return -1;
        b.s[0] = 0;
    }
    *data = (uint8_t *)b.s;
    *len = b.n;
    return 0;
}
