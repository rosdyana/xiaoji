/* ELF64 little-endian loader for static AArch64 executables. */
#ifndef XIAOJI_ELF_H
#define XIAOJI_ELF_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t entry; /* AT_ENTRY (biased for ET_DYN) */
    uint64_t phdr;  /* AT_PHDR: guest address of the program headers */
    uint64_t phent; /* AT_PHENT */
    uint64_t phnum; /* AT_PHNUM */
    uint64_t brk;   /* initial program break */
} elf_info;

/* Maps the PT_LOAD segments of img into guest memory and fills in info.
 * Returns 0 on success. On failure it prints a message and returns the exit
 * status to use (126). */
int elf_load(const uint8_t *img, size_t len, const char *path, elf_info *info);

#endif
