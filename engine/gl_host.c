#include <stdio.h>
#include <string.h>

#include "gl_host.h"
#include "goldfish.h"
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

/* Each not_api call the stream carries, once (the ones not_api_data decodes are passed to the backend's custom entries,
 * the others are consumed). */
static void gl_note_not_api(const char *name)
{
    static const char *seen[96];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == name)
            return;
    if (nseen < (int)(sizeof seen / sizeof seen[0]))
        seen[nseen++] = name;
    fprintf(stderr, "xiaoji: gl: not_api call %s\n", name);
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
 * arrived when SystemUI's render thread started drawing, and the thread waited for a reply that never came). The calls with
 * data or out-parameters are decoded exactly by not_api_data (B17) and go to the backend's custom entries. The rest are
 * consumed by their fixed size, and the reply is the return value's size: zero, except for the sync handles (nonzero: a
 * fence is a fence), GL_TRUE for glIsSyncAEMU, and GL_CONDITION_SATISFIED for glClientWaitSyncAEMU. The fixed sizes include
 * the 8-byte header. */
typedef struct {
    uint32_t op;
    uint16_t fixed;
    uint8_t ret; /* the reply's size in bytes (0: void) */
    const char *name; /* the call */
} not_api_t;

static const not_api_t not_api[] = {
    {2249, 33, 0, "glVertexAttribPointerData"},
    {2250, 29, 0, "glVertexAttribPointerOffset"},
    {2251, 24, 0, "glDrawElementsOffset"},
    {2252, 28, 0, "glDrawElementsData"},
    {2253, 16, 0, "glGetCompressedTextureFormats"},
    {2263, 28, 0, "glMapBufferRangeAEMU"},
    {2264, 32, 0, "glUnmapBufferAEMU"},
    {2265, 28, 0, "glFlushMappedBufferRangeAEMU"},
    {2266, 36, 0, "glReadPixelsOffsetAEMU"},
    {2267, 40, 0, "glCompressedTexImage2DOffsetAEMU"},
    {2268, 44, 0, "glCompressedTexSubImage2DOffsetAEMU"},
    {2269, 44, 0, "glTexImage2DOffsetAEMU"},
    {2270, 44, 0, "glTexSubImage2DOffsetAEMU"},
    {2283, 28, 0, "glGetUniformIndicesAEMU"},
    {2307, 28, 0, "glVertexAttribIPointerOffsetAEMU"},
    {2308, 32, 0, "glVertexAttribIPointerDataAEMU"},
    {2314, 32, 0, "glDrawElementsInstancedDataAEMU"},
    {2315, 28, 0, "glDrawElementsInstancedOffsetAEMU"},
    {2317, 36, 0, "glDrawRangeElementsDataAEMU"},
    {2318, 32, 0, "glDrawRangeElementsOffsetAEMU"},
    {2325, 16, 8, "glFenceSyncAEMU"},
    {2326, 28, 4, "glClientWaitSyncAEMU"},
    {2327, 28, 0, "glWaitSyncAEMU"},
    {2328, 16, 0, "glDeleteSyncAEMU"},
    {2329, 16, 1, "glIsSyncAEMU"},
    {2330, 32, 0, "glGetSyncivAEMU"},
    {2349, 28, 0, "glTransformFeedbackVaryingsAEMU"},
    {2376, 48, 0, "glTexImage3DOffsetAEMU"},
    {2379, 52, 0, "glTexSubImage3DOffsetAEMU"},
    {2381, 44, 0, "glCompressedTexImage3DOffsetAEMU"},
    {2383, 52, 0, "glCompressedTexSubImage3DOffsetAEMU"},
    {2399, 24, 4, "glCreateShaderProgramvAEMU"},
    {2447, 20, 0, "glDrawArraysIndirectDataAEMU"},
    {2448, 16, 0, "glDrawArraysIndirectOffsetAEMU"},
    {2450, 24, 0, "glDrawElementsIndirectDataAEMU"},
    {2451, 20, 0, "glDrawElementsIndirectOffsetAEMU"},
    {2459, 32, 0, "glMapBufferRangeDMA"},
    {2460, 36, 0, "glUnmapBufferDMA"},
    {2461, 32, 8, "glMapBufferRangeDirect"},
    {2462, 44, 0, "glUnmapBufferDirect"},
    {2463, 24, 0, "glFlushMappedBufferRangeDirect"},
    {2470, 24, 0, "glDrawElementsOffsetNullAEMU"},
    {2471, 28, 0, "glDrawElementsDataNullAEMU"},
    {2472, 32, 0, "glUnmapBufferAsyncAEMU"},
    {2473, 28, 0, "glFlushMappedBufferRangeAEMU2"},
    {2474, 24, 1, "glBufferDataSyncAEMU"},
    {1168, 24, 0, "glVertexPointerOffset"},
    {1169, 24, 0, "glColorPointerOffset"},
    {1170, 20, 0, "glNormalPointerOffset"},
    {1171, 20, 0, "glPointSizePointerOffset"},
    {1172, 24, 0, "glTexCoordPointerOffset"},
    {1173, 24, 0, "glWeightPointerOffset"},
    {1174, 24, 0, "glMatrixIndexPointerOffset"},
    {1175, 28, 0, "glVertexPointerData"},
    {1176, 28, 0, "glColorPointerData"},
    {1177, 24, 0, "glNormalPointerData"},
    {1178, 32, 0, "glTexCoordPointerData"},
    {1179, 24, 0, "glPointSizePointerData"},
    {1180, 28, 0, "glWeightPointerData"},
    {1181, 28, 0, "glMatrixIndexPointerData"},
    {1182, 24, 0, "glDrawElementsOffset"},
    {1183, 28, 0, "glDrawElementsData"},
    {1184, 16, 0, "glGetCompressedTextureFormats"},
    {1185, 8, 4, "glFinishRoundTrip"},
};

static uint64_t next_sync = 1; /* the handle of the next glFenceSyncAEMU */

/* A cursor over a request's arguments (after the 8-byte header). Every read is bounds-checked; `bad` records a short
 * packet. */
typedef struct {
    const uint8_t *p;
    size_t left;
    int bad;
} cur_t;

static uint32_t c32(cur_t *c)
{
    if (c->left < 4) {
        c->bad = 1;
        return 0;
    }
    uint32_t v = ld32(c->p);
    c->p += 4;
    c->left -= 4;
    return v;
}

static uint64_t c64(cur_t *c)
{
    uint64_t lo = c32(c);
    return lo | (uint64_t)c32(c) << 32;
}

/* An "in" pointer argument: its 4-byte size, then that many bytes. NULL for size 0. */
static const uint8_t *cbuf(cur_t *c, uint32_t *n)
{
    *n = c32(c);
    if (c->bad || c->left < *n) {
        c->bad = 1;
        return NULL;
    }
    const uint8_t *b = *n ? c->p : NULL;
    c->p += *n;
    c->left -= *n;
    return b;
}

/* Out-parameter replies: n bytes from src, or zeros when src is NULL. */
static int put_out(gl_reply *rep, const void *src, uint32_t n)
{
    if (src)
        return gl_reply_put(rep, src, n);
    static const uint8_t zero[256];
    while (n) {
        uint32_t k = n < sizeof zero ? n : (uint32_t)sizeof zero;
        if (gl_reply_put(rep, zero, k) != 0)
            return -1;
        n -= k;
    }
    return 0;
}

/* The sync object queries (glGetSyncivAEMU). The fences are signalled when they are made (see not_api above), so the
 * status is GL_SIGNALED. */
static int32_t sync_param(uint32_t pname)
{
    switch (pname) {
    case 0x9112: return 0x9116; /* GL_OBJECT_TYPE: GL_SYNC_FENCE */
    case 0x9113: return 0x9117; /* GL_SYNC_CONDITION: GL_SYNC_GPU_COMMANDS_COMPLETE */
    case 0x9114: return 0x9119; /* GL_SYNC_STATUS: GL_SIGNALED */
    default: return 0;          /* GL_SYNC_FLAGS, and anything else */
    }
}

/* The not_api calls whose packets carry data (the guest's bytes after a size word) or whose replies carry out-parameters.
 * The wire layout is the generated encoder's: each argument in order, scalars as 4 bytes (8 for 64-bit types), an "in"
 * pointer as its size and its bytes, an "out" pointer as its size only; the reply is each out-parameter's bytes, in order.
 * Before B17 these were consumed by their fixed size and answered with the return type's size only, so a call with an out
 * pointer (glMapBufferRangeAEMU, glUnmapBufferAEMU, glGetSyncivAEMU, glGetUniformIndicesAEMU,
 * glGetCompressedTextureFormats) left the guest waiting in its readback for bytes that never came. Returns 1 when op
 * is not one of these, 0 when decoded, -1 for a malformed packet. */
static int not_api_data(const gl_custom_backend_t *cb, void *ctx, uint32_t op, const uint8_t *pkt, size_t len,
                        gl_reply *rep)
{
    cur_t c = {pkt + 8, len - 8, 0};
    switch (op) {
    case 2253: { /* glGetCompressedTextureFormats(count, formats out): no compressed formats are advertised */
        (void)c32(&c);
        uint32_t nf = c32(&c);
        if (c.bad || c.left)
            return -1;
        return put_out(rep, NULL, nf);
    }
    case 2263: { /* glMapBufferRangeAEMU(target, offset, length, access, mapped out) */
        uint32_t target = c32(&c), offset = c32(&c), length = c32(&c), access = c32(&c), nm = c32(&c);
        if (c.bad || c.left)
            return -1;
        if (!nm)
            return 0;
        uint8_t *m = calloc(1, nm);
        if (!m)
            return -1;
        if (cb && cb->glMapBufferRangeAEMU && nm == length)
            cb->glMapBufferRangeAEMU(ctx, target, offset, length, access, m);
        int r = put_out(rep, m, nm);
        free(m);
        return r;
    }
    case 2264:   /* glUnmapBufferAEMU(target, offset, length, access, guest_buffer in, out_res out) */
    case 2472: { /* glUnmapBufferAsyncAEMU: the same, with out_res an "in" byte and no reply */
        uint32_t target = c32(&c), offset = c32(&c), length = c32(&c), access = c32(&c), nb;
        const uint8_t *b = cbuf(&c, &nb);
        uint32_t nres = c32(&c);
        if (op == 2472 && !c.bad && nres) { /* the "in" out_res: its byte follows its size */
            if (c.left < nres)
                return -1;
            c.p += nres;
            c.left -= nres;
        }
        if (c.bad || c.left)
            return -1;
        uint8_t res = 1; /* GL_TRUE */
        if (cb && cb->glUnmapBufferAEMU)
            res = cb->glUnmapBufferAEMU(ctx, target, offset, length, access, nb == length ? b : NULL);
        if (op == 2472 || !nres)
            return 0;
        if (put_out(rep, &res, 1) != 0)
            return -1;
        return nres > 1 ? put_out(rep, NULL, nres - 1) : 0;
    }
    case 2265:   /* glFlushMappedBufferRangeAEMU(target, offset, length, access, guest_buffer in) */
    case 2473: { /* glFlushMappedBufferRangeAEMU2: the same */
        uint32_t target = c32(&c), offset = c32(&c), length = c32(&c), access = c32(&c), nb;
        const uint8_t *b = cbuf(&c, &nb);
        if (c.bad || c.left)
            return -1;
        if (cb && cb->glFlushMappedBufferRangeAEMU && b && nb == length)
            cb->glFlushMappedBufferRangeAEMU(ctx, target, offset, length, access, b);
        return 0;
    }
    case 2474: { /* glBufferDataSyncAEMU(target, size, data in, usage) -> GLboolean */
        uint32_t target = c32(&c), size = c32(&c), nd;
        const uint8_t *d = cbuf(&c, &nd);
        uint32_t usage = c32(&c);
        if (c.bad || c.left || (d && nd != size))
            return -1;
        uint8_t res = 1;
        if (cb && cb->glBufferDataSyncAEMU)
            res = cb->glBufferDataSyncAEMU(ctx, target, size, d, usage);
        return put_out(rep, &res, 1);
    }
    case 2283: { /* glGetUniformIndicesAEMU(program, uniformCount, packedUniformNames in, packedLen, uniformIndices out) */
        uint32_t program = c32(&c), count = c32(&c), np;
        const uint8_t *names = cbuf(&c, &np);
        uint32_t packed_len = c32(&c), ni = c32(&c);
        if (c.bad || c.left || (uint64_t)count * 4 != ni)
            return -1;
        uint8_t *idx = malloc(ni ? ni : 1);
        if (!idx)
            return -1;
        memset(idx, 0xFF, ni); /* GL_INVALID_INDEX */
        if (cb && cb->glGetUniformIndicesAEMU && names && packed_len <= np)
            cb->glGetUniformIndicesAEMU(ctx, program, count, (const char *)names, packed_len, idx);
        int r = put_out(rep, idx, ni);
        free(idx);
        return r;
    }
    case 2330: { /* glGetSyncivAEMU(sync, pname, bufSize, length out, values out) */
        (void)c64(&c);
        uint32_t pname = c32(&c), bufsize = c32(&c), nl = c32(&c), nv = c32(&c);
        if (c.bad || c.left)
            return -1;
        int32_t v = sync_param(pname);
        uint8_t one[4] = {1, 0, 0, 0};
        uint8_t val[4] = {(uint8_t)v, (uint8_t)((uint32_t)v >> 8), (uint8_t)((uint32_t)v >> 16), (uint8_t)((uint32_t)v >> 24)};
        if (nl && (put_out(rep, bufsize ? one : NULL, 4) != 0 || (nl > 4 && put_out(rep, NULL, nl - 4) != 0)))
            return -1;
        if (nv && (put_out(rep, val, nv < 4 ? nv : 4) != 0 || (nv > 4 && put_out(rep, NULL, nv - 4) != 0)))
            return -1;
        return 0;
    }
    case 2314: { /* glDrawElementsInstancedDataAEMU(mode, count, type, indices in, primcount, datalen) */
        uint32_t mode = c32(&c), count = c32(&c), type = c32(&c), nd;
        const uint8_t *d = cbuf(&c, &nd);
        uint32_t prim = c32(&c), datalen = c32(&c);
        if (c.bad || c.left || datalen != nd)
            return -1;
        if (cb && cb->glDrawElementsInstancedDataAEMU && d)
            cb->glDrawElementsInstancedDataAEMU(ctx, mode, (int32_t)count, type, d, nd, (int32_t)prim);
        return 0;
    }
    case 2315: { /* glDrawElementsInstancedOffsetAEMU(mode, count, type, offset, primcount) */
        uint32_t mode = c32(&c), count = c32(&c), type = c32(&c), offset = c32(&c), prim = c32(&c);
        if (c.bad || c.left)
            return -1;
        if (cb && cb->glDrawElementsInstancedOffsetAEMU)
            cb->glDrawElementsInstancedOffsetAEMU(ctx, mode, (int32_t)count, type, offset, (int32_t)prim);
        return 0;
    }
    case 2317: { /* glDrawRangeElementsDataAEMU(mode, start, end, count, type, indices in, datalen) */
        uint32_t mode = c32(&c), start = c32(&c), end = c32(&c), count = c32(&c), type = c32(&c), nd;
        const uint8_t *d = cbuf(&c, &nd);
        uint32_t datalen = c32(&c);
        if (c.bad || c.left || datalen != nd)
            return -1;
        if (cb && cb->glDrawRangeElementsDataAEMU && d)
            cb->glDrawRangeElementsDataAEMU(ctx, mode, start, end, (int32_t)count, type, d, nd);
        return 0;
    }
    case 2318: { /* glDrawRangeElementsOffsetAEMU(mode, start, end, count, type, offset) */
        uint32_t mode = c32(&c), start = c32(&c), end = c32(&c), count = c32(&c), type = c32(&c), offset = c32(&c);
        if (c.bad || c.left)
            return -1;
        if (cb && cb->glDrawRangeElementsOffsetAEMU)
            cb->glDrawRangeElementsOffsetAEMU(ctx, mode, start, end, (int32_t)count, type, offset);
        return 0;
    }
    case 2307: { /* glVertexAttribIPointerOffsetAEMU(index, size, type, stride, offset) */
        uint32_t index = c32(&c), size = c32(&c), type = c32(&c), stride = c32(&c), offset = c32(&c);
        if (c.bad || c.left)
            return -1;
        if (cb && cb->glVertexAttribIPointerOffsetAEMU)
            cb->glVertexAttribIPointerOffsetAEMU(ctx, index, (int32_t)size, type, (int32_t)stride, offset);
        return 0;
    }
    case 2308: { /* glVertexAttribIPointerDataAEMU(index, size, type, stride, data in, datalen) */
        uint32_t index = c32(&c), size = c32(&c), type = c32(&c), stride = c32(&c), nd;
        const uint8_t *d = cbuf(&c, &nd);
        uint32_t datalen = c32(&c);
        if (c.bad || c.left || datalen != nd)
            return -1;
        if (cb && cb->glVertexAttribIPointerDataAEMU && d)
            cb->glVertexAttribIPointerDataAEMU(ctx, index, (int32_t)size, type, (int32_t)stride, d, nd);
        return 0;
    }
    case 2269:   /* glTexImage2DOffsetAEMU(target, level, internalformat, width, height, border, format, type, offset) */
    case 2270: { /* glTexSubImage2DOffsetAEMU(target, level, xoffset, yoffset, width, height, format, type, offset) */
        uint32_t a[9];
        for (int i = 0; i < 9; i++)
            a[i] = c32(&c);
        if (c.bad || c.left)
            return -1;
        if (op == 2269 && cb && cb->glTexImage2DOffsetAEMU)
            cb->glTexImage2DOffsetAEMU(ctx, a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4],
                                       (int32_t)a[5], a[6], a[7], a[8]);
        if (op == 2270 && cb && cb->glTexSubImage2DOffsetAEMU)
            cb->glTexSubImage2DOffsetAEMU(ctx, a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4],
                                          (int32_t)a[5], a[6], a[7], a[8]);
        return 0;
    }
    case 2266: { /* glReadPixelsOffsetAEMU(x, y, width, height, format, type, offset) */
        uint32_t a[7];
        for (int i = 0; i < 7; i++)
            a[i] = c32(&c);
        if (c.bad || c.left)
            return -1;
        if (cb && cb->glReadPixelsOffsetAEMU)
            cb->glReadPixelsOffsetAEMU(ctx, (int32_t)a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], a[4], a[5], a[6]);
        return 0;
    }
    default:
        return 1;
    }
}

static int not_api_decode(const gl_custom_backend_t *cb, void *ctx, uint32_t op, const uint8_t *pkt, size_t len,
                          gl_reply *rep)
{
    const not_api_t *e = NULL;
    for (size_t i = 0; i < sizeof not_api / sizeof not_api[0] && !e; i++)
        if (not_api[i].op == op)
            e = &not_api[i];
    if (!e)
        return 1; /* not a not_api call */
    if (len < e->fixed)
        return -1;
    gl_note_not_api(e->name);
    int d = not_api_data(cb, ctx, op, pkt, len, rep);
    if (d != 1)
        return d;
    if (e->ret == 0)
        return 0;
    uint64_t v = 0;
    if (op == 2325) /* glFenceSyncAEMU */
        v = next_sync++;
    else if (op == 2326) /* glClientWaitSyncAEMU: GL_CONDITION_SATISFIED */
        v = 0x911C;
    else if (op == 2329) /* glIsSyncAEMU: GL_TRUE */
        v = 1;
    uint8_t out[8];
    for (uint8_t i = 0; i < e->ret; i++)
        out[i] = (uint8_t)(v >> (8 * i));
    return gl_reply_put(rep, out, e->ret) == 0 ? 0 : -1;
}

/* rcUpdateColorBufferDMA (10034) and rcReadColorBufferDMA (10064): colorbuffer, x, y, width, height, format, type, the
 * pixels' guest-physical address (uint64, from the mapper's lockAndWriteDma: the address-space window, goldfish.c), and
 * pixels_size. The encoder counts a size word for the pixels in the packet's total (52 bytes) but writes none, so the
 * total is consumed and the four bytes after pixels_size are ignored. The reply is an int (0). The gralloc mapper
 * (gralloc_30.cpp) sends an update for every unlock of a buffer the CPU wrote, so software-drawn windows reach their
 * color buffers this way. */
static int dma_decode(const gl_host_t *h, void *ctx, uint32_t op, const uint8_t *pkt, size_t len, gl_reply *rep)
{
    if (len < 48)
        return -1;
    cur_t c = {pkt + 8, len - 8, 0};
    uint32_t a[7];
    for (int i = 0; i < 7; i++)
        a[i] = c32(&c);
    uint64_t phys = c64(&c);
    uint32_t size = c32(&c);
    if (c.bad)
        return -1;
    int32_t ret = 0;
    uint8_t *buf = size ? malloc(size) : NULL;
    if (size && !buf)
        return -1;
    if (op == 10034) {
        if (size && goldfish_as_dma(phys, buf, size, 0) == 0)
            ret = h->rc->rcUpdateColorBuffer(ctx, a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4], a[5],
                                             a[6], buf, size);
        else if (size)
            ret = -1;
    } else if (size) {
        memset(buf, 0, size);
        h->rc->rcReadColorBuffer(ctx, a[0], (int32_t)a[1], (int32_t)a[2], (int32_t)a[3], (int32_t)a[4], a[5], a[6], buf,
                                 size);
        if (goldfish_as_dma(phys, buf, size, 1) != 0)
            ret = -1;
    }
    free(buf);
    uint8_t r[4] = {(uint8_t)ret, (uint8_t)((uint32_t)ret >> 8), (uint8_t)((uint32_t)ret >> 16), (uint8_t)((uint32_t)ret >> 24)};
    return gl_reply_put(rep, r, 4) == 0 ? 0 : -1;
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
    int na = not_api_decode(h->custom, ctx, op, pkt, len, rep);
    if (na != 1)
        return na;
    if (op == 10034 || op == 10064)
        return dma_decode(h, ctx, op, pkt, len, rep);
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
