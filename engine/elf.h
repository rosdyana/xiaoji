/* ELF64 little-endian loader for AArch64 executables and interpreters. */
#ifndef XIAOJI_ELF_H
#define XIAOJI_ELF_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t entry;   /* entry point, biased by base */
    uint64_t phdr;    /* AT_PHDR: guest address of the program headers */
    uint64_t phent;   /* AT_PHENT */
    uint64_t phnum;   /* AT_PHNUM */
    uint64_t brk;     /* first address above the loaded segments */
    uint64_t base;    /* load bias (AT_BASE for an interpreter) */
    char interp[256]; /* PT_INTERP path, or "" */
} elf_info;

/* Maps the PT_LOAD segments of img into guest memory and fills in info. An
 * ET_DYN image is loaded at dyn_base (see elf.c for how the bias is chosen).
 * PT_INTERP is recorded in info->interp; the caller loads the interpreter.
 * Returns 0 on success. On failure it prints a message and returns the exit
 * status to use (126). */
int elf_load(const uint8_t *img, size_t len, const char *path, uint64_t dyn_base, elf_info *info);

#endif
