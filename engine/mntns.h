/* Mount namespaces (B4c): a per-process table of bind and tmpfs mounts over the guest's
 * visible paths. vfs.c translates a visible guest path to a host path through the running
 * process's namespace; the table itself is changed by mount(2), umount2(2) and unshare or
 * clone with CLONE_NEWNS (which copies it). The namespace is refcounted: fork shares it. */
#ifndef XIAOJI_MNTNS_H
#define XIAOJI_MNTNS_H

#include <stddef.h>
#include <stdint.h>

typedef struct mntns mntns_t;

/* The mount(2) and umount2(2) flags that are modelled. */
#define MNT_MS_RDONLY 1ull
#define MNT_MS_NOSUID 2ull
#define MNT_MS_NODEV 4ull
#define MNT_MS_NOEXEC 8ull
#define MNT_MS_REMOUNT 32ull
#define MNT_MS_BIND 4096ull
#define MNT_MS_REC 16384ull
#define MNT_MS_MOVE 8192ull
#define MNT_MS_PROPAGATION ((1ull << 17) | (1ull << 18) | (1ull << 19) | (1ull << 20))
#define MNT_UMOUNT_DETACH 2ull

/* A namespace with only the root mount (the host root, the guest's "/"). Refs 1. */
mntns_t *mntns_new(void);
/* A new reference to ns (NULL stays NULL: a NULL namespace is the root only). */
mntns_t *mntns_ref(mntns_t *ns);
/* Drops a reference; frees the namespace at 0. */
void mntns_unref(mntns_t *ns);
/* A copy for unshare(CLONE_NEWNS) or clone(CLONE_NEWNS): the mounts are copied, the tmpfs
 * contents are shared. Returns a new namespace with refs 1, or NULL on out of memory. */
mntns_t *mntns_copy(mntns_t *src);

/* mount(source, target, type, flags): returns 0 or -LE_*. src and fstype may be NULL. */
int64_t mntns_mount(mntns_t *ns, const char *src, const char *tgt, const char *fstype, uint64_t flags);
/* umount2(target, flags): returns 0 or -LE_*. */
int64_t mntns_umount(mntns_t *ns, const char *tgt, uint64_t flags);

/* Translates a canonical guest path. Returns 1 and writes the host path into out when a live
 * mount covers it (the longest target prefix), with *ro set when that mount is read-only.
 * Returns 0 when no mount covers it (the caller uses the root), or -LE_NAMETOOLONG. */
int mntns_map(mntns_t *ns, const char *guest, char *out, size_t cap, int *ro);

/* The /proc/<pid>/mounts text (info 0) or mountinfo text (info 1) of ns. Returns 0 and sets
 * *data (malloc'd) and *len, or -1 when out of memory. */
int mntns_text(mntns_t *ns, int info, uint8_t **data, size_t *len);

/* Snapshots (snap.c): the host directory of the i-th live tmpfs (they are host directories under /tmp), or NULL past
 * the last. */
const char *mntns_tmpfs_dir(int i);

#endif
