/* Guest filesystem. Paths are resolved here, component by component, against the
 * host directory tree below the guest root. Absolute symlink targets restart at
 * the guest root, so Android's /system/bin/linker64 -> /apex/... resolves inside
 * the root and a guest path never reaches the host directly. */
/* XSI (not just POSIX 2008) for telldir and seekdir, which getdents64 resumes with. */
#define _XOPEN_SOURCE 700

#include "vfs.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "linux.h"
#include "procfs.h"

#define MAX_SYMLINKS 40
#define NAME_MAX_LEN 255
#define RESOLVE_BUF (2 * VFS_PATH_MAX)

static char root[VFS_PATH_MAX];  /* host directory for guest "/", no trailing '/' */
/* One process's file state: its fd table, cwd, program name and argv. The host root
 * is shared by all processes and stays global. */
struct fdtab {
    vfs_fd *fds; /* VFS_MAX_FD slots */
    char cwd[VFS_PATH_MAX];
    char prog[VFS_PATH_MAX];
    uint8_t *cmdline;
    size_t cmdline_len;
};

static fdtab_t *cur_fdt;

static void die_oom(void)
{
    fputs("xiaoji: out of host memory\n", stderr);
    exit(1);
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (!d)
        die_oom();
    memcpy(d, s, n);
    return d;
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* Host path for a canonical guest path. */
static int host_path_of(const char *guest, char *out, size_t cap)
{
    size_t rl = strlen(root), gl = strlen(guest);
    if (gl == 1 && guest[0] == '/') {
        if (rl == 0) {
            if (cap < 2)
                return -LE_NAMETOOLONG;
            strcpy(out, "/");
        } else {
            if (rl + 1 > cap)
                return -LE_NAMETOOLONG;
            memcpy(out, root, rl + 1);
        }
        return 0;
    }
    if (rl + gl + 1 > cap)
        return -LE_NAMETOOLONG;
    memcpy(out, root, rl);
    memcpy(out + rl, guest, gl + 1);
    return 0;
}

/* Replaces rest[pos..] by target followed by "/" and the rest of the path, so the
 * resolver continues from the link's target. */
static int splice_link(char *rest, size_t pos, const char *target)
{
    static char tmp[RESOLVE_BUF];
    size_t tl = strlen(target), rem = strlen(rest + pos);
    if (tl + 1 + rem >= sizeof tmp)
        return -LE_NAMETOOLONG;
    memcpy(tmp, target, tl);
    size_t n = tl;
    if (rem) {
        tmp[n++] = '/';
        memcpy(tmp + n, rest + pos, rem);
        n += rem;
    }
    tmp[n] = 0;
    memcpy(rest, tmp, n + 1);
    return 0;
}

static int in_virtual(const char *p)
{
    return strncmp(p, "/dev/", 5) == 0 || strncmp(p, "/proc/", 6) == 0 || strncmp(p, "/sys/", 5) == 0;
}

static int is_digits(const char *p, size_t n)
{
    if (!n)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (p[i] < '0' || p[i] > '9')
            return 0;
    return 1;
}

static vfs_syn syn_of(const char *p)
{
    if (!strcmp(p, "/dev/null")) return SYN_NULL;
    if (!strcmp(p, "/dev/zero")) return SYN_ZERO;
    if (!strcmp(p, "/dev/urandom") || !strcmp(p, "/dev/random")) return SYN_RANDOM;
    if (!strcmp(p, "/proc/self/cmdline")) return SYN_DATA;
    if (procfs_known(p)) return SYN_DATA; /* generated at open, see vfs_open */
    return SYN_NONE;
}

int vfs_resolve(int dirfd, const char *path, int follow, vfs_res *out)
{
    /* Static buffers keep the stack small (the wasm build has a small stack). */
    static char cur[VFS_PATH_MAX], rest[RESOLVE_BUF], cand[VFS_PATH_MAX], hp[VFS_PATH_MAX];
    static char tgt[VFS_PATH_MAX], name[NAME_MAX_LEN + 1];

    size_t plen = strlen(path);
    if (plen == 0)
        return -LE_NOENT;
    if (plen >= VFS_PATH_MAX)
        return -LE_NAMETOOLONG;
    int trailing = plen > 1 && path[plen - 1] == '/';
    if (trailing)
        follow = 1;

    /* Cleared to 0 only when the final component is absent (creatable). */
    out->exists = 1;
    out->is_link = 0;
    out->syn = SYN_NONE;

    if (path[0] == '/') {
        strcpy(cur, "/");
    } else if (dirfd == LAT_FDCWD) {
        strcpy(cur, cur_fdt->cwd);
    } else {
        vfs_fd *f = vfs_get(dirfd);
        if (!f)
            return -LE_BADF;
        if (f->kind != VFS_DIR && f->kind != VFS_PATH)
            return -LE_NOTDIR;
        host_stat st;
        if (f->host) {
            int rc = host_stat_path(f->host, 0, &st);
            if (rc)
                return rc;
            if ((st.mode & LS_IFMT) != LS_IFDIR)
                return -LE_NOTDIR;
        }
        strcpy(cur, f->guest);
    }
    if (strlen(cur) + plen + 2 > RESOLVE_BUF)
        return -LE_NAMETOOLONG;
    strcpy(rest, path);

    size_t pos = 0;
    int links = 0;
    for (;;) {
        while (rest[pos] == '/')
            pos++;
        if (!rest[pos])
            break;
        size_t b = pos, e = b;
        while (rest[e] && rest[e] != '/')
            e++;
        size_t nl = e - b;
        if (nl > NAME_MAX_LEN)
            return -LE_NAMETOOLONG;
        memcpy(name, rest + b, nl);
        name[nl] = 0;
        size_t a = e;
        while (rest[a] == '/')
            a++;
        int last = rest[a] == 0;
        pos = a;

        if (nl == 1 && name[0] == '.')
            continue;
        if (nl == 2 && name[0] == '.' && name[1] == '.') {
            if (strcmp(cur, "/") != 0) {
                char *sl = strrchr(cur, '/');
                if (sl == cur)
                    cur[1] = 0;
                else
                    *sl = 0;
            }
            continue;
        }

        size_t cl = strlen(cur);
        if (cl + 1 + nl >= VFS_PATH_MAX)
            return -LE_NAMETOOLONG;
        if (cl == 1 && cur[0] == '/')
            snprintf(cand, sizeof cand, "/%s", name);
        else
            snprintf(cand, sizeof cand, "%s/%s", cur, name);

        if (in_virtual(cand)) {
            /* Synthetic namespace: /dev, /proc and /sys are never read from the host. */
            vfs_syn s = syn_of(cand);
            if (s != SYN_NONE) {
                if (!last)
                    return -LE_NOTDIR;
                out->exists = 1;
                out->syn = s;
                strcpy(cur, cand);
                break;
            }
            if (procfs_dir(cand)) {
                if (last)
                    return -LE_NOENT;
                strcpy(cur, cand);
                continue;
            }
            if (!strcmp(cand, "/proc/self") || !strcmp(cand, "/proc/self/fd")) {
                if (last)
                    return -LE_NOENT;
                strcpy(cur, cand);
                continue;
            }
            int is_exe = !strcmp(cand, "/proc/self/exe");
            int is_fd = strncmp(cand, "/proc/self/fd/", 14) == 0 && is_digits(cand + 14, strlen(cand + 14));
            if (is_exe || is_fd) {
                const char *target;
                if (is_exe) {
                    target = cur_fdt->prog;
                } else {
                    long n = strtol(cand + 14, NULL, 10);
                    if (n < 0 || n >= VFS_MAX_FD || !cur_fdt->fds[n].used)
                        return -LE_NOENT;
                    target = cur_fdt->fds[n].guest;
                }
                if (last && !follow) {
                    out->is_link = 1;
                    strcpy(out->target, target);
                    strcpy(cur, cand);
                    break;
                }
                if (++links > MAX_SYMLINKS)
                    return -LE_LOOP;
                if (splice_link(rest, pos, target) < 0)
                    return -LE_NAMETOOLONG;
                pos = 0;
                strcpy(cur, "/");
                continue;
            }
            return -LE_NOENT;
        }

        if (host_path_of(cand, hp, sizeof hp) < 0)
            return -LE_NAMETOOLONG;
        host_stat st;
        int rc = host_stat_path(hp, 0, &st);
        if (rc == -LE_NOENT) {
            if (!last)
                return -LE_NOENT;
            strcpy(cur, cand);
            out->exists = 0; /* a creatable final component */
            break;
        }
        if (rc)
            return rc;

        if ((st.mode & LS_IFMT) == LS_IFLNK) {
            if (last && !follow) {
                int64_t n = host_readlink(hp, out->target, VFS_PATH_MAX - 1);
                if (n < 0)
                    return (int)n;
                out->target[n] = 0;
                out->is_link = 1;
                out->exists = 1;
                strcpy(cur, cand);
                break;
            }
            int64_t n = host_readlink(hp, tgt, VFS_PATH_MAX - 1);
            if (n < 0)
                return (int)n;
            tgt[n] = 0;
            if (++links > MAX_SYMLINKS)
                return -LE_LOOP;
            if (splice_link(rest, pos, tgt) < 0)
                return -LE_NAMETOOLONG;
            pos = 0;
            if (tgt[0] == '/')
                strcpy(cur, "/");
            continue;
        }
        if ((!last || trailing) && (st.mode & LS_IFMT) != LS_IFDIR)
            return -LE_NOTDIR;
        strcpy(cur, cand);
        out->exists = 1;
    }

    /* Resolved: cur holds the canonical guest path. */
    strcpy(out->guest, cur);
    if (out->syn == SYN_NONE) {
        if (host_path_of(cur, out->host, sizeof out->host) < 0)
            return -LE_NAMETOOLONG;
    } else {
        out->host[0] = 0;
    }
    if (!out->is_link)
        out->target[0] = 0;
    return 0;
}

/* ---------- fd table ---------- */

vfs_fd *vfs_get(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FD || !cur_fdt->fds[fd].used)
        return NULL;
    return &cur_fdt->fds[fd];
}

static int free_fd_from(int from)
{
    for (int i = from; i < VFS_MAX_FD; i++)
        if (!cur_fdt->fds[i].used)
            return i;
    return -1;
}

static void fd_reset(vfs_fd *f)
{
    free(f->guest);
    free(f->host);
    free(f->data);
    if (f->dir)
        closedir((DIR *)f->dir);
    memfd_unref(f->mfd);
    memset(f, 0, sizeof *f);
    f->hfd = -1;
}

int vfs_memfd_create(const char *name, uint32_t mflags)
{
    int slot = free_fd_from(0);
    if (slot < 0)
        return -LE_MFILE;
    memfd_t *m = memfd_new(name);
    if (!m)
        return -LE_NOMEM;
    char guest[128];
    snprintf(guest, sizeof guest, "/memfd:%s (deleted)", name ? name : "");
    vfs_fd *d = &cur_fdt->fds[slot];
    memset(d, 0, sizeof *d);
    d->used = 1;
    d->cloexec = (mflags & 1u) != 0;
    d->kind = VFS_MEMFD;
    d->hfd = -1;
    d->lflags = LO_RDWR;
    d->mfd = m;
    d->guest = xstrdup(guest);
    return slot;
}

int vfs_close(int fd)
{
    vfs_fd *f = vfs_get(fd);
    if (!f)
        return -LE_BADF;
    if (f->hfd >= 0)
        host_close(f->hfd);
    fd_reset(f);
    return 0;
}

fdtab_t *vfs_fdtab_new(void)
{
    fdtab_t *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->fds = calloc(VFS_MAX_FD, sizeof *t->fds);
    if (!t->fds) {
        free(t);
        return NULL;
    }
    strcpy(t->cwd, "/");
    strcpy(t->prog, "/");
    return t;
}

/* Makes t the running fd table. */
void vfs_fdtab_switch(fdtab_t *t) { cur_fdt = t; }

int vfs_init(const char *host_root, const char *prog_guest, int argc, char **argv)
{
    root[0] = 0;
    if (host_root) {
        size_t n = strlen(host_root);
        if (n >= sizeof root)
            return -LE_NAMETOOLONG;
        memcpy(root, host_root, n + 1);
        while (n > 0 && root[n - 1] == '/')
            root[--n] = 0;
    }
    strcpy(cur_fdt->cwd, "/");
    if (strlen(prog_guest) >= sizeof cur_fdt->prog)
        return -LE_NAMETOOLONG;
    strcpy(cur_fdt->prog, prog_guest);

    size_t total = 0;
    for (int i = 0; i < argc; i++)
        total += strlen(argv[i]) + 1;
    free(cur_fdt->cmdline);
    cur_fdt->cmdline = malloc(total ? total : 1);
    if (!cur_fdt->cmdline)
        die_oom();
    size_t o = 0;
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]) + 1;
        memcpy(cur_fdt->cmdline + o, argv[i], n);
        o += n;
    }
    cur_fdt->cmdline_len = total;

    static const char *std_names[3] = {"/dev/stdin", "/dev/stdout", "/dev/stderr"};
    for (int i = 0; i < 3; i++) {
        vfs_fd *f = &cur_fdt->fds[i];
        memset(f, 0, sizeof *f);
        f->used = 1;
        f->kind = VFS_FILE;
        f->hfd = i;
        f->lflags = i == 0 ? LO_RDONLY : LO_WRONLY;
        f->guest = xstrdup(std_names[i]);
    }
    return 0;
}

void vfs_set_prog(const char *guest)
{
    size_t n = strlen(guest);
    if (n >= sizeof cur_fdt->prog)
        n = sizeof cur_fdt->prog - 1;
    memcpy(cur_fdt->prog, guest, n);
    cur_fdt->prog[n] = 0;
}

int vfs_open(int dirfd, const char *path, uint32_t lflags, uint32_t mode)
{
    static vfs_res r;
    int slot = free_fd_from(0);
    if (slot < 0)
        return -LE_MFILE;
    int rc = vfs_resolve(dirfd, path, !(lflags & LO_NOFOLLOW), &r);
    if (rc)
        return rc;
    if (r.is_link)
        return -LE_LOOP;
    if ((lflags & LO_CREAT) && (lflags & LO_EXCL) && r.exists)
        return -LE_EXIST;
    if (!r.exists && !(lflags & LO_CREAT))
        return -LE_NOENT;

    vfs_fd *f = &cur_fdt->fds[slot];
    if (r.syn != SYN_NONE) {
        if (lflags & LO_DIRECTORY)
            return -LE_NOTDIR;
        /* Generated contents (/proc/self/stat and the like) are made before the slot is
         * taken, so a failure leaves the fd table alone. */
        uint8_t *gen = NULL;
        size_t glen = 0;
        if (r.syn == SYN_DATA && procfs_known(r.guest) && procfs_read(r.guest, &gen, &glen) < 0)
            return -LE_NOMEM;
        memset(f, 0, sizeof *f);
        f->used = 1;
        f->kind = VFS_SYN;
        f->syn = r.syn;
        f->hfd = -1;
        f->lflags = lflags & ~(uint32_t)LO_CLOEXEC;
        f->cloexec = (lflags & LO_CLOEXEC) != 0;
        f->guest = xstrdup(r.guest);
        if (gen) {
            f->data = gen;
            f->dlen = glen;
        } else if (r.syn == SYN_DATA) {
            f->data = malloc(cur_fdt->cmdline_len ? cur_fdt->cmdline_len : 1);
            if (!f->data)
                die_oom();
            memcpy(f->data, cur_fdt->cmdline, cur_fdt->cmdline_len);
            f->dlen = cur_fdt->cmdline_len;
        }
        return slot;
    }

    host_stat st;
    int isdir = 0;
    if (r.exists) {
        rc = host_stat_path(r.host, 0, &st);
        if (rc)
            return rc;
        isdir = (st.mode & LS_IFMT) == LS_IFDIR;
    }
    if (isdir && !(lflags & LO_PATH) && (lflags & LO_ACCMODE) != LO_RDONLY)
        return -LE_ISDIR;
    if ((lflags & LO_DIRECTORY) && r.exists && !isdir)
        return -LE_NOTDIR;

    vfs_kind kind;
    int hfd;
    if (lflags & LO_PATH) {
        /* No host O_PATH: keep a readable host fd if the path allows one, else a
         * path-only fd (hfd -1). */
        kind = VFS_PATH;
        hfd = r.exists ? host_open(r.host, LO_RDONLY, 0) : -1;
        if (hfd < 0)
            hfd = -1;
    } else if (isdir) {
        kind = VFS_DIR;
        hfd = host_open(r.host, LO_RDONLY, 0);
    } else {
        kind = VFS_FILE;
        hfd = host_open(r.host, lflags, mode);
    }
    if (hfd < 0 && kind != VFS_PATH)
        return hfd;

    memset(f, 0, sizeof *f);
    f->used = 1;
    f->kind = kind;
    f->hfd = hfd;
    f->lflags = lflags & ~(uint32_t)LO_CLOEXEC;
    f->cloexec = (lflags & LO_CLOEXEC) != 0;
    f->guest = xstrdup(r.guest);
    f->host = xstrdup(r.host);
    return slot;
}

int vfs_dup(int oldfd, int minfd, int exact, int cloexec)
{
    vfs_fd *src = vfs_get(oldfd);
    if (!src)
        return -LE_BADF;
    int slot;
    if (exact) {
        if (minfd < 0 || minfd >= VFS_MAX_FD)
            return -LE_BADF;
        slot = minfd;
        if (cur_fdt->fds[slot].used)
            vfs_close(slot);
    } else {
        slot = free_fd_from(minfd < 0 ? 0 : minfd);
        if (slot < 0)
            return -LE_MFILE;
    }
    int hfd = -1;
    if (src->hfd >= 0) {
        hfd = host_dup(src->hfd);
        if (hfd < 0)
            return hfd;
    }
    vfs_fd *d = &cur_fdt->fds[slot];
    memset(d, 0, sizeof *d);
    d->used = 1;
    d->cloexec = cloexec;
    d->kind = src->kind;
    d->syn = src->syn;
    d->hfd = hfd;
    d->lflags = src->lflags;
    d->guest = xstrdup(src->guest);
    d->host = src->host ? xstrdup(src->host) : NULL;
    if (src->kind == VFS_MEMFD) {
        d->mfd = src->mfd;
        memfd_ref(d->mfd);
        d->dpos = src->dpos;
    }
    if (src->data) {
        d->data = malloc(src->dlen ? src->dlen : 1);
        if (!d->data)
            die_oom();
        memcpy(d->data, src->data, src->dlen);
        d->dlen = src->dlen;
        d->dpos = src->dpos;
    }
    return slot;
}

/* ---------- reads and writes ---------- */

static int64_t syn_read(vfs_fd *f, void *buf, size_t n, uint64_t off, int use_off)
{
    switch (f->syn) {
    case SYN_NULL:
        return 0;
    case SYN_ZERO:
        memset(buf, 0, n);
        return (int64_t)n;
    case SYN_RANDOM:
        host_random(buf, n);
        return (int64_t)n;
    case SYN_DATA: {
        uint64_t p = use_off ? off : f->dpos;
        if (p >= f->dlen)
            return 0;
        size_t avail = (size_t)(f->dlen - p);
        size_t k = n < avail ? n : avail;
        memcpy(buf, f->data + p, k);
        if (!use_off)
            f->dpos += k;
        return (int64_t)k;
    }
    default:
        return -LE_BADF;
    }
}

int64_t vfs_read(int fd, void *buf, size_t n)
{
    vfs_fd *f = vfs_get(fd);
    if (!f)
        return -LE_BADF;
    switch (f->kind) {
    case VFS_PATH: return -LE_BADF;
    case VFS_DIR: return -LE_ISDIR;
    case VFS_SYN: return syn_read(f, buf, n, 0, 0);
    case VFS_MEMFD: {
        int64_t r = memfd_pread(f->mfd, buf, n, f->dpos);
        if (r > 0)
            f->dpos += (uint64_t)r;
        return r;
    }
    default: break;
    }
    if ((f->lflags & LO_ACCMODE) == LO_WRONLY)
        return -LE_BADF;
    return host_read(f->hfd, buf, n);
}

int64_t vfs_pread(int fd, void *buf, size_t n, uint64_t off)
{
    vfs_fd *f = vfs_get(fd);
    if (!f)
        return -LE_BADF;
    switch (f->kind) {
    case VFS_PATH: return -LE_BADF;
    case VFS_DIR: return -LE_ISDIR;
    case VFS_SYN: return syn_read(f, buf, n, off, 1);
    case VFS_MEMFD: return memfd_pread(f->mfd, buf, n, off);
    default: break;
    }
    if ((f->lflags & LO_ACCMODE) == LO_WRONLY)
        return -LE_BADF;
    return host_pread(f->hfd, buf, n, off);
}

int64_t vfs_write(int fd, const void *buf, size_t n)
{
    vfs_fd *f = vfs_get(fd);
    if (!f)
        return -LE_BADF;
    switch (f->kind) {
    case VFS_PATH: return -LE_BADF;
    case VFS_DIR: return -LE_ISDIR;
    case VFS_SYN:
        if (f->syn == SYN_DATA)
            return -LE_BADF;
        return (int64_t)n; /* /dev/null, /dev/zero and /dev/random discard writes */
    case VFS_MEMFD: {
        int64_t r = memfd_pwrite(f->mfd, buf, n, f->dpos);
        if (r < 0)
            return -LE_NOMEM;
        f->dpos += (uint64_t)r;
        return r;
    }
    default: break;
    }
    if ((f->lflags & LO_ACCMODE) == LO_RDONLY)
        return -LE_BADF;
    return host_write(f->hfd, buf, n);
}

int64_t vfs_lseek(int fd, int64_t off, int whence)
{
    vfs_fd *f = vfs_get(fd);
    if (!f)
        return -LE_BADF;
    switch (f->kind) {
    case VFS_PATH:
        return -LE_BADF;
    case VFS_DIR:
        if (whence == 0 && off == 0) { /* SEEK_SET 0 rewinds */
            if (f->dir)
                rewinddir((DIR *)f->dir);
            f->nent = 0;
            return 0;
        }
        return -LE_INVAL;
    case VFS_SYN: {
        if (f->syn != SYN_DATA)
            return 0;
        int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->dpos : (int64_t)f->dlen;
        if (base + off < 0)
            return -LE_INVAL;
        f->dpos = (uint64_t)(base + off);
        return (int64_t)f->dpos;
    }
    case VFS_MEMFD: {
        int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)f->dpos
                     : whence == 2 ? (int64_t)memfd_size(f->mfd) : -1;
        if (base < 0 || base + off < 0)
            return -LE_INVAL;
        f->dpos = (uint64_t)(base + off);
        return (int64_t)f->dpos;
    }
    default:
        return host_lseek(f->hfd, off, whence);
    }
}

/* ---------- directories ---------- */

static uint8_t child_dtype(const char *dir_host, const char *name)
{
    static char child[VFS_PATH_MAX];
    size_t dl = strlen(dir_host), nl = strlen(name);
    if (dl + nl + 2 > sizeof child)
        return LDT_UNKNOWN;
    if (dl && dir_host[dl - 1] == '/')
        snprintf(child, sizeof child, "%s%s", dir_host, name);
    else
        snprintf(child, sizeof child, "%s/%s", dir_host, name);
    host_stat st;
    if (host_stat_path(child, 0, &st) != 0)
        return LDT_UNKNOWN;
    switch (st.mode & LS_IFMT) {
    case LS_IFREG: return LDT_REG;
    case LS_IFDIR: return LDT_DIR;
    case LS_IFLNK: return LDT_LNK;
    case LS_IFCHR: return LDT_CHR;
    case LS_IFBLK: return LDT_BLK;
    case LS_IFIFO: return LDT_FIFO;
    case LS_IFSOCK: return LDT_SOCK;
    default: return LDT_UNKNOWN;
    }
}

int64_t vfs_getdents(int fd, uint8_t *buf, size_t cap)
{
    vfs_fd *f = vfs_get(fd);
    if (!f)
        return -LE_BADF;
    if (f->kind == VFS_PATH)
        return -LE_BADF;
    if (f->kind != VFS_DIR)
        return -LE_NOTDIR;
    if (!f->dir) {
        f->dir = opendir(f->host);
        if (!f->dir)
            return -host_linux_errno(errno);
    }
    size_t w = 0;
    for (;;) {
        long pos = telldir((DIR *)f->dir);
        errno = 0;
        struct dirent *de = readdir((DIR *)f->dir);
        if (!de) {
            if (errno)
                return -host_linux_errno(errno);
            break;
        }
        size_t nl = strlen(de->d_name);
        size_t reclen = (19 + nl + 1 + 7) & ~(size_t)7;
        if (w + reclen > cap) {
            seekdir((DIR *)f->dir, pos); /* leave this entry for the next call */
            if (w == 0)
                return -LE_INVAL;
            break;
        }
        uint8_t *r = buf + w;
        memset(r, 0, reclen);
        put_u64(r, (uint64_t)de->d_ino);
        put_u64(r + 8, f->nent + 1);
        put_u16(r + 16, (uint16_t)reclen);
        r[18] = child_dtype(f->host, de->d_name);
        memcpy(r + 19, de->d_name, nl + 1);
        w += reclen;
        f->nent++;
    }
    return (int64_t)w;
}

/* ---------- cwd ---------- */

int vfs_getcwd(char *buf, size_t cap)
{
    size_t n = strlen(cur_fdt->cwd) + 1;
    if (n > cap)
        return -LE_RANGE;
    memcpy(buf, cur_fdt->cwd, n);
    return (int)n;
}

int vfs_chdir(const char *path)
{
    static vfs_res r;
    int rc = vfs_resolve(LAT_FDCWD, path, 1, &r);
    if (rc)
        return rc;
    if (!r.exists)
        return -LE_NOENT;
    if (r.syn != SYN_NONE)
        return -LE_NOTDIR;
    host_stat st;
    rc = host_stat_path(r.host, 0, &st);
    if (rc)
        return rc;
    if ((st.mode & LS_IFMT) != LS_IFDIR)
        return -LE_NOTDIR;
    strcpy(cur_fdt->cwd, r.guest);
    return 0;
}

int vfs_fchdir(int fd)
{
    vfs_fd *f = vfs_get(fd);
    if (!f)
        return -LE_BADF;
    if (f->kind != VFS_DIR && f->kind != VFS_PATH)
        return -LE_NOTDIR;
    host_stat st;
    int rc = host_stat_path(f->host, 0, &st);
    if (rc)
        return rc;
    if ((st.mode & LS_IFMT) != LS_IFDIR)
        return -LE_NOTDIR;
    strcpy(cur_fdt->cwd, f->guest);
    return 0;
}

/* ---------- metadata ---------- */

static void synthetic_stat(vfs_syn s, uint64_t size, host_stat *out)
{
    memset(out, 0, sizeof *out);
    out->blksize = 4096;
    out->nlink = 1;
    out->blocks = 0;
    out->size = size;
    out->atime_s = out->mtime_s = out->ctime_s = 0;
    switch (s) {
    case SYN_NULL: out->mode = LS_IFCHR | 0666; out->rdev = 0x103; out->ino = 0x1001; break;
    case SYN_ZERO: out->mode = LS_IFCHR | 0666; out->rdev = 0x105; out->ino = 0x1002; break;
    case SYN_RANDOM: out->mode = LS_IFCHR | 0666; out->rdev = 0x109; out->ino = 0x1003; break;
    default: out->mode = LS_IFREG | 0444; out->ino = 0x1004; break;
    }
}

int vfs_stat_res(const vfs_res *r, host_stat *out)
{
    int rc;
    if (r->syn != SYN_NONE) {
        synthetic_stat(r->syn, 0, out);
        rc = 0;
    } else {
        rc = host_stat_path(r->host, 0, out);
    }
    if (rc)
        return rc;
    out->uid = 0; /* the root-run oracle reports uid and gid 0 */
    out->gid = 0;
    return 0;
}

int vfs_stat_fd(int fd, host_stat *out)
{
    vfs_fd *f = vfs_get(fd);
    if (!f)
        return -LE_BADF;
    int rc;
    if (f->kind == VFS_MEMFD) {
        memset(out, 0, sizeof *out);
        out->dev = 0x10;
        out->ino = 0x3000u + (uint64_t)fd;
        out->mode = LS_IFREG | 0600;
        out->nlink = 1;
        out->size = memfd_size(f->mfd);
        out->blksize = 4096;
        out->blocks = (out->size + 511) / 512;
        return 0;
    }
    if (f->kind == VFS_SYN) {
        synthetic_stat(f->syn, f->syn == SYN_DATA ? f->dlen : 0, out);
        rc = 0;
    } else if (f->hfd >= 0) {
        rc = host_stat_fd(f->hfd, out);
    } else {
        rc = host_stat_path(f->host, 0, out);
    }
    if (rc)
        return rc;
    out->uid = 0;
    out->gid = 0;
    return 0;
}

int vfs_statfs_fd(int fd, host_statfs *out)
{
    vfs_fd *f = vfs_get(fd);
    if (!f)
        return -LE_BADF;
    if (f->kind == VFS_SYN) {
        memset(out, 0, sizeof *out);
        out->bsize = out->frsize = 4096;
        return 0;
    }
    if (f->host)
        return host_statfs_path(f->host, out);
    return host_statfs_path(root[0] ? root : "/", out);
}

int vfs_load_file(const char *path, uint8_t **data, size_t *len, char *canon, size_t cap)
{
    static vfs_res r;
    int rc = vfs_resolve(LAT_FDCWD, path, 1, &r);
    if (rc)
        return rc;
    if (!r.exists || r.syn != SYN_NONE)
        return -LE_NOENT;
    if (strlen(r.guest) >= cap)
        return -LE_NAMETOOLONG;
    if (host_read_file(r.host, data, len) != 0)
        return -LE_IO;
    strcpy(canon, r.guest);
    return 0;
}
