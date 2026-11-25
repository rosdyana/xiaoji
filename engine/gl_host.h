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
    /* The not_api calls with data or out-parameters (gl_host.c, not_api_data). Each may be NULL: the call is then
     * consumed, an out-parameter is answered with zeros (GL_INVALID_INDEX for the uniform indices), and the booleans
     * with GL_TRUE. Map fills `mapped` (length bytes, zeroed first) with the range's contents; unmap writes guest_buffer
     * (NULL when the guest sent none) and returns the result; flush writes the range. The Async and AEMU2 forms use the
     * same entries. */
    void (*glMapBufferRangeAEMU)(void *ctx, uint32_t target, uint32_t offset, uint32_t length, uint32_t access,
                                 void *mapped);
    uint8_t (*glUnmapBufferAEMU)(void *ctx, uint32_t target, uint32_t offset, uint32_t length, uint32_t access,
                                 const void *guest_buffer);
    void (*glFlushMappedBufferRangeAEMU)(void *ctx, uint32_t target, uint32_t offset, uint32_t length, uint32_t access,
                                         const void *guest_buffer);
    uint8_t (*glBufferDataSyncAEMU)(void *ctx, uint32_t target, uint32_t size, const void *data, uint32_t usage);
    /* names: packedLen bytes of names separated by ';' (the guest's packVarNames); indices: count little-endian words. */
    void (*glGetUniformIndicesAEMU)(void *ctx, uint32_t program, uint32_t count, const char *names, uint32_t packed_len,
                                    uint8_t *indices);
    void (*glDrawElementsInstancedDataAEMU)(void *ctx, uint32_t mode, int32_t count, uint32_t type, const void *data,
                                            uint32_t datalen, int32_t primcount);
    void (*glDrawElementsInstancedOffsetAEMU)(void *ctx, uint32_t mode, int32_t count, uint32_t type, uint32_t offset,
                                              int32_t primcount);
    void (*glDrawRangeElementsDataAEMU)(void *ctx, uint32_t mode, uint32_t start, uint32_t end, int32_t count,
                                        uint32_t type, const void *data, uint32_t datalen);
    void (*glDrawRangeElementsOffsetAEMU)(void *ctx, uint32_t mode, uint32_t start, uint32_t end, int32_t count,
                                          uint32_t type, uint32_t offset);
    void (*glVertexAttribIPointerDataAEMU)(void *ctx, uint32_t index, int32_t size, uint32_t type, int32_t stride,
                                           const void *data, uint32_t datalen);
    void (*glVertexAttribIPointerOffsetAEMU)(void *ctx, uint32_t index, int32_t size, uint32_t type, int32_t stride,
                                             uint32_t offset);
    void (*glTexImage2DOffsetAEMU)(void *ctx, uint32_t target, int32_t level, int32_t internalformat, int32_t width,
                                   int32_t height, int32_t border, uint32_t format, uint32_t type, uint32_t offset);
    void (*glTexSubImage2DOffsetAEMU)(void *ctx, uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset,
                                      int32_t width, int32_t height, uint32_t format, uint32_t type, uint32_t offset);
    void (*glReadPixelsOffsetAEMU)(void *ctx, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format,
                                   uint32_t type, uint32_t offset);
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

/* Snapshots (docs/snapshot.md): the WebGL backend turns its WebGL objects into a blob in engine memory before the image
 * is taken, and makes them again after a restore (then posts the last frame again). Nothing in the other builds, whose
 * GL state is all in engine memory. resume: 1 when it restored the WebGL objects, 0 when there were none, -1 on failure. */
void gl_host_snap_save(void);
int gl_host_snap_resume(void);

/* The backend the next request goes to: the selected one, or null when the selected one is not available. */
const gl_host_t *gl_host_current(void);

int gl_host_decode(const gl_host_t *h, void *ctx, uint32_t op, const uint8_t *pkt, size_t len, gl_reply *rep);

/* The last opcodes decoded (a ring of GL_HOST_NOPS, gl_host_nops counts them all): the WebGL backend's --debug-faults
 * reports name the calls before a GL error with them. */
#define GL_HOST_NOPS 16
extern uint32_t gl_host_ops[GL_HOST_NOPS];
extern uint32_t gl_host_nops;

#endif
