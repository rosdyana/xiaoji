/* Guest file metadata (meta.h). In-memory records keyed by the host (dev, ino). */
#define _XOPEN_SOURCE 700 /* opendir, readdir and lstat, as vfs.c */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cred.h"
#include "linux.h"
#include "meta.h"
#include "proc.h"

#define META_BUCKETS 1024

#define M_ISUID 04000u
#define M_ISGID 02000u
#define M_ISVTX 01000u
#define M_IXGRP 00010u
#define M_IFDIR 0040000u
#define M_IFMT 0170000u
#define M_PERMS 07777u
#define M_OTHER_GROUP_WRITE 0022u

typedef struct mrec {
    uint64_t dev, ino;
    uint32_t uid, gid, mode;
    struct mrec *next;
} mrec_t;

static mrec_t *bucket[META_BUCKETS];

static void die_oom(void)
{
    fputs("xiaoji: out of host memory\n", stderr);
    exit(1);
}

static unsigned slot_of(uint64_t dev, uint64_t ino)
{
    uint64_t h = dev * 0x9E3779B97F4A7C15ull ^ (ino + 0x632BE59BD9B4E019ull);
    h ^= h >> 29;
    return (unsigned)(h % META_BUCKETS);
}

static mrec_t *find(uint64_t dev, uint64_t ino)
{
    for (mrec_t *r = bucket[slot_of(dev, ino)]; r; r = r->next)
        if (r->dev == dev && r->ino == ino)
            return r;
    return NULL;
}

static mrec_t *find_or_add(uint64_t dev, uint64_t ino)
{
    mrec_t *r = find(dev, ino);
    if (r)
        return r;
    r = malloc(sizeof *r);
    if (!r)
        die_oom();
    unsigned s = slot_of(dev, ino);
    r->dev = dev;
    r->ino = ino;
    r->uid = r->gid = r->mode = 0;
    r->next = bucket[s];
    bucket[s] = r;
    return r;
}

/* The calling process's credentials. */
static const proc_cred_t *cur(void) { return &proc_cur()->cred; }

static int cap(int c) { return cred_has_cap(proc_cur(), c); }

/* in_group_p: the fsgid or a supplementary group (fs/inode.c uses current_fsgid()). */
static int in_group(uint32_t gid)
{
    const proc_cred_t *c = cur();
    if (gid == c->fsgid)
        return 1;
    for (int i = 0; i < c->ngroups; i++)
        if (c->groups[i] == gid)
            return 1;
    return 0;
}

/* in_group_or_capable: the caller is in the group, or has CAP_FSETID. */
static int in_group_or_fsetid(uint32_t gid) { return in_group(gid) || cap(LCAP_FSETID); }

static void defaults(const host_stat *st, meta_t *m)
{
    m->uid = 0;
    m->gid = 0;
    m->mode = st->mode & M_PERMS & ~M_OTHER_GROUP_WRITE;
}

void meta_get(const host_stat *st, meta_t *out)
{
    mrec_t *r = find(st->dev, st->ino);
    if (!r) {
        defaults(st, out);
        return;
    }
    out->uid = r->uid;
    out->gid = r->gid;
    out->mode = r->mode;
}

void meta_set(const host_stat *st, const meta_t *m)
{
    mrec_t *r = find_or_add(st->dev, st->ino);
    r->uid = m->uid;
    r->gid = m->gid;
    r->mode = m->mode & M_PERMS;
}

void meta_apply_stat(host_stat *st)
{
    meta_t m;
    meta_get(st, &m);
    st->uid = m.uid;
    st->gid = m.gid;
    st->mode = (st->mode & M_IFMT) | m.mode;
}

/* inode_init_owner and vfs_prepare_mode (fs/inode.c, fs/namei.c), for a new object under dir. */
static void create_record(const host_stat *obj, const host_stat *dir, int is_dir, uint32_t mode, uint32_t umask_bits)
{
    const proc_cred_t *c = cur();
    meta_t parent = {0, 0, 0};
    int dir_sgid = 0;
    if (dir) {
        meta_get(dir, &parent);
        dir_sgid = (parent.mode & M_ISGID) != 0;
    }
    meta_t m;
    m.uid = c->fsuid;
    m.gid = dir_sgid ? parent.gid : c->fsgid;

    uint32_t md = mode & M_PERMS;
    /* mode_strip_sgid: a setgid file in a setgid directory keeps the bit only for a member of that group. */
    if (!is_dir && (md & (M_ISGID | M_IXGRP)) == (M_ISGID | M_IXGRP) && dir_sgid && !in_group_or_fsetid(parent.gid))
        md &= ~M_ISGID;
    md &= ~(umask_bits & 0777u);
    /* Directories always inherit the setgid bit of a setgid parent (inode_init_owner). */
    if (is_dir && dir_sgid)
        md |= M_ISGID;
    m.mode = md & M_PERMS;
    meta_set(obj, &m);
}

int meta_new(const char *host, int is_dir, uint32_t mode, uint32_t umask_bits)
{
    host_stat obj, dir;
    char parent[4096];
    size_t n = strlen(host);
    if (n >= sizeof parent)
        return -36; /* ENAMETOOLONG */
    memcpy(parent, host, n + 1);
    while (n > 1 && parent[n - 1] == '/')
        parent[--n] = 0;
    char *slash = strrchr(parent, '/');
    if (!slash)
        strcpy(parent, ".");
    else if (slash == parent)
        parent[1] = 0;
    else
        *slash = 0;
    int rc = host_stat_path(host, 0, &obj);
    if (rc)
        return rc;
    if (host_stat_path(parent, 1, &dir))
        return -LE_NOENT;
    create_record(&obj, &dir, is_dir, mode, umask_bits);
    return 0;
}

/* chown_common (fs/open.c), with notify_change's setid kills (fs/attr.c). */
int meta_chown(const host_stat *st, uint32_t uid, uint32_t gid)
{
    meta_t m;
    meta_get(st, &m);
    const proc_cred_t *c = cur();
    int is_dir = (st->mode & M_IFMT) == M_IFDIR;
    int owner = m.uid == c->fsuid;

    /* chown_ok: the owner may keep its own uid, anyone else needs CAP_CHOWN. */
    if (uid != META_NOCHANGE && !(owner && uid == m.uid) && !cap(LCAP_CHOWN))
        return -LE_PERM;
    /* chgrp_ok: the owner may set its own gid or a group it is in. */
    if (gid != META_NOCHANGE && !(owner && (gid == m.gid || in_group(gid))) && !cap(LCAP_CHOWN))
        return -LE_PERM;

    /* A chown clears setuid on a non-directory, and setgid when the group-execute bit is set or the caller is
     * not in the file's group (the test is on the group before the change). */
    if (!is_dir) {
        m.mode &= ~M_ISUID;
        if ((m.mode & M_ISGID) && ((m.mode & M_IXGRP) || !in_group_or_fsetid(m.gid)))
            m.mode &= ~M_ISGID;
    }
    if (uid != META_NOCHANGE)
        m.uid = uid;
    if (gid != META_NOCHANGE)
        m.gid = gid;
    meta_set(st, &m);
    return 0;
}

/* chmod_common (fs/open.c) with setattr_prepare's setgid rule. */
int meta_chmod(const host_stat *st, uint32_t mode)
{
    meta_t m;
    meta_get(st, &m);
    if (m.uid != cur()->fsuid && !cap(LCAP_FOWNER))
        return -LE_PERM;
    uint32_t md = mode & M_PERMS;
    if ((md & M_ISGID) && !in_group_or_fsetid(m.gid))
        md &= ~M_ISGID;
    m.mode = md;
    meta_set(st, &m);
    return 0;
}

/* file_remove_privs (fs/inode.c): setattr_should_drop_suidgid for a regular file, unless CAP_FSETID. */
void meta_write_privs(uint64_t dev, uint64_t ino)
{
    mrec_t *r = find(dev, ino);
    if (!r || !(r->mode & (M_ISUID | M_ISGID)))
        return;
    int drop_u = (r->mode & M_ISUID) != 0;
    int drop_g = (r->mode & M_ISGID) && ((r->mode & M_IXGRP) || !in_group_or_fsetid(r->gid));
    if (!(drop_u || drop_g) || cap(LCAP_FSETID))
        return;
    if (drop_u)
        r->mode &= ~M_ISUID;
    if (drop_g)
        r->mode &= ~M_ISGID;
}

/* Persistence (B8 checkpoint 2). With --root, the records of the writable trees are kept in <root>/.xiaoji-meta, one
 * line per record: the guest path, the uid, the gid and the mode in octal, with a tab between them, after the header
 * "xiaoji-meta 1". The file is loaded when the engine starts and written at exit (an atexit handler, which runs on
 * every normal exit and on exit(1)). A write goes to .xiaoji-meta.tmp and is renamed over the file. A record whose file
 * is gone is not written, so a removed file leaves no entry. A kill by a signal writes nothing. The guest cannot see or
 * name the two files (meta_hidden_guest). No other store exists for these records. */
#define META_FILE ".xiaoji-meta"
#define META_TMP ".xiaoji-meta.tmp"
#define META_HEADER "xiaoji-meta 1"

int meta_hidden_guest(const char *guest)
{
    return !strcmp(guest, "/" META_FILE) || !strcmp(guest, "/" META_TMP);
}

int meta_utime_check(const host_stat *st, int explicit_times)
{
    meta_t m;
    meta_get(st, &m);
    if (m.uid == cur()->fsuid || cap(LCAP_FOWNER))
        return 0;
    if (explicit_times)
        return -LE_PERM;
    if (cap(LCAP_DAC_OVERRIDE))
        return 0;
    uint32_t bit = in_group(m.gid) ? 0020u : 0002u; /* the group or the other class (the owner is handled above) */
    return (m.mode & bit) ? 0 : -LE_ACCES;
}

int meta_access(const host_stat *st, uint32_t mode)
{
    if (!mode)
        return 0;
    meta_t m;
    meta_get(st, &m);
    int is_dir = (st->mode & M_IFMT) == M_IFDIR;
    if (cap(LCAP_DAC_OVERRIDE)) {
        /* Read and write always pass; execute needs an exec bit on a file (a directory always passes). */
        if (!(mode & 1) || is_dir || (m.mode & 0111))
            return 0;
        return -LE_ACCES;
    }
    uint32_t cls;
    if (m.uid == cur()->fsuid)
        cls = (m.mode >> 6) & 7;
    else if (in_group(m.gid))
        cls = (m.mode >> 3) & 7;
    else
        cls = m.mode & 7;
    if ((cls & mode) == mode)
        return 0;
    /* CAP_DAC_READ_SEARCH: read any file, and search (execute) a directory; never write. */
    if (cap(LCAP_DAC_READ_SEARCH) && !(mode & 2) && (!(mode & 1) || is_dir))
        return 0;
    return -LE_ACCES;
}

int meta_recorded(const host_stat *st, meta_t *out)
{
    mrec_t *r = find(st->dev, st->ino);
    if (!r)
        return 0;
    out->uid = r->uid;
    out->gid = r->gid;
    out->mode = r->mode;
    return 1;
}

static char persist_root[4096];
static int persist_on;
static const char *const tree_names[] = {"/data", "/cache", "/metadata"};
#define NTREES ((int)(sizeof tree_names / sizeof tree_names[0]))

/* Writes the records under host (guest is its guest path) as sidecar lines, then recurses into directories. */
static void write_tree(FILE *out, const char *host, const char *guest, int depth)
{
    DIR *d = opendir(host);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        if (!strcmp(name, ".") || !strcmp(name, ".."))
            continue;
        if (strchr(name, '\t') || strchr(name, '\n'))
            continue;
        char hp[4096], gp[4096];
        if (snprintf(hp, sizeof hp, "%s/%s", host, name) >= (int)sizeof hp)
            continue;
        if (snprintf(gp, sizeof gp, "%s/%s", !strcmp(guest, "/") ? "" : guest, name) >= (int)sizeof gp)
            continue;
        host_stat st;
        if (host_stat_path(hp, 0, &st) != 0)
            continue;
        meta_t m;
        if (meta_recorded(&st, &m))
            fprintf(out, "%s\t%u\t%u\t%o\n", gp, (unsigned)m.uid, (unsigned)m.gid, (unsigned)m.mode);
        if ((st.mode & M_IFMT) == M_IFDIR && depth < 64)
            write_tree(out, hp, gp, depth + 1);
    }
    closedir(d);
}

/* The sidecar text for the writable trees under root, in a temporary file. NULL on failure. */
static FILE *write_sidecar_tmp(const char *root)
{
    char tmp[4096];
    if (snprintf(tmp, sizeof tmp, "%s/%s", root, META_TMP) >= (int)sizeof tmp)
        return NULL;
    FILE *out = fopen(tmp, "w");
    if (!out)
        return NULL;
    fprintf(out, "%s\n", META_HEADER);
    for (int i = 0; i < NTREES; i++) {
        char host[4096];
        if (snprintf(host, sizeof host, "%s%s", root, tree_names[i]) >= (int)sizeof host)
            continue;
        host_stat st;
        if (host_stat_path(host, 0, &st) == 0 && (st.mode & M_IFMT) == M_IFDIR)
            write_tree(out, host, tree_names[i], 0);
    }
    return out;
}

#ifndef XIAOJI_LAZY_FS
static int tree_allowed(const char *guest)
{
    for (int i = 0; i < NTREES; i++) {
        size_t n = strlen(tree_names[i]);
        if (!strncmp(guest, tree_names[i], n) && (guest[n] == 0 || guest[n] == '/'))
            return 1;
    }
    return 0;
}

/* Parses one sidecar line into path, uid, gid and mode (octal). Returns 0 for a bad line. */
static int parse_line(char *line, char **path, uint32_t *uid, uint32_t *gid, uint32_t *mode)
{
    char *f[4];
    char *p = line;
    for (int i = 0; i < 4; i++) {
        f[i] = p;
        if (i < 3) {
            char *t = strchr(p, '\t');
            if (!t)
                return 0;
            *t = 0;
            p = t + 1;
        }
    }
    if (f[0][0] != '/' || strstr(f[0], "/../") || !tree_allowed(f[0]))
        return 0;
    char *e[3];
    unsigned long v[3];
    v[0] = strtoul(f[1], &e[0], 10);
    v[1] = strtoul(f[2], &e[1], 10);
    v[2] = strtoul(f[3], &e[2], 8);
    if (*f[1] == 0 || *f[2] == 0 || *f[3] == 0 || *e[0] || *e[1] || *e[2])
        return 0;
    if (v[0] > 0xffffffffu || v[1] > 0xffffffffu || v[2] > 07777)
        return 0;
    *path = f[0];
    *uid = (uint32_t)v[0];
    *gid = (uint32_t)v[1];
    *mode = (uint32_t)v[2];
    return 1;
}

static void load_sidecar(void)
{
    char path[4096];
    if (snprintf(path, sizeof path, "%s/%s", persist_root, META_FILE) >= (int)sizeof path)
        return;
    FILE *in = fopen(path, "r");
    if (!in)
        return;
    char line[4096];
    if (!fgets(line, sizeof line, in) || strcmp(line, META_HEADER "\n") != 0) {
        fclose(in);
        return; /* not a sidecar of this version: ignored */
    }
    while (fgets(line, sizeof line, in)) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n')
            line[--n] = 0;
        char *gpath;
        uint32_t uid, gid, mode;
        if (!parse_line(line, &gpath, &uid, &gid, &mode))
            continue;
        char host[4096];
        if (snprintf(host, sizeof host, "%s%s", persist_root, gpath) >= (int)sizeof host)
            continue;
        host_stat st;
        if (host_stat_path(host, 0, &st) != 0)
            continue; /* the file is gone: no record */
        meta_t m = {uid, gid, mode};
        meta_set(&st, &m);
    }
    fclose(in);
}
#endif /* XIAOJI_LAZY_FS */

int meta_persist_open(const char *root)
{
#ifdef XIAOJI_LAZY_FS
    /* The browser build (Makefile target wasm) has a lazy HTTP file system as its root. Loading the sidecar would stat
     * every recorded file, and the first stat of a lazy file downloads it and prints a warning on stdout (the web test
     * caught this). The browser build therefore keeps no sidecar: its records last for the session, as B8 part 1 did
     * before the sidecar. */
    (void)root;
    return 0;
#else
    size_t n = strlen(root);
    while (n > 1 && root[n - 1] == '/')
        n--;
    if (n >= sizeof persist_root)
        return -1;
    memcpy(persist_root, root, n);
    persist_root[n] = 0;
    persist_on = 1;
    load_sidecar();
    return 0;
#endif
}

void meta_persist_save(void)
{
    if (!persist_on)
        return;
    FILE *out = write_sidecar_tmp(persist_root);
    if (!out) {
        fputs("xiaoji: cannot write the guest metadata sidecar\n", stderr);
        return;
    }
    fclose(out);
    char tmp[4096], fin[4096];
    if (snprintf(tmp, sizeof tmp, "%s/%s", persist_root, META_TMP) >= (int)sizeof tmp ||
        snprintf(fin, sizeof fin, "%s/%s", persist_root, META_FILE) >= (int)sizeof fin)
        return;
    if (rename(tmp, fin) != 0)
        fputs("xiaoji: cannot replace the guest metadata sidecar\n", stderr);
}
