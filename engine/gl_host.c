#include <stdio.h>
#include <string.h>

#include "gl_host.h"
#include "gles2_opcodes.h"

void gl_note_default(const char *name)
{
    static const char *seen[512];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == name || !strcmp(seen[i], name))
            return;
    if (nseen < (int)(sizeof seen / sizeof seen[0]))
        seen[nseen++] = name;
    fprintf(stderr, "xiaoji: gl: %s answered by the default (no-op) backend\n", name);
}

static uint32_t ld32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* glShaderString: uint32 shader, uint32 string size, the string, uint32 length (the host's gles2_dec.cpp). No reply.
 * glFinishRoundTrip: no arguments; the reply is one int.
 * The addon calls (gl2_enc.cpp, the *_enc functions named below), each after the op and total words:
 *   glVertexAttribPointerData: index, size, type (uint32 each), normalized (1 byte), stride (uint32),
 *                              datalen, the data (datalen bytes, packed by glUtilsPackPointerData), datalen.
 *   glVertexAttribPointerOffset: index, size, type, normalized (1 byte), stride, offset.
 *   glDrawElementsData: mode, count, type, datalen, the data, datalen.
 *   glDrawElementsOffset: mode, count, type, offset. */
static int custom_decode(const gl_host_t *h, void *ctx, uint32_t op, const uint8_t *pkt, size_t len, gl_reply *rep)
{
    const gl_custom_backend_t *cb = h->custom;
    if (!cb)
        return -1;
    if (op == OP_glShaderString) {
        if (len < 20) /* the header, the string size and the length word */
            return -1;
        uint32_t size = ld32(pkt + 12);
        if (len != 8 + 4 + 4 + (size_t)size + 4)
            return -1;
        cb->glShaderString(ctx, ld32(pkt + 8), pkt + 16, ld32(pkt + 16 + size));
        return 0;
    }
    if (op == OP_glFinishRoundTrip) {
        if (len != 8)
            return -1;
        int32_t r = cb->glFinishRoundTrip(ctx);
        uint8_t b[4] = {(uint8_t)r, (uint8_t)((uint32_t)r >> 8), (uint8_t)((uint32_t)r >> 16), (uint8_t)((uint32_t)r >> 24)};
        return gl_reply_put(rep, b, 4) == 0 ? 0 : -1;
    }
    if (op == OP_glVertexAttribPointerData) {
        if (len < 33)
            return -1;
        uint32_t datalen = ld32(pkt + 25);
        if ((uint64_t)len != 33 + (uint64_t)datalen || ld32(pkt + 29 + datalen) != datalen)
            return -1;
        cb->glVertexAttribPointerData(ctx, ld32(pkt + 8), (int32_t)ld32(pkt + 12), ld32(pkt + 16), pkt[20],
                                      (int32_t)ld32(pkt + 21), pkt + 29, datalen);
        return 0;
    }
    if (op == OP_glVertexAttribPointerOffset) {
        if (len != 29)
            return -1;
        cb->glVertexAttribPointerOffset(ctx, ld32(pkt + 8), (int32_t)ld32(pkt + 12), ld32(pkt + 16), pkt[20],
                                        (int32_t)ld32(pkt + 21), ld32(pkt + 25));
        return 0;
    }
    if (op == OP_glDrawElementsData) {
        if (len < 28)
            return -1;
        uint32_t datalen = ld32(pkt + 20);
        if ((uint64_t)len != 28 + (uint64_t)datalen || ld32(pkt + 24 + datalen) != datalen)
            return -1;
        cb->glDrawElementsData(ctx, ld32(pkt + 8), (int32_t)ld32(pkt + 12), ld32(pkt + 16), pkt + 24, datalen);
        return 0;
    }
    if (op == OP_glDrawElementsOffset) {
        if (len != 24)
            return -1;
        cb->glDrawElementsOffset(ctx, ld32(pkt + 8), (int32_t)ld32(pkt + 12), ld32(pkt + 16), ld32(pkt + 20));
        return 0;
    }
    return -1;
}

static int is_custom(uint32_t op)
{
    return op == OP_glShaderString || op == OP_glFinishRoundTrip || op == OP_glVertexAttribPointerData ||
           op == OP_glVertexAttribPointerOffset || op == OP_glDrawElementsData || op == OP_glDrawElementsOffset;
}

int gl_host_decode(const gl_host_t *h, void *ctx, uint32_t op, const uint8_t *pkt, size_t len, gl_reply *rep)
{
    if (is_custom(op))
        return custom_decode(h, ctx, op, pkt, len, rep);
    if (op >= OP_FIRST_renderControl && op <= OP_LAST_renderControl)
        return renderControl_decode(h->rc, ctx, pkt, len, rep);
    if (op >= OP_FIRST_gles2 && op <= OP_LAST_gles2)
        return gles2_decode(h->gles2, ctx, pkt, len, rep);
    if (op >= OP_FIRST_gles1 && op <= OP_LAST_gles1)
        return gles1_decode(h->gles1, ctx, pkt, len, rep);
    return -1;
}

/* The backend selection (--gl). */
static int want_webgl;

int gl_host_select(const char *name)
{
    if (!strcmp(name, "null")) {
        want_webgl = 0;
        return 0;
    }
    if (!strcmp(name, "webgl")) {
        want_webgl = 1;
        return 0;
    }
    return -1;
}

const gl_host_t *gl_host_current(void)
{
    if (want_webgl) {
        const gl_host_t *w = gl_host_webgl();
        if (w)
            return w;
    }
    return gl_host_null();
}
