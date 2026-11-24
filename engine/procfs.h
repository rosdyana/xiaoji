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
/* The index-th entry of a /proc/<pid>/task (or /proc/self/task) directory: its thread id, in *name
 * (a static buffer). 1 if there is one, 0 for the end or for another path. */
int procfs_task_child(const char *guest, int index, const char **name);
/* 1 for a task directory (/proc/self/task, /proc/<pid>/task): listable, unlike other directories. */
int procfs_task_dir(const char *guest);

/* Generates the contents of a synthetic file (malloc'd, the caller frees it).
 * Returns 0, or -1 if the path is not synthetic or the memory is short. */
int procfs_read(const char *guest, uint8_t **data, size_t *len);
/* attr/current of the caller (self and thread-self) or of a live process: its context is what setcon
 * writes. procfs_attr_write stores a context (the n bytes, its NUL dropped) and returns n, or -LE_*. */
int procfs_is_attr(const char *guest);
int64_t procfs_attr_write(const char *guest, const uint8_t *buf, size_t n);
/* Writes to the generated files that take one: /proc/pressure/<memory|cpu|io> (a PSI trigger, B9) and
 * /proc/<pid>/oom_score_adj. Returns n, or a negative errno. procfs_writable says whether guest takes writes. */
int procfs_writable(const char *guest);
int64_t procfs_write(const char *guest, const uint8_t *buf, size_t n);

#endif
