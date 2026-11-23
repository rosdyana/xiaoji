/* execve and execveat (B1 step 4). The path, argv and envp are copied out of guest memory
 * first. The file is resolved through the VFS, checked (a regular file with an exec bit)
 * and read, and any #! interpreter chain is followed. Only then is a new address space
 * built and loaded. The old one is dropped after the new one is ready, so a failed exec
 * leaves the caller as it was. */
#include "exec.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cred.h"
#include "host.h"
#include "linux.h"
#include "loader.h"
#include "mem.h"
#include "proc.h"
#include "sched.h"
#include "signals.h"
#include "vfs.h"

#define ARG_MAX_TOTAL (2u << 20) /* argv and envp, strings and pointers together */
#define ARG_MAX_STR (128u << 10) /* one string */
#define BINPRM_BUF 256           /* a #! line, with its "#!", ends within this many bytes */
#define SCRIPT_MAX_DEPTH 4       /* #! levels, as Linux's BINPRM_MAX_RECURSION */
#define PTR_SIZE 8u

/* A vector of NUL-terminated host strings (owned). */
typedef struct {
    char **v;
    int n;
} strvec_t;

static void free_vec(strvec_t *s)
{
    for (int i = 0; i < s->n; i++)
        free(s->v[i]);
    free(s->v);
    s->v = NULL;
    s->n = 0;
}

/* Appends str (owned by s on success; the caller keeps it on failure). */
static int push_str(strvec_t *s, char *str, int *cap)
{
    if (s->n == *cap) {
        int nc = *cap ? *cap * 2 : 8;
        char **nv = realloc(s->v, (size_t)nc * sizeof *nv);
        if (!nv)
            return -LE_NOMEM;
        s->v = nv;
        *cap = nc;
    }
    s->v[s->n++] = str;
    return 0;
}

static char *dup_str(const char *src)
{
    char *d = malloc(strlen(src) + 1);
    if (d)
        strcpy(d, src);
    return d;
}

/* Appends a host copy of src to s. */
static int push_dup(strvec_t *s, const char *src, int *cap)
{
    char *d = dup_str(src);
    if (!d)
        return -LE_NOMEM;
    int rc = push_str(s, d, cap);
    if (rc)
        free(d);
    return rc;
}

/* Copies the string at guest address va to a new host string. */
static int copy_str(uint64_t va, char **out)
{
    int64_t n = mem_strlen(va);
    if (n < 0)
        return -LE_FAULT;
    if ((uint64_t)n >= ARG_MAX_STR)
        return -LE_2BIG;
    char *s = malloc((size_t)n + 1);
    if (!s)
        return -LE_NOMEM;
    if (n && mem_copy_out(s, va, (size_t)n) < 0) {
        free(s);
        return -LE_FAULT;
    }
    s[n] = 0;
    *out = s;
    return 0;
}

/* Copies the NULL-terminated pointer array at va into s (0 means empty). total counts
 * the bytes of pointers and strings; past ARG_MAX_TOTAL the result is E2BIG. */
static int copy_vec(uint64_t va, strvec_t *s, size_t *total)
{
    int cap = 0, rc = 0;
    s->v = NULL;
    s->n = 0;
    for (uint64_t i = 0; va; i++) {
        uint64_t p;
        if (mem_copy_out(&p, va + i * PTR_SIZE, sizeof p) < 0) {
            rc = -LE_FAULT;
            break;
        }
        if (!p)
            break;
        *total += PTR_SIZE;
        char *str;
        rc = copy_str(p, &str);
        if (rc)
            break;
        *total += strlen(str) + 1;
        if (*total > ARG_MAX_TOTAL) {
            free(str);
            rc = -LE_2BIG;
            break;
        }
        rc = push_str(s, str, &cap);
        if (rc) {
            free(str);
            break;
        }
    }
    if (rc)
        free_vec(s);
    return rc;
}

/* A program is a regular file with at least one exec bit (the guest runs as root, so
 * the bits are not checked against an owner). */
static int check_program(const vfs_res *r)
{
    host_stat st;
    int rc = vfs_stat_res(r, &st);
    if (rc < 0)
        return rc;
    if ((st.mode & LS_IFMT) != LS_IFREG || !(st.mode & 0111))
        return -LE_ACCES;
    return 0;
}

/* Resolves path (relative to dirfd) to a program in r. A final symlink is followed
 * unless follow is 0; then it is not followed, and Linux reports ELOOP for it. */
static int resolve_program(int dirfd, const char *path, int follow, vfs_res *r)
{
    int rc = vfs_resolve(dirfd, path, follow, r);
    if (rc)
        return rc;
    if (r->is_link)
        return -LE_LOOP;
    if (!r->exists)
        return -LE_NOENT;
    if (r->syn != SYN_NONE)
        return -LE_ACCES;
    return check_program(r);
}

/* The #! line of img: the interpreter, and at most one argument (the rest of the line,
 * trimmed). Both are new host strings; arg is NULL when there is none. */
static int parse_shebang(const uint8_t *img, size_t len, char **interp, char **arg)
{
    size_t lim = len < BINPRM_BUF ? len : BINPRM_BUF, eol = 0;
    while (eol < lim && img[eol] != '\n')
        eol++;
    /* No end of line: a file shorter than the buffer ends the line (Linux runs
     * "#!/bin/echo hi" with no newline); a longer one without a newline is ENOEXEC. */
    if (eol == lim && len >= BINPRM_BUF)
        return -LE_NOEXEC;
    size_t i = 2;
    while (i < eol && (img[i] == ' ' || img[i] == '\t'))
        i++;
    size_t s = i;
    while (i < eol && img[i] != ' ' && img[i] != '\t' && img[i] != '\r')
        i++;
    if (i == s)
        return -LE_NOEXEC; /* "#!" with no interpreter */
    char *in = malloc(i - s + 1);
    if (!in)
        return -LE_NOMEM;
    memcpy(in, img + s, i - s);
    in[i - s] = 0;
    while (i < eol && (img[i] == ' ' || img[i] == '\t'))
        i++;
    size_t e = eol;
    while (e > i && (img[e - 1] == ' ' || img[e - 1] == '\t' || img[e - 1] == '\r'))
        e--;
    char *a = NULL;
    if (e > i) {
        a = malloc(e - i + 1);
        if (!a) {
            free(in);
            return -LE_NOMEM;
        }
        memcpy(a, img + i, e - i);
        a[e - i] = 0;
    }
    *interp = in;
    *arg = a;
    return 0;
}

/* The argv of a #! program: interpreter, its argument (if any), the script's name as
 * it was passed, then argv[1..] of the old argv. */
static int shebang_argv(const strvec_t *av, const char *interp, const char *arg, const char *script,
                        strvec_t *out)
{
    int cap = 0, rc = push_dup(out, interp, &cap);
    if (!rc && arg)
        rc = push_dup(out, arg, &cap);
    if (!rc)
        rc = push_dup(out, script, &cap);
    for (int i = 1; !rc && i < av->n; i++)
        rc = push_dup(out, av->v[i], &cap);
    if (rc)
        free_vec(out);
    return rc;
}

/* Loads the program named name (relative to dir) and follows its #! chain. On success
 * *img_out and *len_out are its bytes, r is its resolution (r->guest is what
 * /proc/self/exe reports), and av is rewritten to the argv the final program gets. */
static int load_program(const char *name, int dir, int follow, strvec_t *av, vfs_res *r, uint8_t **img_out,
                        size_t *len_out)
{
    uint8_t *img = NULL;
    size_t len = 0;
    char *cur = dup_str(name);
    if (!cur)
        return -LE_NOMEM;
    int rc = 0;
    for (int depth = 0;; depth++) {
        rc = resolve_program(dir, cur, follow, r);
        if (rc)
            break;
        if (host_read_file(r->host, &img, &len) != 0) {
            rc = -LE_IO;
            break;
        }
        if (len < 2 || img[0] != '#' || img[1] != '!')
            break;
        if (depth == SCRIPT_MAX_DEPTH) {
            rc = -LE_LOOP;
            break;
        }
        char *interp = NULL, *arg = NULL;
        rc = parse_shebang(img, len, &interp, &arg);
        if (rc)
            break;
        strvec_t nav = {NULL, 0};
        rc = shebang_argv(av, interp, arg, cur, &nav);
        if (!rc) {
            free_vec(av);
            *av = nav;
            free(cur);
            cur = interp;
            interp = NULL;
            dir = LAT_FDCWD;
            follow = 1;
            free(img);
            img = NULL;
        }
        free(interp);
        free(arg);
        if (rc)
            break;
    }
    free(cur);
    if (rc) {
        free(img);
        return rc;
    }
    *img_out = img;
    *len_out = len;
    return 0;
}

int64_t exec_run(cpu_t *c, int dirfd, uint64_t pathva, uint64_t argvva, uint64_t envpva, uint64_t flags)
{
    if (flags & ~(uint64_t)(LAT_EMPTY_PATH | LAT_SYMLINK_NOFOLLOW))
        return -LE_INVAL;
    char *path = NULL, *name = NULL;
    strvec_t av = {NULL, 0}, ev = {NULL, 0};
    vfs_res *r = NULL;
    uint8_t *img = NULL;
    size_t len = 0;
    size_t total = 0;
    int rc = copy_str(pathva, &path);
    if (rc)
        return rc;
    if (strlen(path) >= VFS_PATH_MAX) {
        rc = -LE_NAMETOOLONG;
        goto out;
    }
    /* An empty path names the file that dirfd refers to (execveat with AT_EMPTY_PATH). */
    const char *lookup = path;
    int lookup_dir = dirfd;
    if (path[0] == 0) {
        vfs_fd *f = NULL;
        if (!(flags & LAT_EMPTY_PATH) || dirfd == LAT_FDCWD)
            rc = -LE_NOENT;
        else if (!(f = vfs_get(dirfd)))
            rc = -LE_BADF;
        else {
            lookup = f->guest; /* the descriptor's canonical guest path */
            lookup_dir = LAT_FDCWD;
        }
        if (rc)
            goto out;
    }
    int follow = !(flags & LAT_SYMLINK_NOFOLLOW);
    /* The program name is kept here: a descriptor it may come from can close at commit. */
    name = dup_str(lookup);
    r = calloc(1, sizeof *r);
    if (!name || !r) {
        rc = -LE_NOMEM;
        goto out;
    }
    if ((rc = copy_vec(argvva, &av, &total)) != 0)
        goto out;
    if ((rc = copy_vec(envpva, &ev, &total)) != 0)
        goto out;
    if ((rc = load_program(name, lookup_dir, follow, &av, r, &img, &len)) != 0)
        goto out;

    /* Build the new image in a new address space. The process switches to it so that the
     * loader maps into it; the old one is kept until the new one has loaded. */
    proc_t *p = proc_cur();
    mm_t *old = p->mm;
    mm_t *nm = mem_mm_new();
    if (!nm) {
        rc = -LE_NOMEM;
        goto out;
    }
    uint64_t old_brk_base = p->brk_base, old_brk_cur = p->brk_cur;
    mem_mm_switch(nm);
    uint64_t entry = 0, sp = 0;
    rc = loader_load(img, len, r->guest, av.n, av.v, ev.n, ev.v, &entry, &sp);
    if (rc < 0) {
        /* The caller keeps its image: switch back, then drop the new space. */
        mem_mm_switch(old);
        mem_mm_unref(nm);
        p->brk_base = old_brk_base;
        p->brk_cur = old_brk_cur;
        goto out;
    }

    /* Commit. The old space is dropped (a vfork parent keeps its shared one). */
    uint64_t rss = mem_mm_rss_kib(old);
    if (rss > p->maxrss_kib)
        p->maxrss_kib = rss;
    p->mm = nm;
    mem_mm_unref(old);
    vfs_close_cloexec();
    cred_exec(p); /* the ids and capability sets of the new program (cred.c) */
    sig_exec_reset(c);
    sched_exec_threads(c); /* the other threads of the process are gone */
    vfs_set_prog(r->guest);
    vfs_set_argv(av.n, av.v);
    sched_set_name(strrchr(name, '/') ? strrchr(name, '/') + 1 : name);
    if (p->vfork) {
        /* The child has its own address space now, so the suspended parent goes on. */
        p->vfork = 0;
        sched_wake_key((uint64_t)(uintptr_t)p);
    }

    /* The thread starts the new program with fresh registers. Its signal mask, pending
     * signals and tid stay as they are. */
    memset(c->x, 0, sizeof c->x);
    memset(c->v, 0, sizeof c->v);
    c->sp = sp;
    c->pc = entry;
    c->nzcv = 0;
    c->fpcr = 0;
    c->fpsr = 0;
    c->tpidr_el0 = 0;
    c->excl_valid = 0;
    c->excl_addr = 0;
    c->blocked = 0;
    c->io_done = 0;
    c->sig_restore = 0;
    c->sig_suspended = 0;
    c->sig_saved = 0;
    c->sig_wait_set = 0;
    c->sig_wait_info = 0;
    rc = 0;

out:
    free(img);
    free_vec(&av);
    free_vec(&ev);
    free(r);
    free(name);
    free(path);
    return rc;
}
