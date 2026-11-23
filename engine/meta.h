#ifndef XIAOJI_META_H
#define XIAOJI_META_H

/* Guest file metadata (B8): the owner, the group and the permission bits (07777, with setuid, setgid and
 * sticky) of each file the guest sees. It is kept apart from the host's own metadata, so the host modes stay
 * usable by the engine whatever the guest sets.
 *
 * A file is named by its host (dev, ino), so a hard link and a rename keep its record, and a stale record
 * is replaced when the same inode number is created again (meta_new). A file with no record reports root
 * (uid 0, gid 0) and its host permission bits with the group and other write bits removed. Synthetic files,
 * pipes, sockets and memfds have no record and keep the old report (root, host-independent bits).
 *
 * The rules are the Linux 6.12 ones (fs/attr.c chown_ok, chgrp_ok, setattr_prepare, setattr_should_drop_sgid,
 * setattr_copy; fs/inode.c in_group_or_capable, mode_strip_sgid; fs/namei.c vfs_prepare_mode; fs/open.c
 * chown_common, chmod_common). Permission checks use the calling process's fsuid, fsgid, supplementary groups
 * and effective capabilities (cred.c). Enforcement of mode bits on access (DAC) is not part of this layer.
 *
 * The records are in memory for the engine's life (meta_save and meta_load are the persistence hook: B8
 * checkpoint 2 writes them to the rootfs). */

#include <stdint.h>

#include "host.h"

#define META_NOCHANGE 0xffffffffu /* chown's -1: that id is not changed */

typedef struct {
    uint32_t uid, gid, mode; /* mode: 07777 */
} meta_t;

/* The attributes of a host file (identified by st->dev and st->ino): its record, or the default. */
void meta_get(const host_stat *st, meta_t *out);

/* Writes a record (used by meta_chown, meta_chmod and the persistence loader). */
void meta_set(const host_stat *st, const meta_t *m);

/* Reports the record's owner and mode in a stat result (the type bits stay as the host gives them). */
void meta_apply_stat(host_stat *st);

/* A new object at host path: a file (is_dir 0), a directory or a symlink (mode is the requested mode, umask the
 * process's umask, which symlinks ignore). Owner is the fsuid; the group is the parent's when the parent is
 * setgid, else the fsgid. Returns 0 or -errno. Replaces any record that the same inode number had. */
int meta_new(const char *host, int is_dir, uint32_t mode, uint32_t umask_bits);

/* chown(uid, gid) on a host object; META_NOCHANGE leaves an id alone. Returns 0 or -errno. */
int meta_chown(const host_stat *st, uint32_t uid, uint32_t gid);

/* chmod(mode) on a host object. Returns 0 or -errno. */
int meta_chmod(const host_stat *st, uint32_t mode);

/* A write to a regular file: clears setuid, and setgid where Linux does, unless the caller has CAP_FSETID. */
void meta_write_privs(uint64_t dev, uint64_t ino);

/* 1 and the record in out when the host file has a record, else 0 (meta_get gives the default). */
int meta_recorded(const host_stat *st, meta_t *out);

/* 1 for the guest names the sidecar uses (/.xiaoji-meta and its temporary): the guest cannot see or name them. */
int meta_hidden_guest(const char *guest);

/* Persistence (B8 checkpoint 2). meta_persist_open loads <root>/.xiaoji-meta (a missing file is no records) and
 * arms the save; meta_persist_save writes it, and main registers it with atexit. Returns 0, or -1 when root is too
 * long. The file format is in meta.c. */
int meta_persist_open(const char *root);
void meta_persist_save(void);

#endif
