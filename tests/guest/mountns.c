/* mountns (B4c): mount namespaces, in one program. Run natively as root (the oracle runs it under
 * sudo, in its own mount namespace, with MS_PRIVATE on /) and in the engine. It prints normalised
 * results only: return codes, file contents, and the mountinfo lines under the test's base directory
 * (the mount point, read-only or read-write, and whether the fs type is tmpfs or a bind).
 *
 * Steps: a private namespace; a bind mount and its read-only remount (EROFS on create, mkdir and
 * unlink; reads still work); a bind's write reaching the source; umount2 (and EINVAL for a path that
 * is not a mount point, ENOENT after the unmount); a tmpfs, stacked with a second tmpfs on the same
 * path (the top hides the lower one, and umount2 brings it back); ".." out of a tmpfs into the
 * source directory; a child with CLONE_NEWNS whose tmpfs is invisible to the parent.
 * Options: argv[1] is the base directory, relative to the working directory (default
 * build/tests/mountns-work; the runner starts from the repository root). Its absolute path is read with
 * getcwd, so the mountinfo lines are matched by the path the process sees. */
#include <stdint.h>
#include "sys.h"

#define SYS_GETCWD 17
#define SYS_MKDIRAT 34
#define SYS_UNLINKAT 35
#define SYS_UMOUNT2 39
#define SYS_MOUNT 40
#define SYS_CHDIR 49
#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_READ 63
#define SYS_EXIT 93
#define SYS_CLONE 220
#define SYS_WAIT4 260
#define SYS_UNSHARE 97

#define AT_FDCWD (-100)
#define AT_REMOVEDIR 0x200
#define O_WRONLY 01
#define O_RDWR 02
#define O_CREAT 0100
#define O_TRUNC 01000
#define MS_RDONLY 1
#define MS_BIND 4096
#define MS_REMOUNT 32
#define MS_PRIVATE (1 << 18)
#define MS_REC 16384
#define CLONE_NEWNS 0x00020000
#define MS_BIND_FLAG 4096
#define MS_PRIVATE_FLAG (1 << 18)
#define MNT_DETACH 2
#define EBUSY_RC (-16)
#define SIGCHLD 17

static char base[256] = "build/tests/mountns-work";
static char absbase[512];
static char mi[16384];

/* 1 if s begins with the bytes of pre (n of them). */
static int starts_n(const char *s, const char *pre, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (s[i] != pre[i])
            return 0;
    return 1;
}

static i64 pth(const char *p) { return (i64)(u64)(uintptr_t)p; }

static void putl(const char *label, i64 v)
{
    put(label);
    put(" ");
    putd(v);
    put("\n");
}

static i64 do_mount(const char *src, const char *tgt, const char *type, u64 flags)
{
    return sc6(SYS_MOUNT, src ? pth(src) : 0, pth(tgt), type ? pth(type) : 0, (i64)flags, 0, 0);
}

static i64 do_umount(const char *tgt, u64 flags) { return sc6(SYS_UMOUNT2, pth(tgt), (i64)flags, 0, 0, 0, 0); }

static i64 do_open(const char *p, i64 flags, i64 mode) { return sc6(SYS_OPENAT, AT_FDCWD, pth(p), flags, mode, 0, 0); }

static i64 do_mkdir(const char *p) { return sc6(SYS_MKDIRAT, AT_FDCWD, pth(p), 0755, 0, 0, 0); }

static i64 do_unlink(const char *p, i64 flags) { return sc6(SYS_UNLINKAT, AT_FDCWD, pth(p), flags, 0, 0, 0); }

/* 0 if path can be opened for reading (and is closed again), else the open's error. */
static i64 open_rc(const char *p)
{
    i64 fd = do_open(p, 0, 0);
    if (fd < 0)
        return fd;
    sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return 0;
}

/* 0 if a file can be created at path (and is closed again), else the open's error. */
static i64 create_rc(const char *p)
{
    i64 fd = do_open(p, O_WRONLY | O_CREAT, 0644);
    if (fd < 0)
        return fd;
    sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return 0;
}

/* Writes data to path (created or truncated). Returns the open's result, or the write's. */
static i64 write_file(const char *p, const char *data)
{
    i64 fd = do_open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return fd;
    i64 n = sys_write((int)fd, data, strlen(data));
    sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    return n < 0 ? n : 0;
}

/* Reads path into buf (NUL-terminated). Returns the byte count or the open's error. */
static i64 read_file(const char *p, char *buf, size_t cap)
{
    i64 fd = do_open(p, 0, 0);
    if (fd < 0)
        return fd;
    i64 total = 0;
    for (;;) {
        i64 n = sc6(SYS_READ, fd, pth(buf + total), (i64)(cap - 1 - (size_t)total), 0, 0, 0);
        if (n <= 0)
            break;
        total += n;
        if ((size_t)total >= cap - 1)
            break;
    }
    sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    buf[total] = 0;
    return total;
}

static void show_file(const char *label, const char *p)
{
    char buf[128];
    i64 n = read_file(p, buf, sizeof buf);
    put(label);
    put(" ");
    if (n < 0)
        putd(n);
    else {
        put("\"");
        put(buf);
        put("\"");
    }
    put("\n");
}

/* The mountinfo lines whose mount point is under base: "<mountpoint without base> <rw|ro> <bind|tmpfs>". */
static void show_mountinfo(const char *label)
{
    i64 fd = do_open("/proc/self/mountinfo", 0, 0);
    put(label);
    put("\n");
    if (fd < 0) {
        putl("mountinfo open", fd);
        return;
    }
    i64 total = 0;
    for (;;) {
        i64 n = sc6(SYS_READ, fd, pth(mi + total), (i64)(sizeof mi - 1 - (size_t)total), 0, 0, 0);
        if (n <= 0)
            break;
        total += n;
    }
    sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0);
    mi[total] = 0;
    size_t bl = strlen(absbase);
    char *line = mi;
    while (*line) {
        char *eol = line;
        while (*eol && *eol != '\n')
            eol++;
        char keep = *eol;
        *eol = 0;
        /* Fields: id parent dev root mountpoint options [optional...] - fstype source superopts */
        char *f[6] = {0};
        char *p = line;
        for (int i = 0; i < 6 && *p; i++) {
            f[i] = p;
            while (*p && *p != ' ')
                p++;
            if (*p)
                *p++ = 0;
        }
        char *dash = p;
        while (dash && *dash) {
            if (dash[0] == '-' && dash[1] == ' ') {
                dash += 2;
                break;
            }
            dash++;
        }
        if (f[4] && starts_n(f[4], absbase, bl) && (f[4][bl] == '/' || f[4][bl] == 0)) {
            put("  ");
            put(f[4] + bl);
            put(" ");
            put(f[5] && f[5][0] == 'r' && f[5][1] == 'o' ? "ro" : "rw");
            put(" ");
            /* A tmpfs has its own root ("/"); a bind shows its source's path, whatever fs that is. */
            int own_root = f[3] && f[3][0] == '/' && f[3][1] == 0;
            put(dash && starts_n(dash, "tmpfs", 5) && own_root ? "tmpfs" : "bind");
            put("\n");
        }
        if (!keep)
            break;
        line = eol + 1;
    }
}

int main(int argc, char **argv)
{
    if (argc > 1)
        memcpy(base, argv[1], strlen(argv[1]) + 1);
    put("mountns\n");
    do_mkdir(base); /* EEXIST is fine */
    putl("chdir", sc6(SYS_CHDIR, pth(base), 0, 0, 0, 0, 0));
    if (sc6(SYS_GETCWD, pth(absbase), sizeof absbase, 0, 0, 0, 0) <= 0)
        return 1;

    /* A private namespace: the mounts below stay in this process, and nothing propagates. */
    putl("unshare newns", sc6(SYS_UNSHARE, CLONE_NEWNS, 0, 0, 0, 0, 0));
    putl("make / private", do_mount(NULL, "/", NULL, MS_REC | MS_PRIVATE));

    do_mkdir("src");
    do_mkdir("mnt");
    do_mkdir("tm");
    do_mkdir("iso");
    putl("write src/hello", write_file("src/hello", "content-A"));

    /* Bind, then read and write through it. */
    putl("bind src mnt", do_mount("src", "mnt", NULL, MS_BIND));
    show_file("mnt/hello", "mnt/hello");
    putl("write mnt/new (rw)", write_file("mnt/new", "x"));
    putl("src/new seen", open_rc("src/new"));
    show_mountinfo("mountinfo after bind");

    /* Read-only remount: every create, mkdir and unlink under it is EROFS; reads still work. */
    putl("remount mnt ro", do_mount(NULL, "mnt", NULL, MS_REMOUNT | MS_BIND | MS_RDONLY));
    putl("create mnt/newer", create_rc("mnt/newer"));
    putl("write mnt/new (ro)", write_file("mnt/new", "y"));
    putl("mkdir mnt/d", do_mkdir("mnt/d"));
    putl("unlink mnt/new", do_unlink("mnt/new", 0));
    show_file("read mnt/hello (ro)", "mnt/hello");
    putl("create src/newer (not ro)", create_rc("src/newer"));
    show_mountinfo("mountinfo after ro remount");

    /* umount2: a path that is not a mount point is EINVAL; after the unmount the source is not seen. */
    putl("umount src (not a mount)", do_umount("src", 0));
    putl("umount mnt", do_umount("mnt", 0));
    show_file("mnt/hello after umount", "mnt/hello");

    /* tmpfs, stacked: the top mount hides the file, and umount2 brings it back. */
    putl("tmpfs tm", do_mount("tmpfs", "tm", "tmpfs", 0));
    putl("write tm/f", write_file("tm/f", "tmp"));
    show_file("read tm/f", "tm/f");
    putl("stack tmpfs on tm", do_mount("tmpfs", "tm", "tmpfs", 0));
    show_file("read tm/f under top tmpfs", "tm/f");
    putl("umount tm (top)", do_umount("tm", 0));
    show_file("read tm/f after top umount", "tm/f");
    show_mountinfo("mountinfo after tmpfs");

    /* ".." out of the tmpfs reaches the directory that holds it (the source, not a mount). */
    show_file("tm/../src/hello", "tm/../src/hello");
    putl("umount tm (lower)", do_umount("tm", 0));
    show_file("read tm/f after lower umount", "tm/f");

    /* rbind (MS_REC) copies the submounts of the source; a plain bind does not. src2/sub holds a tmpfs. */
    do_mkdir("src2");
    do_mkdir("src2/sub");
    do_mkdir("mnt3");
    do_mkdir("mnt4");
    do_mkdir("mnt5");
    do_mkdir("mnt6");
    do_mkdir("mnt7");
    do_mkdir("iso2");
    putl("tmpfs on src2/sub", do_mount("tmpfs", "src2/sub", "tmpfs", 0));
    putl("write src2/sub/t", write_file("src2/sub/t", "sub"));
    putl("rbind src2 mnt3", do_mount("src2", "mnt3", NULL, MS_BIND | MS_REC));
    show_file("mnt3/sub/t (rbind)", "mnt3/sub/t");
    putl("bind src2 mnt4 (no rec)", do_mount("src2", "mnt4", NULL, MS_BIND));
    show_file("mnt4/sub/t (bind)", "mnt4/sub/t");
    putl("umount mnt3 with a child", do_umount("mnt3", 0));
    putl("umount mnt3/sub (child)", do_umount("mnt3/sub", 0));
    show_file("mnt3/sub/t after child umount", "mnt3/sub/t");
    putl("umount mnt3", do_umount("mnt3", 0));
    /* A rbind again, then a lazy (MNT_DETACH) unmount of the parent: the child goes with it. */
    putl("rbind src2 mnt5", do_mount("src2", "mnt5", NULL, MS_BIND | MS_REC));
    show_file("mnt5/sub/t (rbind again)", "mnt5/sub/t");
    show_mountinfo("mountinfo with rbind and submount copy");
    putl("umount mnt5 MNT_DETACH", do_umount("mnt5", MNT_DETACH));
    show_file("mnt5/sub/t after detach", "mnt5/sub/t");
    show_mountinfo("mountinfo after detach");
    putl("umount mnt4", do_umount("mnt4", 0));
    putl("umount src2/sub", do_umount("src2/sub", 0));

    /* A bind with MS_PRIVATE is a bind (the propagation flag is applied to it, not a propagation change). */
    putl("bind src mnt6 (MS_PRIVATE)", do_mount("src", "mnt6", NULL, MS_BIND_FLAG | MS_PRIVATE_FLAG));
    show_file("mnt6/hello", "mnt6/hello");
    putl("umount mnt6", do_umount("mnt6", 0));

    /* MS_RDONLY on the bind call is not applied (Linux ignores it there); a remount sets it. */
    putl("bind src mnt7 (MS_RDONLY)", do_mount("src", "mnt7", NULL, MS_BIND | MS_RDONLY));
    putl("create mnt7/q (not ro yet)", create_rc("mnt7/q"));
    putl("remount mnt7 ro", do_mount(NULL, "mnt7", NULL, MS_REMOUNT | MS_BIND | MS_RDONLY));
    putl("create mnt7/q2 (ro)", create_rc("mnt7/q2"));
    putl("umount mnt7", do_umount("mnt7", 0));

    /* A child made with clone(CLONE_NEWNS): it copies this namespace (sees mnt-free state here), and
     * its tmpfs is invisible to this process. */
    do_mkdir("iso3");
    putl("tmpfs iso3", do_mount("tmpfs", "iso3", "tmpfs", 0));
    putl("write iso3/z", write_file("iso3/z", "copy"));
    i64 cpid = sc6(SYS_CLONE, CLONE_NEWNS | SIGCHLD, 0, 0, 0, 0, 0);
    if (cpid == 0) {
        /* The copy has iso3 (the parent's mount at clone time); unmounting it here is private. */
        show_file("clone child iso3/z", "iso3/z");
        putl("clone child umount iso3", do_umount("iso3", 0));
        i64 m = do_mount("tmpfs", "iso2", "tmpfs", 0);
        i64 w = write_file("iso2/y", "clone");
        i64 seen = read_file("src2/sub/t", mi, sizeof mi);
        put("clone child mount ");
        putd(m);
        put(" write ");
        putd(w);
        put(" src2/sub/t ");
        putd(seen);
        put("\n");
        sc6(SYS_EXIT, 0, 0, 0, 0, 0, 0);
    }
    i64 cstatus = 0;
    sc6(SYS_WAIT4, cpid, pth((char *)&cstatus), 0, 0, 0, 0);
    putl("clone child exit", (cstatus >> 8) & 0xff);
    putl("parent sees iso2/y", open_rc("iso2/y"));
    show_file("parent iso3/z after child umount", "iso3/z");
    putl("umount iso3", do_umount("iso3", 0));

    /* A child in its own namespace: its tmpfs is invisible to this process. */
    i64 pid = sc6(SYS_CLONE, SIGCHLD, 0, 0, 0, 0, 0);
    if (pid == 0) {
        i64 rc = sc6(SYS_UNSHARE, CLONE_NEWNS, 0, 0, 0, 0, 0);
        i64 m = do_mount("tmpfs", "iso", "tmpfs", 0);
        i64 w = write_file("iso/x", "child");
        put("child unshare ");
        putd(rc);
        put(" mount ");
        putd(m);
        put(" write ");
        putd(w);
        put("\n");
        sc6(SYS_EXIT, 0, 0, 0, 0, 0, 0);
    }
    i64 status = 0;
    sc6(SYS_WAIT4, pid, pth((char *)&status), 0, 0, 0, 0);
    putl("child exit status", (status >> 8) & 0xff);
    putl("parent sees iso/x", open_rc("iso/x"));
    show_mountinfo("mountinfo at end");

    /* B14, the mount walk: a tmpfs mounted over a path hides a bind made below it before, and the bind is back when
     * the tmpfs is unmounted. A later bind below the tmpfs is visible. */
    do_mkdir("wk");
    do_mkdir("wk/a");
    do_mkdir("wk/a/x");
    do_mkdir("wk/src");
    putl("write wk/src/marker", write_file("wk/src/marker", "under-bind"));
    putl("bind wk/src over wk/a/x", do_mount("wk/src", "wk/a/x", NULL, MS_BIND));
    show_file("wk/a/x/marker (bind)", "wk/a/x/marker");
    putl("tmpfs over wk/a", do_mount("tmpfs", "wk/a", "tmpfs", 0));
    show_file("wk/a/x/marker under tmpfs", "wk/a/x/marker");
    putl("mkdir wk/a/x on tmpfs", do_mkdir("wk/a/x"));
    putl("bind wk/src over wk/a/x (on tmpfs)", do_mount("wk/src", "wk/a/x", NULL, MS_BIND));
    show_file("wk/a/x/marker (bind on tmpfs)", "wk/a/x/marker");
    show_mountinfo("mountinfo in walk (tmpfs over bind)");
    putl("umount wk/a/x (bind on tmpfs)", do_umount("wk/a/x", 0));
    putl("umount wk/a (tmpfs)", do_umount("wk/a", 0));
    show_file("wk/a/x/marker after umount", "wk/a/x/marker");
    show_mountinfo("mountinfo after walk");

    /* B14, zygote's per-app isolation: a clone child (its own copy of the namespace) mounts a tmpfs over data/user and
     * binds the app's directory from the mirror under it. The child sees the bind and not the parent's files; the
     * parent sees neither the tmpfs nor the bind. */
    do_mkdir("zd");
    do_mkdir("zd/user");
    do_mkdir("zm");
    do_mkdir("zm/0");
    do_mkdir("zm/0/pkg");
    putl("write zd/user/parent.txt", write_file("zd/user/parent.txt", "parent-view"));
    putl("write zm/0/pkg/app.txt", write_file("zm/0/pkg/app.txt", "per-app"));
    i64 zpid = sc6(SYS_CLONE, SIGCHLD, 0, 0, 0, 0, 0);
    if (zpid == 0) {
        putl("zygote child unshare newns", sc6(SYS_UNSHARE, CLONE_NEWNS, 0, 0, 0, 0, 0));
        putl("zygote child make / private", do_mount(NULL, "/", NULL, MS_REC | MS_PRIVATE));
        putl("zygote child tmpfs over zd/user", do_mount("tmpfs", "zd/user", "tmpfs", 0));
        putl("zygote child mkdir zd/user/0", do_mkdir("zd/user/0"));
        putl("zygote child mkdir zd/user/0/pkg", do_mkdir("zd/user/0/pkg"));
        putl("zygote child bind zm/0/pkg", do_mount("zm/0/pkg", "zd/user/0/pkg", NULL, MS_BIND));
        show_file("zygote child zd/user/0/pkg/app.txt", "zd/user/0/pkg/app.txt");
        show_file("zygote child zd/user/parent.txt (hidden)", "zd/user/parent.txt");
        show_mountinfo("mountinfo in zygote child");
        sc6(SYS_EXIT, 0, 0, 0, 0, 0, 0);
    }
    i64 zstatus = 0;
    sc6(SYS_WAIT4, zpid, pth((char *)&zstatus), 0, 0, 0, 0);
    putl("zygote child exit status", (zstatus >> 8) & 0xff);
    show_file("parent zd/user/parent.txt", "zd/user/parent.txt");
    show_file("parent zd/user/0/pkg/app.txt (no bind)", "zd/user/0/pkg/app.txt");
    show_mountinfo("mountinfo after zygote pattern");

    /* Cleanup: the files and directories this run made. */
    putl("umount wk/a/x (first bind)", do_umount("wk/a/x", 0));
    do_unlink("zm/0/pkg/app.txt", 0);
    do_unlink("zd/user/parent.txt", 0);
    do_unlink("wk/src/marker", 0);
    do_unlink("zm/0/pkg", AT_REMOVEDIR);
    do_unlink("zm/0", AT_REMOVEDIR);
    do_unlink("zm", AT_REMOVEDIR);
    do_unlink("zd/user", AT_REMOVEDIR);
    do_unlink("zd", AT_REMOVEDIR);
    do_unlink("wk/src", AT_REMOVEDIR);
    do_unlink("wk/a/x", AT_REMOVEDIR);
    do_unlink("wk/a", AT_REMOVEDIR);
    do_unlink("wk", AT_REMOVEDIR);
    do_unlink("src/q", 0);
    do_unlink("src/hello", 0);
    do_unlink("src2/sub/t", 0);
    do_unlink("src2/sub", AT_REMOVEDIR);
    do_unlink("src2", AT_REMOVEDIR);
    do_unlink("mnt3", AT_REMOVEDIR);
    do_unlink("mnt4", AT_REMOVEDIR);
    do_unlink("mnt5", AT_REMOVEDIR);
    do_unlink("mnt6", AT_REMOVEDIR);
    do_unlink("mnt7", AT_REMOVEDIR);
    do_unlink("iso2", AT_REMOVEDIR);
    do_unlink("iso3", AT_REMOVEDIR);
    do_unlink("src/new", 0);
    do_unlink("src/newer", 0);
    do_unlink("mnt", AT_REMOVEDIR);
    do_unlink("tm", AT_REMOVEDIR);
    do_unlink("iso", AT_REMOVEDIR);
    do_unlink("src", AT_REMOVEDIR);
    put("done\n");
    return 0;
}
