#ifndef XIAOJI_XATTR_H
#define XIAOJI_XATTR_H

/* Extended attributes (B7), kept in memory for the life of the engine. A file is named by its canonical
 * guest path (the path the VFS resolved), so a path form and an fd form of the same file share attributes.
 * The attributes are not written to the host: they are gone when the engine exits, and an engine run starts
 * with none. That is enough for the SELinux labels (security.selinux) that installd and init set with
 * restorecon, which read them back. The names need a namespace (user., trusted., security., system.);
 * the others are ENOTSUP. The errors are the Linux errno values (linux.h). */

#include <stddef.h>
#include <stdint.h>

#define XATTR_CREATE 1
#define XATTR_REPLACE 2
#define XATTR_VALUE_MAX 65536

/* Sets one attribute. flags is 0, XATTR_CREATE or XATTR_REPLACE. Returns 0 or -errno. */
int64_t xattr_set(const char *guest, const char *name, const uint8_t *val, size_t len, int flags);

/* Copies one attribute into buf (cap bytes, cap 0 asks for the size). Returns the length, or -errno (ENODATA when
 * the attribute is not set, ERANGE when buf is too small). */
int64_t xattr_get(const char *guest, const char *name, uint8_t *buf, size_t cap);

/* The attribute names of a file, each NUL-terminated, in buf. Returns the total length, or -errno (ERANGE). */
int64_t xattr_list(const char *guest, uint8_t *buf, size_t cap);

/* Removes one attribute. Returns 0, or -errno (ENODATA when it is not set). */
int64_t xattr_remove(const char *guest, const char *name);

#endif
