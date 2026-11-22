/* Initial process state: the Linux stack with argv, envp and auxv. */
#ifndef XIAOJI_LOADER_H
#define XIAOJI_LOADER_H

#include "cpu_a64.h"
#include "elf.h"

/* Maps the 8 MiB stack and sets c->sp. ei describes the program (AT_PHDR,
 * AT_ENTRY); at_base is AT_BASE, the interpreter's load bias (0 if static).
 * Returns 0 or -1. */
int loader_setup_stack(cpu_t *c, const elf_info *ei, uint64_t at_base, int argc, char **argv, int envc,
                       char **envp);

/* The main thread's stack: [lo, hi) as set up by loader_setup_stack. */
void loader_stack_range(uint64_t *lo, uint64_t *hi);

#endif
