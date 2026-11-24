/* goldfish-gl (B17): the GLES calls the protocol marks not_api that carry out-parameters or data. The guest's encoder
 * (gl2_enc.cpp, generated from gles2.in and gles2.attrib) sends each argument in order, an "in" pointer as its size and
 * its bytes and an "out" pointer as its size alone, and then reads back each out-parameter's bytes. Before B17 the engine
 * answered only the return type, so a map (glMapBufferRangeAEMU), an unmap (glUnmapBufferAEMU, whose out_res is one
 * byte), glGetSyncivAEMU, glGetUniformIndicesAEMU and glGetCompressedTextureFormats left the guest thread waiting in
 * its read for bytes that never came (gl2_enc.cpp reads them back unconditionally). The layouts were checked against the
 * product's gl2_enc.cpp and GL2Encoder.cpp.
 *
 * Each line is one request: the bytes written and the reply's length and content, read with a nonblocking read
 * (-11, EAGAIN: no reply, as the protocol has none). A final rcGetRendererVersion shows the stream still decodes. The
 * values are the host's (gfxstream's GLESv2Decoder and the null backend: a mapped range reads as zeros, an unmap
 * returns GL_TRUE, a fence is signalled). There is no goldfish device in the builder VM, so no native oracle: the
 * expectation is tests/expect/goldfish-gl.out. */
#include "sys.h"

#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_READ 63
#define SYS_IOCTL 29
#define AT_FDCWD (-100)
#define O_RDWR 2
#define O_NONBLOCK 04000

static i64 sys_openat(const char *path, int flags) { return sc6(SYS_OPENAT, AT_FDCWD, (i64)path, flags, 0, 0, 0); }
static i64 sys_read(int fd, void *buf, size_t n) { return sc6(SYS_READ, fd, (i64)buf, (i64)n, 0, 0, 0); }
static i64 sys_close(int fd) { return sc6(SYS_CLOSE, fd, 0, 0, 0, 0, 0); }
static i64 sys_ioctl(int fd, u32 cmd, void *arg) { return sc6(SYS_IOCTL, fd, cmd, (i64)arg, 0, 0, 0); }

static u64 get64(const u8 *p)
{
    u64 v = 0;
    for (int i = 0; i < 8; i++)
        v |= (u64)p[i] << (8 * i);
    return v;
}

static u8 req[256];
static u32 rlen;

static void begin(u32 op)
{
    rlen = 8;
    for (int i = 0; i < 4; i++)
        req[i] = (u8)(op >> (8 * i));
}

static void w32(u32 v)
{
    for (int i = 0; i < 4; i++)
        req[rlen++] = (u8)(v >> (8 * i));
}

static void wbytes(const void *b, u32 n)
{
    w32(n);
    for (u32 i = 0; i < n; i++)
        req[rlen++] = ((const u8 *)b)[i];
}

/* Sends the request and prints the reply: its length, then its bytes in hex (at most 24). */
static void call(int fd, const char *tag)
{
    for (int i = 0; i < 4; i++)
        req[4 + i] = (u8)(rlen >> (8 * i));
    i64 w = sys_write(fd, req, rlen);
    u8 rep[128];
    i64 r = sys_read(fd, rep, sizeof rep);
    put(tag);
    put(" write ");
    putd(w);
    put(" reply ");
    putd(r);
    for (i64 i = 0; i < r && i < 24; i++) {
        static const char h[] = "0123456789abcdef";
        char b[4] = {' ', h[rep[i] >> 4], h[rep[i] & 15], 0};
        put(b);
    }
    put("\n");
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    int fd = (int)sys_openat("/dev/goldfish_pipe", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        put("open failed\n");
        return 1;
    }
    static const char svc[] = "pipe:opengles";
    sys_write(fd, svc, sizeof svc);
    static const u8 flags[4] = {0, 0, 0, 0};
    sys_write(fd, flags, 4);

    /* glMapBufferRangeAEMU(GL_ARRAY_BUFFER, 0, 16, GL_MAP_READ_BIT, mapped out 16): 16 bytes come back. */
    begin(2263);
    w32(0x8892);
    w32(0);
    w32(16);
    w32(0x1);
    w32(16);
    call(fd, "glMapBufferRangeAEMU read 16");

    /* The same with a NULL mapped (size 0, the write-invalidate case): no reply. */
    begin(2263);
    w32(0x8892);
    w32(0);
    w32(16);
    w32(0x2 | 0x4);
    w32(0);
    call(fd, "glMapBufferRangeAEMU null");

    /* glUnmapBufferAEMU(target, 0, 8, GL_MAP_WRITE_BIT, guest_buffer in 8, out_res out 1): GL_TRUE. */
    static const u8 data8[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    begin(2264);
    w32(0x8892);
    w32(0);
    w32(8);
    w32(0x2);
    wbytes(data8, 8);
    w32(1);
    call(fd, "glUnmapBufferAEMU");

    /* glFlushMappedBufferRangeAEMU(target, 4, 4, access, guest_buffer in 4): no reply. */
    begin(2265);
    w32(0x8892);
    w32(4);
    w32(4);
    w32(0x2 | 0x10);
    wbytes(data8, 4);
    call(fd, "glFlushMappedBufferRangeAEMU");

    /* glUnmapBufferAsyncAEMU: out_res is an "in" byte, so no reply. */
    begin(2472);
    w32(0x8892);
    w32(0);
    w32(8);
    w32(0x2);
    wbytes(data8, 8);
    {
        u8 res = 0;
        wbytes(&res, 1);
    }
    call(fd, "glUnmapBufferAsyncAEMU");

    /* glBufferDataSyncAEMU(target, 8, data in 8, GL_STATIC_DRAW): GL_TRUE. */
    begin(2474);
    w32(0x8892);
    w32(8);
    wbytes(data8, 8);
    w32(0x88E4);
    call(fd, "glBufferDataSyncAEMU");

    /* glFenceSyncAEMU, then glGetSyncivAEMU(sync, GL_SYNC_STATUS, 1, length out 4, values out 4): 1, GL_SIGNALED. */
    begin(2325);
    w32(0x9117);
    w32(0);
    call(fd, "glFenceSyncAEMU");
    begin(2330);
    w32(1);
    w32(0);
    w32(0x9114);
    w32(1);
    w32(4);
    w32(4);
    call(fd, "glGetSyncivAEMU status");
    begin(2330);
    w32(1);
    w32(0);
    w32(0x9112);
    w32(1);
    w32(0);
    w32(4);
    call(fd, "glGetSyncivAEMU type, no length");

    /* glGetUniformIndicesAEMU(program 0, 2, "a;b;" in, 4, indices out 8): GL_INVALID_INDEX twice (no program). */
    begin(2283);
    w32(0);
    w32(2);
    wbytes("a;b;", 4);
    w32(4);
    w32(8);
    call(fd, "glGetUniformIndicesAEMU");

    /* glGetCompressedTextureFormats(2, formats out 8): zeros. */
    begin(2253);
    w32(2);
    w32(8);
    call(fd, "glGetCompressedTextureFormats");

    /* The draws and uploads with data or a buffer offset: no reply. */
    static const u8 idx[6] = {0, 0, 1, 0, 2, 0};
    begin(2314);
    w32(4);
    w32(3);
    w32(0x1403);
    wbytes(idx, 6);
    w32(2);
    w32(6);
    call(fd, "glDrawElementsInstancedDataAEMU");
    begin(2315);
    w32(4);
    w32(3);
    w32(0x1403);
    w32(0);
    w32(2);
    call(fd, "glDrawElementsInstancedOffsetAEMU");
    begin(2317);
    w32(4);
    w32(0);
    w32(2);
    w32(3);
    w32(0x1403);
    wbytes(idx, 6);
    w32(6);
    call(fd, "glDrawRangeElementsDataAEMU");
    begin(2307);
    w32(1);
    w32(2);
    w32(0x1404);
    w32(0);
    w32(0);
    call(fd, "glVertexAttribIPointerOffsetAEMU");
    begin(2269);
    for (int i = 0; i < 9; i++)
        w32(i == 0 ? 0x0DE1 : 0);
    call(fd, "glTexImage2DOffsetAEMU");

    /* rcUpdateColorBufferDMA and rcReadColorBufferDMA (renderControl 10034 and 10064): the pixels are named by their
     * guest-physical address in the address-space window (gralloc_30.cpp: the block's physical address). The packet
     * is 52 bytes: the encoder counts a size word for the pixels that it does not write. The reply is an int, 0; an
     * address outside the window is -1. */
    begin(10012); /* rcCreateColorBuffer(16, 16, GL_RGBA) */
    w32(16);
    w32(16);
    w32(0x1908);
    call(fd, "rcCreateColorBuffer");
    int as = (int)sys_openat("/dev/goldfish_address_space", O_RDWR);
    u8 ping[40] = {0};
    ping[16] = 5;
    ping[24] = 40;
    sys_ioctl(as, (3u << 30) | (40u << 16) | ('G' << 8) | 12, ping);
    u8 al[24] = {0};
    al[1] = 4; /* 1024 bytes: 16 x 16 RGBA */
    sys_ioctl(as, (3u << 30) | (24u << 16) | ('G' << 8) | 10, al);
    u64 phys = get64(al + 16);
    for (int op = 0; op < 3; op++) {
        begin(op == 2 ? 10064 : 10034);
        w32(1); /* the first color buffer's handle */
        w32(0);
        w32(0);
        w32(16);
        w32(16);
        w32(0x1908);
        w32(0x1401);
        u64 p = op == 1 ? 0 : phys;
        w32((u32)p);
        w32((u32)(p >> 32));
        w32(1024);
        w32(0); /* the size word the encoder counts but does not write */
        call(fd, op == 0 ? "rcUpdateColorBufferDMA" : op == 1 ? "rcUpdateColorBufferDMA outside the window" : "rcReadColorBufferDMA");
    }
    sys_close(as);

    /* The stream still decodes after all of them. */
    begin(10000);
    call(fd, "rcGetRendererVersion");
    sys_close(fd);
    put("done\n");
    return 0;
}
