/* Program loading: the image and its interpreter, then the Linux initial stack with
 * argv, envp and auxv (see plan). */
#include "loader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elf.h"
#include "host.h"
#include "linux.h"
#include "mem.h"
#include "proc.h"
#include "syscall.h"
#include "vfs.h"

#define STACK_TOP 0x7FFFFFFF0000ull
#define STACK_SIZE (8ull << 20)
#define DYN_BASE 0x555555550000ull    /* ET_DYN programs */
#define INTERP_BASE 0x7FF000000000ull /* PT_INTERP interpreters */

#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_FLAGS 8
#define AT_ENTRY 9
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_PLATFORM 15
#define AT_HWCAP 16
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_HWCAP2 26
#define AT_EXECFN 31

#define MAX_AUX 32

/* Copies data below *sp and moves *sp down. The stack's lowest address is base. */
static int push_bytes(uint64_t *sp, uint64_t base, const void *data, size_t len, uint64_t *va_out)
{
    if (*sp - base < len)
        return -LE_2BIG;
    *sp -= len;
    if (mem_copy_in(*sp, data, len) < 0)
        return -LE_NOMEM;
    *va_out = *sp;
    return 0;
}

/* Maps the stack and writes the initial frame; *sp_out is the initial sp. */
static int setup_stack(uint64_t *sp_out, const elf_info *ei, uint64_t at_base, int argc, char **argv, int envc,
                       char **envp)
{
    uint64_t base = STACK_TOP - STACK_SIZE;
    if (mem_map(base, STACK_SIZE, MEM_PROT_R | MEM_PROT_W) < 0)
        return -LE_NOMEM;

    uint64_t sp = STACK_TOP;
    uint8_t rnd[16];
    uint64_t random_va, platform_va;
    uint64_t *argv_va = calloc(argc > 0 ? argc : 1, sizeof *argv_va);
    uint64_t *env_va = calloc(envc > 0 ? envc : 1, sizeof *env_va);
    uint64_t *buf = NULL;
    int rc = -LE_NOMEM;
    if (!argv_va || !env_va)
        goto out;

    host_random(rnd, sizeof rnd);
    if ((rc = push_bytes(&sp, base, rnd, sizeof rnd, &random_va)) < 0)
        goto out;
    if ((rc = push_bytes(&sp, base, "aarch64", 8, &platform_va)) < 0)
        goto out;
    for (int i = envc - 1; i >= 0; i--)
        if ((rc = push_bytes(&sp, base, envp[i], strlen(envp[i]) + 1, &env_va[i])) < 0)
            goto out;
    for (int i = argc - 1; i >= 0; i--)
        if ((rc = push_bytes(&sp, base, argv[i], strlen(argv[i]) + 1, &argv_va[i])) < 0)
            goto out;

    uint64_t aux[MAX_AUX * 2];
    unsigned n = 0;
#define AUX(t, v)                 \
    do {                          \
        aux[n++] = (t);           \
        aux[n++] = (uint64_t)(v); \
    } while (0)
    AUX(AT_PHDR, ei->phdr);
    AUX(AT_PHENT, ei->phent);
    AUX(AT_PHNUM, ei->phnum);
    AUX(AT_PAGESZ, 4096);
    AUX(AT_BASE, at_base);
    AUX(AT_FLAGS, 0);
    AUX(AT_ENTRY, ei->entry);
    AUX(AT_UID, 0); /* root, as the oracle runs */
    AUX(AT_EUID, 0);
    AUX(AT_GID, 0);
    AUX(AT_EGID, 0);
    AUX(AT_PLATFORM, platform_va);
    AUX(AT_HWCAP, 0x3); /* FP | ASIMD */
    AUX(AT_CLKTCK, 100);
    AUX(AT_SECURE, 0);
    AUX(AT_RANDOM, random_va);
    AUX(AT_HWCAP2, 0);
    AUX(AT_EXECFN, argc > 0 ? argv_va[0] : platform_va);
    AUX(AT_NULL, 0);
#undef AUX

    /* argc + argv + NULL + envp + NULL + auxv, all 8-byte words. */
    uint64_t words = 1 + (uint64_t)argc + 1 + (uint64_t)envc + 1 + n;
    sp &= ~15ull;
    uint64_t frame = (sp - words * 8) & ~15ull;
    if (frame < base) {
        rc = -LE_2BIG;
        goto out;
    }

    buf = calloc(words, sizeof *buf);
    if (!buf)
        goto out;
    uint64_t *p = buf;
    *p++ = (uint64_t)argc;
    for (int i = 0; i < argc; i++)
        *p++ = argv_va[i];
    *p++ = 0;
    for (int i = 0; i < envc; i++)
        *p++ = env_va[i];
    *p++ = 0;
    memcpy(p, aux, n * sizeof(uint64_t));

    if (mem_copy_in(frame, buf, words * 8) < 0)
        goto out;
    *sp_out = frame;
    rc = 0;
out:
    free(buf);
    free(argv_va);
    free(env_va);
    return rc;
}

int loader_load(const uint8_t *img, size_t len, const char *path, int argc, char **argv, int envc, char **envp,
                uint64_t *entry, uint64_t *sp)
{
    if (len < 4 || memcmp(img, "\177ELF", 4) != 0)
        return -LE_NOEXEC;

    elf_info ei;
    if (elf_load(img, len, path, DYN_BASE, &ei))
        return -LE_NOEXEC; /* elf_load has printed why */

    uint64_t start = ei.entry, at_base = 0;
    if (ei.interp[0]) {
        uint8_t *iimg = NULL;
        size_t ilen = 0;
        char icanon[VFS_PATH_MAX];
        if (vfs_load_file(ei.interp, &iimg, &ilen, icanon, sizeof icanon) < 0) {
            fprintf(stderr, "xiaoji: cannot read interpreter %s\n", ei.interp);
            return -LE_NOENT;
        }
        elf_info ii;
        int rc = elf_load(iimg, ilen, ei.interp, INTERP_BASE, &ii);
        free(iimg);
        if (rc)
            return -LE_NOEXEC;
        start = ii.entry;
        at_base = ii.base;
    }

    syscall_init(ei.brk);
    int rc = setup_stack(sp, &ei, at_base, argc, argv, envc, envp);
    if (rc == -LE_NOMEM)
        fprintf(stderr, "xiaoji: cannot set up the guest stack\n");
    if (rc < 0)
        return rc;
    *entry = start;
    return 0;
}

void loader_stack_range(uint64_t *lo, uint64_t *hi)
{
    *lo = STACK_TOP - STACK_SIZE;
    *hi = STACK_TOP;
}
