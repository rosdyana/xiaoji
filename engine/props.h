/* Android system properties. The engine plays init's part: it builds the property
 * area from the guest's build.prop files and the engine's own values, serves it
 * read-only at /dev/__properties__, and runs the property service on
 * /dev/socket/property_service.
 *
 * The area is bionic's single-file ("pre-split") format: a 128-byte prop_area
 * header, then a trie of prop_bt nodes and prop_info records. Its layout and
 * update protocol follow bionic's system_properties code for Android 13. */
#ifndef XIAOJI_PROPS_H
#define XIAOJI_PROPS_H

#include <stddef.h>
#include <stdint.h>

#include "memfd.h"
#include "sock.h"

#define PROPS_AREA_PATH "/dev/__properties__"
#define PROPS_SERVICE_PATH "/dev/socket/property_service"

/* Builds the area: the build.prop files (later ones override, except ro.* where the
 * first value wins), then the engine values, then the overrides ("K=V" strings).
 * Missing files are skipped. Returns 0, or -1 if the area could not be built. */
int props_init(const char *const *overrides, int noverrides);

/* The memfd that holds the area (owned by the engine, not referenced for the caller). */
memfd_t *props_memfd(void);

/* The property service's set (init's PropertySet). Returns PROP_SUCCESS (0) or a
 * PROP_ERROR_* code from bionic's sys/_system_properties.h. */
uint32_t props_set(const char *name, size_t namelen, const char *value, size_t valuelen);

/* The endpoint for /dev/socket/property_service. */
const sock_ops *props_service_ops(void);

#endif
