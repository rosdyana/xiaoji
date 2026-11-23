/* Guest filesystem: path resolution inside the guest root, the guest fd table,
 * and synthetic files. Every function returns a Linux errno as -LE_* on failure.
 * Only host.c translates host errno values. */
#ifndef XIAOJI_VFS_H
#define XIAOJI_VFS_H

#include <stddef.h>
#include <stdint.h>

#include "host.h"
#include "memfd.h"
#include "mntns.h"
#include "sock.h"

#define VFS_PATH_MAX 4096
#define VFS_MAX_FD 1024

typedef enum { VFS_FILE, VFS_DIR, VFS_PATH, VFS_SYN, VFS_MEMFD, VFS_PIPE, VFS_SOCK, VFS_BINDER, VFS_EVENTFD, VFS_TIMERFD, VFS_EPOLL,
               VFS_GOLDFISH } vfs_kind;
typedef enum { SYN_NONE, SYN_NULL, SYN_ZERO, SYN_RANDOM, SYN_DATA, SYN_BINDER,
               SYN_DIR, SYN_SELFS_STATUS, SYN_SELFS_ACCESS,
               SYN_SELFS_CONTEXT, SYN_GOLDFISH, SYN_ASHMEM } vfs_syn;

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
    sock_t *sock;    /* VFS_SOCK: the socket (sock.h) */
    int ro_area;     /* VFS_MEMFD: the property area, read-only (PROT_WRITE is refused) */
    int ashmem;      /* VFS_MEMFD: an /dev/ashmem region (ashmem.h) */
    uint32_t ash_prot;   /* ashmem: the protection mask (ASHMEM_SET_PROT_MASK) */
    int ash_unpinned;    /* ashmem: 1 after ASHMEM_UNPIN */
    void *binder;    /* VFS_BINDER: the binder proc (binder.h); released with the description */
    void *obj;       /* VFS_EVENTFD, VFS_TIMERFD, VFS_EPOLL: the object (fdobj.h, poll.h); VFS_GOLDFISH: the pipe (goldfish.h) */
    uint64_t ino;    /* VFS_PIPE, VFS_SOCK: the inode, numbered at creation (both pipe ends share it) */
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
    int ro;              /* the path is under a read-only mount (mutating calls give EROFS) */
} vfs_res;

/* host_root NULL means the host root "/". prog is the guest path of the
 * program (for /proc/self/exe) and argv its arguments (for /proc/self/cmdline). */
/* Per-process fd tables. vfs_fdtab_switch makes t the running table. */
typedef struct fdtab fdtab_t;
fdtab_t *vfs_fdtab_new(void);
void vfs_fdtab_switch(fdtab_t *t);
/* A copy for fork: every descriptor shares its description with the source. */
fdtab_t *vfs_fdtab_fork(fdtab_t *src);
/* The running process's mount namespace (NULL when it has no mounts), and the namespace of t. */
mntns_t *vfs_ns(void);
mntns_t *vfs_fdtab_ns(const fdtab_t *t);
/* The initial process ends: its namespace is released (and its tmpfs directories removed). */
void vfs_fdtab_exit_ns(fdtab_t *t);
/* unshare(CLONE_NEWNS) and clone(CLONE_NEWNS): t gets a copy of its namespace (out of memory is fatal). */
void vfs_fdtab_copy_ns(fdtab_t *t);
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
/* A new unconnected AF_UNIX socket of type (sock.h) in a free slot. Returns the fd, or -LE_*. */
int vfs_socket(int cloexec, int type, int nonblock);
/* A descriptor for a socket the caller already holds a reference to (sock_new or sock_accept).
 * On failure the reference is released. Returns the fd, or -LE_*. */
int vfs_socket_adopt(sock_t *s, int cloexec, int nonblock);
/* Takes another reference on a description (for a message that keeps a copy of it). */
void vfs_desc_ref(vfs_fd *d);
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
/* stat of PROPS_AREA_PATH: a regular file, mode 0444, owned by root, the area's size. */
int vfs_stat_props(host_stat *out);
/* chmod of an open file (pipes and memfds accept and ignore it). */
int vfs_fchmod(int fd, uint32_t mode);
int vfs_statfs_fd(int fd, host_statfs *out);

/* Descriptions, for passing an fd to another process (binder). vfs_desc_hold takes a
 * reference on the description of fd (NULL if fd is not open). vfs_desc_install puts a
 * held description in the running table and returns its fd (the reference moves into
 * the table); on failure it drops the reference and returns -LE_*. vfs_desc_drop gives
 * back a reference that was never installed. */
/* A new fd for an object of kind (eventfd, timerfd or epoll). The description takes obj;
 * on failure obj is freed. Returns the fd or -LE_*. */
int vfs_obj_new(vfs_kind kind, void *obj, int cloexec, uint32_t lflags);
vfs_fd *vfs_desc_hold(int fd);
int vfs_desc_install(vfs_fd *d, int cloexec);
void vfs_desc_drop(vfs_fd *d);

/* Reads a program or interpreter through the guest namespace. canon receives
 * its canonical guest path. */
int vfs_load_file(const char *path, uint8_t **data, size_t *len, char *canon, size_t cap);

#endif
