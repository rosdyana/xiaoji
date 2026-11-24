#ifndef XIAOJI_GL_HOST_H
#define XIAOJI_GL_HOST_H

#include <stddef.h>
#include <stdint.h>

#include "gl_backend.h"
#include "gl_ranges.h"
#include "gl_wire.h"

/* Calls the decoders do not dispatch: the hand-decoded ones (glShaderString, glFinishRoundTrip) and the client-array
 * and client-index calls the guest encoder sends as *Data and *Offset (gl2_enc.cpp). Their wire layouts are in
 * gl_host.c. */
typedef struct gl_custom_backend {
    void (*glShaderString)(void *ctx, uint32_t shader, const void *string, uint32_t len);
    int32_t (*glFinishRoundTrip)(void *ctx);
    void (*glVertexAttribPointerData)(void *ctx, uint32_t index, int32_t size, uint32_t type, uint8_t normalized,
                                      int32_t stride, const void *data, uint32_t datalen);
    void (*glVertexAttribPointerOffset)(void *ctx, uint32_t index, int32_t size, uint32_t type, uint8_t normalized,
                                        int32_t stride, uint32_t offset);
    void (*glDrawElementsData)(void *ctx, uint32_t mode, int32_t count, uint32_t type, const void *data,
                               uint32_t datalen);
    void (*glDrawElementsOffset)(void *ctx, uint32_t mode, int32_t count, uint32_t type, uint32_t offset);
} gl_custom_backend_t;

typedef struct gl_host {
    const renderControl_backend_t *rc;
    const gles2_backend_t *gles2;
    const gles1_backend_t *gles1;
    const gl_custom_backend_t *custom;
} gl_host_t;

const gl_host_t *gl_host_null(void);

/* The WebGL2 backend (gl_webgl.c, built for the web only). Returns NULL when it is not built or could not start;
 * the caller then uses the null backend. */
const gl_host_t *gl_host_webgl(void);

/* --gl null|webgl (main.c): which backend the streams use. The default is null. */
int gl_host_select(const char *name);

/* The backend the next request goes to: the selected one, or null when the selected one is not available. */
const gl_host_t *gl_host_current(void);

int gl_host_decode(const gl_host_t *h, void *ctx, uint32_t op, const uint8_t *pkt, size_t len, gl_reply *rep);

#endif
