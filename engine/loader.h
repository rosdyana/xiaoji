/* Program loading: a program image, its interpreter, and the initial process state
 * (the Linux stack with argv, envp and auxv). Used by main (the first program) and by
 * execve. */
#ifndef XIAOJI_LOADER_H
#define XIAOJI_LOADER_H

#include <stddef.h>
#include <stdint.h>

/* Loads the program image img (named path in messages) into the running process's
 * address space, which must be fresh, with its PT_INTERP interpreter (read through the
 * guest VFS). It sets the process's brk base, maps an 8 MiB stack holding argc/argv,
 * envc/envp and the auxiliary vector, and returns the initial pc in *entry and the
 * initial sp in *sp. Returns 0, or a negative errno:
 *   -LE_NOEXEC: not an ELF program (silently), or a malformed one (elf_load has printed why);
 *   -LE_NOENT:  the interpreter cannot be read (printed);
 *   -LE_2BIG:   argv and envp do not fit on the stack;
 *   -LE_NOMEM:  the stack cannot be mapped (printed).
 * On failure the address space is partly loaded; the caller discards it. */
int loader_load(const uint8_t *img, size_t len, const char *path, int argc, char **argv, int envc, char **envp,
                uint64_t *entry, uint64_t *sp);

/* The main thread's stack: [lo, hi) as set up by loader_load. */
void loader_stack_range(uint64_t *lo, uint64_t *hi);

#endif
