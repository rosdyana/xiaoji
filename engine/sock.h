/* AF_UNIX sockets, between guest processes and with the engine's own endpoints.
 *
 * A socket object (sock_t) is owned by open file descriptions (vfs_fd) and by two kinds of
 * internal reference: a pending server-side stream socket is held by its listener's accept
 * queue (hold), and a connected datagram socket holds its destination (hold). The last
 * description to go runs the close effects (sock_unref): the name is unbound, the peer sees
 * a hangup, queued messages are dropped and their descriptions released. The object is
 * freed when no description and no internal hold is left.
 *
 * Blocking is done by the callers through sock_block, which parks the thread with a restart
 * (sched_block_restart). A blocked call therefore has no side effects before it parks, and
 * it runs again from its start when woken. Every change to a socket (queue, state, accept
 * queue) wakes the socket's key, and poll_notify. The key of a socket is its address.
 *
 * Endpoints (props and logd) are served by an ops table, as before: a connect to their path
 * creates a connection object. */
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

#define SOCK_T_STREAM 1
#define SOCK_T_DGRAM 2
#define SOCK_T_SEQPACKET 5
#define SOCK_MAX_FDS 253 /* SCM_MAX_FD: descriptions in one message */
#define SOCK_PATH_MAX 108

/* The process that does an operation (SO_PEERCRED and SCM_CREDENTIALS). */
typedef struct {
    int32_t pid;
    uint32_t uid, gid;
} sock_cred_t;

/* A receive request. buf and cap are the destination. On success n bytes are in buf, and
 * the fds, cred and name are the message's. The fds are references the caller must install
 * (vfs_desc_install) or drop (vfs_desc_drop). */
typedef struct {
    uint8_t *buf;
    size_t cap;
    int peek;     /* MSG_PEEK: the message stays queued (its fds are held, not moved) */
    int waitall;  /* MSG_WAITALL on a stream: wait until cap bytes (or EOF) are queued */
    void *fds[SOCK_MAX_FDS];
    int nfds;
    sock_cred_t cred;
    size_t msglen;  /* the length of the message (datagram and seqpacket) */
    int trunc;      /* a message longer than cap (its tail is discarded) */
    uint8_t from[SOCK_PATH_MAX + 2]; /* the sender's name, sockaddr_un path bytes */
    size_t fromlen; /* 0: the sender is unnamed */
    int fromfs;     /* the sender's name is a path (NUL-terminated in a sockaddr) */
} sock_recv_t;

/* A new unconnected socket of the given type, with one description reference (refs=1). NULL
 * when out of memory. */
sock_t *sock_new(int type);
/* A connected pair of sockets of one type, each with one description reference. 0 or -LE_*. */
int sock_pair(int type, sock_t **a, sock_t **b);
int sock_type(const sock_t *s);
/* 1 for a message-oriented socket (SOCK_DGRAM and SOCK_SEQPACKET): each send is one message. */
int sock_is_dgram(const sock_t *s);
/* A description takes a reference (accept adopts the pending socket this way). */
void sock_ref(sock_t *s);
/* A description releases its reference. The last one runs the close effects. */
void sock_unref(sock_t *s);

/* Names. A name is the sun_path bytes: a filesystem path (fsnode=1, the canonical guest
 * path, without NUL) or an abstract name (fsnode=0, leading NUL included). */
/* Binds an unbound socket to name. disp is the name as the guest gave it (getsockname and
 * the sender of a message report it, so a path under a symlink keeps the spelling it was bound
 * with). -LE_ADDRINUSE if a live socket has the name, -LE_INVAL if s is bound already. For a
 * path, the caller created the socket node first (sock_fsnode_add). */
int sock_bind(sock_t *s, const uint8_t *name, size_t nlen, int fsnode, const uint8_t *disp, size_t dlen);
/* 1 when s is not bound, not an endpoint and not listening (bind may proceed). */
int sock_can_bind(const sock_t *s);
/* SO_PASSCRED: the receiver gets SCM_CREDENTIALS. */
int sock_passcred(const sock_t *s);
/* Records that a socket node exists at a guest path (bind created the file). */
int sock_fsnode_add(const char *guest);
/* 1 when guest is a socket node made by bind (stat reports S_IFSOCK). */
int sock_is_fsnode(const char *guest);
/* Forgets a socket node name (init removes its nodes when it stops). */
void sock_fsnode_del(const char *guest);
/* listen: a bound stream or seqpacket socket. backlog is clamped to 4096. */
int sock_listen(sock_t *s, int backlog);
/* connect to a name. exists says whether the path names an existing file (ENOENT for a path
 * that is not there). Returns 0; -LE_AGAIN when the listener's backlog is full, with *wait set
 * to the listener (the caller blocks on it with sock_block); or another -LE_*. */
int sock_connect(sock_t *s, const uint8_t *name, size_t nlen, int fsnode, int exists, sock_t **wait);
/* accept: the next queued connection, with one description reference (the caller installs
 * it with sock_ref already taken). -LE_AGAIN when none is queued. */
int sock_accept(sock_t *l, sock_t **out);
/* shutdown: how is SHUT_RD (0), SHUT_WR (1) or SHUT_RDWR (2). -LE_NOTCONN if unconnected. */
int sock_shutdown(sock_t *s, int how);
/* sockaddr_un bytes of the socket (peer: its peer's, or its destination's) into sa, up to cap.
 * Returns the full length (2 when unnamed). */
size_t sock_addr(const sock_t *s, int peer, uint8_t *sa, size_t cap);
/* The live socket bound to a name, or NULL. */
sock_t *sock_find(const uint8_t *name, size_t nlen, int fsnode);
/* Whether the socket is connected (to a peer or a destination). */
int sock_connected(const sock_t *s);

/* Data. Each returns the byte count, or a negative LE_* error. -LE_AGAIN means the call
 * would block (the caller decides with sock_block). On success the fds in fds are taken; on
 * failure they stay the caller's. */
int64_t sock_send(sock_t *s, const uint8_t *buf, size_t n, void **fds, int nfds, sock_t *to);
int64_t sock_recv(sock_t *s, sock_recv_t *r);
/* read and write on a socket (no ancillary data): a stream read drops the fds it finds. */
int64_t sock_read(sock_t *s, void *buf, size_t n);
int64_t sock_write(sock_t *s, const void *buf, size_t n);

/* The socket a blocked send waits on: its target (peer or destination), or s itself. */
sock_t *sock_send_target(sock_t *s, sock_t *to);

/* Blocking. sock_block parks the calling thread on key (sched_block_restart) unless
 * nonblock, and returns 1 when it parked (the call then returns 0 and restarts). With
 * tmo_ns (the SO_RCVTIMEO or SO_SNDTIMEO value) the thread gives up at a deadline and gets 0
 * back: the deadline is kept per thread, so a rerun does not extend it. sock_io_block is
 * sock_block for a read (write 0) or a write (write 1) on s. */
int sock_block(const sock_t *key, uint64_t tmo_ns, int nonblock);
int sock_io_block(sock_t *s, int write, int nonblock);
/* The timeout of a socket for reads (write 0) or writes (write 1), in ns, 0 for none. */
uint64_t sock_timeo(const sock_t *s, int write);

/* Readiness (poll): readable (data, a pending connection, or end of file), writable (room to
 * send), and hup (the peer is gone). Endpoints report readable and writable. */
void sock_state(const sock_t *s, int *readable, int *writable, int *hup);

/* Options (SOL_SOCKET only). Returns 0, or -LE_NOPROTOOPT. getsockopt writes at most *len
 * bytes and sets *len to the full size. */
int sock_getopt(sock_t *s, int level, int opt, uint8_t *out, size_t *len);
int sock_setopt(sock_t *s, int level, int opt, const uint8_t *val, size_t len);

/* Endpoints (the property service, logd). A connect to the path creates the connection. */
int sock_is_endpoint(const uint8_t *name, size_t nlen, int dgram);
const sock_ops *sock_endpoint_ops(const uint8_t *name, size_t nlen, int dgram);

#endif
