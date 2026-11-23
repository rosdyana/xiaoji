/* Mount namespaces (B4c). A namespace is an ordered list of mounts over the guest's visible paths.
 * The root (the host root that vfs.c maps "/" to) is not in the list: it is the namespace when the
 * list is empty, and every lookup with no covering mount uses it unchanged.
 *
 * A lookup walks the mount tree from the root, as Linux does: a prefix of the path that ends at a
 * component is entered when a mount with that target and the current mount as its parent exists (the
 * newest one, and a mount stacked on the same target in turn). So a mount is reached only through the
 * mount that covers its target's parent path: a tmpfs mounted over a directory hides the mounts made
 * below it before, and a mount made under a hidden mount is hidden with it. A detached mount (umount2
 * with MNT_DETACH) leaves its children unreachable: their parent is gone.
 *
 * Bind mounts keep the host path of their source, taken at mount time, so a later change to the
 * namespace does not re-point them. tmpfs mounts own a host directory under /tmp, shared by copies of
 * the namespace and removed when the last copy drops it. Propagation is recorded (shared or not) and
 * shown in mountinfo; it is not applied: a copy is a snapshot, and no mount is seen across namespaces. */
#include "mntns.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "host.h"
#include "linux.h"
#include "vfs.h"

typedef struct {
    int refs;
    char dir[VFS_PATH_MAX];
} tmpfs_t;

typedef struct {
    int id, parent;
    int tmpfs;   /* 0: bind of a host directory (back); 1: tmpfs (tfs) */
    int shared;  /* MS_SHARED was set on the mount */
    int ro, nosuid, nodev, noexec;
    char target[VFS_PATH_MAX]; /* canonical guest path of the mount point */
    char back[VFS_PATH_MAX];   /* host directory that the mount shows */
    char root[VFS_PATH_MAX];   /* the source's canonical guest path (mountinfo's root), or "/" */
    tmpfs_t *tfs;
} mnt_t;

struct mntns {
    int refs;
    int next_id;
    int n, cap;
    mnt_t *m;
};

/* Removes a host path and, for a directory, everything under it (symlinks are removed, not followed). */
static void rm_tree(const char *path)
{
    host_stat st;
    if (host_stat_path(path, 0, &st))
        return;
    if ((st.mode & LS_IFMT) != LS_IFDIR) {
        host_unlink(path);
        return;
    }
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char child[VFS_PATH_MAX];
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            rm_tree(child);
        }
        closedir(d);
    }
    host_rmdir(path);
}

/* Drops a tmpfs reference; the last one removes its directory. */
static void tfs_unref(tmpfs_t *t)
{
    if (!t || --t->refs > 0)
        return;
    rm_tree(t->dir);
    free(t);
}

static void free_mounts(mntns_t *ns)
{
    for (int i = 0; i < ns->n; i++)
        tfs_unref(ns->m[i].tfs);
    free(ns->m);
    ns->m = NULL;
    ns->n = ns->cap = 0;
}

mntns_t *mntns_new(void)
{
    mntns_t *ns = calloc(1, sizeof *ns);
    if (!ns)
        return NULL;
    ns->refs = 1;
    ns->next_id = 2; /* 1 is the root */
    return ns;
}

mntns_t *mntns_ref(mntns_t *ns)
{
    if (ns)
        ns->refs++;
    return ns;
}

void mntns_unref(mntns_t *ns)
{
    if (!ns || --ns->refs > 0)
        return;
    free_mounts(ns);
    free(ns);
}

mntns_t *mntns_copy(mntns_t *src)
{
    mntns_t *ns = mntns_new();
    if (!ns)
        return NULL;
    if (!src)
        return ns;
    ns->next_id = src->next_id;
    if (src->n) {
        ns->m = malloc((size_t)src->n * sizeof *ns->m);
        if (!ns->m) {
            mntns_unref(ns);
            return NULL;
        }
        memcpy(ns->m, src->m, (size_t)src->n * sizeof *ns->m);
        ns->n = ns->cap = src->n;
        for (int i = 0; i < ns->n; i++)
            if (ns->m[i].tfs)
                ns->m[i].tfs->refs++;
    }
    return ns;
}

/* Appends a mount; returns its slot or NULL on out of memory. */
static mnt_t *add_mount(mntns_t *ns)
{
    if (ns->n == ns->cap) {
        int cap = ns->cap ? ns->cap * 2 : 4;
        mnt_t *m = realloc(ns->m, (size_t)cap * sizeof *m);
        if (!m)
            return NULL;
        ns->m = m;
        ns->cap = cap;
    }
    mnt_t *slot = &ns->m[ns->n];
    memset(slot, 0, sizeof *slot);
    slot->id = ns->next_id++;
    ns->n++;
    return slot;
}

/* The rest of guest below target (starting with "/" or empty), or NULL when guest is not under it. */
static const char *under(const char *target, const char *guest)
{
    size_t tl = strlen(target);
    if (strncmp(guest, target, tl))
        return NULL;
    if (guest[tl] == 0 || guest[tl] == '/')
        return guest + tl;
    return NULL;
}

/* The walk of the mount tree (see the header). Returns the index of the last mount entered (-1 for the root) and sets
 * *rem to the rest of guest below that mount's target. A mount is entered only as the child of the mount the walk is
 * at, so a mount under a covered path is not reached. */
static int cover_index(const mntns_t *ns, const char *guest, const char **rem)
{
    int cur = -1, cur_id = 0;
    size_t gl = strlen(guest), done = 0;
    for (size_t L = 1; L <= gl; L++) {
        if (L != gl && guest[L] != '/')
            continue;
        for (;;) {
            int pick = -1;
            for (int i = 0; i < ns->n; i++)
                if (ns->m[i].parent == cur_id && strlen(ns->m[i].target) == L && !strncmp(ns->m[i].target, guest, L))
                    pick = i; /* the newest of the children at this target (a stacked mount is newer) */
            if (pick < 0)
                break;
            cur = pick;
            cur_id = ns->m[pick].id;
            done = L;
        }
    }
    *rem = guest + done;
    return cur;
}

int mntns_map(mntns_t *ns, const char *guest, char *out, size_t cap, int *ro)
{
    if (!ns || ns->n == 0)
        return 0;
    const char *rem = "";
    int i = cover_index(ns, guest, &rem);
    if (i < 0)
        return 0;
    const mnt_t *m = &ns->m[i];
    size_t bl = strlen(m->back), rl = strlen(rem);
    if (bl + rl + 1 > cap)
        return -LE_NAMETOOLONG;
    memcpy(out, m->back, bl);
    memcpy(out + bl, rem, rl + 1);
    if (ro)
        *ro = m->ro;
    return 1;
}

/* Index of the live mount at exactly target, or -1. */
static int live_at(const mntns_t *ns, const char *target)
{
    const char *rem = "";
    int i = cover_index(ns, target, &rem);
    return i >= 0 && rem[0] == 0 ? i : -1;
}

/* A mount is live when the walk enters it at its own target (see cover_index). */
static int is_live(const mntns_t *ns, int i)
{
    return live_at(ns, ns->m[i].target) == i;
}

/* The id of the mount that the walk reaches at target (the new mount's parent), or 0 for the root. */
static int parent_of(const mntns_t *ns, const char *target)
{
    const char *rem = "";
    int i = cover_index(ns, target, &rem);
    return i < 0 ? 0 : ns->m[i].id;
}

static void set_flags(mnt_t *m, uint64_t flags)
{
    m->ro = (flags & MNT_MS_RDONLY) != 0;
    m->nosuid = (flags & MNT_MS_NOSUID) != 0;
    m->nodev = (flags & MNT_MS_NODEV) != 0;
    m->noexec = (flags & MNT_MS_NOEXEC) != 0;
}

/* Resolves a guest path that must name an existing directory or file (the mount point, or a source).
 * Synthetic paths (/proc, /sys, /dev and their children) are not mountable here: EINVAL. */
static int resolve_host(const char *guest, vfs_res *r)
{
    int rc = vfs_resolve(LAT_FDCWD, guest, 1, r);
    if (rc)
        return rc;
    if (!r->exists)
        return -LE_NOENT;
    if (r->syn != SYN_NONE)
        return -LE_INVAL;
    return 0;
}

/* Makes a bind of the source tree at target, copying the live submounts of the source when rec is set. */
static int64_t bind_mount(mntns_t *ns, const vfs_res *src, const vfs_res *tgt, int rec)
{
    /* Static buffers: the wasm build has a small stack (as vfs.c does). Not re-entrant, as the engine is not. */
    static char target[VFS_PATH_MAX];
    /* The flags of the mount that covers the source (none for the root). The flags of the bind call are
     * not applied: Linux ignores MS_RDONLY and the other mount flags there, and a remount sets them. Read
     * before the new mount is added, so the new mount cannot cover its own source. */
    int sro = 0, snosuid = 0, snodev = 0, snoexec = 0;
    const char *srem = "";
    int si = cover_index(ns, src->guest, &srem);
    if (si >= 0) {
        sro = ns->m[si].ro;
        snosuid = ns->m[si].nosuid;
        snodev = ns->m[si].nodev;
        snoexec = ns->m[si].noexec;
    }
    /* The submounts to copy are the live mounts strictly below the source (MS_REC), found before
     * the new mount is added: they are the first n entries. */
    int n = ns->n;
    /* Computed before the slot is added: an empty target would cover every path. */
    int parent = parent_of(ns, tgt->guest);
    mnt_t *m = add_mount(ns);
    if (!m)
        return -LE_NOMEM;
    m->parent = parent;
    m->tmpfs = 0;
    snprintf(m->target, sizeof m->target, "%s", tgt->guest);
    snprintf(m->back, sizeof m->back, "%s", src->host);
    snprintf(m->root, sizeof m->root, "%s", src->guest);
    m->ro = sro;
    m->nosuid = snosuid;
    m->nodev = snodev;
    m->noexec = snoexec;
    if (!rec)
        return 0;
    for (int i = 0; i < n; i++) {
        const char *rest = under(src->guest, ns->m[i].target); /* the submount's path below the source */
        if (!rest || rest[0] == 0 || !is_live(ns, i))
            continue; /* the source's own mount point is the one being bound */
        if (strlen(tgt->guest) + strlen(rest) + 1 > sizeof target)
            return -LE_NAMETOOLONG;
        snprintf(target, sizeof target, "%s%s", tgt->guest, rest);
        int cparent = parent_of(ns, target);
        mnt_t *c = add_mount(ns); /* may move ns->m: index i is used below, not pointers */
        if (!c)
            return -LE_NOMEM;
        int new_id = c->id;
        *c = ns->m[i];
        c->id = new_id;
        c->parent = cparent;
        snprintf(c->target, sizeof c->target, "%s", target);
        if (c->tfs)
            c->tfs->refs++;
    }
    return 0;
}

int64_t mntns_mount(mntns_t *ns, const char *src, const char *tgt, const char *fstype, uint64_t flags)
{
    static vfs_res tr, sr;
    if (!tgt)
        return -LE_INVAL;
    int rc = resolve_host(tgt, &tr);
    if (rc)
        return rc;
    int root = !strcmp(tr.guest, "/");

    /* The root's flags and propagation are not modelled: they are accepted, with no effect. */
    if (flags & MNT_MS_REMOUNT) {
        if (root)
            return 0;
        int i = live_at(ns, tr.guest);
        if (i < 0)
            return -LE_INVAL;
        set_flags(&ns->m[i], flags);
        return 0;
    }
    if (flags & MNT_MS_BIND) {
        if (!src)
            return -LE_INVAL;
        if (root)
            return -LE_BUSY; /* a bind onto the root is not modelled (a mount on "/" covers nothing) */
        rc = resolve_host(src, &sr);
        if (rc)
            return rc;
        return bind_mount(ns, &sr, &tr, (flags & MNT_MS_REC) != 0);
    }

    if (flags & MNT_MS_PROPAGATION) {
        /* Propagation only: recorded on the live mount at target (the root has none). */
        int i = live_at(ns, tr.guest);
        if (i >= 0)
            ns->m[i].shared = (flags & (1ull << 20)) != 0;
        return 0;
    }
    if (flags & MNT_MS_MOVE)
        return -LE_INVAL; /* not modelled */
    if (root)
        return -LE_BUSY; /* a bind or tmpfs over the root */

    if (fstype && !strcmp(fstype, "tmpfs")) {
        static unsigned seq;
        tmpfs_t *t = calloc(1, sizeof *t);
        if (!t)
            return -LE_NOMEM;
        t->refs = 1;
        snprintf(t->dir, sizeof t->dir, "/tmp/xiaoji-tmpfs-%d-%u", (int)getpid(), ++seq);
        rc = host_mkdir(t->dir, 0755);
        if (rc) {
            free(t);
            return rc;
        }
        int parent = parent_of(ns, tr.guest);
        mnt_t *m = add_mount(ns);
        if (!m) {
            tfs_unref(t);
            return -LE_NOMEM;
        }
        m->parent = parent;
        m->tmpfs = 1;
        m->tfs = t;
        snprintf(m->target, sizeof m->target, "%s", tr.guest);
        snprintf(m->back, sizeof m->back, "%s", t->dir);
        snprintf(m->root, sizeof m->root, "/");
        set_flags(m, flags);
        return 0;
    }
    return fstype ? -LE_NODEV : -LE_INVAL;
}

/* Removes the mount at index idx and every mount under it (MNT_DETACH takes the subtree, as Linux's
 * umount_tree does). A parent always precedes its children in the table, so one pass finds them.
 * Returns 0, or -LE_NOMEM (nothing is removed). */
static int64_t detach_subtree(mntns_t *ns, int idx)
{
    char *dead = calloc((size_t)ns->n, 1);
    if (!dead)
        return -LE_NOMEM;
    dead[idx] = 1;
    for (int j = idx + 1; j < ns->n; j++)
        for (int k = 0; k < j; k++)
            if (dead[k] && ns->m[k].id == ns->m[j].parent) {
                dead[j] = 1;
                break;
            }
    int w = 0;
    for (int r = 0; r < ns->n; r++) {
        if (dead[r]) {
            tfs_unref(ns->m[r].tfs);
            continue;
        }
        if (w != r)
            ns->m[w] = ns->m[r];
        w++;
    }
    ns->n = w;
    free(dead);
    return 0;
}

int64_t mntns_umount(mntns_t *ns, const char *tgt, uint64_t flags)
{
    static vfs_res tr;
    int rc = vfs_resolve(LAT_FDCWD, tgt, 1, &tr);
    if (rc)
        return rc;
    int i = live_at(ns, tr.guest);
    if (i < 0)
        return -LE_INVAL; /* not a mount point */
    int id = ns->m[i].id;
    if (!(flags & MNT_UMOUNT_DETACH)) {
        for (int j = 0; j < ns->n; j++)
            if (ns->m[j].parent == id)
                return -LE_BUSY;
    }
    return detach_subtree(ns, i);
}

/* Octal escapes for the characters that the mount tables separate fields by (as the kernel does). */
static void put_escaped(char *out, size_t cap, size_t *len, const char *s)
{
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        char tmp[8];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\\')
            snprintf(tmp, sizeof tmp, "\\%03o", c);
        else
            snprintf(tmp, sizeof tmp, "%c", c);
        size_t tl = strlen(tmp);
        if (*len + tl + 1 > cap)
            return;
        memcpy(out + *len, tmp, tl);
        *len += tl;
    }
    out[*len] = 0;
}

typedef struct {
    char *p;
    size_t n, cap;
    int oom;
} tbuf;

static void tb_add(tbuf *b, const char *s)
{
    size_t l = strlen(s);
    if (b->oom)
        return;
    if (b->n + l + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + l + 1)
            cap *= 2;
        char *np = realloc(b->p, cap);
        if (!np) {
            b->oom = 1;
            return;
        }
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, l + 1);
    b->n += l;
}

/* A field, escaped, into the buffer. */
static void tb_field(tbuf *b, const char *s)
{
    static char esc[VFS_PATH_MAX * 4];
    size_t len = 0;
    put_escaped(esc, sizeof esc, &len, s);
    tb_add(b, esc);
}

int mntns_text(mntns_t *ns, int info, uint8_t **data, size_t *len)
{
    tbuf b = {0};
    char line[64];
    if (info) {
        tb_add(&b, "1 1 0:1 / / rw - rootfs rootfs rw\n");
    } else {
        tb_add(&b, "rootfs / rootfs rw 0 0\n");
    }
    /* Every mount of the namespace is listed, in the order they were made, the covered ones too (as Linux lists them):
     * the table holds only attached mounts (umount compacts it, and MNT_DETACH takes the subtree). */
    for (int i = 0; ns && i < ns->n; i++) {
        const mnt_t *m = &ns->m[i];
        char opts[64];
        snprintf(opts, sizeof opts, "%s%s%s%s", m->ro ? "ro" : "rw", m->nosuid ? ",nosuid" : "",
                 m->nodev ? ",nodev" : "", m->noexec ? ",noexec" : "");
        const char *fstype = m->tmpfs ? "tmpfs" : "rootfs";
        if (info) {
            snprintf(line, sizeof line, "%d %d 0:%d ", m->id, m->parent ? m->parent : 1, m->id);
            tb_add(&b, line);
            tb_field(&b, m->root);
            tb_add(&b, " ");
            tb_field(&b, m->target);
            tb_add(&b, " ");
            tb_add(&b, opts);
            if (m->shared) {
                snprintf(line, sizeof line, " shared:%d", m->id);
                tb_add(&b, line);
            }
            tb_add(&b, " - ");
            tb_add(&b, fstype);
            tb_add(&b, " ");
            tb_add(&b, m->tmpfs ? "tmpfs" : "rootfs");
            tb_add(&b, " ");
            tb_add(&b, opts);
            tb_add(&b, "\n");
        } else {
            tb_add(&b, m->tmpfs ? "tmpfs " : "rootfs ");
            tb_field(&b, m->target);
            tb_add(&b, " ");
            tb_add(&b, fstype);
            tb_add(&b, " ");
            tb_add(&b, opts);
            tb_add(&b, " 0 0\n");
        }
    }
    if (b.oom) {
        free(b.p);
        return -1;
    }
    if (!b.p) {
        b.p = malloc(1);
        if (!b.p)
            return -1;
        b.p[0] = 0;
    }
    *data = (uint8_t *)b.p;
    *len = b.n;
    return 0;
}
