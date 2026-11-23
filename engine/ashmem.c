/* /dev/ashmem (ashmem.h). The ioctl numbers are _IOW/_IO(0x77, nr, ...) in the kernel's header. */
#include "ashmem.h"

#include "linux.h"
#include "mem.h"
#include "memfd.h"
#include "vfs.h"

#define ASH_TYPE 0x77
#define ASH_NAME_LEN 256
#define ASH_PAGE 4096ULL
#define ASH_PROT_ALL 7u /* PROT_READ | PROT_WRITE | PROT_EXEC */

enum {
    ASH_SET_NAME = 1,
    ASH_GET_NAME = 2,
    ASH_SET_SIZE = 3,
    ASH_GET_SIZE = 4,
    ASH_SET_PROT_MASK = 5,
    ASH_GET_PROT_MASK = 6,
    ASH_PIN = 7,
    ASH_UNPIN = 8,
    ASH_GET_PIN_STATUS = 9,
    ASH_PURGE_ALL_CACHES = 10,
};

/* ASHMEM_IS_UNPINNED 0, ASHMEM_IS_PINNED 1; ASHMEM_NOT_PURGED 0 (uapi/ashmem.h). */
#define ASH_IS_UNPINNED 0
#define ASH_IS_PINNED 1
#define ASH_NOT_PURGED 0

int64_t ashmem_ioctl(vfs_fd *f, uint32_t cmd, uint64_t arg)
{
    if (((cmd >> 8) & 0xff) != ASH_TYPE)
        return -LE_NOTTY;
    switch (cmd & 0xff) {
    case ASH_SET_NAME: {
        /* The name is only checked (it is readable, at most ASH_NAME_LEN bytes, NUL-terminated). */
        uint8_t b[ASH_NAME_LEN];
        if (mem_copy_out(b, arg, sizeof b) < 0)
            return -LE_FAULT;
        return 0;
    }
    case ASH_GET_NAME:
        return -LE_INVAL;
    case ASH_SET_SIZE: {
        /* Once only: a region that has a size is not resized (the kernel refuses it once it is mapped). */
        if (memfd_size(f->mfd) != 0)
            return -LE_INVAL;
        uint64_t size = (arg + ASH_PAGE - 1) & ~(ASH_PAGE - 1);
        if (size && memfd_truncate(f->mfd, size) != 0)
            return -LE_NOMEM;
        return 0;
    }
    case ASH_GET_SIZE:
        return (int64_t)memfd_size(f->mfd);
    case ASH_SET_PROT_MASK:
        f->ash_prot = (uint32_t)arg & ASH_PROT_ALL;
        return 0;
    case ASH_GET_PROT_MASK:
        return (int64_t)f->ash_prot;
    case ASH_PIN: {
        uint8_t pin[8];
        if (mem_copy_out(pin, arg, sizeof pin) < 0)
            return -LE_FAULT;
        f->ash_unpinned = 0;
        return ASH_NOT_PURGED;
    }
    case ASH_UNPIN: {
        uint8_t pin[8];
        if (mem_copy_out(pin, arg, sizeof pin) < 0)
            return -LE_FAULT;
        f->ash_unpinned = 1;
        return ASH_IS_UNPINNED;
    }
    case ASH_GET_PIN_STATUS:
        return f->ash_unpinned ? ASH_IS_UNPINNED : ASH_IS_PINNED;
    case ASH_PURGE_ALL_CACHES:
        return ASH_NOT_PURGED;
    default:
        return -LE_NOTTY;
    }
}
