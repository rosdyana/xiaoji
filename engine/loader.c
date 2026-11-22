/* Linux initial stack: strings, then argc, argv, envp and auxv (see plan). */
#include "loader.h"

#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "mem.h"

#define STACK_TOP 0x7FFFFFFF0000ull
#define STACK_SIZE (8ull << 20)

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

static int push_bytes(uint64_t *sp, const void *data, size_t len, uint64_t *va_out)
{
    *sp -= len;
    if (mem_copy_in(*sp, data, len) < 0)
        return -1;
    *va_out = *sp;
    return 0;
}

int loader_setup_stack(cpu_t *c, const elf_info *ei, uint64_t at_base, int argc, char **argv, int envc,
                       char **envp)
{
    uint64_t base = STACK_TOP - STACK_SIZE;
    if (mem_map(base, STACK_SIZE, MEM_PROT_R | MEM_PROT_W) < 0)
        return -1;

    uint64_t sp = STACK_TOP;
    uint8_t rnd[16];
    uint64_t random_va, platform_va;
    uint64_t argv_va[argc > 0 ? argc : 1], env_va[envc > 0 ? envc : 1];

    host_random(rnd, sizeof rnd);
    if (push_bytes(&sp, rnd, sizeof rnd, &random_va) < 0)
        return -1;
    if (push_bytes(&sp, "aarch64", 8, &platform_va) < 0)
        return -1;
    for (int i = envc - 1; i >= 0; i--)
        if (push_bytes(&sp, envp[i], strlen(envp[i]) + 1, &env_va[i]) < 0)
            return -1;
    for (int i = argc - 1; i >= 0; i--)
        if (push_bytes(&sp, argv[i], strlen(argv[i]) + 1, &argv_va[i]) < 0)
            return -1;

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
    AUX(AT_EXECFN, argv_va[0]);
    AUX(AT_NULL, 0);
#undef AUX

    /* argc + argv + NULL + envp + NULL + auxv, all 8-byte words. */
    uint64_t words = 1 + (uint64_t)argc + 1 + (uint64_t)envc + 1 + n;
    sp &= ~15ull;
    uint64_t frame = (sp - words * 8) & ~15ull;
    if (frame < base)
        return -1;

    uint64_t *buf = calloc(words, sizeof *buf);
    if (!buf)
        return -1;
    uint64_t *p = buf;
    *p++ = (uint64_t)argc;
    for (int i = 0; i < argc; i++)
        *p++ = argv_va[i];
    *p++ = 0;
    for (int i = 0; i < envc; i++)
        *p++ = env_va[i];
    *p++ = 0;
    memcpy(p, aux, n * sizeof(uint64_t));

    int rc = mem_copy_in(frame, buf, words * 8);
    free(buf);
    if (rc < 0)
        return -1;
    c->sp = frame;
    return 0;
}
