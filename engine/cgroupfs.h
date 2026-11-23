/* Synthetic cgroups (B11): the cgroup mounts that Android's init makes from /system/etc/cgroups.json, and the
 * /dev/cgroup_info/cgroup.rc file that libprocessgroup maps. Membership is tracked per process and reported
 * coherently (cgroup.procs, tasks, /proc/<pid>/cgroup), but nothing is scheduled or limited. See docs/boot.md B11. */
#ifndef XIAOJI_CGROUPFS_H
#define XIAOJI_CGROUPFS_H

#include <stddef.h>
#include <stdint.h>

struct proc;
struct memfd;

#define CG_MAX_MOUNTS 8  /* mount points (one hierarchy each) */
#define CG_MAX_NODES 1024 /* directories, the mount roots included */

/* What a guest path names in the cgroup namespace. */
typedef enum { CG_NONE, CG_DIR, CG_FILE, CG_RC } cg_kind;

/* Reads cgroups.json (system, then vendor) from the guest tree and builds the mounts, the cgroup.rc records and the
 * cgroup.rc memfd. Returns 0; with no cgroups.json there are no mounts and no cgroup.rc (the boot then fails as before). */
int cgroupfs_init(void);

/* 1 if guest is a mount point or lies under one. The fallback for a missing path there is the cgroup namespace's ENOENT,
 * never the host. */
int cgroupfs_under(const char *guest);

/* What guest names: a directory, a file, the cgroup.rc file, or nothing (missing or outside the namespace). */
cg_kind cgroupfs_lookup(const char *guest);

/* The contents of a cgroup file (malloc'd, at least one byte; the caller frees it). Returns 0, or -1 if guest is not a file. */
int cgroupfs_read(const char *guest, uint8_t **data, size_t *len);

/* A write to a cgroup file: membership files move the named processes (0 is the caller), other writable files store the
 * value. Returns n, or a negative LE_* error. */
int64_t cgroupfs_write(const char *guest, const uint8_t *buf, size_t n);

/* mkdir and rmdir in the namespace. Return 0 or a negative LE_* error (EEXIST, ENOENT, EBUSY, ...). */
int cgroupfs_mkdir(const char *guest);
int cgroupfs_rmdir(const char *guest);

/* The index-th entry of a cgroup directory (child directories, then files, then nothing): name and is_dir. 1 if there is one. */
int cgroupfs_child(const char *guest, int index, const char **name, int *is_dir);

/* The cgroup.rc memfd (read-only, mapped by libprocessgroup). NULL if there is no cgroupfs. */
struct memfd *cgroupfs_rc_memfd(void);

/* A process's membership: copied at fork (proc.c) and cleared when it exits (proc_release; members are only the non-exited processes). */
void cgroupfs_proc_exit(struct proc *p);

/* The /proc/<pid>/cgroup text (malloc'd) for a live process: one line per mount, v2 as "0::" and v1 as "N:name:". */
int cgroupfs_proc_text(int pid, uint8_t **data, size_t *len);

#endif
