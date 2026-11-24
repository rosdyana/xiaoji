/* bpf(2), the maps-only subset (B15, docs/boot.md), and the synthetic bpffs at /sys/fs/bpf. See bpf.c.
 * Every function returns a Linux errno as -LE_* on failure (linux.h); only host.c translates host errno values. */
#ifndef XIAOJI_BPF_H
#define XIAOJI_BPF_H

#include <stddef.h>
#include <stdint.h>

#define BPF_FS_ROOT "/sys/fs/bpf"

/* bpf(cmd, attr, size): syscall 280. Returns the result, or -LE_*. */
int64_t bpf_syscall(uint64_t cmd, uint64_t attr, uint64_t size);

/* Drops one reference of a map (the description of a VFS_BPF fd, or a pinned object). */
void bpf_map_unref(void *map);

/* The synthetic bpffs. Created empty by bpffs_init (init.c). A path is under it when it is BPF_FS_ROOT or below. */
typedef enum { BPFFS_NONE, BPFFS_DIR, BPFFS_OBJ } bpffs_kind;
void bpffs_init(void);
/* bpfloader's pre-creation (bpf.c, B15): the maps of the image's BPF objects, created and pinned at boot. */
void bpf_preload(void);
int bpffs_under(const char *guest);
bpffs_kind bpffs_lookup(const char *guest);
/* Stat fields of a pinned object or directory (guest is canonical). Returns 0 or -LE_NOENT. */
int bpffs_stat(const char *guest, uint32_t *mode, uint32_t *uid, uint32_t *gid);
/* mkdir and rmdir/unlink under the bpffs. mode is the final mode (umask applied). Returns 0 or -LE_*. */
int bpffs_mkdir(const char *guest, uint32_t mode);
int bpffs_unlink(const char *guest, int rmdir);

#endif
