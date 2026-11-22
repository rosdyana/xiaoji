/* execve and execveat: replace the calling process's program. */
#ifndef XIAOJI_EXEC_H
#define XIAOJI_EXEC_H

#include <stdint.h>

#include "cpu_a64.h"

/* Runs execveat(dirfd, path, argv, envp, flags) for the thread c (execve passes
 * LAT_FDCWD and 0). path, argv and envp are guest addresses. Returns 0 (the caller's
 * registers are then the new program's, and x0 is 0), or a negative errno. On an error
 * the caller keeps its image. Errors: EFAULT, ENAMETOOLONG, ENOENT, EACCES (not a regular
 * file, or no exec bit), ENOEXEC (not an ELF program and no #! line), E2BIG (argv/envp
 * too large), ELOOP (more than 4 levels of #!), EINVAL (flags), EBADF, ENOMEM. */
int64_t exec_run(cpu_t *c, int dirfd, uint64_t path, uint64_t argv, uint64_t envp, uint64_t flags);

#endif
