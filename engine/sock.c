/* AF_UNIX stream sockets: the socket object and the table of endpoints the engine
 * serves. See sock.h. */
#include <stdlib.h>
#include <string.h>

#include "props.h"
#include "sock.h"

struct sock {
    int refs;
    const sock_ops *ops; /* NULL until connected */
    void *conn;
};

sock_t *sock_new(void)
{
    sock_t *s = calloc(1, sizeof *s);
    if (s)
        s->refs = 1;
    return s;
}

void sock_ref(sock_t *s)
{
    s->refs++;
}

void sock_unref(sock_t *s)
{
    if (!s || --s->refs > 0)
        return;
    if (s->conn)
        s->ops->close(s->conn);
    free(s);
}

int sock_connect(sock_t *s, const char *path)
{
    if (s->ops)
        return -LE_ISCONN;
    const sock_ops *ops = NULL;
    if (!strcmp(path, PROPS_SERVICE_PATH))
        ops = props_service_ops();
    if (!ops)
        return -LE_NOENT;
    void *conn = ops->connect();
    if (!conn)
        return -LE_CONNREFUSED;
    s->ops = ops;
    s->conn = conn;
    return 0;
}

int64_t sock_write(sock_t *s, const void *buf, size_t n)
{
    if (!s->ops)
        return -LE_NOTCONN;
    return s->ops->write(s->conn, (const uint8_t *)buf, n);
}

int64_t sock_read(sock_t *s, void *buf, size_t n)
{
    if (!s->ops)
        return -LE_NOTCONN;
    return s->ops->read(s->conn, (uint8_t *)buf, n);
}
