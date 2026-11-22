/* The logd sink: the engine serves /dev/socket/logdw, the AF_UNIX SOCK_DGRAM socket that
 * liblog writes text log messages to. Each write is one datagram (liblog's writev of the
 * header and payload). The engine decodes it and prints it to stderr when logcat output
 * is on (XIAOJI_LOGCAT=1 or --logcat); otherwise the datagram is dropped. */
#ifndef XIAOJI_LOGD_H
#define XIAOJI_LOGD_H

#include "sock.h"

#define LOGD_PATH "/dev/socket/logdw"

/* Turns the stderr output on or off. Off by default. */
void logd_set_print(int on);

/* The endpoint's operations (connect creates a connection; sock.c uses it). */
const sock_ops *logd_ops(void);

#endif
