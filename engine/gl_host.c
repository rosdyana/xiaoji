/* The GL host dispatch (gl_host.h). The opcode ranges are the protocols' (gl_ranges.h); the decoders are
 * generated. gl_note_default is called by a default backend call (gl_defaults.c): it logs each call name
 * once, so the log lists the calls that still answer by default (the worklist for the backends). */
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
 * glFinishRoundTrip: no arguments; the reply is one int. */
static int custom_decode(const gl_host_t *h, void *ctx, uint32_t op, const uint8_t *pkt, size_t len, gl_reply *rep)
{
    if (!h->custom)
        return -1;
    if (op == OP_glShaderString) {
        if (len < 20) /* the header, the string size and the length word */
            return -1;
        uint32_t size = ld32(pkt + 12);
        if (len != 8 + 4 + 4 + (size_t)size + 4)
            return -1;
        h->custom->glShaderString(ctx, ld32(pkt + 8), pkt + 16, ld32(pkt + 16 + size));
        return 0;
    }
    if (op == OP_glFinishRoundTrip) {
        if (len != 8)
            return -1;
        int32_t r = h->custom->glFinishRoundTrip(ctx);
        uint8_t b[4] = {(uint8_t)r, (uint8_t)((uint32_t)r >> 8), (uint8_t)((uint32_t)r >> 16), (uint8_t)((uint32_t)r >> 24)};
        return gl_reply_put(rep, b, 4) == 0 ? 0 : -1;
    }
    return -1;
}

int gl_host_decode(const gl_host_t *h, void *ctx, uint32_t op, const uint8_t *pkt, size_t len, gl_reply *rep)
{
    if (op == OP_glShaderString || op == OP_glFinishRoundTrip)
        return custom_decode(h, ctx, op, pkt, len, rep);
    if (op >= OP_FIRST_renderControl && op <= OP_LAST_renderControl)
        return renderControl_decode(h->rc, ctx, pkt, len, rep);
    if (op >= OP_FIRST_gles2 && op <= OP_LAST_gles2)
        return gles2_decode(h->gles2, ctx, pkt, len, rep);
    if (op >= OP_FIRST_gles1 && op <= OP_LAST_gles1)
        return gles1_decode(h->gles1, ctx, pkt, len, rep);
    return -1;
}
