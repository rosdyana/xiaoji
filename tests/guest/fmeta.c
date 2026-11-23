/* File metadata (meta.c, B8): the owner, group and mode of files, with the Linux rules for chown, chmod, the owner
 * and group of new objects, setgid directories, umask, the setid bits that chown and write clear, the record that
 * follows a hard link and a rename, and EPERM for a non-owner after setresuid.
 *
 * argv[1] is a scratch directory (absolute). The test makes it, works in it, and removes what it made. It runs as
 * root (run.sh runs the native oracle under sudo). The output has only ids, modes, errno values and relations
 * between files: no inode numbers, pids or times. The engine and the native oracle print the same text. */
#include "sys.h"

#define SYS_UNLINKAT 35
#define SYS_MKDIRAT 34
#define SYS_LINKAT 37
#define SYS_RENAMEAT 38
#define SYS_FCHMODAT 53
#define SYS_FCHOWNAT 54
#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_NEWFSTATAT 79
#define SYS_UMASK 166
#define SYS_SETRESUID 147
#define SYS_SETRESGID 149
#define SYS_CLONE 220
#define SYS_WAIT4 260

#define SIGCHLD_N 17
#define AT_FDCWD (-100)
#define AT_REMOVEDIR 0x200
#define AT_SYMLINK_NOFOLLOW 0x100
#define O_WRONLY 1
#define O_CREAT 0100
#define O_TRUNC 01000
#define EPERM_N 1
#define ENOENT_N 2
#define EEXIST_N 17

#define S_IFMT_N 0170000
#define S_IFDIR_N 0040000
#define S_ISUID_N 04000
#define S_ISGID_N 02000
#define S_IXGRP_N 00010

static char base[400];
static u8 stbuf[128];

static void putoct(u64 v)
{
    char b[24];
    int i = (int)sizeof b;
    b[--i] = 0;
    do {
        b[--i] = (char)('0' + (v & 7));
        v >>= 3;
    } while (v);
    put(b + i);
}

static u32 rd32(int off)
{
    u32 v;
    memcpy(&v, stbuf + off, 4);
    return v;
}

/* newfstatat: the arm64 struct stat, with mode at 16, uid at 24, gid at 28. */
static i64 st(const char *p, int nofollow)
{
    return sc6(SYS_NEWFSTATAT, AT_FDCWD, (i64)p, (i64)stbuf, nofollow ? AT_SYMLINK_NOFOLLOW : 0, 0, 0);
}

/* A path in the scratch directory. Four slots, so a call can name two paths. */
static const char *J(int slot, const char *name)
{
    static char bufs[4][480];
    char *d = bufs[slot];
    size_t n = strlen(base);
    memcpy(d, base, n);
    d[n] = '/';
    size_t m = strlen(name);
    memcpy(d + n + 1, name, m + 1);
    return d;
}

static void line(const char *label, const char *name)
{
    put(label);
    put(": ");
    if (st(name, 0) != 0) {
        put("missing\n");
        return;
    }
    u32 m = rd32(16);
    put((m & S_IFMT_N) == S_IFDIR_N ? "dir" : "file");
    put(" mode ");
    putoct(m & 07777);
    put(" uid ");
    putu(rd32(24));
    put(" gid ");
    putu(rd32(28));
    put("\n");
}

static void num(const char *label, i64 rc)
{
    put(label);
    put(": ");
    putd(rc);
    put("\n");
}

static i64 mkfile(const char *p, u32 mode)
{
    i64 fd = sc6(SYS_OPENAT, AT_FDCWD, (i64)p, O_WRONLY | O_CREAT | O_TRUNC, mode, 0, 0);
    if (fd >= 0)
        sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return fd;
}

static i64 mkdir_(const char *p, u32 mode) { return sc6(SYS_MKDIRAT, AT_FDCWD, (i64)p, mode, 0, 0, 0); }
static i64 chown_(const char *p, u32 uid, u32 gid) { return sc6(SYS_FCHOWNAT, AT_FDCWD, (i64)p, uid, gid, 0, 0); }
static i64 chmod_(const char *p, u32 mode) { return sc6(SYS_FCHMODAT, AT_FDCWD, (i64)p, mode, 0, 0, 0); }
static i64 umask_(u32 m) { return sc6(SYS_UMASK, m, 0, 0, 0, 0, 0); }
static i64 unlink_(const char *p, int dir) { return sc6(SYS_UNLINKAT, AT_FDCWD, (i64)p, dir ? AT_REMOVEDIR : 0, 0, 0, 0); }
static i64 link_(const char *a, const char *b) { return sc6(SYS_LINKAT, AT_FDCWD, (i64)a, AT_FDCWD, (i64)b, 0, 0); }
static i64 rename_(const char *a, const char *b) { return sc6(SYS_RENAMEAT, AT_FDCWD, (i64)a, AT_FDCWD, (i64)b, 0, 0); }
static i64 setres(i64 nr, u32 r, u32 e, u32 s) { return sc6(nr, r, e, s, 0, 0, 0); }

/* Removes every name this test makes (children first). Errors are ignored: the names may not exist. */
static void clean(void)
{
    unlink_(J(0, "d/sub"), 1);
    unlink_(J(0, "d/x"), 0);
    unlink_(J(0, "d/y"), 0);
    unlink_(J(0, "d/u"), 0);
    unlink_(J(0, "d"), 1);
    unlink_(J(0, "f"), 0);
    unlink_(J(0, "u"), 0);
    unlink_(J(0, "g"), 0);
    unlink_(J(0, "h2"), 0);
    unlink_(J(0, "h"), 0);
    unlink_(J(0, "root.txt"), 0);
    unlink_(base, 1);
}

static void scenario_owner_mode(void)
{
    put("-- create, chown, chmod\n");
    mkfile(J(0, "f"), 0640);
    line("create f (0640, umask 022)", J(0, "f"));
    num("chown f 1000 1001", chown_(J(0, "f"), 1000, 1001));
    line("after chown", J(0, "f"));
    num("chmod f 04755", chmod_(J(0, "f"), 04755));
    line("after chmod setuid", J(0, "f"));
    num("chown f -1 2000", chown_(J(0, "f"), 0xffffffffu, 2000));
    line("chown clears setuid", J(0, "f"));
    num("chmod f 02750", chmod_(J(0, "f"), 02750));
    num("chown f -1 3000 (setgid with group exec)", chown_(J(0, "f"), 0xffffffffu, 3000));
    line("chown clears setgid with g+x", J(0, "f"));
    num("chmod f 02640", chmod_(J(0, "f"), 02640));
    num("chown f -1 3001 (setgid without g+x, root)", chown_(J(0, "f"), 0xffffffffu, 3001));
    line("root chown keeps setgid without g+x", J(0, "f"));
}

static void scenario_setgid_dir(void)
{
    put("-- setgid directory\n");
    num("mkdir d 0777", mkdir_(J(0, "d"), 0777));
    line("mkdir d", J(0, "d"));
    num("chown d 0 2000", chown_(J(0, "d"), 0, 2000));
    num("chmod d 02777", chmod_(J(0, "d"), 02777));
    line("setgid d", J(0, "d"));
    mkfile(J(0, "d/x"), 0666);
    line("file in setgid dir", J(0, "d/x"));
    mkfile(J(0, "d/y"), 02775);
    line("setgid file in setgid dir (root)", J(0, "d/y"));
    num("mkdir d/sub 0777", mkdir_(J(0, "d/sub"), 0777));
    line("subdir inherits setgid", J(0, "d/sub"));
}

static void scenario_umask(void)
{
    put("-- umask\n");
    umask_(077);
    mkfile(J(0, "u"), 0666);
    line("create u (0666, umask 077)", J(0, "u"));
    umask_(022);
    line("u after umask restored", J(0, "u"));
}

static void scenario_links(void)
{
    put("-- hard link and rename\n");
    mkfile(J(0, "g"), 0600);
    num("chown g 1000 1000", chown_(J(0, "g"), 1000, 1000));
    num("link g h", link_(J(0, "g"), J(1, "h")));
    line("h (hard link)", J(0, "h"));
    num("chmod h 0644", chmod_(J(0, "h"), 0644));
    line("g after chmod through h", J(0, "g"));
    num("rename h h2", rename_(J(0, "h"), J(1, "h2")));
    line("h2 after rename", J(0, "h2"));
    num("link g h again", link_(J(0, "g"), J(1, "h")));
    unlink_(J(0, "h"), 0);
}

static void child_nonroot(void)
{
    /* Runs as uid 1000 and gid 1001, after the ids are set. Only checks whose result does not depend on
     * directory permissions (the engine does not enforce them, the native root oracle's uid 1000 would be
     * refused on the root directories). */
    num("setresuid 1000", setres(SYS_SETRESUID, 1000, 1000, 1000));
    num("chmod root.txt as 1000 (not the owner)", chmod_(J(0, "root.txt"), 0777));
    num("chown root.txt to 1000 as 1000 (not root)", chown_(J(0, "root.txt"), 1000, 0xffffffffu));
    num("create d/u as 1000", mkfile(J(0, "d/u"), 0644) >= 0 ? 0 : -1);
    line("d/u owner and group", J(0, "d/u"));
    num("chown d/u to group 3333 (not a member)", chown_(J(0, "d/u"), 0xffffffffu, 3333));
    num("chown d/u to group 1001 (its own group)", chown_(J(0, "d/u"), 0xffffffffu, 1001));
    num("chmod d/u 0600 (owner)", chmod_(J(0, "d/u"), 0600));
    line("d/u after chmod", J(0, "d/u"));
}

static void scenario_nonowner(void)
{
    put("-- non-owner after setresuid\n");
    mkfile(J(0, "root.txt"), 0644);
    i64 pid = sc6(SYS_CLONE, SIGCHLD_N, 0, 0, 0, 0, 0);
    if (pid == 0) {
        setres(SYS_SETRESGID, 1001, 1001, 1001);
        child_nonroot();
        sys_exit_group(0);
    }
    int status = 0;
    sc6(SYS_WAIT4, pid, (i64)&status, 0, 0, 0, 0);
    line("root.txt after child", J(0, "root.txt"));
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        put("usage: fmeta DIR\n");
        return 2;
    }
    size_t n = strlen(argv[1]);
    if (n >= sizeof base)
        return 2;
    memcpy(base, argv[1], n + 1);
    umask_(022);
    clean();
    mkdir_(base, 0777);
    chmod_(base, 0777);

    scenario_owner_mode();
    scenario_setgid_dir();
    scenario_umask();
    scenario_links();
    scenario_nonowner();

    clean();
    put("done\n");
    return 0;
}
