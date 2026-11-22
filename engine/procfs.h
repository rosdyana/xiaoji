/* Synthetic /proc and /sys files. Their contents are generated when the file is
 * opened, from the engine's own state (the mapping list, the thread list). Only
 * the files ART reads are provided. */
#ifndef XIAOJI_PROCFS_H
#define XIAOJI_PROCFS_H

#include <stddef.h>
#include <stdint.h>

/* 1 if guest path names a synthetic file. */
int procfs_known(const char *guest);
/* "/proc/<pid>[/rest]": 1 and the pid and rest ("" or "/stat" ...) if the path has that form. */
int procfs_pid_path(const char *guest, int *pid, const char **rest);
/* 1 if guest path is a directory on the way to a synthetic file (not listable). */
int procfs_dir(const char *guest);

/* Generates the contents of a synthetic file (malloc'd, the caller frees it).
 * Returns 0, or -1 if the path is not synthetic or the memory is short. */
int procfs_read(const char *guest, uint8_t **data, size_t *len);

#endif
