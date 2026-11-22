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
#include "binder.h"
#include "fdobj.h"
#include "pipe.h"
#include "poll.h"
#include "procfs.h"
#include "props.h"
#include "selinuxfs.h"
#include "proc.h"
#include "sched.h"

#define MAX_SYMLINKS 40
#define NAME_MAX_LEN 255
#define RESOLVE_BUF (2 * VFS_PATH_MAX)

static char root[VFS_PATH_MAX];  /* host directory for guest "/", no trailing '/' */
/* One process's file state: its fd table, cwd, program name and argv. The host root
 * is shared by all processes and stays global. */
struct fdtab {
    vfs_slot *slots; /* VFS_MAX_FD descriptors */
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
    if (binder_device(p) >= 0) return SYN_BINDER;
    switch (selinuxfs_kind(p)) {
    case SELFS_FILE: return SYN_DATA;
    case SELFS_DIR: return SYN_DIR;
    case SELFS_STATUS: return SYN_SELFS_STATUS;
    case SELFS_ACCESS: return SYN_SELFS_ACCESS;
    default: break;
    }
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
        if (f->kind != VFS_DIR && f->kind != VFS_PATH && !(f->kind == VFS_SYN && f->syn == SYN_DIR))
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
            if (s == SYN_DIR) {
                strcpy(cur, cand);
                if (last) {
                    out->exists = 1;
                    out->syn = SYN_DIR;
                    break;
                }
                continue;
            }
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
            if (!strcmp(cand, "/sys/fs")) { /* the parent of the selinux mount */
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
            /* /proc/<pid>/exe: the program of that (live) process. */
            int epid;
            const char *erest;
            int is_pid_exe = procfs_pid_path(cand, &epid, &erest) && !strcmp(erest, "/exe");
            int is_exe = !strcmp(cand, "/proc/self/exe") || is_pid_exe;
            int is_fd = strncmp(cand, "/proc/self/fd/", 14) == 0 && is_digits(cand + 14, strlen(cand + 14));
            if (is_exe || is_fd) {
                const char *target;
                if (is_pid_exe) {
                    proc_t *ep = proc_find(epid);
                    if (!ep || ep->zombie || ep->exited)
                        return -LE_NOENT;
                    target = vfs_prog_of(ep->fds);
                } else if (is_exe) {
                    target = cur_fdt->prog;
                } else {
                    long n = strtol(cand + 14, NULL, 10);
                    if (n < 0 || n >= VFS_MAX_FD || !cur_fdt->slots[n].desc)
                        return -LE_NOENT;
                    target = cur_fdt->slots[n].desc->guest;
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
        if (rc == -LE_NOENT && !last &&
            (!strcmp(cand, "/proc") || !strcmp(cand, "/sys") || !strcmp(cand, "/sys/fs"))) {
            /* A host without /proc or /sys (macOS): the guest's /proc and /sys are synthetic. */
            strcpy(cur, cand);
            continue;
        }
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

    /* A path that ends in "." (perms/.) names the synthetic directory it was resolved to. */
    if (out->syn == SYN_NONE && !out->is_link && selinuxfs_kind(cur) == SELFS_DIR) {
        out->exists = 1;
        out->syn = SYN_DIR;
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
    if (fd < 0 || fd >= VFS_MAX_FD)
        return NULL;
    return cur_fdt->slots[fd].desc;
}

static int free_fd_from(int from)
{
    for (int i = from; i < VFS_MAX_FD; i++)
        if (!cur_fdt->slots[i].desc)
            return i;
    return -1;
}

/* A new description, with one reference. */
static vfs_fd *desc_new(void)
{
    vfs_fd *d = calloc(1, sizeof *d);
    if (!d)
        die_oom();
    d->refs = 1;
    d->hfd = -1;
    return d;
}

/* Drops one reference to a description; the last one releases its host fd and
 * objects. */
static void desc_unref(vfs_fd *f)
{
    if (!f || --f->refs > 0)
        return;
    if (f->hfd >= 0)
        host_close(f->hfd);
    if (f->kind == VFS_PIPE)
        pipe_close(f->pipe, (f->lflags & LO_ACCMODE) == LO_WRONLY);
    if (f->kind == VFS_SOCK)
        sock_unref(f->sock);
    if (f->kind == VFS_BINDER)
        binder_release(f->binder); /* the last description of a proc: its death */
    if (f->kind == VFS_EVENTFD)
        efd_free(f->obj);
    if (f->kind == VFS_TIMERFD)
        tfd_free(f->obj);
    if (f->kind == VFS_EPOLL)
        poll_epoll_free(f->obj);
    free(f->guest);
    free(f->host);
    free(f->data);
    if (f->dir)
        closedir((DIR *)f->dir);
    memfd_unref(f->mfd);
    free(f);
}

/* Puts d in slot (which must be free) with the given cloexec flag. */
static void slot_install(int slot, vfs_fd *d, int cloexec)
{
    cur_fdt->slots[slot].desc = d;
    cur_fdt->slots[slot].cloexec = cloexec;
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
    vfs_fd *d = desc_new();
    d->kind = VFS_MEMFD;
    d->lflags = LO_RDWR;
    d->mfd = m;
    d->guest = xstrdup(guest);
    slot_install(slot, d, (mflags & 1u) != 0);
    return slot;
}

int vfs_close(int fd)
{
    vfs_fd *f = vfs_get(fd);
    if (!f)
        return -LE_BADF;
    cur_fdt->slots[fd].desc = NULL;
    cur_fdt->slots[fd].cloexec = 0;
    desc_unref(f);
    return 0;
}

static void obj_free(vfs_kind kind, void *obj)
{
    if (kind == VFS_EVENTFD)
        efd_free(obj);
    else if (kind == VFS_TIMERFD)
        tfd_free(obj);
    else if (kind == VFS_EPOLL)
        poll_epoll_free(obj);
}

int vfs_obj_new(vfs_kind kind, void *obj, int cloexec, uint32_t lflags)
{
    int slot = free_fd_from(0);
    if (slot < 0) {
        obj_free(kind, obj);
        return -LE_MFILE;
    }
    vfs_fd *d = desc_new();
    d->kind = kind;
    d->obj = obj;
    d->lflags = lflags;
    d->guest = xstrdup(kind == VFS_EVENTFD ? "anon_inode:[eventfd]"
                       : kind == VFS_TIMERFD ? "anon_inode:[timerfd]" : "anon_inode:[eventpoll]");
    slot_install(slot, d, cloexec != 0);
    return slot;
}

vfs_fd *vfs_desc_hold(int fd)
{
    vfs_fd *f = vfs_get(fd);
    if (f)
        f->refs++;
    return f;
}

int vfs_desc_install(vfs_fd *d, int cloexec)
{
    int slot = free_fd_from(0);
    if (slot < 0) {
        desc_unref(d);
        return -LE_MFILE;
    }
    slot_install(slot, d, cloexec != 0);
    return slot;
}

void vfs_desc_drop(vfs_fd *d)
{
    desc_unref(d);
}

int vfs_pipe2(int out[2], uint32_t flags)
{
    int r = free_fd_from(0);
    if (r < 0)
        return -LE_MFILE;
    int w = -1;
    for (int i = r + 1; i < VFS_MAX_FD; i++)
        if (!cur_fdt->slots[i].desc) {
            w = i;
            break;
        }
    if (w < 0)
        return -LE_MFILE;
    pipe_t *p = pipe_new();
    if (!p)
        return -LE_NOMEM;
    static int serial;
    char guest[48];
    snprintf(guest, sizeof guest, "pipe:[%d]", ++serial);
    int cloexec = (flags & LO_CLOEXEC) != 0;
    uint32_t nb = flags & LO_NONBLOCK;
    vfs_fd *rd = desc_new(), *wr = desc_new();
    rd->kind = wr->kind = VFS_PIPE;
    rd->pipe = wr->pipe = p;
    rd->lflags = LO_RDONLY | nb;
    wr->lflags = LO_WRONLY | nb;
    rd->guest = xstrdup(guest);
    wr->guest = xstrdup(guest);
    pipe_open(p, 0);
    pipe_open(p, 1);
    slot_install(r, rd, cloexec);
    slot_install(w, wr, cloexec);
    out[0] = r;
    out[1] = w;
    return 0;
}

int vfs_cloexec_get(int fd)
{
    if (!vfs_get(fd))
        return -LE_BADF;
    return cur_fdt->slots[fd].cloexec;
}

int vfs_cloexec_set(int fd, int on)
{
    if (!vfs_get(fd))
        return -LE_BADF;
    cur_fdt->slots[fd].cloexec = on != 0;
    return 0;
}

int vfs_pipe_block(int fd)
{
    vfs_fd *f = vfs_get(fd);
    if (!f || (f->lflags & LO_NONBLOCK))
        return 0;
    if (f->kind == VFS_PIPE) {
        sched_block_restart((uint64_t)(uintptr_t)f->pipe);
        return 1;
    }
    if (f->kind == VFS_EVENTFD) {
        sched_block_restart((uint64_t)(uintptr_t)f->obj);
        return 1;
    }
    if (f->kind == VFS_TIMERFD) {
        /* A timer read blocks until its next expiry, so the wait carries that deadline. */
        uint64_t dl = 0;
        int has = tfd_deadline(f->obj, &dl);
        sched_block_restart_until((uint64_t)(uintptr_t)f->obj, has, dl);
        return 1;
    }
    return 0;
}

fdtab_t *vfs_fdtab_new(void)
{
    fdtab_t *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->slots = calloc(VFS_MAX_FD, sizeof *t->slots);
    if (!t->slots) {
        free(t);
        return NULL;
    }
    strcpy(t->cwd, "/");
    strcpy(t->prog, "/");
    return t;
}

/* Makes t the running fd table. */
void vfs_fdtab_switch(fdtab_t *t) { cur_fdt = t; }

fdtab_t *vfs_fdtab_fork(fdtab_t *src)
{
    fdtab_t *t = vfs_fdtab_new();
    if (!t)
        return NULL;
    memcpy(t->cwd, src->cwd, sizeof t->cwd);
    memcpy(t->prog, src->prog, sizeof t->prog);
    t->cmdline_len = src->cmdline_len;
    t->cmdline = malloc(src->cmdline_len ? src->cmdline_len : 1);
    if (!t->cmdline)
        die_oom();
    memcpy(t->cmdline, src->cmdline, src->cmdline_len);
    for (int i = 0; i < VFS_MAX_FD; i++) {
        if (!src->slots[i].desc)
            continue;
        src->slots[i].desc->refs++;
        t->slots[i].desc = src->slots[i].desc;
        t->slots[i].cloexec = src->slots[i].cloexec;
    }
    return t;
}

void vfs_fdtab_release(fdtab_t *t)
{
    if (!t)
        return;
    for (int i = 0; i < VFS_MAX_FD; i++) {
        vfs_fd *d = t->slots[i].desc;
        if (!d)
            continue;
        t->slots[i].desc = NULL;
        desc_unref(d);
    }
    if (cur_fdt == t)
        cur_fdt = NULL;
    free(t->slots);
    free(t->cmdline);
    free(t);
}

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
    vfs_set_argv(argc, argv);

    static const char *std_names[3] = {"/dev/stdin", "/dev/stdout", "/dev/stderr"};
    for (int i = 0; i < 3; i++) {
        vfs_fd *f = desc_new();
        f->kind = VFS_FILE;
        f->hfd = i;
        f->lflags = i == 0 ? LO_RDONLY : LO_WRONLY;
        f->guest = xstrdup(std_names[i]);
        slot_install(i, f, 0);
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

void vfs_set_argv(int argc, char **argv)
{
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
}

void vfs_close_cloexec(void)
{
    for (int fd = 0; fd < VFS_MAX_FD; fd++)
        if (cur_fdt->slots[fd].desc && cur_fdt->slots[fd].cloexec)
            vfs_close(fd);
}

int vfs_open(int dirfd, const char *path, uint32_t lflags, uint32_t mode)
{
    static vfs_res r;
    int slot = free_fd_from(0);
    if (slot < 0)
        return -LE_MFILE;
    /* The property area: the engine's shared memfd, read-only, whatever the dirfd. */
    if (!strcmp(path, PROPS_AREA_PATH)) {
        if ((lflags & LO_ACCMODE) != LO_RDONLY)
            return -LE_ACCES;
        vfs_fd *f = desc_new();
        f->kind = VFS_MEMFD;
        f->mfd = props_memfd();
        memfd_ref(f->mfd);
        f->ro_area = 1;
        f->lflags = lflags & ~(uint32_t)LO_CLOEXEC;
        f->guest = xstrdup(PROPS_AREA_PATH);
        slot_install(slot, f, (lflags & LO_CLOEXEC) != 0);
        return slot;
    }
    int rc = vfs_resolve(dirfd, path, !(lflags & LO_NOFOLLOW), &r);
    if (rc)
        return rc;
    if (r.is_link)
        return -LE_LOOP;
    if ((lflags & LO_CREAT) && (lflags & LO_EXCL) && r.exists)
        return -LE_EXIST;
    if (!r.exists && !(lflags & LO_CREAT))
        return -LE_NOENT;

    if (r.syn == SYN_BINDER) {
        if (lflags & LO_DIRECTORY)
            return -LE_NOTDIR;
        binder_proc_t *bp = binder_open(binder_device(r.guest));
        if (!bp)
            return -LE_NOMEM;
        vfs_fd *f = desc_new();
        f->kind = VFS_BINDER;
        f->binder = bp;
        f->lflags = lflags & ~(uint32_t)LO_CLOEXEC;
        f->guest = xstrdup(r.guest);
        slot_install(slot, f, (lflags & LO_CLOEXEC) != 0);
        return slot;
    }
    if (r.syn == SYN_SELFS_STATUS) {
        /* The selinux status page: a shared, read-only memfd (as the property area). */
        memfd_t *m = selinuxfs_status_memfd();
        if (!m)
            return -LE_NOMEM;
        vfs_fd *f = desc_new();
        f->kind = VFS_MEMFD;
        f->mfd = m;
        memfd_ref(m);
        f->ro_area = 1;
        f->lflags = lflags & ~(uint32_t)LO_CLOEXEC;
        f->guest = xstrdup(r.guest);
        slot_install(slot, f, (lflags & LO_CLOEXEC) != 0);
        return slot;
    }
    if (r.syn != SYN_NONE) {
        if (r.syn == SYN_DIR) {
            if ((lflags & LO_ACCMODE) != LO_RDONLY)
                return -LE_ISDIR;
        } else if (lflags & LO_DIRECTORY) {
            return -LE_NOTDIR;
        }
        /* Generated contents (/proc/self/stat and the like) are made before the slot is
         * taken, so a failure leaves the fd table alone. */
        uint8_t *gen = NULL;
        size_t glen = 0;
        if (r.syn == SYN_DATA && procfs_known(r.guest) && procfs_read(r.guest, &gen, &glen) < 0)
            return -LE_NOMEM;
        if (r.syn == SYN_DATA && selinuxfs_kind(r.guest) == SELFS_FILE &&
            selinuxfs_read(r.guest, &gen, &glen) < 0)
            return -LE_NOMEM;
        vfs_fd *f = desc_new();
        f->kind = VFS_SYN;
        f->syn = r.syn;
        f->lflags = lflags & ~(uint32_t)LO_CLOEXEC;
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
        slot_install(slot, f, (lflags & LO_CLOEXEC) != 0);
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

    vfs_fd *f = desc_new();
    f->kind = kind;
    f->hfd = hfd;
    f->lflags = lflags & ~(uint32_t)LO_CLOEXEC;
    f->guest = xstrdup(r.guest);
    f->host = xstrdup(r.host);
    slot_install(slot, f, (lflags & LO_CLOEXEC) != 0);
    return slot;
}

/* dup and dup3 (exact: the target slot is closed first). The new fd shares the
 * description of oldfd: offset and status flags are shared, as in Linux. */
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
        if (cur_fdt->slots[slot].desc)
            vfs_close(slot);
    } else {
        slot = free_fd_from(minfd < 0 ? 0 : minfd);
        if (slot < 0)
            return -LE_MFILE;
    }
    src->refs++;
    slot_install(slot, src, cloexec);
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
    case SYN_DIR:
        return -LE_ISDIR;
    case SYN_DATA:
    case SYN_SELFS_ACCESS: {
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
    case VFS_PIPE:
        if ((f->lflags & LO_ACCMODE) != LO_RDONLY)
            return -LE_BADF;
        return pipe_read(f->pipe, buf, n);
    case VFS_SOCK: return sock_read(f->sock, buf, n);
    case VFS_BINDER: return -LE_INVAL; /* binder fds have no read: work comes through BINDER_WRITE_READ */
    case VFS_EVENTFD: return efd_read(f->obj, buf, n);
    case VFS_TIMERFD: return tfd_read(f->obj, buf, n);
    case VFS_EPOLL: return -LE_INVAL;
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
    case VFS_PIPE:
    case VFS_SOCK: return -LE_SPIPE;
    case VFS_BINDER: return -LE_INVAL;
    case VFS_EVENTFD:
    case VFS_TIMERFD:
    case VFS_EPOLL: return -LE_SPIPE;
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
        if (f->syn == SYN_DIR)
            return -LE_ISDIR;
        if (f->syn == SYN_SELFS_ACCESS) {
            /* A request: the reply is what the next read returns, from the start. */
            uint8_t *reply;
            size_t rlen;
            if (selinuxfs_access((const char *)buf, n, &reply, &rlen) < 0)
                return -LE_INVAL;
            free(f->data);
            f->data = reply;
            f->dlen = rlen;
            f->dpos = 0;
            return (int64_t)n;
        }
        if (f->syn == SYN_DATA)
            return -LE_BADF;
        return (int64_t)n; /* /dev/null, /dev/zero and /dev/random discard writes */
    case VFS_SOCK: return sock_write(f->sock, buf, n);
    case VFS_BINDER: return -LE_INVAL;
    case VFS_EVENTFD: return efd_write(f->obj, buf, n);
    case VFS_TIMERFD: return -LE_INVAL;
    case VFS_EPOLL: return -LE_INVAL;
    case VFS_MEMFD: {
        if (f->ro_area)
            return -LE_BADF;
        int64_t r = memfd_pwrite(f->mfd, buf, n, f->dpos);
        if (r < 0)
            return -LE_NOMEM;
        f->dpos += (uint64_t)r;
        return r;
    }
    case VFS_PIPE:
        if ((f->lflags & LO_ACCMODE) != LO_WRONLY)
            return -LE_BADF;
        return pipe_write(f->pipe, buf, n);
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
    case VFS_PIPE:
    case VFS_SOCK:
    case VFS_BINDER:
    case VFS_EVENTFD:
    case VFS_TIMERFD:
    case VFS_EPOLL:
        return -LE_SPIPE;
    case VFS_DIR:
        if (whence == 0 && off == 0) { /* SEEK_SET 0 rewinds */
            if (f->dir)
                rewinddir((DIR *)f->dir);
            f->nent = 0;
            return 0;
        }
        return -LE_INVAL;
    case VFS_SYN: {
        if (f->syn == SYN_DIR) {
            if (whence == 0 && off == 0)
                f->dpos = 0; /* rewinddir */
            return 0;
        }
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

/* getdents of a synthetic directory: ".", "..", then its children. dpos is the index of
 * the next entry. */
static int64_t syn_getdents(vfs_fd *f, uint8_t *buf, size_t cap)
{
    size_t w = 0;
    for (;;) {
        const char *name;
        int is_dir = 1;
        if (f->dpos == 0) {
            name = ".";
        } else if (f->dpos == 1) {
            name = "..";
        } else if (!selinuxfs_child(f->guest, (int)(f->dpos - 2), &name, &is_dir)) {
            break;
        }
        size_t nl = strlen(name);
        size_t reclen = (19 + nl + 1 + 7) & ~(size_t)7;
        if (w + reclen > cap) {
            if (w == 0)
                return -LE_INVAL;
            break;
        }
        uint8_t *r = buf + w;
        memset(r, 0, reclen);
        put_u64(r, 0x1100 + f->dpos);
        put_u64(r + 8, f->dpos + 1);
        put_u16(r + 16, (uint16_t)reclen);
        r[18] = (f->dpos < 2 || is_dir) ? 4 : 8; /* DT_DIR or DT_REG */
        memcpy(r + 19, name, nl + 1);
        w += reclen;
        f->dpos++;
    }
    return (int64_t)w;
}

int64_t vfs_getdents(int fd, uint8_t *buf, size_t cap)
{
    vfs_fd *f = vfs_get(fd);
    if (!f)
        return -LE_BADF;
    if (f->kind == VFS_SYN && f->syn == SYN_DIR)
        return syn_getdents(f, buf, cap);
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
    case SYN_DIR: out->mode = LS_IFDIR | 0555; out->ino = 0x1005; break;
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
    if (f->kind == VFS_PIPE) {
        memset(out, 0, sizeof *out);
        out->dev = 0x11;
        out->ino = 0x4000u + (uint64_t)fd;
        out->mode = LS_IFIFO | 0600;
        out->nlink = 1;
        out->blksize = 4096;
        out->uid = out->gid = 0;
        return 0;
    }
    if (f->kind == VFS_SOCK) {
        memset(out, 0, sizeof *out);
        out->dev = 0x10;
        out->ino = 0x4000u + (uint64_t)fd;
        out->mode = 0140777; /* S_IFSOCK */
        out->nlink = 1;
        out->blksize = 4096;
        return 0;
    }
    if (f->kind == VFS_MEMFD) {
        memset(out, 0, sizeof *out);
        out->dev = 0x10;
        out->ino = 0x3000u + (uint64_t)fd;
        out->mode = LS_IFREG | (f->ro_area ? 0444u : 0600u);
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

int vfs_stat_props(host_stat *out)
{
    memset(out, 0, sizeof *out);
    out->dev = 0x10;
    out->ino = 0x3001u;
    out->mode = LS_IFREG | 0444u;
    out->nlink = 1;
    out->size = memfd_size(props_memfd());
    out->blksize = 4096;
    out->blocks = (out->size + 511) / 512;
    return 0;
}

int vfs_socket(int cloexec, int dgram)
{
    int slot = free_fd_from(0);
    if (slot < 0)
        return -LE_MFILE;
    sock_t *s = sock_new(dgram);
    if (!s)
        die_oom();
    vfs_fd *f = desc_new();
    f->kind = VFS_SOCK;
    f->sock = s;
    f->lflags = LO_RDWR;
    slot_install(slot, f, cloexec != 0);
    return slot;
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

const char *vfs_prog_of(const fdtab_t *t) { return t ? t->prog : "/"; }

const uint8_t *vfs_cmdline_bytes(const fdtab_t *t, size_t *len)
{
    static const uint8_t none[1] = {0};
    *len = t ? t->cmdline_len : 0;
    return t && t->cmdline ? t->cmdline : none;
}

int vfs_fchmod(int fd, uint32_t mode)
{
    vfs_fd *f = vfs_get(fd);
    if (!f)
        return -LE_BADF;
    if (f->kind == VFS_PIPE || f->kind == VFS_MEMFD)
        return 0;
    if (f->hfd >= 0)
        return host_fchmod(f->hfd, mode);
    if (f->host)
        return host_chmod(f->host, mode);
    return -LE_PERM;
}
