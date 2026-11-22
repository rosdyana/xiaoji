/* ELF64 loader: PT_LOAD segments, 4 KiB granularity. */
#include "elf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"

#define EI_NIDENT 16
#define ET_EXEC 2
#define ET_DYN 3
#define EM_AARCH64 183
#define PT_LOAD 1
#define PT_INTERP 3
#define PT_PHDR 6
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define PHDR_SIZE 56
#define EXIT_LOAD_FAILED 126

#define PAGE_DOWN(x) ((x) & ~(uint64_t)(MEM_PAGE_SIZE - 1))
#define PAGE_UP(x) (((x) + MEM_PAGE_SIZE - 1) & ~(uint64_t)(MEM_PAGE_SIZE - 1))

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, filesz, memsz;
} phdr_t;

static uint64_t rd(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static int fail(const char *path, const char *msg)
{
    fprintf(stderr, "xiaoji: %s: %s\n", path, msg);
    return EXIT_LOAD_FAILED;
}

static unsigned prot_of(uint32_t flags)
{
    unsigned p = 0;
    if (flags & PF_R)
        p |= MEM_PROT_R;
    if (flags & PF_W)
        p |= MEM_PROT_W;
    if (flags & PF_X)
        p |= MEM_PROT_X;
    return p;
}

int elf_load(const uint8_t *img, size_t len, const char *path, uint64_t dyn_base, elf_info *info)
{
    info->interp[0] = 0;
    if (len < 64 || img[0] != 0x7f || img[1] != 'E' || img[2] != 'L' || img[3] != 'F')
        return fail(path, "not an ELF file");
    if (img[4] != 2 || img[5] != 1)
        return fail(path, "not a little-endian ELF64 file");
    unsigned type = (unsigned)rd(img + 16, 2);
    unsigned machine = (unsigned)rd(img + 18, 2);
    if (machine != EM_AARCH64)
        return fail(path, "not an AArch64 executable");
    if (type != ET_EXEC && type != ET_DYN)
        return fail(path, "unsupported ELF type");

    uint64_t entry = rd(img + 24, 8);
    uint64_t phoff = rd(img + 32, 8);
    unsigned phentsize = (unsigned)rd(img + 54, 2);
    unsigned phnum = (unsigned)rd(img + 56, 2);
    if (phentsize != PHDR_SIZE)
        return fail(path, "unexpected program header size");
    if (phoff > len || (uint64_t)phnum * PHDR_SIZE > len - phoff)
        return fail(path, "program headers are outside the file");

    phdr_t *ph = calloc(phnum ? phnum : 1, sizeof *ph);
    if (!ph)
        return fail(path, "out of host memory");
    for (unsigned i = 0; i < phnum; i++) {
        const uint8_t *p = img + phoff + (size_t)i * PHDR_SIZE;
        ph[i].type = (uint32_t)rd(p, 4);
        ph[i].flags = (uint32_t)rd(p + 4, 4);
        ph[i].offset = rd(p + 8, 8);
        ph[i].vaddr = rd(p + 16, 8);
        ph[i].filesz = rd(p + 32, 8);
        ph[i].memsz = rd(p + 40, 8);
        if (ph[i].type == PT_INTERP) {
            uint64_t off = ph[i].offset;
            size_t n = 0;
            if (off < len) {
                while (n + 1 < sizeof info->interp && off + n < len && img[off + n]) {
                    info->interp[n] = (char)img[off + n];
                    n++;
                }
            }
            info->interp[n] = 0;
        }
    }

    /* Load bias: 0 for ET_EXEC, otherwise the lowest segment goes to dyn_base. */
    uint64_t bias = 0;
    uint64_t lowest = ~0ull;
    for (unsigned i = 0; i < phnum; i++)
        if (ph[i].type == PT_LOAD && ph[i].vaddr < lowest)
            lowest = ph[i].vaddr;
    if (type == ET_DYN) {
        if (lowest == ~0ull) {
            free(ph);
            return fail(path, "no loadable segments");
        }
        bias = dyn_base - (lowest & ~0xFFFFull);
    }

    uint64_t top = 0;
    int loads = 0;
    for (unsigned i = 0; i < phnum; i++) {
        phdr_t *s = &ph[i];
        if (s->type != PT_LOAD)
            continue;
        loads++;
        if (s->filesz > s->memsz) {
            free(ph);
            return fail(path, "segment file size exceeds memory size");
        }
        if (s->filesz && (s->offset > len || s->filesz > len - s->offset)) {
            free(ph);
            return fail(path, "segment data is outside the file");
        }
        uint64_t va = s->vaddr + bias;
        if (va < s->vaddr || va >= MEM_VA_LIMIT || s->memsz > MEM_VA_LIMIT - va) {
            free(ph);
            return fail(path, "segment is outside the guest address space");
        }
        uint64_t start = PAGE_DOWN(va);
        uint64_t end = PAGE_UP(va + s->memsz);
        /* Writable while loading, then the final protection. */
        if (mem_map(start, end - start, MEM_PROT_R | MEM_PROT_W) < 0) {
            free(ph);
            return fail(path, "cannot map segment");
        }
        if ((s->filesz && mem_copy_in(va, img + s->offset, (size_t)s->filesz) < 0) ||
            mem_set(va + s->filesz, 0, (size_t)(s->memsz - s->filesz)) < 0) {
            free(ph);
            return fail(path, "cannot copy segment");
        }
        if (mem_protect(start, end - start, prot_of(s->flags)) < 0) {
            free(ph);
            return fail(path, "cannot protect segment");
        }
        if (end > top)
            top = end;
    }
    if (loads == 0) {
        free(ph);
        return fail(path, "no loadable segments");
    }

    /* AT_PHDR: the PT_PHDR segment if present, otherwise the load segment that
     * contains the program header table in the file. */
    uint64_t phdr_va = 0;
    for (unsigned i = 0; i < phnum && !phdr_va; i++)
        if (ph[i].type == PT_PHDR)
            phdr_va = ph[i].vaddr + bias;
    for (unsigned i = 0; i < phnum && !phdr_va; i++) {
        phdr_t *s = &ph[i];
        if (s->type == PT_LOAD && phoff >= s->offset && phoff < s->offset + s->filesz)
            phdr_va = s->vaddr + (phoff - s->offset) + bias;
    }

    free(ph);
    info->entry = entry + bias;
    info->phdr = phdr_va;
    info->phent = PHDR_SIZE;
    info->phnum = phnum;
    info->brk = top;
    info->base = bias;
    return 0;
}
