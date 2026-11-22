/* AF_UNIX stream sockets, minimal. A socket is unconnected until connect() names a
 * path that the engine serves (today only /dev/socket/property_service). The engine
 * serves that endpoint itself: each connection is an ops-table object, and writes and
 * reads go through it. Guest-to-guest unix sockets (B4) would add a listener table
 * and a second ops implementation; the fd and VFS hooks stay the same. */
#ifndef XIAOJI_SOCK_H
#define XIAOJI_SOCK_H

#include <stddef.h>
#include <stdint.h>

#include "linux.h"

/* An endpoint served by the engine. connect creates a connection object; write
 * consumes the bytes the client sends and may queue replies; read returns queued
 * replies (0 when none are left); close frees the connection. */
typedef struct sock_ops {
    void *(*connect)(void);
    int64_t (*write)(void *conn, const uint8_t *buf, size_t n);
    int64_t (*read)(void *conn, uint8_t *buf, size_t n);
    void (*close)(void *conn);
} sock_ops;

typedef struct sock sock_t;

/* A new unconnected AF_UNIX SOCK_STREAM socket with one reference, or NULL. */
sock_t *sock_new(void);
void sock_ref(sock_t *s);
void sock_unref(sock_t *s);

/* Returns 0, or a negative LE_* error: ENOENT for a path the engine does not serve,
 * EISCONN if already connected. */
int sock_connect(sock_t *s, const char *path);

/* Return the byte count, or a negative LE_* error (ENOTCONN when unconnected). */
int64_t sock_write(sock_t *s, const void *buf, size_t n);
int64_t sock_read(sock_t *s, void *buf, size_t n);

#endif
