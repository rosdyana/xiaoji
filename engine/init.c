/* The built-in init (B4a). See init.h.
 *
 * It runs as the initial process with no thread of its own: sched.c calls init_tick between
 * slices, and whenever nothing is runnable. Every action (a spawn, a restart, a stop) is taken
 * there, never inside a guest syscall, so no guest call sees a half-made process.
 *
 * Config: a subset of Android's .rc syntax (see docs/boot.md, B4a). Unknown options are
 * ignored, so Android's own service blocks can be read.
 *
 * Classpaths: BOOTCLASSPATH, DEX2OATBOOTCLASSPATH, SYSTEMSERVERCLASSPATH and
 * STANDALONE_SYSTEMSERVER_JARS are read from the classpath fragments (classpaths.proto) in the
 * order of derive_classpath.cpp: the ART fragment, then the system one, then the APEX ones. */
#include "bpf.h"
#include "init.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cgroupfs.h"
#include "cred.h"
#include "host.h"
#include "ids_gen.h"
#include "linux.h"
#include "loader.h"
#include "mntns.h"
#include "proc.h"
#include "props.h"
#include "sched.h"
#include "signals.h"
#include "sock.h"
#include "vfs.h"

#define INIT_MAX_SVC 32
#define INIT_MAX_SOCK 4
#define INIT_MAX_OPT 16
#define INIT_MAX_GENV 128
#define INIT_MAX_BPROP 64
#define INIT_MAX_ARGV 64
#define INIT_CTLQ 32
#define INIT_MAX_JARS 256
#define INIT_MAX_FRAG 64
#define INIT_MAX_IFACE 16 /* stock HAL blocks declare several interfaces (audio, for one) */
#define INIT_IFACE_MAX 128
#define INIT_RESTART_NS 1000000000ull
#define INIT_CRASH_WINDOW_NS 60000000000ull
#define INIT_CRASH_LIMIT 4
#define INIT_SIGKILL 9
#define INIT_SDK_DEFAULT 33
#define INIT_CLASSES 5 /* indexes 1 to 4: the Classpath enum of classpaths.proto */

enum { CP_BOOT = 1, CP_SYSTEMSERVER = 2, CP_DEX2OAT = 3, CP_STANDALONE = 4 };
enum { CTL_START = 1, CTL_STOP = 2, CTL_RESTART = 3 };
enum { ST_STOPPED, ST_RUNNING, ST_RESTARTING };

static int aid_lookup(const char *name, uint32_t *out);
static int cap_lookup(const char *name, uint32_t *out);

typedef struct {
    char name[32];
    int type; /* SOCK_T_* (sock.h) */
    int passcred; /* +passcred on the socket line: SO_PASSCRED */
    uint32_t mode;
} sockdef_t;

typedef struct {
    char name[64];
    char *path;
    char *argv[INIT_MAX_ARGV];
    int argc;
    char *env[INIT_MAX_OPT]; /* setenv K=V */
    int nenv;
    sockdef_t socks[INIT_MAX_SOCK];
    int nsocks;
    /* "interface <kind> <name>": the interfaces the service provides, as "<kind>/<name>" (aidl/x, or a
     * HIDL fully qualified name and its instance). ctl.interface_* acts on the declaring service. */
    char iface[INIT_MAX_IFACE][INIT_IFACE_MAX];
    int niface;
    int oneshot, disabled;
    uint64_t restart_delay_ns;
    /* user, group (the primary gid, then the supplementary groups) and capabilities: applied to the
     * service's process before its program loads (cred_apply_service). */
    uint32_t uid, gid;
    uint32_t supp[CRED_MAX_GROUPS];
    int nsupp;
    int has_caps;
    uint64_t caps;
    /* runtime */
    int state;
    int pid;
    int manual_stop;
    int restart_now;
    uint64_t restart_at;
    uint64_t crash_at[INIT_CRASH_LIMIT];
    int ncrash;
    int sockfd[INIT_MAX_SOCK];
} svc_t;

typedef struct {
    int action;
    char svc[64];
} ctl_req_t;

typedef struct {
    char path[256];
    int cp;
    char min[32];
    char max[32];
} jar_t;

static int active;
static int trace_on;
static int stdio_host;   /* --init-stdio host: services keep the engine's stdio */
static proc_t *self;
static svc_t svcs[INIT_MAX_SVC];
static int nsvc;
static char *genv[INIT_MAX_GENV];
static int ngenv;
static char *bprop_k[INIT_MAX_BPROP], *bprop_v[INIT_MAX_BPROP];
static int nbprop;
/* The on-block actions of the rc, in file order (B11: mkdirs; B14: mount and symlink, as the stock init's builtins).
 * The product rc has more than 16 mkdirs; the old cap dropped the rest without a word. */
#define INIT_MAX_DIR 256
enum { BA_MKDIR, BA_MOUNT, BA_SYMLINK };
typedef struct {
    int kind;
    char *a, *b, *c; /* mkdir: a = path; mount: a = type, b = source, c = target; symlink: a = target, b = path */
    uint64_t flags;  /* mount: MNT_MS_* (mntns.h) */
} boot_act_t;
static boot_act_t boot_act[INIT_MAX_DIR];
static int nboot_act;
static ctl_req_t ctlq[INIT_CTLQ];
static int ctl_n;
static volatile sig_atomic_t stop_flag;
static uint64_t start_ns, timeout_ns;
static int device_sdk = INIT_SDK_DEFAULT;

static void on_sigint(int sig)
{
    (void)sig;
    stop_flag = 1;
}

static char *dupn(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (!d)
        return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static void set_state(svc_t *s, int st)
{
    static const char *names[] = {"stopped", "running", "restarting"};
    s->state = st;
    char key[96];
    snprintf(key, sizeof key, "init.svc.%s", s->name);
    props_set(key, strlen(key), names[st], strlen(names[st]));
}

/* ---------- config ---------- */

/* Splits a line into words. A word in double quotes may hold spaces (the quotes go). */
static int split(char *line, char **tok, int max)
{
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t' || *p == '\r')
            p++;
        if (!*p)
            break;
        if (*p == '"') {
            char *close = strchr(p + 1, '"');
            tok[n++] = p + 1;
            if (!close) {
                p += strlen(p);
                break;
            }
            *close = 0;
            p = close + 1;
            if (*p == ' ' || *p == '\t' || *p == '\r')
                *p++ = 0;
            continue;
        }
        tok[n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r')
            p++;
        if (*p)
            *p++ = 0;
    }
    return n;
}

static int parse_socktype(const char *t)
{
    if (!strcmp(t, "stream"))
        return SOCK_T_STREAM;
    if (!strcmp(t, "dgram"))
        return SOCK_T_DGRAM;
    if (!strcmp(t, "seqpacket"))
        return SOCK_T_SEQPACKET;
    return 0;
}

/* Splits "K=V" (or "K V") into an environment string. */
static char *env_string(const char *k, const char *v)
{
    size_t kl = strlen(k), vl = strlen(v);
    char *s = malloc(kl + vl + 2);
    if (!s)
        return NULL;
    memcpy(s, k, kl);
    s[kl] = '=';
    memcpy(s + kl + 1, v, vl + 1);
    return s;
}

static void genv_set(const char *k, const char *v)
{
    size_t kl = strlen(k);
    for (int i = 0; i < ngenv; i++) {
        if (!strncmp(genv[i], k, kl) && genv[i][kl] == '=') {
            char *s = env_string(k, v);
            if (s) {
                free(genv[i]);
                genv[i] = s;
            }
            return;
        }
    }
    if (ngenv < INIT_MAX_GENV) {
        char *s = env_string(k, v);
        if (s)
            genv[ngenv++] = s;
    }
}

/* The service that provides an interface ("aidl/name" or "<fqname>/<instance>"), or NULL. Looks at
 * the services read so far (parse time) or all of them (after parsing). */
static svc_t *iface_find(const char *full)
{
    for (int i = 0; i < nsvc; i++)
        for (int k = 0; k < svcs[i].niface; k++)
            if (!strcmp(svcs[i].iface[k], full))
                return &svcs[i];
    return NULL;
}

static int parse_config(char *text)
{
    svc_t *cur = NULL;
    int in_block = 0;
    int lineno = 0;
    char *save = text;
    while (save && *save) {
        char *eol = strchr(save, '\n');
        if (eol)
            *eol = 0;
        lineno++;
        int indented = save[0] == ' ' || save[0] == '\t';
        char *tok[INIT_MAX_ARGV];
        int n = split(save, tok, INIT_MAX_ARGV);
        save = eol ? eol + 1 : NULL;
        if (n == 0 || tok[0][0] == '#')
            continue;
        if (!indented) {
            cur = NULL;
            in_block = 0;
            if (!strcmp(tok[0], "service")) {
                if (n < 3 || nsvc == INIT_MAX_SVC) {
                    fprintf(stderr, "xiaoji: init: line %d: bad service\n", lineno);
                    return -1;
                }
                cur = &svcs[nsvc++];
                memset(cur, 0, sizeof *cur);
                snprintf(cur->name, sizeof cur->name, "%s", tok[1]);
                cur->restart_delay_ns = INIT_RESTART_NS;
                cur->path = dupn(tok[2], strlen(tok[2]));
                for (int i = 2; i < n && cur->argc < INIT_MAX_ARGV - 1; i++)
                    cur->argv[cur->argc++] = dupn(tok[i], strlen(tok[i]));
                cur->argv[cur->argc] = NULL;
            } else if (!strcmp(tok[0], "on")) {
                in_block = 1;
            } else if (!strcmp(tok[0], "export") && n >= 3) {
                genv_set(tok[1], tok[2]);
            } else if (!strcmp(tok[0], "property") && n >= 3 && nbprop < INIT_MAX_BPROP) {
                bprop_k[nbprop] = dupn(tok[1], strlen(tok[1]));
                bprop_v[nbprop] = dupn(tok[2], strlen(tok[2]));
                nbprop++;
            } else if (!strcmp(tok[0], "import")) {
                /* Imports are not followed: list the files in the config instead. */
            } else {
                fprintf(stderr, "xiaoji: init: line %d: unknown directive %s (ignored)\n", lineno, tok[0]);
            }
            continue;
        }
        if (in_block) {
            if (!strcmp(tok[0], "export") && n >= 3)
                genv_set(tok[1], tok[2]);
            else if (!strcmp(tok[0], "mkdir") && n >= 2) {
                if (nboot_act < INIT_MAX_DIR) {
                    boot_act_t *ba = &boot_act[nboot_act++];
                    memset(ba, 0, sizeof *ba);
                    ba->kind = BA_MKDIR;
                    ba->a = dupn(tok[1], strlen(tok[1]));
                } else
                    fprintf(stderr, "xiaoji: init: more than %d on-block actions; %s is not made\n", INIT_MAX_DIR, tok[1]);
            }
            else if (!strcmp(tok[0], "mount") && n >= 4) {
                /* mount <type> <source> <target> [flags...] [options]: flags are the stock init's names (the
                 * last word, when it is no flag, is the options string: the engine ignores it, as the tmpfs mode). */
                static const struct { const char *name; uint64_t flag; } mflags[] = {
                    {"noatime", 0}, {"nodiratime", 0}, {"rw", 0}, {"defaults", 0}, {"wait", 0},
                    {"ro", MNT_MS_RDONLY}, {"nosuid", MNT_MS_NOSUID}, {"nodev", MNT_MS_NODEV},
                    {"noexec", MNT_MS_NOEXEC}, {"remount", MNT_MS_REMOUNT}, {"bind", MNT_MS_BIND},
                    {"rec", MNT_MS_REC}, {"unbindable", 1ull << 17}, {"private", 1ull << 18},
                    {"slave", 1ull << 19}, {"shared", 1ull << 20},
                    {0, 0}};
                uint64_t fl = 0;
                for (int k = 4; k < n; k++)
                    for (int m = 0; mflags[m].name; m++)
                        if (!strcmp(tok[k], mflags[m].name)) {
                            fl |= mflags[m].flag;
                            break;
                        }
                if (nboot_act < INIT_MAX_DIR) {
                    boot_act_t *ba = &boot_act[nboot_act++];
                    memset(ba, 0, sizeof *ba);
                    ba->kind = BA_MOUNT;
                    ba->a = dupn(tok[1], strlen(tok[1]));
                    ba->b = dupn(tok[2], strlen(tok[2]));
                    ba->c = dupn(tok[3], strlen(tok[3]));
                    ba->flags = fl;
                } else
                    fprintf(stderr, "xiaoji: init: more than %d on-block actions; mount %s is not made\n", INIT_MAX_DIR, tok[3]);
            }
            else if (!strcmp(tok[0], "symlink") && n >= 3) {
                if (nboot_act < INIT_MAX_DIR) {
                    boot_act_t *ba = &boot_act[nboot_act++];
                    memset(ba, 0, sizeof *ba);
                    ba->kind = BA_SYMLINK;
                    ba->a = dupn(tok[1], strlen(tok[1]));
                    ba->b = dupn(tok[2], strlen(tok[2]));
                } else
                    fprintf(stderr, "xiaoji: init: more than %d on-block actions; symlink %s is not made\n", INIT_MAX_DIR, tok[2]);
            }
            /* restorecon and the other labelling lines are not modelled: no label is kept, so they are not read. */
            else if (!strcmp(tok[0], "setprop") && n >= 3 && nbprop < INIT_MAX_BPROP) {
                bprop_k[nbprop] = dupn(tok[1], strlen(tok[1]));
                bprop_v[nbprop] = dupn(tok[2], strlen(tok[2]));
                nbprop++;
            }
            continue;
        }
        if (!cur)
            continue;
        if (!strcmp(tok[0], "socket") && n >= 4) {
            /* "socket <name> <type>[+passcred] <mode> <user> <group>": +passcred is SO_PASSCRED on the socket
             * (lmkd reads the sender's credentials from every message; system/memory/lmkd/lmkd.rc). */
            char tbuf[32];
            snprintf(tbuf, sizeof tbuf, "%s", tok[2]);
            int pc = 0;
            char *plus = strchr(tbuf, '+');
            if (plus) {
                *plus = 0;
                pc = !strcmp(plus + 1, "passcred");
            }
            int t = pc || !plus ? parse_socktype(tbuf) : 0;
            if (!t || cur->nsocks == INIT_MAX_SOCK || strlen(tok[1]) >= sizeof cur->socks[0].name) {
                fprintf(stderr, "xiaoji: init: line %d: bad socket\n", lineno);
                return -1;
            }
            sockdef_t *d = &cur->socks[cur->nsocks++];
            snprintf(d->name, sizeof d->name, "%s", tok[1]);
            d->type = t;
            d->passcred = pc;
            d->mode = (uint32_t)strtoul(tok[3], NULL, 8);
        } else if (!strcmp(tok[0], "oneshot")) {
            cur->oneshot = 1;
        } else if (!strcmp(tok[0], "disabled")) {
            cur->disabled = 1;
        } else if (!strcmp(tok[0], "setenv") && n >= 3 && cur->nenv < INIT_MAX_OPT) {
            cur->env[cur->nenv++] = env_string(tok[1], tok[2]);
        } else if (!strcmp(tok[0], "interface") && n >= 3) {
            char full[INIT_IFACE_MAX];
            int len = snprintf(full, sizeof full, "%s/%s", tok[1], tok[2]);
            if (cur->niface == INIT_MAX_IFACE || len < 0 || (size_t)len >= sizeof full) {
                fprintf(stderr, "xiaoji: init: line %d: bad interface\n", lineno);
                return -1;
            }
            /* Android's init refuses a second definition of an interface (ServiceParser::ParseInterface). */
            if (iface_find(full)) {
                fprintf(stderr, "xiaoji: init: line %d: interface %s redefined\n", lineno, full);
                return -1;
            }
            snprintf(cur->iface[cur->niface++], INIT_IFACE_MAX, "%s", full);
        } else if (!strcmp(tok[0], "user") && n >= 2) {
            if (aid_lookup(tok[1], &cur->uid) < 0) {
                fprintf(stderr, "xiaoji: init: line %d: unknown user %s\n", lineno, tok[1]);
                return -1;
            }
        } else if (!strcmp(tok[0], "group") && n >= 2) {
            if (aid_lookup(tok[1], &cur->gid) < 0) {
                fprintf(stderr, "xiaoji: init: line %d: unknown group %s\n", lineno, tok[1]);
                return -1;
            }
            cur->nsupp = 0;
            for (int i = 2; i < n; i++) {
                if (cur->nsupp == CRED_MAX_GROUPS || aid_lookup(tok[i], &cur->supp[cur->nsupp]) < 0) {
                    fprintf(stderr, "xiaoji: init: line %d: bad group %s\n", lineno, tok[i]);
                    return -1;
                }
                cur->nsupp++;
            }
        } else if (!strcmp(tok[0], "capabilities")) {
            cur->has_caps = 1;
            cur->caps = 0;
            for (int i = 1; i < n; i++) {
                uint32_t cap = 0;
                if (cap_lookup(tok[i], &cap) < 0 || cap > 63) {
                    fprintf(stderr, "xiaoji: init: line %d: unknown capability %s\n", lineno, tok[i]);
                    return -1;
                }
                cur->caps |= 1ull << cap;
            }
        } else if (!strcmp(tok[0], "restart_delay_ms") && n >= 2) {
            cur->restart_delay_ns = strtoull(tok[1], NULL, 10) * 1000000ull;
        }
        /* Other options (class, user, group, critical, onrestart, ...) are not modelled. */
    }
    return 0;
}

/* ---------- user, group and capabilities (ids_gen.h, from the AOSP sources) ---------- */

/* An Android name from the AID table (init's DecodeUid: the name, or a number). */
static int aid_lookup(const char *name, uint32_t *out)
{
    if (name[0] >= '0' && name[0] <= '9') {
        *out = (uint32_t)strtoul(name, NULL, 10);
        return 0;
    }
    for (size_t i = 0; i < sizeof ids_aid / sizeof ids_aid[0]; i++) {
        if (!strcmp(ids_aid[i].name, name)) {
            *out = ids_aid[i].id;
            return 0;
        }
    }
    return -1;
}

/* A capability name, as init's LookupCap takes it (BLOCK_SUSPEND, ...). */
static int cap_lookup(const char *name, uint32_t *out)
{
    for (size_t i = 0; i < sizeof ids_cap / sizeof ids_cap[0]; i++) {
        if (!strcmp(ids_cap[i].name, name)) {
            *out = ids_cap[i].id;
            return 0;
        }
    }
    return -1;
}

/* ---------- classpaths (derive_classpath.cpp) ---------- */

static int read_file(const char *path, uint8_t **data, size_t *len)
{
    char canon[VFS_PATH_MAX];
    return vfs_load_file(path, data, len, canon, sizeof canon);
}

/* The device SDK level: ro.build.version.sdk in /system/build.prop. */
static void read_sdk(void)
{
    uint8_t *d = NULL;
    size_t n = 0;
    if (read_file("/system/build.prop", &d, &n) < 0)
        return;
    const char *key = "ro.build.version.sdk=";
    size_t kl = strlen(key);
    for (size_t i = 0; i + kl < n; i++) {
        if ((i == 0 || d[i - 1] == '\n') && !memcmp(d + i, key, kl)) {
            device_sdk = atoi((const char *)d + i + kl);
            break;
        }
    }
    free(d);
}

/* A SDK version from a min or max string: a number, or one of the release codenames. */
static int version_of(const char *v)
{
    if (v[0] >= '0' && v[0] <= '9')
        return atoi(v);
    static const struct {
        const char *name;
        int level;
    } codenames[] = {{"S", 31}, {"Sv2", 32}, {"Tiramisu", 33}, {"T", 33},
                     {"UpsideDownCake", 34}, {"U", 34}, {"VanillaIceCream", 35}, {"V", 35}};
    for (size_t i = 0; i < sizeof codenames / sizeof codenames[0]; i++)
        if (!strcmp(v, codenames[i].name))
            return codenames[i].level;
    return -1;
}

static int jar_installable(const jar_t *j)
{
    if (j->min[0]) {
        int v = version_of(j->min);
        if (v < 0 || device_sdk < v)
            return 0;
    }
    if (j->max[0]) {
        int v = version_of(j->max);
        if (v < 0 || device_sdk > v)
            return 0;
    }
    return 1;
}

typedef struct {
    jar_t *jars;
    int n;
} jarlist_t;

static int pb_varint(const uint8_t **p, const uint8_t *end, uint64_t *v)
{
    *v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (*p >= end)
            return 0;
        uint8_t c = *(*p)++;
        *v |= (uint64_t)(c & 0x7f) << shift;
        if (!(c & 0x80))
            return 1;
    }
    return 0;
}

static void copy_str(char *dst, size_t cap, const uint8_t *s, size_t n)
{
    size_t k = n < cap - 1 ? n : cap - 1;
    memcpy(dst, s, k);
    dst[k] = 0;
}

/* One Jar message (path 1, classpath 2, min_sdk_version 3, max_sdk_version 4). */
static int parse_jar(const uint8_t *p, const uint8_t *end, jar_t *j)
{
    memset(j, 0, sizeof *j);
    while (p < end) {
        uint64_t tag, v;
        if (!pb_varint(&p, end, &tag))
            return 0;
        int field = (int)(tag >> 3), wt = (int)(tag & 7);
        if (wt == 0) {
            if (!pb_varint(&p, end, &v))
                return 0;
            if (field == 2)
                j->cp = (int)v;
        } else if (wt == 2) {
            uint64_t n;
            if (!pb_varint(&p, end, &n) || n > (uint64_t)(end - p))
                return 0;
            if (field == 1)
                copy_str(j->path, sizeof j->path, p, (size_t)n);
            else if (field == 3)
                copy_str(j->min, sizeof j->min, p, (size_t)n);
            else if (field == 4)
                copy_str(j->max, sizeof j->max, p, (size_t)n);
            p += n;
        } else if (wt == 1) {
            if (end - p < 8)
                return 0;
            p += 8;
        } else if (wt == 5) {
            if (end - p < 4)
                return 0;
            p += 4;
        } else {
            return 0;
        }
    }
    return 1;
}

/* Adds the jars of one fragment (ExportedClasspathsJars: repeated Jar jars = 1). */
static int read_fragment(const char *path, jarlist_t *out)
{
    uint8_t *d = NULL;
    size_t n = 0;
    if (read_file(path, &d, &n) < 0)
        return -1;
    const uint8_t *p = d, *end = d + n;
    int rc = 0;
    while (p < end) {
        uint64_t tag, len;
        if (!pb_varint(&p, end, &tag) || (tag >> 3) != 1 || (tag & 7) != 2 || !pb_varint(&p, end, &len) ||
            len > (uint64_t)(end - p)) {
            rc = -1;
            break;
        }
        jar_t j;
        if (!parse_jar(p, p + len, &j)) {
            rc = -1;
            break;
        }
        p += len;
        if (!jar_installable(&j))
            continue;
        if (out->n < INIT_MAX_JARS) {
            out->jars[out->n++] = j;
        }
    }
    free(d);
    return rc;
}

/* The APEX fragments (the etc/classpaths/<file> of each /apex directory), in name order. */
static int glob_apex(const char *file, char frags[][256], int *nfrag)
{
    char canon[VFS_PATH_MAX];
    vfs_res r;
    if (vfs_resolve(-100, "/apex", 1, &r) < 0 || !r.exists || r.syn != SYN_NONE)
        return 0;
    DIR *dir = opendir(r.host);
    if (!dir)
        return 0;
    char names[64][64];
    int nn = 0;
    struct dirent *e;
    while ((e = readdir(dir)) && nn < 64) {
        if (e->d_name[0] == '.' || strchr(e->d_name, '@'))
            continue;
        snprintf(names[nn], sizeof names[nn], "%.63s", e->d_name); /* names[] holds 64 bytes: 63 and the NUL */
        nn++;
    }
    closedir(dir);
    for (int i = 1; i < nn; i++) { /* insertion sort: strcmp order */
        char t[64];
        memcpy(t, names[i], sizeof t);
        int k = i - 1;
        while (k >= 0 && strcmp(names[k], t) > 0) {
            memcpy(names[k + 1], names[k], sizeof t);
            k--;
        }
        memcpy(names[k + 1], t, sizeof t);
    }
    for (int i = 0; i < nn; i++) {
        if (snprintf(canon, sizeof canon, "/apex/%s/etc/classpaths/%s", names[i], file) >= (int)sizeof canon)
            continue;
        uint8_t *d = NULL;
        size_t n = 0;
        if (read_file(canon, &d, &n) < 0)
            continue;
        free(d);
        int dup = 0; /* derive_classpath keeps each fragment once */
        for (int k = 0; k < *nfrag; k++)
            if (!strcmp(frags[k], canon))
                dup = 1;
        if (!dup && *nfrag < INIT_MAX_FRAG)
            snprintf(frags[(*nfrag)++], 256, "%.255s", canon); /* a fragment holds 255 bytes and the NUL */
    }
    return 0;
}

/* Appends a fragment that exists (once). */
static void add_fragment(const char *p, char frags[][256], int *nfrag)
{
    uint8_t *d = NULL;
    size_t n = 0;
    if (read_file(p, &d, &n) < 0)
        return;
    free(d);
    for (int k = 0; k < *nfrag; k++)
        if (!strcmp(frags[k], p))
            return;
    if (*nfrag < INIT_MAX_FRAG)
        snprintf(frags[(*nfrag)++], 256, "%s", p);
}

/* The fragments of one list, in derive_classpath's order: for BOOTCLASSPATH the ART one first,
 * then the system one, then the APEX ones. SYSTEMSERVERCLASSPATH has the system one, then the
 * APEX ones. */
static void fragments_for(const char *file, int boot, char frags[][256], int *nfrag)
{
    char p[256];
    *nfrag = 0;
    if (boot) {
        snprintf(p, sizeof p, "/apex/com.android.art/etc/classpaths/%s", file);
        add_fragment(p, frags, nfrag);
    }
    snprintf(p, sizeof p, "/system/etc/classpaths/%s", file);
    add_fragment(p, frags, nfrag);
    glob_apex(file, frags, nfrag);
}

static void derive_classpath(void)
{
    static jar_t jars[INIT_MAX_JARS * 2];
    static char frags[INIT_MAX_FRAG][256];
    static const char *files[] = {"bootclasspath.pb", "systemserverclasspath.pb"};
    jarlist_t all = {jars, 0};
    read_sdk();
    for (int f = 0; f < 2; f++) {
        int nfrag = 0;
        fragments_for(files[f], f == 0, frags, &nfrag);
        for (int i = 0; i < nfrag; i++)
            read_fragment(frags[i], &all);
    }
    static const struct {
        int cp;
        const char *var;
    } vars[] = {{CP_BOOT, "BOOTCLASSPATH"},
                {CP_DEX2OAT, "DEX2OATBOOTCLASSPATH"},
                {CP_SYSTEMSERVER, "SYSTEMSERVERCLASSPATH"},
                {CP_STANDALONE, "STANDALONE_SYSTEMSERVER_JARS"}};
    for (size_t v = 0; v < sizeof vars / sizeof vars[0]; v++) {
        static char val[64 * 1024];
        size_t len = 0;
        val[0] = 0;
        for (int i = 0; i < all.n; i++) {
            if (all.jars[i].cp != vars[v].cp)
                continue;
            size_t pl = strlen(all.jars[i].path);
            if (len + pl + 2 > sizeof val)
                break;
            if (len)
                val[len++] = ':';
            memcpy(val + len, all.jars[i].path, pl + 1);
            len += pl;
        }
        genv_set(vars[v].var, val);
    }
}

/* ---------- sockets and services ---------- */

/* A socket of a service, made in the initial process's table (the child inherits it). The
 * name is an engine node under /dev/socket. */
static int make_socket(const sockdef_t *d, int *fd_out)
{
    int fd = vfs_socket(0, d->type, 0);
    if (fd < 0)
        return fd;
    if (d->passcred)
        sock_set_passcred(vfs_get(fd)->sock, 1);
    char path[128];
    snprintf(path, sizeof path, "/dev/socket/%s", d->name);
    size_t n = strlen(path);
    int rc = sock_fsnode_add(path);
    if (rc == 0) {
        vfs_fd *f = vfs_get(fd);
        rc = sock_bind(f->sock, (const uint8_t *)path, n, 1, (const uint8_t *)path, n);
    }
    if (rc) {
        vfs_close(fd);
        return rc;
    }
    *fd_out = fd;
    return 0;
}

static const char *base_name(const char *p)
{
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

/* A service's stdin, stdout and stderr: /dev/null on each (init's SetupStdio, which opens it
 * O_RDWR and dup2s it onto 0, 1 and 2). Called in the child's table. The descriptors are not
 * close-on-exec, so the service's children inherit them. */
static int svc_stdio_null(void)
{
    int fd = vfs_open(-100, "/dev/null", LO_RDWR, 0);
    if (fd < 0)
        return -1;
    for (int i = 0; i < 3; i++) {
        if (vfs_dup(fd, i, 1, 0) < 0) {
            vfs_close(fd);
            return -1;
        }
    }
    vfs_close(fd);
    return 0;
}

static int svc_start(svc_t *s)
{
    proc_switch(self);
    for (int i = 0; i < s->nsocks; i++) {
        s->sockfd[i] = -1;
        int fd = -1;
        if (make_socket(&s->socks[i], &fd) < 0) {
            fprintf(stderr, "xiaoji: init: %s: cannot create socket %s\n", s->name, s->socks[i].name);
            for (int k = 0; k < i; k++)
                vfs_close(s->sockfd[k]);
            set_state(s, ST_STOPPED);
            return -1;
        }
        s->sockfd[i] = fd;
    }
    uint8_t *img = NULL;
    size_t len = 0;
    char canon[VFS_PATH_MAX];
    if (vfs_load_file(s->path, &img, &len, canon, sizeof canon) < 0) {
        fprintf(stderr, "xiaoji: init: %s: cannot read %s\n", s->name, s->path);
        for (int k = 0; k < s->nsocks; k++)
            vfs_close(s->sockfd[k]);
        set_state(s, ST_STOPPED);
        return -1;
    }
    /* The environment: the global one, the service's, and ANDROID_SOCKET_<name>=<fd>. */
    int nenv = ngenv + s->nenv + s->nsocks;
    char **envp = calloc((size_t)nenv + 1, sizeof *envp);
    char *sockenv[INIT_MAX_SOCK];
    int ne = 0;
    if (!envp) {
        free(img);
        return -1;
    }
    for (int i = 0; i < ngenv; i++)
        envp[ne++] = genv[i];
    for (int i = 0; i < s->nenv; i++)
        envp[ne++] = s->env[i];
    for (int i = 0; i < s->nsocks; i++) {
        char v[16];
        snprintf(v, sizeof v, "%d", s->sockfd[i]);
        char key[64];
        snprintf(key, sizeof key, "ANDROID_SOCKET_%s", s->socks[i].name);
        sockenv[i] = env_string(key, v);
        envp[ne++] = sockenv[i];
    }
    envp[ne] = NULL;

    proc_t *child = sched_proc_new_child();
    if (!child) {
        fprintf(stderr, "xiaoji: init: %s: out of memory\n", s->name);
        free(img);
        free(envp);
        return -1;
    }
    proc_switch(child);
    uint64_t entry = 0, sp = 0;
    int rc = 0;
    if (!stdio_host && svc_stdio_null() < 0) {
        rc = -LE_NOENT; /* reported below, with the same release as a failed load */
    } else if ((rc = cred_apply_service(child, s->uid, s->gid, s->supp, s->nsupp, s->has_caps, s->caps)) < 0) {
        fprintf(stderr, "xiaoji: init: %s: cannot set its ids or capabilities (%d)\n", s->name, rc);
    } else {
        vfs_set_prog(canon);
        vfs_set_argv(s->argc, s->argv);
        rc = loader_load(img, len, s->path, s->argc, s->argv, ne, envp, &entry, &sp);
        if (rc >= 0)
            cred_exec(child); /* the program's execve: the ids and sets it starts with (cred.c) */
    }
    free(img);
    proc_switch(self);
    for (int i = 0; i < s->nsocks; i++) {
        vfs_close(s->sockfd[i]); /* the child holds its own references */
        s->sockfd[i] = -1;
        free(sockenv[i]);
    }
    free(envp);
    if (rc < 0) {
        fprintf(stderr, "xiaoji: init: %s: cannot start (%d)\n", s->name, rc);
        /* The child never ran: release it (its descriptors, and so the socket references). */
        child->exited = 1;
        child->zombie = 1;
        proc_release(child);
        proc_free(child);
        set_state(s, ST_STOPPED);
        return -1;
    }
    if (sched_start_thread(child, entry, sp, base_name(s->path), trace_on) < 0) {
        set_state(s, ST_STOPPED);
        return -1;
    }
    s->pid = child->pid;
    set_state(s, ST_RUNNING);
    fprintf(stderr, "xiaoji: init: started %s (pid %d)\n", s->name, s->pid);
    return 0;
}

static svc_t *svc_find(const char *name)
{
    for (int i = 0; i < nsvc; i++)
        if (!strcmp(svcs[i].name, name))
            return &svcs[i];
    return NULL;
}

/* sig_kill takes the running process as the sender, so init is switched in first. This runs only
 * when there is a service to kill (a switch flushes the translation caches). */
static void svc_kill(svc_t *s)
{
    if (!s->pid)
        return;
    if (proc_cur() != self)
        proc_switch(self);
    sig_kill((uint64_t)s->pid, INIT_SIGKILL);
}

/* A service has ended: reap it, then restart it, or leave it stopped (oneshot, a stop asked
 * for, or a crash loop). */
static void svc_exited(svc_t *s, proc_t *p, uint64_t now)
{
    fprintf(stderr, "xiaoji: init: %s exited (status %d%s)\n", s->name, p->exit_code,
            p->term_sig ? ", by a signal" : "");
    proc_free(p);
    s->pid = 0;
    if (s->restart_now) {
        s->restart_now = 0;
        svc_start(s);
        return;
    }
    if (s->oneshot || s->manual_stop) {
        set_state(s, ST_STOPPED);
        return;
    }
    int kept = 0;
    for (int i = 0; i < s->ncrash; i++)
        if (now - s->crash_at[i] < INIT_CRASH_WINDOW_NS)
            s->crash_at[kept++] = s->crash_at[i];
    s->ncrash = kept;
    if (s->ncrash >= INIT_CRASH_LIMIT) {
        fprintf(stderr, "xiaoji: init: %s is crash-looping; not restarting\n", s->name);
        set_state(s, ST_STOPPED);
        return;
    }
    s->crash_at[s->ncrash++] = now;
    s->restart_at = now + s->restart_delay_ns;
    set_state(s, ST_RESTARTING);
}

static void init_stop_all(void)
{
    for (int i = 0; i < nsvc; i++) {
        svcs[i].manual_stop = 1;
        svc_kill(&svcs[i]);
    }
    /* The socket nodes are names in the engine: they go with init. */
    for (int i = 0; i < nsvc; i++) {
        for (int k = 0; k < svcs[i].nsocks; k++) {
            char path[128];
            snprintf(path, sizeof path, "/dev/socket/%s", svcs[i].socks[k].name);
            sock_fsnode_del(path);
        }
    }
}

/* ---------- the tick (between slices) ---------- */

static void handle_ctl(void)
{
    while (ctl_n > 0) {
        ctl_req_t r = ctlq[0];
        memmove(ctlq, ctlq + 1, (size_t)(ctl_n - 1) * sizeof ctlq[0]);
        ctl_n--;
        svc_t *s = svc_find(r.svc);
        if (!s) {
            fprintf(stderr, "xiaoji: init: no service %s\n", r.svc);
            continue;
        }
        if (r.action == CTL_START) {
            s->manual_stop = 0;
            if (s->pid == 0 && s->state != ST_RUNNING)
                svc_start(s);
        } else if (r.action == CTL_STOP) {
            s->manual_stop = 1;
            if (s->pid)
                svc_kill(s);
            else
                set_state(s, ST_STOPPED);
        } else if (r.action == CTL_RESTART) {
            s->manual_stop = 0;
            if (s->pid) {
                s->restart_now = 1;
                svc_kill(s);
            } else {
                svc_start(s);
            }
        }
    }
}

static void init_tick(void)
{
    uint64_t now = host_monotonic_ns();
    if (stop_flag || (timeout_ns && now - start_ns >= timeout_ns)) {
        init_stop_all();
        self->exit_code = 0;
        self->exited = 1;
        return;
    }
    for (int i = 0; i < nsvc; i++) {
        svc_t *s = &svcs[i];
        if (!s->pid)
            continue;
        proc_t *p = proc_find(s->pid);
        if (!p || p->zombie) {
            if (p)
                svc_exited(s, p, now);
            else
                s->pid = 0;
        }
    }
    handle_ctl();
    for (int i = 0; i < nsvc; i++) {
        svc_t *s = &svcs[i];
        if (s->state == ST_RESTARTING && now >= s->restart_at)
            svc_start(s);
    }
}

/* The host time of init's next timer that can wake a thread (a service restart). The --init-timeout
 * end is not one: it only stops the run, and init_tick checks it on every pass. Reporting it here
 * would make a hang look like a pending wake-up to the deadlock check (sched.c). */
static uint64_t init_deadline(void)
{
    uint64_t dl = 0;
    for (int i = 0; i < nsvc; i++) {
        if (svcs[i].state == ST_RESTARTING && (!dl || svcs[i].restart_at < dl))
            dl = svcs[i].restart_at;
    }
    return dl;
}

/* ---------- entry points ---------- */

int init_active(void)
{
    return active;
}

void init_set_trace(int on)
{
    trace_on = on != 0;
}

void init_set_stdio_host(int on)
{
    stdio_host = on != 0;
}

void init_ctl_request(const char *action, size_t alen, const char *svc, size_t slen)
{
    if (!active)
        return;
    int a = 0, by_iface = 0;
    if (alen == 9 && !memcmp(action, "ctl.start", 9))
        a = CTL_START;
    else if (alen == 8 && !memcmp(action, "ctl.stop", 8))
        a = CTL_STOP;
    else if (alen == 11 && !memcmp(action, "ctl.restart", 11))
        a = CTL_RESTART;
    else if (alen == 19 && !memcmp(action, "ctl.interface_start", 19)) {
        a = CTL_START;
        by_iface = 1;
    } else if (alen == 18 && !memcmp(action, "ctl.interface_stop", 18)) {
        a = CTL_STOP;
        by_iface = 1;
    } else if (alen == 21 && !memcmp(action, "ctl.interface_restart", 21)) {
        a = CTL_RESTART;
        by_iface = 1;
    }
    if (!a) {
        fprintf(stderr, "xiaoji: init: %.*s is not a ctl action\n", (int)alen, action);
        return;
    }
    if (by_iface) {
        /* Android's init: the interface names a service (HandleControlMessage, FindInterface). The
         * queue then holds the service's name, which is short. */
        char iname[INIT_IFACE_MAX];
        if (slen >= sizeof iname) {
            fprintf(stderr, "xiaoji: init: could not find interface (name too long) for %.*s\n", (int)alen, action);
            return;
        }
        memcpy(iname, svc, slen);
        iname[slen] = 0;
        svc_t *s = iface_find(iname);
        if (!s) {
            fprintf(stderr, "xiaoji: init: Control message: Could not find '%s' for %.*s\n", iname, (int)alen, action);
            return;
        }
        svc = s->name;
        slen = strlen(s->name);
    }
    if (ctl_n == INIT_CTLQ || slen >= sizeof ctlq[0].svc) {
        fprintf(stderr, "xiaoji: init: %.*s for %.*s dropped (queue full or name too long)\n", (int)alen, action,
                (int)slen, svc);
        return;
    }
    ctlq[ctl_n].action = a;
    memcpy(ctlq[ctl_n].svc, svc, slen);
    ctlq[ctl_n].svc[slen] = 0;
    ctl_n++;
}

void init_prop_changed(const char *name, size_t nlen, const char *value, size_t vlen)
{
    (void)value;
    (void)vlen;
    if (active && nlen == 11 && !memcmp(name, "xiaoji.exit", 11))
        stop_flag = 1;
}

/* One on-block action (B14). mkdir: as before (B11; the mode and owner are the engine's). mount and symlink: the stock
 * init's do_mount and do_symlink (system/core/init/builtins.cpp), made in the init's own mount namespace, which the
 * services it forks inherit. A failed action is reported and skipped, as the stock init does. */
static void boot_run(const boot_act_t *ba)
{
    vfs_res r;
    if (ba->kind == BA_MKDIR) {
        if (vfs_resolve(-100, ba->a, 1, &r) != 0)
            return;
        if (cgroupfs_under(r.guest)) {
            int crc = cgroupfs_mkdir(r.guest);
            if (crc && crc != -LE_EXIST)
                fprintf(stderr, "xiaoji: init: mkdir %s failed (cgroup error %d)\n", ba->a, -crc);
        } else if (r.syn == SYN_NONE && mkdir(r.host, 0771) < 0 && errno != EEXIST) {
            fprintf(stderr, "xiaoji: init: mkdir %s failed (errno %d)\n", ba->a, errno);
        }
        return;
    }
    if (ba->kind == BA_MOUNT) {
        /* "none" is the stock init's name for no source or no type (mount(2) takes NULL). */
        const char *type = strcmp(ba->a, "none") ? ba->a : NULL;
        const char *src = strcmp(ba->b, "none") ? ba->b : NULL;
        int64_t mrc = mntns_mount(vfs_ns(), src, ba->c, type, ba->flags);
        if (mrc)
            fprintf(stderr, "xiaoji: init: mount %s %s on %s failed (error %d)\n", ba->a, ba->b, ba->c, (int)-mrc);
        return;
    }
    /* BA_SYMLINK: the target is kept as given, and the resolver reads it as a guest path (vfs.c). */
    int rc = vfs_resolve(-100, ba->b, 0, &r);
    if (rc) {
        fprintf(stderr, "xiaoji: init: symlink %s: cannot resolve (error %d)\n", ba->b, -rc);
        return;
    }
    if (r.exists || r.is_link || r.syn != SYN_NONE) {
        fprintf(stderr, "xiaoji: init: symlink %s: exists\n", ba->b);
        return;
    }
    rc = host_symlink(ba->a, r.host);
    if (rc)
        fprintf(stderr, "xiaoji: init: symlink %s -> %s failed (error %d)\n", ba->b, ba->a, -rc);
}

void init_snap_replay(void)
{
    vfs_res r;
    proc_t *was = proc_cur();
    proc_switch(self); /* the rc's paths are in init's own mount namespace */
    for (int i = 0; i < nboot_act; i++) {
        const boot_act_t *ba = &boot_act[i];
        if (ba->kind == BA_MKDIR) {
            if (vfs_resolve(-100, ba->a, 1, &r) == 0 && !r.exists && r.syn == SYN_NONE && !cgroupfs_under(r.guest))
                (void)mkdir(r.host, 0771);
        } else if (ba->kind == BA_SYMLINK) {
            if (vfs_resolve(-100, ba->b, 0, &r) == 0 && !r.exists && !r.is_link && r.syn == SYN_NONE)
                (void)host_symlink(ba->a, r.host);
        }
    }
    proc_switch(was);
}

int init_resume(uint64_t timeout_s)
{
    /* A restored image (snap.c): the services, the control queue and the init process are the image's. The timeout
     * counts from the restore. */
    start_ns = host_monotonic_ns();
    timeout_ns = timeout_s * 1000000000ull;
    stop_flag = 0;
    signal(SIGINT, on_sigint);
    int rc = sched_resume();
    init_stop_all();
    active = 0;
    return rc;
}

int init_main(const char *config, uint64_t timeout_s)
{
    uint8_t *buf = NULL;
    size_t len = 0;
    if (host_read_file(config, &buf, &len) < 0) {
        fprintf(stderr, "xiaoji: init: cannot read %s\n", config);
        return 2;
    }
    char *text = malloc(len + 1);
    if (!text) {
        free(buf);
        return 2;
    }
    memcpy(text, buf, len);
    text[len] = 0;
    free(buf);
    if (parse_config(text) < 0) {
        free(text);
        return 2;
    }
    free(text);

    self = proc_cur();
    active = 1;
    start_ns = host_monotonic_ns();
    timeout_ns = timeout_s * 1000000000ull;
    signal(SIGINT, on_sigint);

    /* The cgroups of the system (B11): the mounts of /system/etc/cgroups.json and /dev/cgroup_info/cgroup.rc. Before the
     * mkdirs, which may make subgroups of them. */
    cgroupfs_init();
    bpffs_init(); /* the bpffs at /sys/fs/bpf (B15) */
    bpf_preload(); /* bpfloader's maps, created and pinned before any service starts (B15) */
    /* The on-block actions, in file order (the mkdirs, mounts and symlinks of the rc; see boot_act_t). A failed action is
     * reported and the next one runs, as the stock init does for its builtins. */
    for (int i = 0; i < nboot_act; i++)
        boot_run(&boot_act[i]);
    for (int i = 0; i < nbprop; i++)
        props_set(bprop_k[i], strlen(bprop_k[i]), bprop_v[i], strlen(bprop_v[i]));
    derive_classpath();

    for (int i = 0; i < nsvc; i++) {
        svc_t *s = &svcs[i];
        s->state = ST_STOPPED;
        if (!s->disabled)
            svc_start(s);
    }
    int rc = sched_run_init(init_tick, init_deadline);
    init_stop_all();
    active = 0;
    return rc;
}
