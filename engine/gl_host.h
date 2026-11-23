/* The GL host of the goldfish pipe (docs/graphics.md, sections 3.2 and 3.3). The decoders are generated from
 * the protocol specs (tools/gen-glproto.py, into build/gen): they read a request and call one backend. A
 * backend is a table of calls. The null backend (gl_null.c) answers without a GPU. A native (host GLES) or
 * WebGL2 backend fills the same tables, and the decoders do not change. */
#ifndef XIAOJI_GL_HOST_H
#define XIAOJI_GL_HOST_H

#include <stddef.h>
#include <stdint.h>

#include "gl_backend.h"
#include "gl_ranges.h"
#include "gl_wire.h"

/* The calls the spec marks not_api, which the host decodes by hand (gles2_dec.cpp, OP_glShaderString and
 * OP_glFinishRoundTrip). Their wire layouts are the host's: see gl_host.c. */
typedef struct gl_custom_backend {
    void (*glShaderString)(void *ctx, uint32_t shader, const void *string, uint32_t len);
    int32_t (*glFinishRoundTrip)(void *ctx);
} gl_custom_backend_t;

typedef struct gl_host {
    const renderControl_backend_t *rc;
    const gles2_backend_t *gles2;
    const gles1_backend_t *gles1;
    const gl_custom_backend_t *custom;
} gl_host_t;

/* The null host: renderControl and GLES answered without a GPU (gl_null.c). */
const gl_host_t *gl_host_null(void);

/* Decodes one request (opcode op, total length len, header included) for the stream ctx, and appends its
 * reply to rep. Returns 0, or -1 for an opcode no decoder handles, or a malformed request. */
int gl_host_decode(const gl_host_t *h, void *ctx, uint32_t op, const uint8_t *pkt, size_t len, gl_reply *rep);

#endif
