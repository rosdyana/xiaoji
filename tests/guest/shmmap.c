/* MAP_SHARED of host files (filecache.c): views of one file in two processes, coherence between the views and
 * read/write, msync and munmap with the contents read back after the last view goes, a -shm file sized with
 * ftruncate and mapped (the SQLite WAL pattern), and SIGBUS for a page past the end of a file after ftruncate
 * (the file grows back to zeros).
 *
 * argv[1] is a scratch directory (absolute). The test makes it, works in it, and removes what it made. The output
 * has only ok/FAIL lines and exit statuses: no addresses, pids or inode numbers. The engine and the native oracle
 * print the same text. */
#include "sys.h"

#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_PIPE2 59
#define SYS_READ 63
#define SYS_PREAD64 67
#define SYS_PWRITE64 68
#define SYS_FTRUNCATE 46
#define SYS_MKDIRAT 34
#define SYS_UNLINKAT 35
#define SYS_MSYNC 227
#define SYS_CLONE 220
#define SYS_WAIT4 260
#define SYS_MUNMAP_N 215

#define SIGCHLD_N 17
#define SIGBUS_N 7
#define AT_FDCWD (-100)
#define O_RDONLY 0
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 0x01
#define MS_SYNC 4
#define PAGE 4096u
#define FILE_PAGES 4u
#define FILE_LEN (FILE_PAGES * PAGE)
#define SHM_LEN 32768u

static char dir[300], f_data[340], f_shm[340];
static u8 page[PAGE];

static void report(const char *label, int ok)
{
    put(label);
    put(ok ? "=ok\n" : "=FAIL\n");
}

static i64 sys_open(const char *p, int flags, int mode) { return sc6(SYS_OPENAT, AT_FDCWD, (i64)p, flags, mode, 0, 0); }
static i64 sys_close(i64 fd) { return sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0); }
static i64 sys_pread(i64 fd, void *buf, u64 n, u64 off) { return sc6(SYS_PREAD64, fd, (i64)buf, (i64)n, (i64)off, 0, 0); }
static i64 sys_pwrite(i64 fd, const void *buf, u64 n, u64 off) { return sc6(SYS_PWRITE64, fd, (i64)buf, (i64)n, (i64)off, 0, 0); }
static i64 sys_ftruncate(i64 fd, u64 len) { return sc6(SYS_FTRUNCATE, fd, (i64)len, 0, 0, 0, 0); }
static i64 sys_msync(u64 addr, u64 len, int flags) { return sc6(SYS_MSYNC, (i64)addr, (i64)len, flags, 0, 0, 0); }
static i64 sys_unlink(const char *p) { return sc6(SYS_UNLINKAT, AT_FDCWD, (i64)p, 0, 0, 0, 0); }
static i64 sys_mkdir(const char *p, u32 mode) { return sc6(SYS_MKDIRAT, AT_FDCWD, (i64)p, mode, 0, 0, 0); }
static i64 sys_pipe(int fds[2]) { return sc6(SYS_PIPE2, (i64)fds, 0, 0, 0, 0, 0); }
static i64 sys_read(i64 fd, void *buf, u64 n) { return sc6(SYS_READ, fd, (i64)buf, (i64)n, 0, 0, 0); }
static i64 do_fork(void) { return sc6(SYS_CLONE, SIGCHLD_N, 0, 0, 0, 0, 0); }
static i64 wait_for(i64 pid, int *st) { return sc6(SYS_WAIT4, pid, (i64)st, 0, 0, 0, 0); }

static void join(char *out, const char *name)
{
    size_t n = strlen(dir);
    memcpy(out, dir, n);
    out[n] = '/';
    memcpy(out + n + 1, name, strlen(name) + 1);
}

/* The byte the file holds at off before any store: a function of the page and the offset. */
static u8 pat(u32 p, u32 off) { return (u8)(p * 61u + off * 7u + 3u); }

static int file_is_pattern(const volatile u8 *m, u32 p)
{
    for (u32 i = 0; i < PAGE; i++)
        if (m[p * PAGE + i] != pat(p, i))
            return 0;
    return 1;
}

/* Writes the pattern into the file, one page at a time. */
static int write_pattern(i64 fd)
{
    for (u32 p = 0; p < FILE_PAGES; p++) {
        for (u32 i = 0; i < PAGE; i++)
            page[i] = pat(p, i);
        if (sys_pwrite(fd, page, PAGE, (u64)p * PAGE) != (i64)PAGE)
            return -1;
    }
    return 0;
}

static void scenario_views_and_fork(i64 fd)
{
    volatile u8 *m = (volatile u8 *)sys_mmap(0, FILE_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if ((i64)m < 0) {
        report("mmap shared", 0);
        return;
    }
    report("view reads the file", file_is_pattern(m, 0) && file_is_pattern(m, 1) && file_is_pattern(m, 2) &&
                                      file_is_pattern(m, 3));

    /* A store through the view is seen by pread on the descriptor. */
    m[PAGE + 100] = 0xAB;
    u8 b = 0;
    sys_pread(fd, &b, 1, PAGE + 100);
    report("store through view, pread sees it", b == 0xAB);

    /* A write on the descriptor is seen through the view. */
    static const char w[16] = "WRITE-VIA-FD-OK";
    sys_pwrite(fd, w, 16, 2 * PAGE);
    int same = 1;
    for (u32 i = 0; i < 16; i++)
        if (m[2 * PAGE + i] != (u8)w[i])
            same = 0;
    report("write on fd, view sees it", same);

    /* A second process maps the same file and stores into page 3. The child checks the first store
     * (made before the fork) and reports before it writes; the parent reads the child's store after
     * the child's pipe write. */
    int pfd[2];
    sys_pipe(pfd);
    i64 pid = do_fork();
    if (pid == 0) {
        i64 cfd = sys_open(f_data, O_RDWR, 0);
        volatile u8 *cm = (volatile u8 *)sys_mmap(0, FILE_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 0);
        report("child sees parent's store", cm[PAGE + 100] == 0xAB);
        cm[3 * PAGE] = 0xCD;
        cm[3 * PAGE + 1] = 0xCE;
        sys_close(pfd[0]);
        u8 k = 'k';
        sys_write(pfd[1], &k, 1);
        sys_exit_group(0);
    }
    sys_close(pfd[1]);
    u8 k = 0;
    sys_read(pfd[0], &k, 1);
    report("parent sees child's store", m[3 * PAGE] == 0xCD && m[3 * PAGE + 1] == 0xCE);
    int st = 0;
    wait_for(pid, &st);
    report("child exit 0", (st & 0x7f) == 0 && ((st >> 8) & 0xff) == 0);
    sys_close(pfd[0]);

    /* msync, then munmap of the last view: the contents go to the file. */
    report("msync", sys_msync((u64)m, FILE_LEN, MS_SYNC) == 0);
    report("munmap", sys_munmap((u64)m, FILE_LEN) == 0);
}

/* Reopens the file and checks what the views stored. */
static void scenario_reopen(void)
{
    i64 fd = sys_open(f_data, O_RDONLY, 0);
    int ok = fd >= 0;
    u8 b = 0;
    ok = ok && sys_pread(fd, &b, 1, PAGE + 100) == 1 && b == 0xAB;
    report("reopen: store of page 1 on disk", ok);

    static char got[16];
    ok = sys_pread(fd, got, 16, 2 * PAGE) == 16;
    for (u32 i = 0; i < 16 && ok; i++)
        if ((u8)got[i] != (u8)"WRITE-VIA-FD-OK"[i])
            ok = 0;
    report("reopen: write on disk", ok);

    ok = sys_pread(fd, &b, 1, 3 * PAGE) == 1 && b == 0xCD && sys_pread(fd, &b, 1, 3 * PAGE + 1) == 1 && b == 0xCE;
    report("reopen: child's store on disk", ok);

    ok = sys_pread(fd, page, PAGE, 0) == (i64)PAGE;
    for (u32 i = 0; i < PAGE && ok; i++)
        if (page[i] != pat(0, i))
            ok = 0;
    report("reopen: untouched page 0 unchanged", ok);
    sys_close(fd);
}

/* The -shm pattern: a file sized with ftruncate and mapped by two processes; the header is written by the
 * parent, the counter by the child. */
static void scenario_shm(void)
{
    i64 fd = sys_open(f_shm, O_RDWR | O_CREAT | O_TRUNC, 0644);
    report("shm: created and sized", fd >= 0 && sys_ftruncate(fd, SHM_LEN) == 0);
    volatile u8 *m = (volatile u8 *)sys_mmap(0, SHM_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if ((i64)m < 0) {
        report("shm: mmap", 0);
        sys_close(fd);
        return;
    }
    static const char magic[8] = "WALINDEX";
    for (u32 i = 0; i < 8; i++)
        m[i] = (u8)magic[i];

    i64 pid = do_fork();
    if (pid == 0) {
        i64 cfd = sys_open(f_shm, O_RDWR, 0);
        volatile u8 *cm = (volatile u8 *)sys_mmap(0, SHM_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 0);
        int ok = 1;
        for (u32 i = 0; i < 8; i++)
            if (cm[i] != (u8)magic[i])
                ok = 0;
        report("shm: child sees header", ok);
        cm[4096] = 42;
        cm[4097] = 0;
        sys_exit_group(0);
    }
    int st = 0;
    wait_for(pid, &st);
    report("shm: child exit 0", (st & 0x7f) == 0 && ((st >> 8) & 0xff) == 0);
    report("shm: parent sees counter", m[4096] == 42 && m[4097] == 0);
    sys_munmap((u64)m, SHM_LEN);
    sys_close(fd);

    i64 rfd = sys_open(f_shm, O_RDONLY, 0);
    char h[8];
    int ok = sys_pread(rfd, h, 8, 0) == 8;
    for (u32 i = 0; i < 8 && ok; i++)
        if (h[i] != magic[i])
            ok = 0;
    report("shm: header on disk after last view", ok);
    sys_close(rfd);
}

/* After ftruncate shrinks the file under a view, a page past the end raises SIGBUS (in a process that
 * was forked after the shrink, and in the one that shrank it, where it is checked by a fork). Growing the
 * file again maps zeros. */
static void scenario_sigbus(i64 fd)
{
    volatile u8 *m = (volatile u8 *)sys_mmap(0, FILE_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if ((i64)m < 0) {
        report("sigbus: mmap", 0);
        return;
    }
    report("sigbus: shrink", sys_ftruncate(fd, PAGE) == 0);
    i64 pid = do_fork();
    if (pid == 0) {
        volatile u8 x = m[2 * PAGE];
        (void)x;
        sys_exit_group(0);
    }
    int st = 0;
    wait_for(pid, &st);
    report("sigbus: page past EOF is SIGBUS", (st & 0x7f) == SIGBUS_N);
    report("sigbus: page inside EOF still reads", m[0] == pat(0, 0));

    report("sigbus: grow", sys_ftruncate(fd, FILE_LEN) == 0);
    int zero = 1;
    for (u32 i = 0; i < 2 * PAGE; i++)
        if (m[PAGE + i] != 0)
            zero = 0;
    report("sigbus: grown pages are zero", zero);
    sys_munmap((u64)m, FILE_LEN);
}

/* The same, across address spaces: a child forked while the file is full blocks on a pipe, the parent shrinks the
 * file, and the child touches a page past the new end (SIGBUS in the child, whose address space the shrink must
 * reach). A child forked while the file is short reads the grown pages after the parent grows the file: zeros. */
static void scenario_sigbus_cross(i64 fd)
{
    volatile u8 *m = (volatile u8 *)sys_mmap(0, FILE_LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if ((i64)m < 0) {
        report("cross: mmap", 0);
        return;
    }
    int p1[2];
    sys_pipe(p1);
    i64 a = do_fork();
    if (a == 0) {
        u8 c = 0;
        sys_read(p1[0], &c, 1);
        volatile u8 x = m[2 * PAGE];
        (void)x;
        sys_exit_group(0);
    }
    sys_close(p1[0]);
    report("cross: shrink", sys_ftruncate(fd, PAGE) == 0);
    u8 go = 'g';
    sys_write(p1[1], &go, 1);
    int st = 0;
    wait_for(a, &st);
    report("cross: child SIGBUS past the new end", (st & 0x7f) == SIGBUS_N);
    sys_close(p1[1]);

    int p2[2];
    sys_pipe(p2);
    i64 b = do_fork();
    if (b == 0) {
        u8 c = 0;
        sys_read(p2[0], &c, 1);
        int zero = 1;
        for (u32 i = 0; i < PAGE; i++)
            if (m[2 * PAGE + i] != 0)
                zero = 0;
        sys_exit_group(zero ? 0 : 1);
    }
    sys_close(p2[0]);
    report("cross: grow", sys_ftruncate(fd, FILE_LEN) == 0);
    sys_write(p2[1], &go, 1);
    st = 0;
    wait_for(b, &st);
    report("cross: child reads zeros after grow", (st & 0x7f) == 0 && ((st >> 8) & 0xff) == 0);
    sys_close(p2[1]);
    sys_munmap((u64)m, FILE_LEN);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        put("usage: shmmap DIR\n");
        return 2;
    }
    size_t n = strlen(argv[1]);
    if (n >= sizeof dir)
        return 2;
    memcpy(dir, argv[1], n + 1);
    sys_mkdir(dir, 0777);
    join(f_data, "shared.dat");
    join(f_shm, "wal.db-shm");
    sys_unlink(f_data);
    sys_unlink(f_shm);

    i64 fd = sys_open(f_data, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        put("create failed\n");
        return 1;
    }
    report("create and fill", sys_ftruncate(fd, FILE_LEN) == 0 && write_pattern(fd) == 0);
    scenario_views_and_fork(fd);
    sys_close(fd);
    scenario_reopen();

    fd = sys_open(f_data, O_RDWR, 0);
    scenario_sigbus(fd);
    sys_close(fd);

    fd = sys_open(f_data, O_RDWR, 0);
    scenario_sigbus_cross(fd);
    sys_close(fd);

    scenario_shm();

    sys_unlink(f_data);
    sys_unlink(f_shm);
    put("done\n");
    return 0;
}
