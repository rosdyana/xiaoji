#ifndef XIAOJI_ASHMEM_H
#define XIAOJI_ASHMEM_H

/* /dev/ashmem (B7): the Android shared-memory device that libcutils' ashmem_create_region and libfmq use
 * when sys.use_memfd is not set (system/core/libcutils/ashmem-dev.cpp). A region is a memfd whose size
 * is set once by ASHMEM_SET_SIZE (before that the fd is empty). The ioctls are the kernel's
 * (drivers/staging/android/uapi/ashmem.h); pinning is recorded, and nothing is ever purged. */

#include <stdint.h>

#include "vfs.h"

/* The ioctl on an ashmem descriptor (a VFS_MEMFD with ashmem set). Returns the ioctl result or -errno. */
int64_t ashmem_ioctl(vfs_fd *f, uint32_t cmd, uint64_t arg);

#endif
