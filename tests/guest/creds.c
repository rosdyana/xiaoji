/* Credentials (cred.c, B6): the id, group and capability calls that zygote, init and system_server
 * make, and the sets they leave, including the inheritance across fork and exec.
 *
 * The native oracle is the builder VM's root, which has the full sets (run.sh runs it under sudo).
 * Every scenario runs in a forked child that starts as root, so the parent keeps its sets. Run with
 * argv[1] == "probe", the program is the one an exec started: it prints the ids and sets it inherited.
 * Nothing printed depends on a pid. The ids come from /proc/self/status (Uid, Gid, Groups, Cap*). */
#include "sys.h"

#define SYS_OPENAT 56
#define SYS_READLINKAT 78
#define SYS_CLOSE 57
#define SYS_PIPE2 59
#define SYS_READ 63
#define SYS_EXECVE 221
#define SYS_CLONE 220
#define SYS_WAIT4 260
#define SYS_CAPGET 90
#define SYS_CAPSET 91
#define SYS_SETREUID 145
#define SYS_SETUID 146
#define SYS_SETRESUID 147
#define SYS_SETRESGID 149
#define SYS_SETGID 144
#define SYS_SETFSUID 151
#define SYS_SETFSGID 152
#define SYS_GETGROUPS 158
#define SYS_SETGROUPS 159
#define SYS_PRCTL 167
#define SYS_GETPID 172
#define SYS_SOCKETPAIR 199
#define SYS_SETSOCKOPT 208
#define SYS_GETSOCKOPT 209
#define SYS_SENDMSG 211
#define SYS_RECVMSG 212

#define SIGCHLD_N 17
#define AT_FDCWD (-100)
#define CAP_V1 0x19980330u
#define CAP_V3 0x20080522u
#define CAP_LAST 40

#define PR_SET_KEEPCAPS 8
#define PR_GET_KEEPCAPS 7
#define PR_CAPBSET_READ 23
#define PR_CAPBSET_DROP 24
#define PR_CAP_AMBIENT 47
#define PR_CAP_AMBIENT_IS_SET 1
#define PR_CAP_AMBIENT_RAISE 2
#define PR_CAP_AMBIENT_LOWER 3
#define PR_CAP_AMBIENT_CLEAR_ALL 4

#define CAP_KILL 5
#define CAP_NET_BIND_SERVICE 10
#define CAP_SYS_NICE 23

typedef struct {
    u32 version;
    i32 pid;
} cap_hdr_t;

typedef struct {
    u32 eff, perm, inh;
} cap_data_t;

extern char **xiaoji_envp;
static char g_self[512]; /* this program, absolute (a guest process starts in "/") */

static i64 prctl(i64 opt, i64 a2, i64 a3, i64 a4, i64 a5) { return sc6(SYS_PRCTL, opt, a2, a3, a4, a5, 0); }

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int starts(const char *s, const char *pre)
{
    while (*pre) {
        if (*s != *pre)
            return 0;
        s++;
        pre++;
    }
    return 1;
}

static void line_i(const char *name, i64 v)
{
    put(name);
    put(" = ");
    putd(v);
    put("\n");
}

static void line_x(const char *name, u64 v)
{
    put(name);
    put(" = ");
    putx(v);
    put("\n");
}

static u32 lo(u64 v) { return (u32)v; }
static u32 hi(u64 v) { return (u32)(v >> 32); }
static u64 join(u32 l, u32 h) { return (u64)l | (u64)h << 32; }

/* capget(pid) with version 3: the sets of the process. */
static i64 cap_get3(i32 pid, cap_data_t d[2])
{
    cap_hdr_t h = {CAP_V3, pid};
    return sc6(SYS_CAPGET, (i64)&h, (i64)d, 0, 0, 0, 0);
}

static i64 cap_set3(i32 pid, const cap_data_t d[2])
{
    cap_hdr_t h = {CAP_V3, pid};
    return sc6(SYS_CAPSET, (i64)&h, (i64)d, 0, 0, 0, 0);
}

/* The bounding set, by PR_CAPBSET_READ for each capability up to CAP_LAST. */
static u64 bset(void)
{
    u64 m = 0;
    for (int i = 0; i <= CAP_LAST; i++) {
        if (prctl(PR_CAPBSET_READ, i, 0, 0, 0) == 1)
            m |= 1ull << i;
    }
    return m;
}

/* The current sets, by capget (version 3), in the order effective, permitted, inheritable. */
static void sets_of_self(u64 *eff, u64 *perm, u64 *inh)
{
    cap_data_t d[2] = {{0, 0, 0}, {0, 0, 0}};
    cap_get3(0, d);
    *eff = join(d[0].eff, d[1].eff);
    *perm = join(d[0].perm, d[1].perm);
    *inh = join(d[0].inh, d[1].inh);
}

static void set_sets(cap_data_t d[2], u64 eff, u64 perm, u64 inh)
{
    d[0].eff = lo(eff);
    d[1].eff = hi(eff);
    d[0].perm = lo(perm);
    d[1].perm = hi(perm);
    d[0].inh = lo(inh);
    d[1].inh = hi(inh);
}

/* The lines of /proc/self/status that the credential model sets. Groups only when asked: the
 * native root may start with supplementary groups, the engine's root starts with none. */
static void show_status(int groups)
{
    static char buf[4096];
    i64 fd = sc6(SYS_OPENAT, AT_FDCWD, (i64)"/proc/self/status", 0, 0, 0, 0);
    if (fd < 0) {
        line_i("open /proc/self/status", fd);
        return;
    }
    i64 n = 0, got;
    while (n < (i64)sizeof buf - 1 && (got = sc6(SYS_READ, fd, (i64)(buf + n), sizeof buf - 1 - (size_t)n, 0, 0, 0)) > 0)
        n += got;
    sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    buf[n] = 0;
    char *s = buf;
    while (*s) {
        char *e = s;
        while (*e && *e != '\n')
            e++;
        int keep = starts(s, "Uid:") || starts(s, "Gid:") || starts(s, "CapInh:") || starts(s, "CapPrm:") ||
                   starts(s, "CapEff:") || starts(s, "CapBnd:") || starts(s, "CapAmb:") ||
                   (groups && starts(s, "Groups:"));
        if (keep) {
            sys_write(1, s, (size_t)(e - s));
            put("\n");
        }
        if (!*e)
            break;
        s = e + 1;
    }
}

static void print_groups(void)
{
    u32 g[32];
    i64 n = sc6(SYS_GETGROUPS, 0, 0, 0, 0, 0, 0);
    line_i("getgroups count", n);
    if (n > 0 && n <= 32) {
        sc6(SYS_GETGROUPS, n, (i64)g, 0, 0, 0, 0);
        put("getgroups");
        for (i64 i = 0; i < n; i++) {
            put(" ");
            putu(g[i]);
        }
        put("\n");
    }
}

/* The program an exec started: its ids, groups, sets, and keep-caps. */
static int probe(void)
{
    put("probe\n");
    show_status(1);
    line_i("keepcaps", prctl(PR_GET_KEEPCAPS, 0, 0, 0, 0));
    return 0;
}

/* execve of this same program in probe mode, from a child that already has its ids and sets. */
static void exec_probe(void)
{
    char *av[3] = {g_self, "probe", 0};
    sc6(SYS_EXECVE, (i64)g_self, (i64)av, (i64)xiaoji_envp, 0, 0, 0);
    put("exec failed\n");
}

/* zygote's sequence for an app process (com_android_internal_os_Zygote.cpp, specializeAppProcess):
 * keep-caps, the bounding set dropped, setgroups, setresgid, setresuid, then the sets cleared. */
static void sc_zygote(void)
{
    u32 g[3] = {3003, 9997, 50000};
    line_i("keepcaps", prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0));
    for (int i = 0; i <= CAP_LAST; i++)
        prctl(PR_CAPBSET_DROP, i, 0, 0, 0);
    line_x("bset after drop", bset());
    line_i("setgroups", sc6(SYS_SETGROUPS, 3, (i64)g, 0, 0, 0, 0));
    line_i("setresgid", sc6(149, 10000, 10000, 10000, 0, 0, 0));
    line_i("setresuid", sc6(SYS_SETRESUID, 10000, 10000, 10000, 0, 0, 0));
    put("after setresuid (keep-caps retains the sets)\n");
    show_status(1);
    cap_data_t d[2];
    cap_get3(0, d);
    set_sets(d, join(d[0].eff, d[1].eff), join(d[0].perm, d[1].perm), 0);
    line_i("capset inheritable", cap_set3(0, d));
    set_sets(d, 0, 0, 0);
    line_i("capset clear", cap_set3(0, d));
    show_status(1);
    print_groups();
    exec_probe();
}

/* system_server-style: keep-caps across setresuid, a raised ambient set, then exec. */
static void sc_ambient(void)
{
    u64 want = (1ull << CAP_NET_BIND_SERVICE) | (1ull << CAP_SYS_NICE);
    u32 g[2] = {1000, 1003};
    line_i("setgroups", sc6(SYS_SETGROUPS, 2, (i64)g, 0, 0, 0, 0));
    line_i("keepcaps", prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0));
    line_i("setresuid 1000", sc6(SYS_SETRESUID, 1000, 1000, 1000, 0, 0, 0));
    show_status(1);
    cap_data_t d[2];
    set_sets(d, want, want, want);
    line_i("capset shrink, raise inheritable", cap_set3(0, d));
    line_i("ambient raise 10", prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_NET_BIND_SERVICE, 0, 0));
    line_i("ambient raise 23", prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_SYS_NICE, 0, 0));
    line_i("ambient raise 5 (not permitted)", prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_KILL, 0, 0));
    line_i("ambient raise 41 (invalid)", prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, 41, 0, 0));
    line_i("ambient is_set 10", prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, CAP_NET_BIND_SERVICE, 0, 0));
    line_i("ambient is_set 5", prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, CAP_KILL, 0, 0));
    line_i("ambient is_set with extra arg", prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, CAP_NET_BIND_SERVICE, 1, 0));
    line_i("ambient bad op", prctl(PR_CAP_AMBIENT, 9, CAP_NET_BIND_SERVICE, 0, 0));
    line_i("ambient clear_all with extra arg", prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 1, 0, 0));
    line_i("ambient lower 23", prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_LOWER, CAP_SYS_NICE, 0, 0));
    show_status(1);
    exec_probe();
}

/* Without keep-caps, a uid change to non-root clears the permitted and effective sets. Then the
 * non-root checks: groups, gids, setuid to a held id, the capset rules, and setfsuid to a held id. */
static void sc_drop(void)
{
    u32 g[1] = {5};
    line_i("setresuid 1000", sc6(SYS_SETRESUID, 1000, 1000, 1000, 0, 0, 0));
    show_status(0);
    line_i("setgroups (not privileged)", sc6(SYS_SETGROUPS, 1, (i64)g, 0, 0, 0, 0));
    line_i("setresgid 10000 (not privileged)", sc6(SYS_SETRESGID, 10000, 10000, 10000, 0, 0, 0));
    line_i("setgid 10000 (not privileged)", sc6(SYS_SETGID, 10000, 0, 0, 0, 0, 0));
    line_i("setuid 0 (not held)", sc6(SYS_SETUID, 0, 0, 0, 0, 0, 0));
    line_i("setuid 1000 (held)", sc6(SYS_SETUID, 1000, 0, 0, 0, 0, 0));
    cap_data_t d[2];
    set_sets(d, 0, ~0ull, 0);
    line_i("capset permitted above the old set", cap_set3(0, d));
    set_sets(d, 0, 0, 1ull << CAP_KILL);
    line_i("capset inheritable not held", cap_set3(0, d));
    line_i("capbset drop (no CAP_SETPCAP)", prctl(PR_CAPBSET_DROP, CAP_KILL, 0, 0, 0));
    line_i("setfsuid 0 (not held)", sc6(SYS_SETFSUID, 0, 0, 0, 0, 0, 0));
    show_status(0);
}

/* setreuid rules, and the capabilities that follow the effective uid (commoncap.c). */
static void sc_setreuid(void)
{
    line_i("setreuid -1 1000", sc6(SYS_SETREUID, (i64)-1, 1000, 0, 0, 0, 0));
    show_status(0);
    line_i("setreuid -1 0 (back to root)", sc6(SYS_SETREUID, (i64)-1, 0, 0, 0, 0, 0));
    show_status(0);
    line_i("setreuid 1000 -1", sc6(SYS_SETREUID, 1000, (i64)-1, 0, 0, 0, 0));
    show_status(0);
}

/* setfsuid and setfsgid: the fsuid 0 capabilities (CAP_FS_SET) follow fsuid, the old value comes back. */
static void sc_fs(void)
{
    line_i("setfsuid 1000", sc6(SYS_SETFSUID, 1000, 0, 0, 0, 0, 0));
    show_status(0);
    line_i("setfsuid 0", sc6(SYS_SETFSUID, 0, 0, 0, 0, 0, 0));
    show_status(0);
    line_i("setfsgid 1234", sc6(SYS_SETFSGID, 1234, 0, 0, 0, 0, 0));
    show_status(0);
}

/* capset and the bounding set: a dropped capability cannot enter the inheritable set. */
static void sc_capset(void)
{
    line_i("capbset drop 5", prctl(PR_CAPBSET_DROP, CAP_KILL, 0, 0, 0));
    line_x("bset", bset());
    cap_data_t d[2];
    u64 e, p, i;
    sets_of_self(&e, &p, &i);
    set_sets(d, e, p, 1ull << CAP_KILL);
    line_i("capset inheritable 5 (not in the bounding set)", cap_set3(0, d));
    set_sets(d, p, 0, 0);
    line_i("capset effective not a subset of permitted", cap_set3(0, d));
    set_sets(d, e, p, i);
    line_i("capset to the current sets", cap_set3(0, d));
    cap_hdr_t h = {CAP_V3, 1};
    line_i("capset for pid 1", sc6(SYS_CAPSET, (i64)&h, (i64)d, 0, 0, 0, 0));
    show_status(0);
}

/* capset with version 1 (one struct): the high words are zero, so bits 32 to 40 go. */
static void sc_capset_v1(void)
{
    cap_hdr_t h = {CAP_V1, 0};
    cap_data_t d[1] = {{0xffffffffu, 0xffffffffu, 0}};
    line_i("capset v1 (low words)", sc6(SYS_CAPSET, (i64)&h, (i64)d, 0, 0, 0, 0));
    show_status(0);
}

/* The child that the other-pid probe inspects: it drops its sets, says so, and waits to be released. */
static void sc_other_child(i32 ready_w, i32 release_r)
{
    line_i("child setresuid 1000", sc6(SYS_SETRESUID, 1000, 1000, 1000, 0, 0, 0));
    char b = 'r';
    sc6(64, ready_w, (i64)&b, 1, 0, 0, 0);
    sc6(SYS_READ, release_r, (i64)&b, 1, 0, 0, 0);
}

/* capget for another pid, and the header probes (version, pid), in the parent. */
static void header_probes(void)
{
    cap_hdr_t h;
    cap_data_t d[2];
    h.version = CAP_V1;
    h.pid = 0;
    line_i("capget v1, no data", sc6(SYS_CAPGET, (i64)&h, 0, 0, 0, 0, 0));
    line_x("version after the v1 probe", h.version);
    d[0].eff = d[0].perm = d[0].inh = 0xdeadu;
    d[1].eff = d[1].perm = d[1].inh = 0xdeadu;
    h.version = CAP_V1;
    line_i("capget v1, with data", sc6(SYS_CAPGET, (i64)&h, (i64)d, 0, 0, 0, 0));
    line_x("v1 effective (low)", d[0].eff);
    line_x("v1 second struct untouched", d[1].eff);
    h.version = 0x12345678u;
    h.pid = 0;
    line_i("capget unknown version, no data", sc6(SYS_CAPGET, (i64)&h, 0, 0, 0, 0, 0));
    line_x("version after unknown probe", h.version);
    h.version = 0x12345678u;
    line_i("capget unknown version, with data", sc6(SYS_CAPGET, (i64)&h, (i64)d, 0, 0, 0, 0));
    line_x("version after unknown with data", h.version);
    h.version = CAP_V3;
    h.pid = -1;
    line_i("capget negative pid", sc6(SYS_CAPGET, (i64)&h, (i64)d, 0, 0, 0, 0));
    h.pid = 2000000000;
    line_i("capget pid that does not exist", sc6(SYS_CAPGET, (i64)&h, (i64)d, 0, 0, 0, 0));
}

static void other_pid_probe(void)
{
    i32 to_child[2], from_child[2];
    sc6(SYS_PIPE2, (i64)to_child, 0, 0, 0, 0, 0);
    sc6(SYS_PIPE2, (i64)from_child, 0, 0, 0, 0, 0);
    i64 pid = sc6(SYS_CLONE, SIGCHLD_N, 0, 0, 0, 0, 0);
    if (pid == 0) {
        sc_other_child(from_child[1], to_child[0]);
        sys_exit_group(0);
    }
    char b;
    sc6(SYS_READ, from_child[0], (i64)&b, 1, 0, 0, 0);
    cap_data_t d[2] = {{0, 0, 0}, {0, 0, 0}};
    cap_hdr_t h = {CAP_V3, (i32)pid};
    line_i("capget of another process", sc6(SYS_CAPGET, (i64)&h, (i64)d, 0, 0, 0, 0));
    line_x("other eff", join(d[0].eff, d[1].eff));
    line_x("other perm", join(d[0].perm, d[1].perm));
    line_x("other inh", join(d[0].inh, d[1].inh));
    sc6(64, to_child[1], (i64)&b, 1, 0, 0, 0);
    i64 st = 0;
    sc6(SYS_WAIT4, pid, (i64)&st, 0, 0, 0, 0);
    line_i("other exit status", (st >> 8) & 0xff);
}

/* The socket credentials (B6): SO_PEERCRED is the peer's effective ids, SCM_CREDENTIALS is the sender's real
 * ids unless it gives its own (and then only its own: its real, effective or saved uid, its real, effective or
 * saved gid). The process is not root: ruid 1000, euid 1001, suid 1002; rgid 2000, egid 2001, sgid 2002. */
typedef struct {
    u64 name;
    u32 namelen, pad0;
    u64 iov, iovlen, control, controllen;
    i32 flags, pad1;
} msghdr_t;

typedef struct {
    u64 len;
    i32 level, type;
} cmsghdr_t;

typedef struct {
    i32 pid;
    u32 uid, gid;
} ucred_t;

static void send_creds(int fd, u32 uid, u32 gid, int with)
{
    u8 payload[1] = {'x'};
    u8 ctl[32] = {0};
    struct {
        u64 base;
        u64 len;
    } iov = {(u64)(i64)payload, 1};
    msghdr_t m = {0};
    m.iov = (u64)(i64)&iov;
    m.iovlen = 1;
    if (with) {
        cmsghdr_t *h = (cmsghdr_t *)ctl;
        ucred_t *uc = (ucred_t *)(ctl + sizeof *h);
        h->len = sizeof *h + sizeof *uc;
        h->level = 1;
        h->type = 2;
        uc->pid = (i32)sc6(SYS_GETPID, 0, 0, 0, 0, 0, 0);
        uc->uid = uid;
        uc->gid = gid;
        m.control = (u64)(i64)ctl;
        m.controllen = sizeof ctl;
    }
    line_i(with ? "sendmsg with credentials" : "sendmsg without credentials",
           sc6(SYS_SENDMSG, fd, (i64)&m, 0, 0, 0, 0));
}

static void recv_creds(int fd)
{
    u8 buf[16];
    u8 ctl[64] = {0};
    struct {
        u64 base;
        u64 len;
    } iov = {(u64)(i64)buf, sizeof buf};
    msghdr_t m = {0};
    m.iov = (u64)(i64)&iov;
    m.iovlen = 1;
    m.control = (u64)(i64)ctl;
    m.controllen = sizeof ctl;
    i64 n = sc6(SYS_RECVMSG, fd, (i64)&m, 0, 0, 0, 0);
    line_i("recvmsg bytes", n);
    cmsghdr_t *h = (cmsghdr_t *)ctl;
    if (n > 0 && h->len >= sizeof *h + sizeof(ucred_t) && h->level == 1 && h->type == 2) {
        ucred_t *uc = (ucred_t *)(ctl + sizeof *h);
        line_i("received uid", uc->uid);
        line_i("received gid", uc->gid);
    } else {
        put("received no credentials\n");
    }
}

static void sc_scm(void)
{
    u32 one = 1;
    int sv[2] = {-1, -1};
    line_i("setresgid", sc6(SYS_SETRESGID, 2000, 2001, 2002, 0, 0, 0));
    line_i("setresuid", sc6(SYS_SETRESUID, 1000, 1001, 1002, 0, 0, 0));
    line_i("socketpair", sc6(SYS_SOCKETPAIR, 1, 1, 0, (i64)sv, 0, 0));
    line_i("SO_PASSCRED on the receiver", sc6(SYS_SETSOCKOPT, sv[0], 1, 16, (i64)&one, sizeof one, 0));
    ucred_t pc = {0, 0, 0};
    u32 len = sizeof pc;
    line_i("SO_PEERCRED", sc6(SYS_GETSOCKOPT, sv[0], 1, 17, (i64)&pc, (i64)&len, 0));
    line_i("SO_PEERCRED uid (effective)", pc.uid);
    line_i("SO_PEERCRED gid (effective)", pc.gid);
    send_creds(sv[1], 1002, 2002, 1);
    recv_creds(sv[0]);
    send_creds(sv[1], 0, 2002, 1);
    send_creds(sv[1], 1000, 0, 1);
    send_creds(sv[1], 0, 0, 0);
    recv_creds(sv[0]);
}

static void run_child(void (*fn)(void), const char *title)
{
    put(title);
    put("\n");
    i64 pid = sc6(SYS_CLONE, SIGCHLD_N, 0, 0, 0, 0, 0);
    if (pid == 0) {
        fn();
        sys_exit_group(0);
    }
    i64 st = 0;
    sc6(SYS_WAIT4, pid, (i64)&st, 0, 0, 0, 0);
    line_i("exit status", (st >> 8) & 0xff);
}

int main(int argc, char **argv)
{
    i64 n = sc6(SYS_READLINKAT, AT_FDCWD, (i64)"/proc/self/exe", (i64)g_self, sizeof g_self - 1, 0, 0);
    if (n <= 0) {
        put("cannot read /proc/self/exe\n");
        return 1;
    }
    g_self[n] = 0;
    if (argc > 1 && streq(argv[1], "probe"))
        return probe();

    put("start\n");
    show_status(0);
    line_x("bset", bset());
    header_probes();
    run_child(sc_zygote, "zygote sequence");
    run_child(sc_ambient, "keep-caps, ambient and exec");
    run_child(sc_drop, "uid change without keep-caps");
    run_child(sc_setreuid, "setreuid and the effective uid");
    run_child(sc_fs, "setfsuid and setfsgid");
    run_child(sc_capset, "capset and the bounding set");
    run_child(sc_capset_v1, "capset version 1");
    run_child(sc_scm, "socket credentials");
    other_pid_probe();
    put("done\n");
    return 0;
}
