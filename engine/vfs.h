/* Guest filesystem: path resolution inside the guest root, the guest fd table,
 * and synthetic files. Every function returns a Linux errno as -LE_* on failure.
 * Only host.c translates host errno values. */
#ifndef XIAOJI_VFS_H
#define XIAOJI_VFS_H

#include <stddef.h>
#include <stdint.h>

#include "host.h"
#include "memfd.h"

#define VFS_PATH_MAX 4096
#define VFS_MAX_FD 1024

typedef enum { VFS_FILE, VFS_DIR, VFS_PATH, VFS_SYN, VFS_MEMFD, VFS_PIPE } vfs_kind;
typedef enum { SYN_NONE, SYN_NULL, SYN_ZERO, SYN_RANDOM, SYN_DATA } vfs_syn;

/* An open file description: the offset, the status flags and the object. Every fd
 * that refers to it (dup, fork) shares it; it is freed when refs drops to 0. */
typedef struct pipe pipe_t;
typedef struct {
    int refs;
    vfs_kind kind;
    vfs_syn syn;
    int hfd;         /* host fd, or -1 (O_PATH without access, and synthetic files) */
    uint32_t lflags; /* Linux open flags, as F_GETFL reports them */
    char *guest;     /* canonical guest path */
    char *host;      /* host path, or NULL for fds 0 to 2 and synthetic files */
    uint8_t *data;   /* SYN_DATA contents */
    size_t dlen;
    uint64_t dpos;
    void *dir;       /* DIR * for directories, opened by the first getdents64 */
    uint64_t nent;   /* directory entries returned so far */
    memfd_t *mfd;    /* VFS_MEMFD: the file */
    pipe_t *pipe;    /* VFS_PIPE: the pipe; lflags (LO_RDONLY or LO_WRONLY) says which end */
} vfs_fd;

/* A descriptor: a slot in a process's fd table. The close-on-exec flag is per slot;
 * the description is shared. desc is NULL for a free slot. */
typedef struct {
    vfs_fd *desc;
    int cloexec;
} vfs_slot;

typedef struct {
    int exists;          /* the final component exists on the host or is synthetic */
    int is_link;         /* the final component is a symlink that was not followed */
    vfs_syn syn;         /* SYN_NONE for host files */
    char guest[VFS_PATH_MAX]; /* canonical guest path (or the link itself) */
    char host[VFS_PATH_MAX];  /* host path for guest[] */
    char target[VFS_PATH_MAX]; /* link target when is_link */
} vfs_res;

/* host_root NULL means the host root "/". prog is the guest path of the
 * program (for /proc/self/exe) and argv its arguments (for /proc/self/cmdline). */
/* Per-process fd tables. vfs_fdtab_switch makes t the running table. */
typedef struct fdtab fdtab_t;
fdtab_t *vfs_fdtab_new(void);
void vfs_fdtab_switch(fdtab_t *t);
/* A copy for fork: every descriptor shares its description with the source. */
fdtab_t *vfs_fdtab_fork(fdtab_t *src);
/* Closes every descriptor of t (the last reference to a description frees it) and frees
 * t. If t is the running table, the running table is cleared. */
void vfs_fdtab_release(fdtab_t *t);

int vfs_init(const char *host_root, const char *prog, int argc, char **argv);

/* Resolves path (relative to dirfd, or the guest cwd for LAT_FDCWD) component by
 * component inside the root. follow=0 leaves a final symlink unresolved. */
int vfs_resolve(int dirfd, const char *path, int follow, vfs_res *out);

/* The program path and the argv bytes (NUL-separated) of a process's fd table, for
 * /proc/<pid> (the table is the one of that process). */
const char *vfs_prog_of(const fdtab_t *t);
const uint8_t *vfs_cmdline_bytes(const fdtab_t *t, size_t *len);

/* Sets the guest path of the program (what /proc/self/exe reads as). */
void vfs_set_prog(const char *guest);
/* Sets the argv bytes of the running process (what /proc/self/cmdline reads). */
void vfs_set_argv(int argc, char **argv);
/* execve: closes every descriptor of the running table that has FD_CLOEXEC. */
void vfs_close_cloexec(void);

int vfs_open(int dirfd, const char *path, uint32_t lflags, uint32_t mode);
/* memfd_create: a new read-write memfd. mflags bit 0 is MFD_CLOEXEC. Returns the fd or -LE_*. */
int vfs_memfd_create(const char *name, uint32_t mflags);
int vfs_close(int fd);
/* pipe2: two descriptors, read end in out[0] and write end in out[1]. Returns 0 or -LE_*. */
int vfs_pipe2(int out[2], uint32_t flags);
/* Blocks the running thread if the read or write on fd would block on a pipe (and
 * the descriptor is not O_NONBLOCK). The call is then restarted. Returns 1 if it blocked. */
int vfs_pipe_block(int fd);
/* FD_CLOEXEC of one slot: -LE_BADF for a free slot. */
int vfs_cloexec_get(int fd);
int vfs_cloexec_set(int fd, int on);
int64_t vfs_read(int fd, void *buf, size_t n);
int64_t vfs_pread(int fd, void *buf, size_t n, uint64_t off);
int64_t vfs_write(int fd, const void *buf, size_t n);
int64_t vfs_lseek(int fd, int64_t off, int whence);
/* dup: exact=0 takes the lowest free fd >= minfd; exact=1 uses minfd and closes it. */
int vfs_dup(int oldfd, int minfd, int exact, int cloexec);
vfs_fd *vfs_get(int fd);
/* Linux dirent64 records into buf. Returns the byte count or -LE_*. */
int64_t vfs_getdents(int fd, uint8_t *buf, size_t cap);

/* The guest cwd, copied with its NUL. Returns the length or -LE_RANGE. */
int vfs_getcwd(char *buf, size_t cap);
int vfs_chdir(const char *path);
int vfs_fchdir(int fd);

int vfs_stat_res(const vfs_res *r, host_stat *out);
int vfs_stat_fd(int fd, host_stat *out);
/* chmod of an open file (pipes and memfds accept and ignore it). */
int vfs_fchmod(int fd, uint32_t mode);
int vfs_statfs_fd(int fd, host_statfs *out);

/* Reads a program or interpreter through the guest namespace. canon receives
 * its canonical guest path. */
int vfs_load_file(const char *path, uint8_t **data, size_t *len, char *canon, size_t cap);

#endif
