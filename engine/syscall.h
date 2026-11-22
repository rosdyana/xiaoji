/* Linux arm64 syscall layer. */
#ifndef XIAOJI_SYSCALL_H
#define XIAOJI_SYSCALL_H

#include <stdint.h>

#include "cpu_a64.h"

/* Sets the initial program break. */
void syscall_init(uint64_t brk_base);
/* The program's name for prctl(PR_GET_NAME): the basename of argv[0], 15 chars. */
void syscall_set_comm(const char *argv0);
/* Handles SVC #0: number in x8, arguments in x0-x5, result in x0. */
void syscall_dispatch(cpu_t *c);
/* The name of syscall nr (arm64 numbering), or NULL for one not in the table. */
const char *syscall_name(uint64_t nr);

#endif
