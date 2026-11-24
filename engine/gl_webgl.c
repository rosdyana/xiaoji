/* The WebGL2 backend (docs/graphics.md, G2): the guest's GLES 3.0 and renderControl calls on one WebGL2 context, in
 * the web build only. The engine runs synchronously in a Web Worker, which creates an OffscreenCanvas and passes it to
 * the module as Module.canvas (web/worker.js); emscripten's GL then provides the GLES 3 entry points.
 *
 * The model (docs/graphics.md, G2):
 * - Every guest EGL context is a gctx_t. They all run on the one WebGL2 context, so the host state a context owns
 *   (its framebuffer, program, VAO, active texture unit and the texture on it, viewport and scissor) is tracked per
 *   gctx_t and rebound when a stream switches to it (apply). Other state (enables, blend, the other texture units)
 *   is shared and is not switched: a known gap (docs/graphics.md, G2).
 * - Guest object names are host names: the host allocates them (glGen*) and the guest keeps them. Share groups
 *   therefore need no table here; every context sees every object by its name. A texture or renderbuffer made by
 *   EGLImage or rcBindTexture aliases a color buffer, and the alias table maps its name to that buffer.
 * - A color buffer is an RGBA8 texture with an FBO on it. A window surface is a color buffer; a guest's
 *   framebuffer 0 maps to the FBO of its draw surface's color buffer.
 * - rcFBPost reads the composed color buffer back and posts it to the page (xj_frame_send), when the page has
 *   consumed the last frame (a counter in a shared Int32Array: frames posted and frames consumed). Otherwise the
 *   frame is dropped before the read.
 * Compiled only with XIAOJI_WEB (the web build); the native and Node builds get gl_host_webgl() == NULL. */
#include "gl_host.h"

#ifdef XIAOJI_WEB

#include <emscripten.h>
#include <GLES3/gl3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl_display.h"

/* Constants the GLES 3 header does not give (EXT and OES names). */
#define XJ_TEXTURE_EXTERNAL_OES 0x8D65
#define XJ_NUM_EXTENSIONS 0x821D
#define XJ_MAJOR_VERSION 0x821B
#define XJ_MINOR_VERSION 0x821C

#define NSTREAM 64
#define NCTX 64
#define NSURF 64
#define NCB 256
#define NIMG 128
#define NALIAS 256
#define NATTR 16

/* The strings and the extension list, the same as gl_null.c's (the extensions the design asks for, and
 * GL_OES_EGL_image_external_essl3: external textures are 2D here, see gles2 shader translation below). */
static const char wgl_vendor[] = "xiaoji";
static const char wgl_renderer[] = "xiaoji WebGL2";
static const char wgl_version[] = "OpenGL ES 3.0 xiaoji-webgl2";
static const char wgl_extensions[] =
    "GL_OES_EGL_image_external_essl3 "
    "ANDROID_EMU_gles_max_version_3_0 ANDROID_EMU_native_sync_v2 ANDROID_EMU_gles_no_host_error";
static const char wgl_egl_extensions[] = "EGL_ANDROID_native_fence_sync";

typedef struct gctx {
    uint32_t handle;
    int used;
    uint32_t share;
    uint32_t draw, read; /* window surface handles, 0: none */
    GLuint fbo;          /* the guest's framebuffer binding: 0 is the draw surface's color buffer */
    GLuint program, vao;
    GLuint rbo;          /* the guest's renderbuffer binding */
    GLuint unit;         /* the active texture unit */
    GLuint tex[32];      /* the guest's GL_TEXTURE_2D binding on each unit (a guest name; aliases map it) */
    GLint vp[4], sc[4];  /* viewport and scissor box */
} gctx_t;

typedef struct surf {
    uint32_t handle;
    int used;
    uint32_t w, h;
    uint32_t cb; /* the color buffer (0 until one is set or needed) */
} surf_t;

typedef struct cb {
    uint32_t handle;
    int used;
    uint32_t w, h;
    GLuint tex, fbo;
} cb_t;

typedef struct alias {
    GLuint name; /* the guest's texture or renderbuffer name */
    uint32_t cb; /* the color buffer it stands for (0: src) */
    GLuint src;  /* or the guest texture it stands for (a GL_TEXTURE_2D EGLImage) */
    int used;
} alias_t;

typedef struct img {
    uint32_t handle;
    int used;
    uint32_t cb;  /* a color buffer (gralloc) image, or 0 */
    GLuint src;   /* a GL_TEXTURE_2D image: the guest texture it is made from */
} img_t;

typedef struct stream {
    const void *key; /* the decoder's context: one per guest pipe, so one per guest thread */
    gctx_t *cur;     /* the context made current on it (NULL: none) */
} stream_t;

static gctx_t ctxs[NCTX];
static surf_t surfs[NSURF];
static cb_t cbs[NCB];
static img_t imgs[NIMG];
static alias_t tex_alias[NALIAS], rb_alias[NALIAS];
static stream_t streams[NSTREAM];
static gctx_t *dflt; /* the context for a stream with none made current */
static gctx_t *applied;
static GLuint scratch_vbo[NATTR], scratch_ebo;
static uint32_t next_handle = 1;
static int ready = -1; /* -1: not tried; 0: failed; 1: ready */
static uint32_t frames_posted, frames_dropped;

/* ---------- logs ---------- */

static void wlog(const char *what, const char *detail)
{
    static int n;
    if (n++ < 64)
        fprintf(stderr, "xiaoji: gl(webgl): %s%s\n", what, detail ? detail : "");
}

/* ---------- the page's frame counters and the post (worker only) ---------- */

/* Int32Array [posted, consumed] in globalThis.xjFrameCtl, set by web/worker.js. Without it no frame is posted. */
EM_JS(int, xj_frame_slot_free, (void), {
    const c = globalThis.xjFrameCtl;
    if (!c) return 0;
    return Atomics.load(c, 0) === Atomics.load(c, 1) ? 1 : 0;
});

/* The WebGL2 context on the worker's OffscreenCanvas (globalThis.xjCanvas, set by web/worker.js), through emscripten's
 * own GL.createContext (the same as emscripten_webgl_create_context, which cannot find a canvas in a worker). Returns
 * the handle, or 0. */
EM_JS(int, xj_gl_context_create, (void), {
    const canvas = globalThis.xjCanvas;
    if (!canvas) return 0;
    const handle = GL.createContext(canvas, {
        majorVersion: 2, minorVersion: 0, alpha: false, depth: false, stencil: false, antialias: false,
        premultipliedAlpha: false, preserveDrawingBuffer: false, failIfMajorPerformanceCaveat: false,
        enableExtensionsByDefault: true, explicitSwapControl: false,
    });
    if (!handle) return 0;
    GL.makeContextCurrent(handle);
    return handle;
});

/* Copies the frame to the page: w*h RGBA bytes, top row first. The page takes ownership of the copy. */
EM_JS(void, xj_frame_send, (const uint8_t *px, int w, int h, int seq), {
    const c = globalThis.xjFrameCtl;
    const p = px >>> 0;
    const bytes = HEAPU8.slice(p, p + w * h * 4);
    Atomics.add(c, 0, 1);
    postMessage({ type: 'frame', w: w, h: h, seq: seq, px: bytes.buffer, t: performance.now() }, [bytes.buffer]);
});

/* ---------- host state, saved and restored around the engine's own GL calls ---------- */

typedef struct hstate {
    GLint unit, tex2d, fbo, program, vao, abuf, ebuf, rbo;
} hstate_t;

static void save_host(hstate_t *s)
{
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->unit);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex2d);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s->fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->abuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s->ebuf);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &s->rbo);
}

static void restore_host(const hstate_t *s)
{
    glBindVertexArray((GLuint)s->vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)s->ebuf);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)s->abuf);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)s->fbo);
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)s->rbo);
    glUseProgram((GLuint)s->program);
    glActiveTexture((GLenum)s->unit);
    glBindTexture(GL_TEXTURE_2D, (GLuint)s->tex2d);
}

/* ---------- lookups ---------- */

static gctx_t *ctx_find(uint32_t h)
{
    for (int i = 0; i < NCTX; i++)
        if (ctxs[i].used && ctxs[i].handle == h)
            return &ctxs[i];
    return NULL;
}

static surf_t *surf_find(uint32_t h)
{
    for (int i = 0; i < NSURF; i++)
        if (surfs[i].used && surfs[i].handle == h)
            return &surfs[i];
    return NULL;
}

static cb_t *cb_find(uint32_t h)
{
    for (int i = 0; i < NCB; i++)
        if (cbs[i].used && cbs[i].handle == h)
            return &cbs[i];
    return NULL;
}

static cb_t *cb_create(uint32_t w, uint32_t h)
{
    cb_t *c = NULL;
    for (int i = 0; i < NCB && !c; i++)
        if (!cbs[i].used)
            c = &cbs[i];
    if (!c || !w || !h || (uint64_t)w * h > (1u << 24))
        return NULL;
    hstate_t hs;
    save_host(&hs);
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    restore_host(&hs);
    c->used = 1;
    c->handle = next_handle++;
    c->w = w;
    c->h = h;
    c->tex = tex;
    c->fbo = fbo;
    return c;
}

static void cb_destroy(cb_t *c)
{
    GLuint tex = c->tex, fbo = c->fbo;
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    c->used = 0;
}

/* The color buffer a window surface draws to. A surface without one gets its own, the size of the surface. */
static cb_t *surf_cb(surf_t *s)
{
    if (!s)
        return NULL;
    if (!s->cb) {
        cb_t *c = cb_create(s->w ? s->w : XJ_FB_WIDTH, s->h ? s->h : XJ_FB_HEIGHT);
        if (!c)
            return NULL;
        s->cb = c->handle;
    }
    return cb_find(s->cb);
}

/* The host framebuffer a guest framebuffer name stands for. 0 is the draw surface's color buffer. */
static GLuint host_fbo(const gctx_t *c, GLuint guest)
{
    if (guest)
        return guest;
    cb_t *cb = surf_cb(surf_find(c->draw));
    return cb ? cb->fbo : 0;
}

/* The host texture a guest texture name stands for (through an EGLImage or rcBindTexture alias). */
static GLuint host_tex(GLuint name)
{
    for (int i = 0; i < NALIAS; i++) {
        if (tex_alias[i].used && tex_alias[i].name == name) {
            if (tex_alias[i].cb) {
                cb_t *cb = cb_find(tex_alias[i].cb);
                return cb ? cb->tex : name;
            }
            return tex_alias[i].src ? tex_alias[i].src : name;
        }
    }
    return name;
}

/* Makes the guest name stand for a color buffer (cb) or a texture (src), in the alias table (one entry per name). */
static void set_alias(alias_t *table, GLuint name, uint32_t cb, GLuint src)
{
    if (!name)
        return;
    for (int i = 0; i < NALIAS; i++)
        if (table[i].used && table[i].name == name) {
            table[i].cb = cb;
            table[i].src = src;
            return;
        }
    for (int i = 0; i < NALIAS; i++)
        if (!table[i].used) {
            table[i].used = 1;
            table[i].name = name;
            table[i].cb = cb;
            table[i].src = src;
            return;
        }
}

static GLenum tgt(uint32_t t)
{
    return t == XJ_TEXTURE_EXTERNAL_OES ? GL_TEXTURE_2D : (GLenum)t;
}

/* ---------- context switching ---------- */

static void apply(gctx_t *c)
{
    if (c == applied)
        return;
    applied = c;
    if (!c)
        return;
    glBindFramebuffer(GL_FRAMEBUFFER, host_fbo(c, c->fbo));
    glUseProgram(c->program);
    glBindVertexArray(c->vao);
    glActiveTexture(GL_TEXTURE0 + c->unit);
    glBindTexture(GL_TEXTURE_2D, host_tex(c->tex[c->unit]));
    glViewport(c->vp[0], c->vp[1], c->vp[2], c->vp[3]);
    glScissor(c->sc[0], c->sc[1], c->sc[2], c->sc[3]);
}

/* The stream's slot: the one for its decoder context, or a new one. NULL when all are taken. */
static stream_t *stream_slot(const void *key)
{
    for (int i = 0; i < NSTREAM; i++)
        if (streams[i].key == key)
            return &streams[i];
    for (int i = 0; i < NSTREAM; i++)
        if (!streams[i].key) {
            streams[i].key = key;
            streams[i].cur = NULL;
            return &streams[i];
        }
    return NULL;
}

static gctx_t *stream_ctx(const void *key)
{
    stream_t *s = stream_slot(key);
    if (!s || !s->cur)
        return dflt;
    return s->cur;
}

/* Every GLES call starts here: the stream's context is the applied one, so the call sees its state. */
static gctx_t *enter(const void *key)
{
    gctx_t *c = stream_ctx(key);
    apply(c);
    return c;
}

/* ---------- the shader translation (GLSL ES as the guest sends it, to WebGL2) ---------- */

/* Drops the external-image extension lines, and reads samplerExternalOES as sampler2D (external textures are 2D
 * color buffers here). #version 100 and 300 es both compile in a WebGL2 context. Returns a malloc'd copy. */
/* Does the line src[i..e) contain pat? */
static int line_has(const char *src, uint32_t i, uint32_t e, const char *pat)
{
    size_t n = strlen(pat);
    for (uint32_t k = i; k + n <= e; k++)
        if (!memcmp(src + k, pat, n))
            return 1;
    return 0;
}

static char *translate_shader(const char *src, uint32_t len, uint32_t *outlen)
{
    char *out = malloc((size_t)len * 2 + 1);
    if (!out)
        return NULL;
    size_t o = 0;
    uint32_t i = 0;
    while (i < len) {
        uint32_t e = i;
        while (e < len && src[e] != '\n')
            e++;
        uint32_t next = e < len ? e + 1 : e;
        int drop = (e - i >= 10 && !memcmp(src + i, "#extension", 10) &&
                    line_has(src, i, e, "GL_OES_EGL_image_external"));
        if (!drop) {
            for (uint32_t k = i; k < e;) {
                if (e - k >= 18 && !memcmp(src + k, "samplerExternalOES", 18)) {
                    memcpy(out + o, "sampler2D", 9);
                    o += 9;
                    k += 18;
                } else {
                    out[o++] = src[k++];
                }
            }
            if (e < len)
                out[o++] = '\n';
        }
        i = next;
    }
    out[o] = 0;
    *outlen = (uint32_t)o;
    return out;
}

/* A NUL-terminated copy of a name in the packet (the names of uniforms and attributes). */
static char *cstr_copy(const void *p, uint32_t len)
{
    char *s = malloc((size_t)len + 1);
    if (!s)
        return NULL;
    memcpy(s, p, len);
    s[len] = 0;
    return s;
}

static int32_t put_string(const char *s, void *buffer, uint32_t buffer_len, int32_t bufferSize)
{
    int32_t len = (int32_t)strlen(s) + 1;
    if (!buffer || len > bufferSize || (uint32_t)len > buffer_len)
        return -len;
    memcpy(buffer, s, (size_t)len);
    return len;
}

/* ---------- renderControl ---------- */

static uint32_t rc_CreateContext(void *ctx, uint32_t config, uint32_t share, uint32_t glVersion)
{
    (void)ctx; (void)config; (void)glVersion;
    gctx_t *c = NULL;
    for (int i = 0; i < NCTX && !c; i++)
        if (!ctxs[i].used)
            c = &ctxs[i];
    if (!c)
        return 0;
    memset(c, 0, sizeof *c);
    c->used = 1;
    c->handle = next_handle++;
    c->share = share;
    c->vp[2] = XJ_FB_WIDTH;
    c->vp[3] = XJ_FB_HEIGHT;
    c->sc[2] = XJ_FB_WIDTH;
    c->sc[3] = XJ_FB_HEIGHT;
    return c->handle;
}

static void rc_DestroyContext(void *ctx, uint32_t context)
{
    (void)ctx;
    gctx_t *c = ctx_find(context);
    if (!c)
        return;
    if (applied == c)
        applied = NULL;
    for (int i = 0; i < NSTREAM; i++)
        if (streams[i].cur == c)
            streams[i].cur = NULL;
    c->used = 0;
}

static uint32_t rc_CreateWindowSurface(void *ctx, uint32_t config, uint32_t width, uint32_t height)
{
    (void)ctx; (void)config;
    for (int i = 0; i < NSURF; i++)
        if (!surfs[i].used) {
            memset(&surfs[i], 0, sizeof surfs[i]);
            surfs[i].used = 1;
            surfs[i].handle = next_handle++;
            surfs[i].w = width;
            surfs[i].h = height;
            return surfs[i].handle;
        }
    return 0;
}

static void rc_DestroyWindowSurface(void *ctx, uint32_t windowSurface)
{
    (void)ctx;
    surf_t *s = surf_find(windowSurface);
    if (s)
        s->used = 0;
}

static void rc_SetWindowColorBuffer(void *ctx, uint32_t windowSurface, uint32_t colorBuffer)
{
    (void)ctx;
    surf_t *s = surf_find(windowSurface);
    if (s)
        s->cb = colorBuffer;
    if (applied)
        applied = NULL; /* the draw FBO may be another color buffer now: rebind on the next call */
}

static int32_t rc_FlushWindowColorBuffer(void *ctx, uint32_t windowSurface)
{
    (void)ctx; (void)windowSurface;
    glFlush();
    return 0;
}

static int32_t rc_MakeCurrent(void *ctx, uint32_t context, uint32_t drawSurf, uint32_t readSurf)
{
    gctx_t *c = NULL;
    if (context) {
        c = ctx_find(context);
        if (!c)
            return 0; /* EGL_FALSE: no such context */
    }
    stream_t *s = stream_slot(ctx);
    if (!s)
        return 0;
    s->cur = c;
    if (c) {
        c->draw = drawSurf;
        c->read = readSurf;
        applied = NULL; /* apply again: the draw surface may have changed */
        apply(c);
    }
    return 1; /* EGL_TRUE */
}

static uint32_t rc_CreateColorBuffer(void *ctx, uint32_t width, uint32_t height, uint32_t internalFormat)
{
    (void)ctx; (void)internalFormat;
    cb_t *c = cb_create(width, height);
    return c ? c->handle : 0;
}

static uint32_t rc_CreateColorBufferDMA(void *ctx, uint32_t width, uint32_t height, uint32_t internalFormat,
                                        int32_t frameworkFormat)
{
    (void)frameworkFormat;
    return rc_CreateColorBuffer(ctx, width, height, internalFormat);
}

static void rc_CloseColorBuffer(void *ctx, uint32_t colorbuffer)
{
    (void)ctx;
    cb_t *c = cb_find(colorbuffer);
    if (c)
        cb_destroy(c);
}

/* The rectangle, RGBA8 bytes (other formats are refused, as in the null backend). */
static int cb_rect_ok(const cb_t *c, int32_t x, int32_t y, int32_t w, int32_t h)
{
    return x >= 0 && y >= 0 && w > 0 && h > 0 && (uint32_t)x + (uint32_t)w <= c->w && (uint32_t)y + (uint32_t)h <= c->h;
}

static void rc_ReadColorBuffer(void *ctx, uint32_t colorbuffer, int32_t x, int32_t y, int32_t width, int32_t height,
                               uint32_t format, uint32_t type, void *pixels, uint32_t pixels_len)
{
    (void)ctx;
    cb_t *c = cb_find(colorbuffer);
    if (!c || format != GL_RGBA || type != GL_UNSIGNED_BYTE || !cb_rect_ok(c, x, y, width, height))
        return;
    if ((uint64_t)width * 4 * (uint64_t)height > pixels_len)
        return;
    hstate_t hs;
    save_host(&hs);
    glBindFramebuffer(GL_FRAMEBUFFER, c->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    restore_host(&hs);
}

static int32_t rc_UpdateColorBuffer(void *ctx, uint32_t colorbuffer, int32_t x, int32_t y, int32_t width, int32_t height,
                                    uint32_t format, uint32_t type, const void *pixels, uint32_t pixels_len)
{
    (void)ctx;
    cb_t *c = cb_find(colorbuffer);
    if (!c || format != GL_RGBA || type != GL_UNSIGNED_BYTE || !cb_rect_ok(c, x, y, width, height))
        return -1;
    if ((uint64_t)width * 4 * (uint64_t)height > pixels_len)
        return -1;
    hstate_t hs;
    save_host(&hs);
    glBindTexture(GL_TEXTURE_2D, c->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    restore_host(&hs);
    return 0;
}

/* rcBindTexture (the guest's gralloc images, GLESv2/gl2.cpp): the texture bound on the active unit now stands for the
 * color buffer, and the host binds the color buffer's texture. */
static void rc_BindTexture(void *ctx, uint32_t colorBuffer)
{
    gctx_t *g = enter(ctx);
    cb_t *c = cb_find(colorBuffer);
    if (!c)
        return;
    set_alias(tex_alias, g->tex[g->unit], colorBuffer, 0);
    glBindTexture(GL_TEXTURE_2D, c->tex);
}

/* rcBindRenderbuffer: the renderbuffer bound now stands for the color buffer. A renderbuffer cannot alias a texture in
 * WebGL, so glFramebufferRenderbuffer attaches the color buffer's texture for such a name (rb_alias). */
static void rc_BindRenderbuffer(void *ctx, uint32_t colorBuffer)
{
    gctx_t *g = enter(ctx);
    if (cb_find(colorBuffer))
        set_alias(rb_alias, g->rbo, colorBuffer, 0);
}

#define XJ_EGL_GL_TEXTURE_2D 0x30B1 /* EGL_GL_TEXTURE_2D_KHR: buffer is the guest's texture name */

static uint32_t rc_CreateClientImage(void *ctx, uint32_t context, uint32_t target, uint32_t buffer)
{
    (void)ctx; (void)context;
    int tex = target == XJ_EGL_GL_TEXTURE_2D;
    if (!tex && !cb_find(buffer))
        return 0;
    for (int i = 0; i < NIMG; i++)
        if (!imgs[i].used) {
            imgs[i].used = 1;
            imgs[i].handle = next_handle++;
            imgs[i].cb = tex ? 0 : buffer;
            imgs[i].src = tex ? buffer : 0;
            return imgs[i].handle;
        }
    return 0;
}

static int32_t rc_DestroyClientImage(void *ctx, uint32_t image)
{
    (void)ctx;
    for (int i = 0; i < NIMG; i++)
        if (imgs[i].used && imgs[i].handle == image) {
            imgs[i].used = 0;
            return 1;
        }
    return 0;
}

/* The composed frame (rcFBPost): read back, row order flipped to top-first, alpha set to 255, and posted if the page
 * has consumed the last frame. Dropped frames cost nothing (no read). */
static void rc_FBPost(void *ctx, uint32_t colorBuffer)
{
    (void)ctx;
    cb_t *c = cb_find(colorBuffer);
    if (!c)
        return;
    if (!xj_frame_slot_free()) {
        if (++frames_dropped % 300 == 0)
            fprintf(stderr, "xiaoji: gl(webgl): %u frames dropped (the page has not taken the last one)\n",
                    (unsigned)frames_dropped);
        return;
    }
    uint32_t w = c->w, h = c->h;
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px)
        return;
    hstate_t hs;
    save_host(&hs);
    glBindFramebuffer(GL_FRAMEBUFFER, c->fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    restore_host(&hs);
    size_t row = (size_t)w * 4;
    for (uint32_t y = 0; y < h / 2; y++) {
        uint8_t *a = px + y * row, *b = px + (size_t)(h - 1 - y) * row;
        for (size_t k = 0; k < row; k++) {
            uint8_t t = a[k];
            a[k] = b[k];
            b[k] = t;
        }
    }
    for (size_t i = 3; i < (size_t)w * h * 4; i += 4)
        px[i] = 255;
    frames_posted++;
    xj_frame_send(px, (int)w, (int)h, (int)frames_posted);
    if (frames_posted <= 3)
        fprintf(stderr, "xiaoji: gl(webgl): frame %u posted (%ux%u)\n", (unsigned)frames_posted, (unsigned)w, (unsigned)h);
    free(px);
}

/* The renderControl calls that take a color buffer through their *Async variants, as the sync ones. */
static void rc_MakeCurrentAsync(void *ctx, uint32_t context, uint32_t drawSurf, uint32_t readSurf)
{
    rc_MakeCurrent(ctx, context, drawSurf, readSurf);
}

static void rc_FlushWindowColorBufferAsync(void *ctx, uint32_t windowSurface)
{
    rc_FlushWindowColorBuffer(ctx, windowSurface);
}

static void rc_FlushWindowColorBufferAsyncWithFrameNumber(void *ctx, uint32_t windowSurface, uint32_t frameNumber)
{
    (void)frameNumber;
    rc_FlushWindowColorBuffer(ctx, windowSurface);
}

static int32_t rc_GetGLString(void *ctx, uint32_t name, void *buffer, uint32_t buffer_len, int32_t bufferSize)
{
    (void)ctx;
    switch (name) {
    case GL_VENDOR: return put_string(wgl_vendor, buffer, buffer_len, bufferSize);
    case GL_RENDERER: return put_string(wgl_renderer, buffer, buffer_len, bufferSize);
    case GL_VERSION: return put_string(wgl_version, buffer, buffer_len, bufferSize);
    case GL_EXTENSIONS: return put_string(wgl_extensions, buffer, buffer_len, bufferSize);
    default: return 0;
    }
}

static int32_t rc_QueryEGLString(void *ctx, uint32_t name, void *buffer, uint32_t buffer_len, int32_t bufferSize)
{
    (void)ctx;
    switch (name) {
    case 0x3055: return put_string(wgl_egl_extensions, buffer, buffer_len, bufferSize); /* EGL_EXTENSIONS */
    default: return 0;
    }
}

/* ---------- GLES 2 and 3: the calls the guest sends ---------- */

/* State and binding calls: the guest's names, with the tracked state updated. */

static void w_glEnable(void *ctx, uint32_t cap) { enter(ctx); glEnable(cap); }
static void w_glDisable(void *ctx, uint32_t cap) { enter(ctx); glDisable(cap); }
static uint8_t w_glIsEnabled(void *ctx, uint32_t cap) { enter(ctx); return (uint8_t)glIsEnabled(cap); }
static void w_glBlendColor(void *ctx, float r, float g, float b, float a) { enter(ctx); glBlendColor(r, g, b, a); }
static void w_glBlendEquation(void *ctx, uint32_t mode) { enter(ctx); glBlendEquation(mode); }
static void w_glBlendEquationSeparate(void *ctx, uint32_t m0, uint32_t m1) { enter(ctx); glBlendEquationSeparate(m0, m1); }
static void w_glBlendFunc(void *ctx, uint32_t s, uint32_t d) { enter(ctx); glBlendFunc(s, d); }
static void w_glBlendFuncSeparate(void *ctx, uint32_t a, uint32_t b, uint32_t c2, uint32_t d) { enter(ctx); glBlendFuncSeparate(a, b, c2, d); }
static void w_glClearColor(void *ctx, float r, float g, float b, float a) { enter(ctx); glClearColor(r, g, b, a); }
static void w_glClearDepthf(void *ctx, float d) { enter(ctx); glClearDepthf(d); }
static void w_glClearStencil(void *ctx, int32_t s) { enter(ctx); glClearStencil(s); }
static void w_glClear(void *ctx, uint32_t mask) { enter(ctx); glClear(mask); }
static void w_glColorMask(void *ctx, uint8_t r, uint8_t g, uint8_t b, uint8_t a) { enter(ctx); glColorMask(r, g, b, a); }
static void w_glCullFace(void *ctx, uint32_t m) { enter(ctx); glCullFace(m); }
static void w_glDepthFunc(void *ctx, uint32_t f) { enter(ctx); glDepthFunc(f); }
static void w_glDepthMask(void *ctx, uint8_t f) { enter(ctx); glDepthMask(f); }
static void w_glDepthRangef(void *ctx, float n, float f) { enter(ctx); glDepthRangef(n, f); }
static void w_glFrontFace(void *ctx, uint32_t m) { enter(ctx); glFrontFace(m); }
static void w_glHint(void *ctx, uint32_t t, uint32_t m) { enter(ctx); glHint(t, m); }
static void w_glLineWidth(void *ctx, float w) { enter(ctx); glLineWidth(w); }
static void w_glPolygonOffset(void *ctx, float f, float u) { enter(ctx); glPolygonOffset(f, u); }
static void w_glSampleCoverage(void *ctx, float v, uint8_t inv) { enter(ctx); glSampleCoverage(v, inv); }
static void w_glStencilFunc(void *ctx, uint32_t f, int32_t r, uint32_t m) { enter(ctx); glStencilFunc(f, r, m); }
static void w_glStencilFuncSeparate(void *ctx, uint32_t face, uint32_t f, int32_t r, uint32_t m) { enter(ctx); glStencilFuncSeparate(face, f, r, m); }
static void w_glStencilMask(void *ctx, uint32_t m) { enter(ctx); glStencilMask(m); }
static void w_glStencilMaskSeparate(void *ctx, uint32_t face, uint32_t m) { enter(ctx); glStencilMaskSeparate(face, m); }
static void w_glStencilOp(void *ctx, uint32_t a, uint32_t b, uint32_t c2) { enter(ctx); glStencilOp(a, b, c2); }
static void w_glStencilOpSeparate(void *ctx, uint32_t face, uint32_t a, uint32_t b, uint32_t c2) { enter(ctx); glStencilOpSeparate(face, a, b, c2); }
static void w_glPixelStorei(void *ctx, uint32_t pname, int32_t param) { enter(ctx); glPixelStorei(pname, param); }
static void w_glFlush(void *ctx) { enter(ctx); glFlush(); }
static void w_glFinish(void *ctx) { enter(ctx); glFinish(); }
static uint32_t w_glGetError(void *ctx) { enter(ctx); return glGetError(); }

static void w_glViewport(void *ctx, int32_t x, int32_t y, int32_t w, int32_t h)
{
    gctx_t *c = enter(ctx);
    c->vp[0] = x; c->vp[1] = y; c->vp[2] = w; c->vp[3] = h;
    glViewport(x, y, w, h);
}

static void w_glScissor(void *ctx, int32_t x, int32_t y, int32_t w, int32_t h)
{
    gctx_t *c = enter(ctx);
    c->sc[0] = x; c->sc[1] = y; c->sc[2] = w; c->sc[3] = h;
    glScissor(x, y, w, h);
}

/* Writes n 32-bit values to the guest's buffer, at most as many as fit. */
static void put_ints(void *params, uint32_t params_len, const GLint *v, uint32_t n)
{
    for (uint32_t i = 0; i < n && 4 * (i + 1) <= params_len; i++) {
        int32_t x = v[i];
        memcpy((uint8_t *)params + 4 * i, &x, 4);
    }
}

static void w_glGetIntegerv(void *ctx, uint32_t pname, void *params, uint32_t params_len)
{
    enter(ctx);
    if (!params)
        return;
    /* The counts this backend answers itself (they match the advertised extensions and the ES 3.0 version). */
    if (pname == XJ_NUM_EXTENSIONS) {
        int32_t n = 1;
        for (const char *s = wgl_extensions; *s; s++)
            if (*s == ' ')
                n++;
        put_ints(params, params_len, &n, 1);
        return;
    }
    if (pname == XJ_MAJOR_VERSION || pname == XJ_MINOR_VERSION) {
        GLint v = pname == XJ_MAJOR_VERSION ? 3 : 0;
        put_ints(params, params_len, &v, 1);
        return;
    }
    /* Array-valued pnames (more than the buffer of 16 below) are left to the null answers. */
    if (pname == 0x86A3 || pname == 0x8DF8)
        return;
    GLint v[16] = {0};
    glGetIntegerv(pname, v);
    uint32_t n = 16;
    if (pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX || pname == GL_MAX_VIEWPORT_DIMS)
        n = 4;
    put_ints(params, params_len, v, n);
}

static void w_glGetFloatv(void *ctx, uint32_t pname, void *params, uint32_t params_len)
{
    enter(ctx);
    GLfloat v[16] = {0};
    if (pname == 0x86A3 || pname == 0x8DF8)
        return;
    glGetFloatv(pname, v);
    for (uint32_t i = 0; i < 16 && 4 * (i + 1) <= params_len; i++)
        memcpy((uint8_t *)params + 4 * i, &v[i], 4);
}

static void w_glGetBooleanv(void *ctx, uint32_t pname, void *params, uint32_t params_len)
{
    enter(ctx);
    GLboolean v[16] = {0};
    glGetBooleanv(pname, v);
    for (uint32_t i = 0; i < 16 && i < params_len; i++)
        ((uint8_t *)params)[i] = v[i];
}

/* Buffers. */

static void w_glGenBuffers(void *ctx, int32_t n, void *buffers, uint32_t len)
{
    enter(ctx);
    if (n < 0 || (uint64_t)n * 4 > len)
        return;
    glGenBuffers(n, buffers);
}

static void w_glDeleteBuffers(void *ctx, int32_t n, const void *buffers, uint32_t len)
{
    enter(ctx);
    if (n < 0 || (uint64_t)n * 4 > len)
        return;
    glDeleteBuffers(n, buffers);
}

static void w_glBindBuffer(void *ctx, uint32_t target, uint32_t buffer)
{
    enter(ctx);
    glBindBuffer(target, buffer);
}

static void w_glBufferData(void *ctx, uint32_t target, int32_t size, const void *data, uint32_t data_len, uint32_t usage)
{
    enter(ctx);
    glBufferData(target, size, data_len ? data : NULL, usage);
}

static void w_glBufferSubData(void *ctx, uint32_t target, int32_t offset, int32_t size, const void *data, uint32_t data_len)
{
    enter(ctx);
    if (data_len >= (uint32_t)size)
        glBufferSubData(target, offset, size, data);
}

/* Textures: the names and the aliases. */

static void w_glGenTextures(void *ctx, int32_t n, void *textures, uint32_t len)
{
    enter(ctx);
    if (n < 0 || (uint64_t)n * 4 > len)
        return;
    glGenTextures(n, textures);
}

static void w_glDeleteTextures(void *ctx, int32_t n, const void *textures, uint32_t len)
{
    enter(ctx);
    if (n < 0 || (uint64_t)n * 4 > len)
        return;
    const GLuint *names = textures;
    for (int32_t i = 0; i < n; i++)
        for (int k = 0; k < NALIAS; k++)
            if (tex_alias[k].used && tex_alias[k].name == names[i])
                tex_alias[k].used = 0;
    glDeleteTextures(n, textures);
}

static void w_glActiveTexture(void *ctx, uint32_t texture)
{
    gctx_t *c = enter(ctx);
    if (texture < GL_TEXTURE0 || texture - GL_TEXTURE0 >= 32)
        return;
    c->unit = texture - GL_TEXTURE0;
    glActiveTexture(texture);
}

static void w_glBindTexture(void *ctx, uint32_t target, uint32_t texture)
{
    gctx_t *c = enter(ctx);
    glBindTexture(tgt(target), host_tex(texture));
    if (tgt(target) == GL_TEXTURE_2D)
        c->tex[c->unit] = texture;
}

static void w_glTexImage2D(void *ctx, uint32_t target, int32_t level, int32_t internalformat, int32_t width, int32_t height,
                           int32_t border, uint32_t format, uint32_t type, const void *pixels, uint32_t pixels_len)
{
    enter(ctx);
    (void)pixels_len;
    glTexImage2D(tgt(target), level, internalformat, width, height, border, format, type, pixels);
}

static void w_glTexSubImage2D(void *ctx, uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t width,
                              int32_t height, uint32_t format, uint32_t type, const void *pixels, uint32_t pixels_len)
{
    enter(ctx);
    (void)pixels_len;
    glTexSubImage2D(tgt(target), level, xoffset, yoffset, width, height, format, type, pixels);
}

static void w_glTexStorage2D(void *ctx, uint32_t target, int32_t levels, uint32_t internalformat, int32_t width, int32_t height)
{
    enter(ctx);
    glTexStorage2D(tgt(target), levels, internalformat, width, height);
}

static void w_glTexParameteri(void *ctx, uint32_t target, uint32_t pname, int32_t param)
{
    enter(ctx);
    glTexParameteri(tgt(target), pname, param);
}

static void w_glTexParameterf(void *ctx, uint32_t target, uint32_t pname, float param)
{
    enter(ctx);
    glTexParameterf(tgt(target), pname, param);
}

static void w_glGenerateMipmap(void *ctx, uint32_t target)
{
    enter(ctx);
    glGenerateMipmap(tgt(target));
}

/* EGLImage: the guest's texture bound on the active unit now stands for the color buffer. */
static void w_glEGLImageTargetTexture2DOES(void *ctx, uint32_t target, uint64_t image)
{
    gctx_t *c = enter(ctx);
    img_t *im = NULL;
    for (int i = 0; i < NIMG && !im; i++)
        if (imgs[i].used && imgs[i].handle == image)
            im = &imgs[i];
    GLuint name = c->tex[c->unit];
    if (!im || !name || tgt(target) != GL_TEXTURE_2D)
        return;
    set_alias(tex_alias, name, im->cb, im->src);
    glBindTexture(GL_TEXTURE_2D, host_tex(name));
}

/* Framebuffers and renderbuffers. */

static void w_glGenFramebuffers(void *ctx, int32_t n, void *fbs, uint32_t len)
{
    enter(ctx);
    if (n < 0 || (uint64_t)n * 4 > len)
        return;
    glGenFramebuffers(n, fbs);
}

static void w_glDeleteFramebuffers(void *ctx, int32_t n, const void *fbs, uint32_t len)
{
    enter(ctx);
    if (n < 0 || (uint64_t)n * 4 > len)
        return;
    glDeleteFramebuffers(n, fbs);
}

static void w_glBindFramebuffer(void *ctx, uint32_t target, uint32_t framebuffer)
{
    gctx_t *c = enter(ctx);
    c->fbo = framebuffer;
    glBindFramebuffer(target, host_fbo(c, framebuffer));
}

static uint32_t w_glCheckFramebufferStatus(void *ctx, uint32_t target)
{
    enter(ctx);
    return glCheckFramebufferStatus(target);
}

static void w_glFramebufferTexture2D(void *ctx, uint32_t target, uint32_t attachment, uint32_t textarget, uint32_t texture, int32_t level)
{
    enter(ctx);
    glFramebufferTexture2D(target, attachment, tgt(textarget), host_tex(texture), level);
}

static void w_glGenRenderbuffers(void *ctx, int32_t n, void *rbs, uint32_t len)
{
    enter(ctx);
    if (n < 0 || (uint64_t)n * 4 > len)
        return;
    glGenRenderbuffers(n, rbs);
}

static void w_glDeleteRenderbuffers(void *ctx, int32_t n, const void *rbs, uint32_t len)
{
    enter(ctx);
    if (n < 0 || (uint64_t)n * 4 > len)
        return;
    glDeleteRenderbuffers(n, rbs);
}

static void w_glBindRenderbuffer(void *ctx, uint32_t target, uint32_t rb)
{
    gctx_t *c = enter(ctx);
    c->rbo = rb;
    glBindRenderbuffer(target, rb);
}

static void w_glRenderbufferStorage(void *ctx, uint32_t target, uint32_t internalformat, int32_t width, int32_t height)
{
    enter(ctx);
    glRenderbufferStorage(target, internalformat, width, height);
}

static void w_glFramebufferRenderbuffer(void *ctx, uint32_t target, uint32_t attachment, uint32_t rbtarget, uint32_t rb)
{
    enter(ctx);
    for (int i = 0; i < NALIAS; i++)
        if (rb_alias[i].used && rb_alias[i].name == rb) {
            cb_t *cb = cb_find(rb_alias[i].cb);
            if (cb) {
                glFramebufferTexture2D(target, attachment, GL_TEXTURE_2D, cb->tex, 0);
                return;
            }
        }
    glFramebufferRenderbuffer(target, attachment, rbtarget, rb);
}

static void w_glEGLImageTargetRenderbufferStorageOES(void *ctx, uint32_t target, uint64_t image)
{
    (void)target;
    gctx_t *c = enter(ctx);
    for (int i = 0; i < NIMG; i++)
        if (imgs[i].used && imgs[i].handle == image && c->rbo) {
            for (int k = 0; k < NALIAS; k++)
                if (!rb_alias[k].used || rb_alias[k].name == c->rbo) {
                    rb_alias[k].used = 1;
                    rb_alias[k].name = c->rbo;
                    rb_alias[k].cb = imgs[i].cb;
                    break;
                }
            return;
        }
}

static void w_glReadPixels(void *ctx, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format, uint32_t type,
                           void *pixels, uint32_t pixels_len)
{
    enter(ctx);
    if (format != GL_RGBA || type != GL_UNSIGNED_BYTE || width <= 0 || height <= 0)
        return;
    if ((uint64_t)width * 4 * (uint64_t)height > pixels_len)
        return;
    glReadPixels(x, y, width, height, format, type, pixels);
}

/* Shaders and programs. The source of a shader comes as glShaderString (gl_host.c). */

static uint32_t w_glCreateShader(void *ctx, uint32_t type)
{
    enter(ctx);
    return glCreateShader(type);
}

static void w_glDeleteShader(void *ctx, uint32_t shader)
{
    enter(ctx);
    glDeleteShader(shader);
}

static void w_glCompileShader(void *ctx, uint32_t shader)
{
    enter(ctx);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint n = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &n);
        char *log = malloc((size_t)n + 1);
        if (log) {
            GLsizei got = 0;
            glGetShaderInfoLog(shader, n + 1, &got, log);
            fprintf(stderr, "xiaoji: gl(webgl): shader %u did not compile: %s\n", (unsigned)shader, log);
            free(log);
        }
    }
}

static void w_glGetShaderiv(void *ctx, uint32_t shader, uint32_t pname, void *params, uint32_t params_len)
{
    enter(ctx);
    GLint v = 0;
    glGetShaderiv(shader, pname, &v);
    put_ints(params, params_len, &v, 1);
}

static void w_glGetShaderInfoLog(void *ctx, uint32_t shader, int32_t bufsize, void *length, uint32_t length_len,
                                 void *infolog, uint32_t infolog_len)
{
    enter(ctx);
    uint32_t cap = bufsize > 0 ? (uint32_t)bufsize : 0;
    if (cap > infolog_len)
        cap = infolog_len;
    GLsizei got = 0;
    if (cap)
        glGetShaderInfoLog(shader, (GLsizei)cap, &got, infolog);
    if (length_len >= 4) {
        int32_t g = got;
        memcpy(length, &g, 4);
    }
}

static uint32_t w_glCreateProgram(void *ctx)
{
    enter(ctx);
    return glCreateProgram();
}

static void w_glDeleteProgram(void *ctx, uint32_t program)
{
    enter(ctx);
    glDeleteProgram(program);
}

static void w_glAttachShader(void *ctx, uint32_t program, uint32_t shader)
{
    enter(ctx);
    glAttachShader(program, shader);
}

static void w_glDetachShader(void *ctx, uint32_t program, uint32_t shader)
{
    enter(ctx);
    glDetachShader(program, shader);
}

static void w_glLinkProgram(void *ctx, uint32_t program)
{
    enter(ctx);
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint n = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &n);
        char *log = malloc((size_t)n + 1);
        if (log) {
            GLsizei got = 0;
            glGetProgramInfoLog(program, n + 1, &got, log);
            fprintf(stderr, "xiaoji: gl(webgl): program %u did not link: %s\n", (unsigned)program, log);
            free(log);
        }
    }
}

static void w_glUseProgram(void *ctx, uint32_t program)
{
    gctx_t *c = enter(ctx);
    c->program = program;
    glUseProgram(program);
}

static void w_glValidateProgram(void *ctx, uint32_t program)
{
    enter(ctx);
    glValidateProgram(program);
}

static void w_glGetProgramiv(void *ctx, uint32_t program, uint32_t pname, void *params, uint32_t params_len)
{
    enter(ctx);
    GLint v = 0;
    glGetProgramiv(program, pname, &v);
    put_ints(params, params_len, &v, 1);
}

static void w_glGetProgramInfoLog(void *ctx, uint32_t program, int32_t bufsize, void *length, uint32_t length_len,
                                  void *infolog, uint32_t infolog_len)
{
    enter(ctx);
    uint32_t cap = bufsize > 0 ? (uint32_t)bufsize : 0;
    if (cap > infolog_len)
        cap = infolog_len;
    GLsizei got = 0;
    if (cap)
        glGetProgramInfoLog(program, (GLsizei)cap, &got, infolog);
    if (length_len >= 4) {
        int32_t g = got;
        memcpy(length, &g, 4);
    }
}

static void w_glBindAttribLocation(void *ctx, uint32_t program, uint32_t index, const void *name, uint32_t name_len)
{
    enter(ctx);
    char *s = cstr_copy(name, name_len);
    if (!s)
        return;
    glBindAttribLocation(program, index, s);
    free(s);
}

static int32_t w_glGetAttribLocation(void *ctx, uint32_t program, const void *name, uint32_t name_len)
{
    enter(ctx);
    char *s = cstr_copy(name, name_len);
    if (!s)
        return -1;
    GLint r = glGetAttribLocation(program, s);
    free(s);
    return r;
}

static int32_t w_glGetUniformLocation(void *ctx, uint32_t program, const void *name, uint32_t name_len)
{
    enter(ctx);
    char *s = cstr_copy(name, name_len);
    if (!s)
        return -1;
    GLint r = glGetUniformLocation(program, s);
    free(s);
    return r;
}

static void w_glUniform1i(void *ctx, int32_t loc, int32_t x) { enter(ctx); glUniform1i(loc, x); }
static void w_glUniform1f(void *ctx, int32_t loc, float x) { enter(ctx); glUniform1f(loc, x); }
static void w_glUniform2f(void *ctx, int32_t loc, float x, float y) { enter(ctx); glUniform2f(loc, x, y); }
static void w_glUniform3f(void *ctx, int32_t loc, float x, float y, float z) { enter(ctx); glUniform3f(loc, x, y, z); }
static void w_glUniform4f(void *ctx, int32_t loc, float x, float y, float z, float w) { enter(ctx); glUniform4f(loc, x, y, z, w); }
static void w_glUniform2i(void *ctx, int32_t loc, int32_t x, int32_t y) { enter(ctx); glUniform2i(loc, x, y); }
static void w_glUniform3i(void *ctx, int32_t loc, int32_t x, int32_t y, int32_t z) { enter(ctx); glUniform3i(loc, x, y, z); }
static void w_glUniform4i(void *ctx, int32_t loc, int32_t x, int32_t y, int32_t z, int32_t w) { enter(ctx); glUniform4i(loc, x, y, z, w); }

/* The vector forms take the wire bytes: count values of their size. */
static uint32_t vec_bytes(int32_t count, uint32_t per, uint32_t len)
{
    if (count <= 0)
        return 0;
    uint64_t need = (uint64_t)count * per * 4;
    return need <= len ? (uint32_t)need : 0;
}

static void w_glUniform1fv(void *ctx, int32_t loc, int32_t count, const void *v, uint32_t len) { enter(ctx); if (vec_bytes(count, 1, len)) glUniform1fv(loc, count, v); }
static void w_glUniform2fv(void *ctx, int32_t loc, int32_t count, const void *v, uint32_t len) { enter(ctx); if (vec_bytes(count, 2, len)) glUniform2fv(loc, count, v); }
static void w_glUniform3fv(void *ctx, int32_t loc, int32_t count, const void *v, uint32_t len) { enter(ctx); if (vec_bytes(count, 3, len)) glUniform3fv(loc, count, v); }
static void w_glUniform4fv(void *ctx, int32_t loc, int32_t count, const void *v, uint32_t len) { enter(ctx); if (vec_bytes(count, 4, len)) glUniform4fv(loc, count, v); }
static void w_glUniform1iv(void *ctx, int32_t loc, int32_t count, const void *v, uint32_t len) { enter(ctx); if (vec_bytes(count, 1, len)) glUniform1iv(loc, count, v); }
static void w_glUniform2iv(void *ctx, int32_t loc, int32_t count, const void *v, uint32_t len) { enter(ctx); if (vec_bytes(count, 2, len)) glUniform2iv(loc, count, v); }
static void w_glUniform3iv(void *ctx, int32_t loc, int32_t count, const void *v, uint32_t len) { enter(ctx); if (vec_bytes(count, 3, len)) glUniform3iv(loc, count, v); }
static void w_glUniform4iv(void *ctx, int32_t loc, int32_t count, const void *v, uint32_t len) { enter(ctx); if (vec_bytes(count, 4, len)) glUniform4iv(loc, count, v); }
static void w_glUniformMatrix2fv(void *ctx, int32_t loc, int32_t count, uint8_t t, const void *v, uint32_t len) { enter(ctx); if (vec_bytes(count, 4, len)) glUniformMatrix2fv(loc, count, t, v); }
static void w_glUniformMatrix3fv(void *ctx, int32_t loc, int32_t count, uint8_t t, const void *v, uint32_t len) { enter(ctx); if (vec_bytes(count, 9, len)) glUniformMatrix3fv(loc, count, t, v); }
static void w_glUniformMatrix4fv(void *ctx, int32_t loc, int32_t count, uint8_t t, const void *v, uint32_t len) { enter(ctx); if (vec_bytes(count, 16, len)) glUniformMatrix4fv(loc, count, t, v); }

static void w_glVertexAttrib1f(void *ctx, uint32_t i, float x) { enter(ctx); glVertexAttrib1f(i, x); }
static void w_glVertexAttrib2f(void *ctx, uint32_t i, float x, float y) { enter(ctx); glVertexAttrib2f(i, x, y); }
static void w_glVertexAttrib3f(void *ctx, uint32_t i, float x, float y, float z) { enter(ctx); glVertexAttrib3f(i, x, y, z); }
static void w_glVertexAttrib4f(void *ctx, uint32_t i, float x, float y, float z, float w) { enter(ctx); glVertexAttrib4f(i, x, y, z, w); }
static void w_glVertexAttrib4fv(void *ctx, uint32_t i, const void *v, uint32_t len) { enter(ctx); if (len >= 16) glVertexAttrib4fv(i, v); }

/* glGetActiveAttrib and glGetActiveUniform: the name, size and type of one active variable. The host writes into a
 * local buffer (names are short), and the guest's buffers get what fits. */
static void active_info(int uniform, void *ctx, uint32_t program, uint32_t index, int32_t bufsize, void *length,
                        uint32_t length_len, void *size, uint32_t size_len, void *type, uint32_t type_len, void *name,
                        uint32_t name_len)
{
    enter(ctx);
    char tmp[256];
    GLsizei want = bufsize > 0 ? bufsize : 0;
    if (want > (GLsizei)sizeof tmp)
        want = (GLsizei)sizeof tmp;
    GLsizei lenv = 0;
    GLint sz = 0;
    GLenum ty = 0;
    if (want > 0) {
        if (uniform)
            glGetActiveUniform(program, index, want, &lenv, &sz, &ty, tmp);
        else
            glGetActiveAttrib(program, index, want, &lenv, &sz, &ty, tmp);
    }
    if (length_len >= 4)
        memcpy(length, &lenv, 4);
    if (size_len >= 4)
        memcpy(size, &sz, 4);
    if (type_len >= 4) {
        uint32_t t = ty;
        memcpy(type, &t, 4);
    }
    if (name && name_len > 0) {
        uint32_t n = lenv > 0 ? (uint32_t)lenv : 0;
        if (n + 1 > name_len)
            n = name_len - 1;
        memcpy(name, tmp, n);
        ((char *)name)[n] = 0;
    }
}

static void w_glGetActiveAttrib(void *ctx, uint32_t program, uint32_t index, int32_t bufsize, void *length, uint32_t length_len,
                                void *size, uint32_t size_len, void *type, uint32_t type_len, void *name, uint32_t name_len)
{
    active_info(0, ctx, program, index, bufsize, length, length_len, size, size_len, type, type_len, name, name_len);
}

static void w_glGetActiveUniform(void *ctx, uint32_t program, uint32_t index, int32_t bufsize, void *length, uint32_t length_len,
                                 void *size, uint32_t size_len, void *type, uint32_t type_len, void *name, uint32_t name_len)
{
    active_info(1, ctx, program, index, bufsize, length, length_len, size, size_len, type, type_len, name, name_len);
}

/* Vertex arrays and draws. */

static void w_glEnableVertexAttribArray(void *ctx, uint32_t index) { enter(ctx); glEnableVertexAttribArray(index); }
static void w_glDisableVertexAttribArray(void *ctx, uint32_t index) { enter(ctx); glDisableVertexAttribArray(index); }

static void w_glGenVertexArrays(void *ctx, int32_t n, void *arrays, uint32_t len)
{
    enter(ctx);
    if (n < 0 || (uint64_t)n * 4 > len)
        return;
    glGenVertexArrays(n, arrays);
}

static void w_glDeleteVertexArrays(void *ctx, int32_t n, const void *arrays, uint32_t len)
{
    enter(ctx);
    if (n < 0 || (uint64_t)n * 4 > len)
        return;
    glDeleteVertexArrays(n, arrays);
}

static void w_glBindVertexArray(void *ctx, uint32_t array)
{
    gctx_t *c = enter(ctx);
    c->vao = array;
    glBindVertexArray(array);
}

static void w_glDrawArrays(void *ctx, uint32_t mode, int32_t first, int32_t count)
{
    enter(ctx);
    glDrawArrays(mode, first, count);
}

/* The client-array calls (gl_host.c decodes the wire). The data goes to a scratch buffer, one per attribute index,
 * packed tightly by the guest (glUtilsPackPointerData), so the attribute's stride is 0. The bindings the engine
 * changes are restored. */
static void w_glVertexAttribPointerData(void *ctx, uint32_t index, int32_t size, uint32_t type, uint8_t normalized,
                                        int32_t stride, const void *data, uint32_t datalen)
{
    (void)stride;
    enter(ctx);
    if (index >= NATTR)
        return;
    hstate_t hs;
    save_host(&hs);
    if (!scratch_vbo[index])
        glGenBuffers(1, &scratch_vbo[index]);
    glBindBuffer(GL_ARRAY_BUFFER, scratch_vbo[index]);
    glBufferData(GL_ARRAY_BUFFER, datalen, data, GL_STREAM_DRAW);
    glVertexAttribPointer(index, size, type, normalized, 0, NULL);
    restore_host(&hs);
}

static void w_glVertexAttribPointerOffset(void *ctx, uint32_t index, int32_t size, uint32_t type, uint8_t normalized,
                                          int32_t stride, uint32_t offset)
{
    enter(ctx);
    glVertexAttribPointer(index, size, type, normalized, stride, (const void *)(uintptr_t)offset);
}

static void w_glDrawElementsData(void *ctx, uint32_t mode, int32_t count, uint32_t type, const void *data, uint32_t datalen)
{
    enter(ctx);
    hstate_t hs;
    save_host(&hs);
    if (!scratch_ebo)
        glGenBuffers(1, &scratch_ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, scratch_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, datalen, data, GL_STREAM_DRAW);
    glDrawElements(mode, count, type, NULL);
    restore_host(&hs);
}

static void w_glDrawElementsOffset(void *ctx, uint32_t mode, int32_t count, uint32_t type, uint32_t offset)
{
    enter(ctx);
    glDrawElements(mode, count, type, (const void *)(uintptr_t)offset);
}

/* glShaderString: the source as the guest sent it, translated (translate_shader), replacing the shader's source. */
static void w_glShaderString(void *ctx, uint32_t shader, const void *string, uint32_t len)
{
    enter(ctx);
    uint32_t n = 0;
    char *src = translate_shader(string, len, &n);
    if (!src)
        return;
    GLint l = (GLint)n;
    const GLchar *p = src;
    glShaderSource(shader, 1, &p, &l);
    free(src);
}

static int32_t w_glFinishRoundTrip(void *ctx)
{
    enter(ctx);
    glFinish();
    return 0;
}

/* A self test of the frame path, with no guest (web build only; tests/web.mjs, XIAOJI_WEB_FRAMES=1): one clear, a
 * scissored clear, then rcFBPost, through the same entry points the guest's calls use. Returns the frames posted, or
 * -1 when the backend does not start. The page checks the pixels: the clear colour at (10, 10) of the top-first frame,
 * and the scissored colour inside the box (x 100..299, y 100..399 from the bottom, so rows 560..859 from the top). */
static char selftest_stream;
static uint32_t selftest_alias_px; /* the alias path's pixel, RGB packed (0xRRGGBB) */

int EMSCRIPTEN_KEEPALIVE xj_gl_selftest(void)
{
    const gl_host_t *h = gl_host_webgl();
    if (!h)
        return -1;
    void *k = &selftest_stream;
    const gles2_backend_t *g2 = h->gles2;
    uint32_t ctx = rc_CreateContext(NULL, 0, 0, 0x30000);
    uint32_t surf = rc_CreateWindowSurface(NULL, 0, XJ_FB_WIDTH, XJ_FB_HEIGHT);
    uint32_t cb = rc_CreateColorBuffer(NULL, XJ_FB_WIDTH, XJ_FB_HEIGHT, GL_RGBA);
    rc_SetWindowColorBuffer(NULL, surf, cb);
    rc_MakeCurrent(k, ctx, surf, surf);
    enter(k);
    /* The picture: a clear, and a scissored clear. */
    glClearColor(0.1f, 0.5f, 0.9f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);
    glScissor(100, 100, 200, 300);
    glClearColor(1.0f, 0.2f, 0.2f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    /* The alias path, as RenderEngine's output texture is used: a guest texture name bound, rcBindTexture on the
     * color buffer, the name attached to a guest FBO, and a clear through that FBO. */
    uint32_t cb4 = rc_CreateColorBuffer(NULL, 4, 4, GL_RGBA);
    GLuint tn = 0, fb = 0;
    g2->glGenTextures(k, 1, &tn, sizeof tn);
    g2->glBindTexture(k, GL_TEXTURE_2D, tn);
    rc_BindTexture(k, cb4);
    g2->glGenFramebuffers(k, 1, &fb, sizeof fb);
    g2->glBindFramebuffer(k, GL_FRAMEBUFFER, fb);
    g2->glFramebufferTexture2D(k, GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tn, 0);
    g2->glClearColor(k, 0.9f, 0.9f, 0.1f, 1.0f);
    g2->glClear(k, GL_COLOR_BUFFER_BIT);
    g2->glBindFramebuffer(k, GL_FRAMEBUFFER, 0);
    uint8_t px[4] = {0, 0, 0, 0};
    rc_ReadColorBuffer(k, cb4, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px, sizeof px);
    selftest_alias_px = (uint32_t)px[0] << 16 | (uint32_t)px[1] << 8 | px[2];
    /* The frame: the surface's picture. */
    glFinish();
    rc_FBPost(NULL, cb);
    rc_MakeCurrent(k, 0, 0, 0);
    return (int)frames_posted;
}

uint32_t EMSCRIPTEN_KEEPALIVE xj_gl_selftest_alias(void)
{
    return selftest_alias_px;
}

/* ---------- the backend tables ---------- */

static renderControl_backend_t rc_table;
static gles2_backend_t gles2_table;
static gles1_backend_t gles1_table;
static gl_custom_backend_t custom_table;
static gl_host_t host_table;
static int table_ready;

/* The web backend: the null backend's answers, with the calls above in place of their defaults. The gles1 calls
 * stay the null ones (the guest's GLES 1 is not used by the framework). */
static int init_gl(void)
{
    if (ready >= 0)
        return ready;
    ready = 0;
    if (!xj_gl_context_create()) {
        fprintf(stderr, "xiaoji: gl(webgl): no WebGL2 context on the worker's canvas; the null backend answers\n");
        return 0;
    }
    glViewport(0, 0, XJ_FB_WIDTH, XJ_FB_HEIGHT);
    glScissor(0, 0, XJ_FB_WIDTH, XJ_FB_HEIGHT);
    dflt = NULL;
    for (int i = 0; i < NCTX && !dflt; i++)
        if (!ctxs[i].used) {
            ctxs[i].used = 1;
            ctxs[i].handle = next_handle++;
            ctxs[i].vp[2] = XJ_FB_WIDTH;
            ctxs[i].vp[3] = XJ_FB_HEIGHT;
            ctxs[i].sc[2] = XJ_FB_WIDTH;
            ctxs[i].sc[3] = XJ_FB_HEIGHT;
            dflt = &ctxs[i];
        }
    ready = 1;
    return 1;
}

const gl_host_t *gl_host_webgl(void)
{
    if (table_ready)
        return &host_table;
    if (!init_gl())
        return NULL;
    const gl_host_t *n = gl_host_null();
    rc_table = *n->rc;
    gles2_table = *n->gles2;
    gles1_table = *n->gles1;
    custom_table = *n->custom;

    rc_table.rcCreateContext = rc_CreateContext;
    rc_table.rcDestroyContext = rc_DestroyContext;
    rc_table.rcCreateWindowSurface = rc_CreateWindowSurface;
    rc_table.rcDestroyWindowSurface = rc_DestroyWindowSurface;
    rc_table.rcSetWindowColorBuffer = rc_SetWindowColorBuffer;
    rc_table.rcFlushWindowColorBuffer = rc_FlushWindowColorBuffer;
    rc_table.rcMakeCurrent = rc_MakeCurrent;
    rc_table.rcMakeCurrentAsync = rc_MakeCurrentAsync;
    rc_table.rcFlushWindowColorBufferAsync = rc_FlushWindowColorBufferAsync;
    rc_table.rcFlushWindowColorBufferAsyncWithFrameNumber = rc_FlushWindowColorBufferAsyncWithFrameNumber;
    rc_table.rcCreateColorBuffer = rc_CreateColorBuffer;
    rc_table.rcCreateColorBufferDMA = rc_CreateColorBufferDMA;
    rc_table.rcCloseColorBuffer = rc_CloseColorBuffer;
    rc_table.rcReadColorBuffer = rc_ReadColorBuffer;
    rc_table.rcUpdateColorBuffer = rc_UpdateColorBuffer;
    rc_table.rcBindTexture = rc_BindTexture;
    rc_table.rcBindRenderbuffer = rc_BindRenderbuffer;
    rc_table.rcCreateClientImage = rc_CreateClientImage;
    rc_table.rcDestroyClientImage = rc_DestroyClientImage;
    rc_table.rcFBPost = rc_FBPost;
    rc_table.rcGetGLString = rc_GetGLString;
    rc_table.rcQueryEGLString = rc_QueryEGLString;

    gles2_table.glEnable = w_glEnable;
    gles2_table.glDisable = w_glDisable;
    gles2_table.glIsEnabled = w_glIsEnabled;
    gles2_table.glBlendColor = w_glBlendColor;
    gles2_table.glBlendEquation = w_glBlendEquation;
    gles2_table.glBlendEquationSeparate = w_glBlendEquationSeparate;
    gles2_table.glBlendFunc = w_glBlendFunc;
    gles2_table.glBlendFuncSeparate = w_glBlendFuncSeparate;
    gles2_table.glClearColor = w_glClearColor;
    gles2_table.glClearDepthf = w_glClearDepthf;
    gles2_table.glClearStencil = w_glClearStencil;
    gles2_table.glClear = w_glClear;
    gles2_table.glColorMask = w_glColorMask;
    gles2_table.glCullFace = w_glCullFace;
    gles2_table.glDepthFunc = w_glDepthFunc;
    gles2_table.glDepthMask = w_glDepthMask;
    gles2_table.glDepthRangef = w_glDepthRangef;
    gles2_table.glFrontFace = w_glFrontFace;
    gles2_table.glHint = w_glHint;
    gles2_table.glLineWidth = w_glLineWidth;
    gles2_table.glPolygonOffset = w_glPolygonOffset;
    gles2_table.glSampleCoverage = w_glSampleCoverage;
    gles2_table.glStencilFunc = w_glStencilFunc;
    gles2_table.glStencilFuncSeparate = w_glStencilFuncSeparate;
    gles2_table.glStencilMask = w_glStencilMask;
    gles2_table.glStencilMaskSeparate = w_glStencilMaskSeparate;
    gles2_table.glStencilOp = w_glStencilOp;
    gles2_table.glStencilOpSeparate = w_glStencilOpSeparate;
    gles2_table.glPixelStorei = w_glPixelStorei;
    gles2_table.glFlush = w_glFlush;
    gles2_table.glFinish = w_glFinish;
    gles2_table.glGetError = w_glGetError;
    gles2_table.glViewport = w_glViewport;
    gles2_table.glScissor = w_glScissor;
    gles2_table.glGetIntegerv = w_glGetIntegerv;
    gles2_table.glGetFloatv = w_glGetFloatv;
    gles2_table.glGetBooleanv = w_glGetBooleanv;
    gles2_table.glGenBuffers = w_glGenBuffers;
    gles2_table.glDeleteBuffers = w_glDeleteBuffers;
    gles2_table.glBindBuffer = w_glBindBuffer;
    gles2_table.glBufferData = w_glBufferData;
    gles2_table.glBufferSubData = w_glBufferSubData;
    gles2_table.glGenTextures = w_glGenTextures;
    gles2_table.glDeleteTextures = w_glDeleteTextures;
    gles2_table.glActiveTexture = w_glActiveTexture;
    gles2_table.glBindTexture = w_glBindTexture;
    gles2_table.glTexImage2D = w_glTexImage2D;
    gles2_table.glTexSubImage2D = w_glTexSubImage2D;
    gles2_table.glTexStorage2D = w_glTexStorage2D;
    gles2_table.glTexParameteri = w_glTexParameteri;
    gles2_table.glTexParameterf = w_glTexParameterf;
    gles2_table.glGenerateMipmap = w_glGenerateMipmap;
    gles2_table.glEGLImageTargetTexture2DOES = w_glEGLImageTargetTexture2DOES;
    gles2_table.glGenFramebuffers = w_glGenFramebuffers;
    gles2_table.glDeleteFramebuffers = w_glDeleteFramebuffers;
    gles2_table.glBindFramebuffer = w_glBindFramebuffer;
    gles2_table.glCheckFramebufferStatus = w_glCheckFramebufferStatus;
    gles2_table.glFramebufferTexture2D = w_glFramebufferTexture2D;
    gles2_table.glGenRenderbuffers = w_glGenRenderbuffers;
    gles2_table.glDeleteRenderbuffers = w_glDeleteRenderbuffers;
    gles2_table.glBindRenderbuffer = w_glBindRenderbuffer;
    gles2_table.glRenderbufferStorage = w_glRenderbufferStorage;
    gles2_table.glFramebufferRenderbuffer = w_glFramebufferRenderbuffer;
    gles2_table.glEGLImageTargetRenderbufferStorageOES = w_glEGLImageTargetRenderbufferStorageOES;
    gles2_table.glReadPixels = w_glReadPixels;
    gles2_table.glCreateShader = w_glCreateShader;
    gles2_table.glDeleteShader = w_glDeleteShader;
    gles2_table.glCompileShader = w_glCompileShader;
    gles2_table.glGetShaderiv = w_glGetShaderiv;
    gles2_table.glGetShaderInfoLog = w_glGetShaderInfoLog;
    gles2_table.glCreateProgram = w_glCreateProgram;
    gles2_table.glDeleteProgram = w_glDeleteProgram;
    gles2_table.glAttachShader = w_glAttachShader;
    gles2_table.glDetachShader = w_glDetachShader;
    gles2_table.glLinkProgram = w_glLinkProgram;
    gles2_table.glUseProgram = w_glUseProgram;
    gles2_table.glValidateProgram = w_glValidateProgram;
    gles2_table.glGetProgramiv = w_glGetProgramiv;
    gles2_table.glGetProgramInfoLog = w_glGetProgramInfoLog;
    gles2_table.glBindAttribLocation = w_glBindAttribLocation;
    gles2_table.glGetAttribLocation = w_glGetAttribLocation;
    gles2_table.glGetUniformLocation = w_glGetUniformLocation;
    gles2_table.glUniform1i = w_glUniform1i;
    gles2_table.glUniform1f = w_glUniform1f;
    gles2_table.glUniform2f = w_glUniform2f;
    gles2_table.glUniform3f = w_glUniform3f;
    gles2_table.glUniform4f = w_glUniform4f;
    gles2_table.glUniform2i = w_glUniform2i;
    gles2_table.glUniform3i = w_glUniform3i;
    gles2_table.glUniform4i = w_glUniform4i;
    gles2_table.glUniform1fv = w_glUniform1fv;
    gles2_table.glUniform2fv = w_glUniform2fv;
    gles2_table.glUniform3fv = w_glUniform3fv;
    gles2_table.glUniform4fv = w_glUniform4fv;
    gles2_table.glUniform1iv = w_glUniform1iv;
    gles2_table.glUniform2iv = w_glUniform2iv;
    gles2_table.glUniform3iv = w_glUniform3iv;
    gles2_table.glUniform4iv = w_glUniform4iv;
    gles2_table.glUniformMatrix2fv = w_glUniformMatrix2fv;
    gles2_table.glUniformMatrix3fv = w_glUniformMatrix3fv;
    gles2_table.glUniformMatrix4fv = w_glUniformMatrix4fv;
    gles2_table.glVertexAttrib1f = w_glVertexAttrib1f;
    gles2_table.glVertexAttrib2f = w_glVertexAttrib2f;
    gles2_table.glVertexAttrib3f = w_glVertexAttrib3f;
    gles2_table.glVertexAttrib4f = w_glVertexAttrib4f;
    gles2_table.glVertexAttrib4fv = w_glVertexAttrib4fv;
    gles2_table.glEnableVertexAttribArray = w_glEnableVertexAttribArray;
    gles2_table.glDisableVertexAttribArray = w_glDisableVertexAttribArray;
    gles2_table.glGenVertexArrays = w_glGenVertexArrays;
    gles2_table.glDeleteVertexArrays = w_glDeleteVertexArrays;
    gles2_table.glBindVertexArray = w_glBindVertexArray;
    gles2_table.glDrawArrays = w_glDrawArrays;
    gles2_table.glGetActiveAttrib = w_glGetActiveAttrib;
    gles2_table.glGetActiveUniform = w_glGetActiveUniform;

    custom_table.glShaderString = w_glShaderString;
    custom_table.glFinishRoundTrip = w_glFinishRoundTrip;
    custom_table.glVertexAttribPointerData = w_glVertexAttribPointerData;
    custom_table.glVertexAttribPointerOffset = w_glVertexAttribPointerOffset;
    custom_table.glDrawElementsData = w_glDrawElementsData;
    custom_table.glDrawElementsOffset = w_glDrawElementsOffset;

    host_table.rc = &rc_table;
    host_table.gles2 = &gles2_table;
    host_table.gles1 = &gles1_table;
    host_table.custom = &custom_table;
    table_ready = 1;
    wlog("the WebGL2 backend is ready", NULL);
    return &host_table;
}

#else /* !XIAOJI_WEB: the native and Node builds have no WebGL */

const gl_host_t *gl_host_webgl(void)
{
    return NULL;
}

#endif
