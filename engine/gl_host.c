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

/* The calls the spec marks not_api (66 in the GLES1 and GLES2 protocols, gen_report.txt). The generator leaves their
 * decoders out, and an undecoded request stops the goldfish pipe (B16: an undecoded draw, glDrawElementsInstancedOffsetAEMU,
 * arrived when SystemUI's render thread started drawing, and the thread waited for a reply that never came). The null backend
 * draws nothing, so each packet is consumed by its fixed size (the guest data after it is ignored), and the reply is the return
 * value's size: zero, except for the sync handles (nonzero: a fence is a fence), GL_TRUE for glIsSyncAEMU and
 * glBufferDataSyncAEMU, and GL_CONDITION_SATISFIED for glClientWaitSyncAEMU. The fixed sizes include the 8-byte header. */
typedef struct {
    uint32_t op;
    uint16_t fixed;
    uint8_t ret; /* the reply's size in bytes (0: void) */
    const char *name;
} not_api_t;

static const not_api_t not_api[] = {
    {2249, 33, 0, "void"}, /* glVertexAttribPointerData */
    {2250, 29, 0, "void"}, /* glVertexAttribPointerOffset */
    {2251, 24, 0, "void"}, /* glDrawElementsOffset */
    {2252, 28, 0, "void"}, /* glDrawElementsData */
    {2253, 16, 0, "void"}, /* glGetCompressedTextureFormats */
    {2263, 28, 0, "void"}, /* glMapBufferRangeAEMU */
    {2264, 32, 0, "void"}, /* glUnmapBufferAEMU */
    {2265, 28, 0, "void"}, /* glFlushMappedBufferRangeAEMU */
    {2266, 36, 0, "void"}, /* glReadPixelsOffsetAEMU */
    {2267, 40, 0, "void"}, /* glCompressedTexImage2DOffsetAEMU */
    {2268, 44, 0, "void"}, /* glCompressedTexSubImage2DOffsetAEMU */
    {2269, 44, 0, "void"}, /* glTexImage2DOffsetAEMU */
    {2270, 44, 0, "void"}, /* glTexSubImage2DOffsetAEMU */
    {2283, 28, 0, "void"}, /* glGetUniformIndicesAEMU */
    {2307, 28, 0, "void"}, /* glVertexAttribIPointerOffsetAEMU */
    {2308, 32, 0, "void"}, /* glVertexAttribIPointerDataAEMU */
    {2314, 32, 0, "void"}, /* glDrawElementsInstancedDataAEMU */
    {2315, 28, 0, "void"}, /* glDrawElementsInstancedOffsetAEMU */
    {2317, 36, 0, "void"}, /* glDrawRangeElementsDataAEMU */
    {2318, 32, 0, "void"}, /* glDrawRangeElementsOffsetAEMU */
    {2325, 16, 8, "uint64_t"}, /* glFenceSyncAEMU */
    {2326, 28, 4, "GLenum"}, /* glClientWaitSyncAEMU */
    {2327, 28, 0, "void"}, /* glWaitSyncAEMU */
    {2328, 16, 0, "void"}, /* glDeleteSyncAEMU */
    {2329, 16, 1, "GLboolean"}, /* glIsSyncAEMU */
    {2330, 32, 0, "void"}, /* glGetSyncivAEMU */
    {2349, 28, 0, "void"}, /* glTransformFeedbackVaryingsAEMU */
    {2376, 48, 0, "void"}, /* glTexImage3DOffsetAEMU */
    {2379, 52, 0, "void"}, /* glTexSubImage3DOffsetAEMU */
    {2381, 44, 0, "void"}, /* glCompressedTexImage3DOffsetAEMU */
    {2383, 52, 0, "void"}, /* glCompressedTexSubImage3DOffsetAEMU */
    {2399, 24, 4, "GLuint"}, /* glCreateShaderProgramvAEMU */
    {2447, 20, 0, "void"}, /* glDrawArraysIndirectDataAEMU */
    {2448, 16, 0, "void"}, /* glDrawArraysIndirectOffsetAEMU */
    {2450, 24, 0, "void"}, /* glDrawElementsIndirectDataAEMU */
    {2451, 20, 0, "void"}, /* glDrawElementsIndirectOffsetAEMU */
    {2459, 32, 0, "void"}, /* glMapBufferRangeDMA */
    {2460, 36, 0, "void"}, /* glUnmapBufferDMA */
    {2461, 32, 8, "uint64_t"}, /* glMapBufferRangeDirect */
    {2462, 44, 0, "void"}, /* glUnmapBufferDirect */
    {2463, 24, 0, "void"}, /* glFlushMappedBufferRangeDirect */
    {2470, 24, 0, "void"}, /* glDrawElementsOffsetNullAEMU */
    {2471, 28, 0, "void"}, /* glDrawElementsDataNullAEMU */
    {2472, 32, 0, "void"}, /* glUnmapBufferAsyncAEMU */
    {2473, 28, 0, "void"}, /* glFlushMappedBufferRangeAEMU2 */
    {2474, 24, 1, "GLboolean"}, /* glBufferDataSyncAEMU */
    {1168, 24, 0, "void"}, /* glVertexPointerOffset */
    {1169, 24, 0, "void"}, /* glColorPointerOffset */
    {1170, 20, 0, "void"}, /* glNormalPointerOffset */
    {1171, 20, 0, "void"}, /* glPointSizePointerOffset */
    {1172, 24, 0, "void"}, /* glTexCoordPointerOffset */
    {1173, 24, 0, "void"}, /* glWeightPointerOffset */
    {1174, 24, 0, "void"}, /* glMatrixIndexPointerOffset */
    {1175, 28, 0, "void"}, /* glVertexPointerData */
    {1176, 28, 0, "void"}, /* glColorPointerData */
    {1177, 24, 0, "void"}, /* glNormalPointerData */
    {1178, 32, 0, "void"}, /* glTexCoordPointerData */
    {1179, 24, 0, "void"}, /* glPointSizePointerData */
    {1180, 28, 0, "void"}, /* glWeightPointerData */
    {1181, 28, 0, "void"}, /* glMatrixIndexPointerData */
    {1182, 24, 0, "void"}, /* glDrawElementsOffset */
    {1183, 28, 0, "void"}, /* glDrawElementsData */
    {1184, 16, 0, "void"}, /* glGetCompressedTextureFormats */
    {1185, 8, 4, "int"}, /* glFinishRoundTrip */
};

static uint64_t next_sync = 1; /* the handle of the next glFenceSyncAEMU */

static int not_api_decode(uint32_t op, size_t len, gl_reply *rep)
{
    const not_api_t *e = NULL;
    for (size_t i = 0; i < sizeof not_api / sizeof not_api[0] && !e; i++)
        if (not_api[i].op == op)
            e = &not_api[i];
    if (!e)
        return 1; /* not a not_api call */
    if (len < e->fixed)
        return -1;
    if (e->ret == 0)
        return 0;
    uint64_t v = 0;
    if (op == 2325) /* glFenceSyncAEMU */
        v = next_sync++;
    else if (op == 2326) /* glClientWaitSyncAEMU: GL_CONDITION_SATISFIED */
        v = 0x911C;
    else if (op == 2329 || op == 2474) /* glIsSyncAEMU, glBufferDataSyncAEMU: GL_TRUE */
        v = 1;
    uint8_t out[8];
    for (uint8_t i = 0; i < e->ret; i++)
        out[i] = (uint8_t)(v >> (8 * i));
    return gl_reply_put(rep, out, e->ret) == 0 ? 0 : -1;
}

static int is_custom(uint32_t op)
{
    return op == OP_glShaderString || op == OP_glFinishRoundTrip || op == OP_glVertexAttribPointerData ||
           op == OP_glVertexAttribPointerOffset || op == OP_glDrawElementsData || op == OP_glDrawElementsOffset;
}

int gl_host_decode(const gl_host_t *h, void *ctx, uint32_t op, const uint8_t *pkt, size_t len, gl_reply *rep)
{
    /* The custom calls first (the WebGL backend draws some of the not_api ones), then the rest of not_api. */
    if (is_custom(op))
        return custom_decode(h, ctx, op, pkt, len, rep);
    int na = not_api_decode(op, len, rep);
    if (na != 1)
        return na;
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
