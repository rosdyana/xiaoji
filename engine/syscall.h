/* Linux arm64 syscall layer (M0 subset). */
#ifndef XIAOJI_SYSCALL_H
#define XIAOJI_SYSCALL_H

#include <stdint.h>

#include "cpu_a64.h"

/* Sets the initial program break. */
void syscall_init(uint64_t brk_base);
/* Handles SVC #0: number in x8, arguments in x0-x5, result in x0. */
void syscall_dispatch(cpu_t *c);

#endif
