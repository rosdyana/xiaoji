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

#include "display.h"
#include "gles2_opcodes.h"
#include "signals.h"

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
/* GL_SHADING_LANGUAGE_VERSION (0x8B8C): Skia parses the GLSL version from it, and HWUI's GrDirectContext is not made
 * without it ("Assertion failed: !grContext.get()" in RenderThread, B17; gl_null.c has had it since B16). */
static const char wgl_shading_version[] = "OpenGL ES GLSL ES 3.00 xiaoji-webgl2";
static const char wgl_extensions[] =
    "GL_OES_EGL_image_external_essl3 "
    "ANDROID_EMU_gles_max_version_3_0 ANDROID_EMU_native_sync_v2 ANDROID_EMU_gles_no_host_error "
    "ANDROID_EMU_host_composition_v1";
static const char wgl_egl_extensions[] = "EGL_ANDROID_native_fence_sync";

typedef struct gctx {
    uint32_t handle;
    int used;
    uint32_t share;
    uint32_t draw, read; /* window surface handles, 0: none */
    GLuint fbo;          /* the guest's draw framebuffer binding: 0 is the draw surface's back buffer */
    GLuint rfbo;         /* the guest's read framebuffer binding: 0 is the read surface's back buffer */
    GLuint program, vao;
    GLuint rbo;          /* the guest's renderbuffer binding */
    GLuint unit;         /* the active texture unit */
    GLuint tex[32];      /* the guest's GL_TEXTURE_2D binding on each unit (a guest name; aliases map it) */
    GLint vp[4], sc[4];  /* viewport and scissor box */
    /* The rest of the state a GLES context owns that the guest's libraries set and then assume kept (Skia caches it,
     * RenderEngine sets it per layer), switched with the context (B17; G2 left it shared): */
    uint32_t en;            /* the enables, one bit each (en_bit) */
    GLenum beq[2];          /* blend equation, RGB and alpha */
    GLenum bf[4];           /* blend func: src RGB, dst RGB, src alpha, dst alpha */
    GLfloat bc[4], cc[4];   /* blend color, clear color */
    GLboolean cm[4], dmask; /* color mask, depth mask */
    GLenum dfunc, cull, front;
    GLfloat cdepth;         /* clear depth */
    GLint cstencil;         /* clear stencil */
    GLenum sfunc[2];        /* stencil func, front and back */
    GLint sref[2];
    GLuint smask[2], swmask[2];
    GLenum sop[2][3];       /* stencil op (fail, zfail, zpass), front and back */
    GLfloat poff[2];        /* polygon offset factor and units */
    GLuint abuf;            /* GL_ARRAY_BUFFER binding (context state, not VAO state) */
    GLuint unpack_buf, pack_buf;
    GLint unpack_align, pack_align, unpack_row, unpack_skip_rows, unpack_skip_px;
    GLuint sampler[32];     /* sampler bindings per unit */
    GLuint nunits;          /* units used so far: the ones apply rebinds */
} gctx_t;

typedef struct surf {
    uint32_t handle;
    int used;
    uint32_t w, h;
    uint32_t cb; /* the color buffer (0 until one is set or needed) */
    /* The back buffer (B17): the guest's framebuffer 0 draws here, in GL's orientation, with depth and stencil; a flush
     * (eglSwapBuffers) copies it into the color buffer turned upright (row 0 the top), as gfxstream's window surfaces do.
     * So every color buffer holds its image top row first, the way gralloc memory does, whoever wrote it (GL, the CPU
     * through rcUpdateColorBuffer, RenderEngine through an EGLImage). */
    GLuint btex, bds, bfbo;
    uint32_t bw, bh;
} surf_t;

typedef struct cb {
    uint32_t handle;
    int used;
    uint32_t w, h;
    GLuint tex, fbo;
    int refs; /* the creation reference (held by the allocator's refcount pipe) and one per open (rcOpenColorBuffer) */
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

/* ---------- wire data into the GL entry points ----------
 * The guest's values sit at packet offsets, so they are not necessarily 4-byte aligned, and emscripten's GL glue reads and
 * writes 32-bit and float arguments through typed arrays (HEAP32[p >> 2] and so on), which needs alignment. Values are
 * therefore copied into aligned memory around the call. Byte arguments (glBufferData, the RGBA8 pixels) need no copy. */
static void *copy_in(const void *p, size_t n)
{
    void *c = malloc(n ? n : 1);
    if (c && n)
        memcpy(c, p, n);
    return c;
}

/* glGen*: the names are made in aligned memory, then copied to the guest's buffer (n names, len bytes). */
static void gen_into(void (*gen)(GLsizei, GLuint *), int32_t n, void *out, uint32_t len)
{
    if (n <= 0 || (uint64_t)n * 4 > len)
        return;
    GLuint *t = malloc((size_t)n * 4);
    if (!t)
        return;
    gen((GLsizei)n, t);
    memcpy(out, t, (size_t)n * 4);
    free(t);
}

/* glDelete*: the names are copied to aligned memory first. */
static void delete_from(void (*del)(GLsizei, const GLuint *), int32_t n, const void *in, uint32_t len)
{
    if (n <= 0 || (uint64_t)n * 4 > len)
        return;
    GLuint *t = copy_in(in, (size_t)n * 4);
    if (!t)
        return;
    del((GLsizei)n, t);
    free(t);
}

/* ---------- host state, saved and restored around the engine's own GL calls ---------- */

typedef struct hstate {
    GLint unit, tex2d, fbo, rfbo, program, vao, abuf, ebuf, rbo, unpack_buf, pack_buf;
} hstate_t;

static gctx_t *applied;

static void save_host(hstate_t *s)
{
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->unit);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex2d);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s->rfbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->abuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s->ebuf);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &s->rbo);
    /* The engine's uploads and read-backs use client memory: no pixel buffer may be bound for them. */
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &s->unpack_buf);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &s->pack_buf);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

static void restore_host(const hstate_t *s)
{
    glBindVertexArray((GLuint)s->vao);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)s->ebuf);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)s->abuf);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)s->fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)s->rfbo);
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)s->rbo);
    glUseProgram((GLuint)s->program);
    glActiveTexture((GLenum)s->unit);
    glBindTexture(GL_TEXTURE_2D, (GLuint)s->tex2d);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)s->unpack_buf);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)s->pack_buf);
    /* The pixel store the engine's own calls set: the applied context's values come back (the GLES defaults if none). */
    glPixelStorei(GL_UNPACK_ALIGNMENT, applied ? applied->unpack_align : 4);
    glPixelStorei(GL_PACK_ALIGNMENT, applied ? applied->pack_align : 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, applied ? applied->unpack_row : 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, applied ? applied->unpack_skip_rows : 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, applied ? applied->unpack_skip_px : 0);
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
    c->refs = 1;
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
        cb_t *c = cb_create(s->w ? s->w : XJ_DISPLAY_W, s->h ? s->h : XJ_DISPLAY_H);
        if (!c)
            return NULL;
        s->cb = c->handle;
    }
    return cb_find(s->cb);
}

/* The surface's back buffer, made (or remade, when the size changes) the size of its color buffer, else of the surface. */
static GLuint surf_back(surf_t *s)
{
    if (!s)
        return 0;
    cb_t *cb = s->cb ? cb_find(s->cb) : NULL;
    uint32_t w = cb ? cb->w : (s->w ? s->w : XJ_DISPLAY_W), h = cb ? cb->h : (s->h ? s->h : XJ_DISPLAY_H);
    if (s->bfbo && s->bw == w && s->bh == h)
        return s->bfbo;
    if (!w || !h || (uint64_t)w * h > (1u << 24))
        return 0;
    hstate_t hs;
    save_host(&hs);
    if (s->bfbo) {
        glDeleteFramebuffers(1, &s->bfbo);
        glDeleteTextures(1, &s->btex);
        glDeleteRenderbuffers(1, &s->bds);
    }
    glGenTextures(1, &s->btex);
    glBindTexture(GL_TEXTURE_2D, s->btex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenRenderbuffers(1, &s->bds);
    glBindRenderbuffer(GL_RENDERBUFFER, s->bds);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)w, (GLsizei)h);
    glGenFramebuffers(1, &s->bfbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s->bfbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->btex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s->bds);
    restore_host(&hs);
    s->bw = w;
    s->bh = h;
    return s->bfbo;
}

static void surf_free_back(surf_t *s)
{
    if (!s->bfbo)
        return;
    glDeleteFramebuffers(1, &s->bfbo);
    glDeleteTextures(1, &s->btex);
    glDeleteRenderbuffers(1, &s->bds);
    s->bfbo = s->btex = s->bds = 0;
}

/* The host framebuffer a guest framebuffer name stands for. 0 is the draw surface's back buffer. */
static GLuint host_fbo(const gctx_t *c, GLuint guest)
{
    if (guest)
        return guest;
    return surf_back(surf_find(c->draw));
}

/* The same for the read binding: 0 is the read surface's back buffer. */
static GLuint host_rfbo(const gctx_t *c, GLuint guest)
{
    if (guest)
        return guest;
    return surf_back(surf_find(c->read ? c->read : c->draw));
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

/* The enables a context keeps (WebGL 2 has these; GL_PRIMITIVE_RESTART_FIXED_INDEX is always on there). */
static const GLenum en_caps[] = {GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_DITHER, GL_POLYGON_OFFSET_FILL,
                                 GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE, GL_SCISSOR_TEST, GL_STENCIL_TEST,
                                 GL_RASTERIZER_DISCARD};
#define NEN ((int)(sizeof en_caps / sizeof en_caps[0]))

static int en_bit(GLenum cap)
{
    for (int i = 0; i < NEN; i++)
        if (en_caps[i] == cap)
            return i;
    return -1;
}

/* The GLES defaults of a new context (the spec's initial state). */
static void ctx_defaults(gctx_t *c)
{
    c->en = 1u << en_bit(GL_DITHER);
    c->beq[0] = c->beq[1] = GL_FUNC_ADD;
    c->bf[0] = c->bf[2] = GL_ONE;
    c->bf[1] = c->bf[3] = GL_ZERO;
    c->cm[0] = c->cm[1] = c->cm[2] = c->cm[3] = GL_TRUE;
    c->dmask = GL_TRUE;
    c->dfunc = GL_LESS;
    c->cull = GL_BACK;
    c->front = GL_CCW;
    c->cdepth = 1.0f;
    for (int f = 0; f < 2; f++) {
        c->sfunc[f] = GL_ALWAYS;
        c->smask[f] = c->swmask[f] = 0xFFFFFFFFu;
        c->sop[f][0] = c->sop[f][1] = c->sop[f][2] = GL_KEEP;
    }
    c->unpack_align = c->pack_align = 4;
    c->nunits = 1;
}

static void apply(gctx_t *c)
{
    if (c == applied)
        return;
    applied = c;
    if (!c)
        return;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, host_fbo(c, c->fbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, host_rfbo(c, c->rfbo));
    glUseProgram(c->program);
    glBindVertexArray(c->vao);
    glBindBuffer(GL_ARRAY_BUFFER, c->abuf);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, c->unpack_buf);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, c->pack_buf);
    for (GLuint u = 0; u < c->nunits; u++) {
        glActiveTexture(GL_TEXTURE0 + u);
        glBindTexture(GL_TEXTURE_2D, host_tex(c->tex[u]));
        glBindSampler(u, c->sampler[u]);
    }
    glActiveTexture(GL_TEXTURE0 + c->unit);
    glBindTexture(GL_TEXTURE_2D, host_tex(c->tex[c->unit]));
    glViewport(c->vp[0], c->vp[1], c->vp[2], c->vp[3]);
    glScissor(c->sc[0], c->sc[1], c->sc[2], c->sc[3]);
    for (int i = 0; i < NEN; i++) {
        if (c->en & (1u << i))
            glEnable(en_caps[i]);
        else
            glDisable(en_caps[i]);
    }
    glBlendEquationSeparate(c->beq[0], c->beq[1]);
    glBlendFuncSeparate(c->bf[0], c->bf[1], c->bf[2], c->bf[3]);
    glBlendColor(c->bc[0], c->bc[1], c->bc[2], c->bc[3]);
    glClearColor(c->cc[0], c->cc[1], c->cc[2], c->cc[3]);
    glColorMask(c->cm[0], c->cm[1], c->cm[2], c->cm[3]);
    glDepthMask(c->dmask);
    glDepthFunc(c->dfunc);
    glCullFace(c->cull);
    glFrontFace(c->front);
    glClearDepthf(c->cdepth);
    glClearStencil(c->cstencil);
    for (int f = 0; f < 2; f++) {
        GLenum face = f ? GL_BACK : GL_FRONT;
        glStencilFuncSeparate(face, c->sfunc[f], c->sref[f], c->smask[f]);
        glStencilMaskSeparate(face, c->swmask[f]);
        glStencilOpSeparate(face, c->sop[f][0], c->sop[f][1], c->sop[f][2]);
    }
    glPolygonOffset(c->poff[0], c->poff[1]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, c->unpack_align);
    glPixelStorei(GL_PACK_ALIGNMENT, c->pack_align);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, c->unpack_row);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, c->unpack_skip_rows);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, c->unpack_skip_px);
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
    c->vp[2] = XJ_DISPLAY_W;
    c->vp[3] = XJ_DISPLAY_H;
    c->sc[2] = XJ_DISPLAY_W;
    c->sc[3] = XJ_DISPLAY_H;
    ctx_defaults(c);
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
    if (s) {
        surf_free_back(s);
        s->used = 0;
        applied = NULL; /* a context drawing to it rebinds on its next call */
    }
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

/* eglSwapBuffers: the back buffer goes into the surface's color buffer, turned upright (gfxstream's
 * WindowSurface::flushColorBuffer). The blit is not clipped by the guest's scissor or masked by its color mask. */
static int32_t rc_FlushWindowColorBuffer(void *ctx, uint32_t windowSurface)
{
    (void)ctx;
    surf_t *s = surf_find(windowSurface);
    cb_t *cb = s ? surf_cb(s) : NULL;
    if (!cb || !s->bfbo || s->bw != cb->w || s->bh != cb->h) {
        static int skipped;
        if (sig_debug_faults() && skipped++ < 20)
            fprintf(stderr, "xiaoji: gl(webgl): flush of surface %u skipped (cb %s, back %ux%u)\n", (unsigned)windowSurface,
                    cb ? "set" : "none", s ? (unsigned)s->bw : 0, s ? (unsigned)s->bh : 0);
        return 0;
    }
    hstate_t hs;
    save_host(&hs);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_RASTERIZER_DISCARD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s->bfbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, cb->fbo);
    glBlitFramebuffer(0, 0, (GLint)cb->w, (GLint)cb->h, 0, (GLint)cb->h, (GLint)cb->w, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    restore_host(&hs);
    applied = NULL; /* the enables and the mask come back from the context on its next call */
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

/* A reference per open and per close. The buffer is deleted with its last reference (the allocator's refcount pipe
 * closing releases its own, goldfish_free), so an importer that unregisters does not delete a buffer another process
 * still composes from. */
static void rc_OpenColorBuffer(void *ctx, uint32_t colorbuffer)
{
    (void)ctx;
    cb_t *c = cb_find(colorbuffer);
    if (c)
        c->refs++;
}

static int32_t rc_OpenColorBuffer2(void *ctx, uint32_t colorbuffer)
{
    rc_OpenColorBuffer(ctx, colorbuffer);
    return 0;
}

static void rc_CloseColorBuffer(void *ctx, uint32_t colorbuffer)
{
    (void)ctx;
    cb_t *c = cb_find(colorbuffer);
    if (c && --c->refs <= 0)
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

/* The pixels of an update as RGBA8 (the color buffers' format): RGBA8 as it is; RGB565, RGB888 and BGRA8 (the gralloc
 * formats a CPU-drawn buffer can have, gralloc_30.cpp's glFormat and glType) converted, with alpha 255 for the RGB
 * forms. Returns the RGBA8 bytes (pixels itself, or *tmp to free), or NULL for a format it does not take or a short
 * buffer. */
static const void *as_rgba8(uint32_t format, uint32_t type, const void *pixels, uint32_t pixels_len, int32_t width,
                            int32_t height, uint8_t **tmp)
{
    uint64_t n = (uint64_t)width * (uint64_t)height;
    *tmp = NULL;
    if (format == GL_RGBA && type == GL_UNSIGNED_BYTE)
        return n * 4 <= pixels_len ? pixels : NULL;
    unsigned bpp = (format == GL_RGB && type == GL_UNSIGNED_SHORT_5_6_5) ? 2
                   : (format == GL_RGB && type == GL_UNSIGNED_BYTE) ? 3
                   : (format == 0x80E1 /* GL_BGRA_EXT */ && type == GL_UNSIGNED_BYTE) ? 4 : 0;
    if (!bpp || n * bpp > pixels_len || !(*tmp = malloc(n * 4 ? n * 4 : 1)))
        return NULL;
    const uint8_t *s = pixels;
    uint8_t *d = *tmp;
    for (uint64_t i = 0; i < n; i++, s += bpp, d += 4) {
        if (bpp == 2) {
            unsigned v = s[0] | (unsigned)s[1] << 8;
            unsigned r = v >> 11, g = (v >> 5) & 63, b = v & 31;
            d[0] = (uint8_t)((r << 3) | (r >> 2));
            d[1] = (uint8_t)((g << 2) | (g >> 4));
            d[2] = (uint8_t)((b << 3) | (b >> 2));
            d[3] = 255;
        } else if (bpp == 3) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 255;
        } else {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = s[3];
        }
    }
    return *tmp;
}

static int32_t rc_UpdateColorBuffer(void *ctx, uint32_t colorbuffer, int32_t x, int32_t y, int32_t width, int32_t height,
                                    uint32_t format, uint32_t type, const void *pixels, uint32_t pixels_len)
{
    (void)ctx;
    cb_t *c = cb_find(colorbuffer);
    if (!c || !cb_rect_ok(c, x, y, width, height))
        return -1;
    uint8_t *tmp;
    const void *rgba = as_rgba8(format, type, pixels, pixels_len, width, height, &tmp);
    if (!rgba)
        return -1;
    hstate_t hs;
    save_host(&hs);
    glBindTexture(GL_TEXTURE_2D, c->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    restore_host(&hs);
    free(tmp);
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

/* The composed frame (rcFBPost): read back (a color buffer holds its top row first, see surf_t), alpha set to 255, and
 * posted if the page has consumed the last frame. Dropped frames cost nothing (no read). */
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
    for (size_t i = 3; i < (size_t)w * h * 4; i += 4)
        px[i] = 255;
    frames_posted++;
    xj_frame_send(px, (int)w, (int)h, (int)frames_posted);
    if (frames_posted <= 3)
        fprintf(stderr, "xiaoji: gl(webgl): frame %u posted (%ux%u)\n", (unsigned)frames_posted, (unsigned)w, (unsigned)h);
    free(px);
}

/* ---------- host composition (ANDROID_EMU_host_composition_v1) ----------
 * The composer (hwc2 HostComposer.cpp) sends the layers of a frame as a ComposeDevice (v1: version, targetHandle,
 * numLayers; v2 adds displayId after the version) followed by ComposeLayer records of 56 bytes: cbHandle, composeMode
 * (2 Device, 3 SolidColor), displayFrame (int left, top, right, bottom), crop (float left, top, right, bottom), blendMode
 * (1 none, 2 premultiplied, 3 coverage), alpha (float), color (r, g, b, a bytes) and transform (1 FLIP_H, 2 FLIP_V,
 * 4 ROT_90). The host draws them in order into the target color buffer, then posts it (rcCompose) or not
 * (rcComposeWithoutPost). Every color buffer holds its top row first, so display and buffer coordinates map onto
 * texture coordinates without a flip. */
#define COMPOSE_LAYER_SIZE 56

static GLuint comp_prog, comp_vao, comp_vbo;
static GLint comp_u_tex, comp_u_color, comp_u_mode, comp_u_alpha;

static GLuint comp_shader(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
        fprintf(stderr, "xiaoji: gl(webgl): the composition shader does not compile\n");
    return sh;
}

static int comp_init(void)
{
    if (comp_prog)
        return 1;
    static const char vs[] = "#version 300 es\nlayout(location = 0) in vec2 a_pos;\nlayout(location = 1) in vec2 a_uv;\n"
                             "out vec2 v_uv;\nvoid main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
    static const char fs[] = "#version 300 es\nprecision mediump float;\nin vec2 v_uv;\nuniform sampler2D u_tex;\n"
                             "uniform vec4 u_color;\nuniform int u_mode;\nuniform float u_alpha;\nout vec4 o;\n"
                             "void main() {\n"
                             "  vec4 c = u_mode == 1 ? u_color : texture(u_tex, v_uv);\n"
                             "  if (u_mode == 2) c.a = 1.0;\n" /* blend none: the layer is opaque */
                             "  o = u_mode == 3 ? vec4(c.rgb, c.a * u_alpha) : c * u_alpha;\n"
                             "}\n";
    GLuint v = comp_shader(GL_VERTEX_SHADER, vs), f = comp_shader(GL_FRAGMENT_SHADER, fs);
    comp_prog = glCreateProgram();
    glAttachShader(comp_prog, v);
    glAttachShader(comp_prog, f);
    glLinkProgram(comp_prog);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(comp_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        fprintf(stderr, "xiaoji: gl(webgl): the composition program does not link\n");
        glDeleteProgram(comp_prog);
        comp_prog = 0;
        return 0;
    }
    comp_u_tex = glGetUniformLocation(comp_prog, "u_tex");
    comp_u_color = glGetUniformLocation(comp_prog, "u_color");
    comp_u_mode = glGetUniformLocation(comp_prog, "u_mode");
    comp_u_alpha = glGetUniformLocation(comp_prog, "u_alpha");
    glGenVertexArrays(1, &comp_vao);
    glGenBuffers(1, &comp_vbo);
    glBindVertexArray(comp_vao);
    glBindBuffer(GL_ARRAY_BUFFER, comp_vbo);
    glBufferData(GL_ARRAY_BUFFER, 16 * sizeof(float), NULL, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (const void *)(2 * sizeof(float)));
    return 1;
}

static uint32_t rd_u32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static float rd_f32(const uint8_t *p)
{
    float v;
    memcpy(&v, p, 4);
    return v;
}

static int32_t compose(const void *buffer, uint32_t buffer_len, int post)
{
    const uint8_t *b = buffer;
    if (buffer_len < 12)
        return -1;
    uint32_t version = rd_u32(b);
    uint32_t hdr = version == 2 ? 16 : 12;
    if (buffer_len < hdr)
        return -1;
    uint32_t target = rd_u32(b + hdr - 8), n = rd_u32(b + hdr - 4);
    if ((uint64_t)n * COMPOSE_LAYER_SIZE > buffer_len - hdr)
        return -1;
    cb_t *t = cb_find(target);
    /* --debug-faults (XIAOJI_DEBUG_FAULTS): the first compositions' layers on stderr, to see what SurfaceFlinger sends. */
    static int logged;
    static uint64_t last_sig;
    uint64_t sig = n;
    for (uint32_t i = 0; i < n; i++) /* the layer set: a composition is logged when it changes */
        sig = sig * 1000003u + rd_u32(b + hdr + (size_t)i * COMPOSE_LAYER_SIZE) * 31u + rd_u32(b + hdr + (size_t)i * COMPOSE_LAYER_SIZE + 4);
    int dbg = sig_debug_faults() && logged < 200 && sig != last_sig;
    if (dbg) {
        logged++;
        last_sig = sig;
        fprintf(stderr, "xiaoji: gl(webgl): compose v%u target %u (%s) %u layers%s\n", (unsigned)version, (unsigned)target,
                t ? "found" : "missing", (unsigned)n, post ? ", post" : "");
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *l = b + hdr + (size_t)i * COMPOSE_LAYER_SIZE;
            cb_t *lc = cb_find(rd_u32(l));
            fprintf(stderr, "xiaoji: gl(webgl):   layer %u cb %u (%ux%u) mode %u frame %d,%d-%d,%d crop %.0f,%.0f-%.0f,%.0f "
                            "blend %u alpha %.2f color %02x%02x%02x%02x transform %u\n",
                    (unsigned)i, (unsigned)rd_u32(l), lc ? (unsigned)lc->w : 0, lc ? (unsigned)lc->h : 0, (unsigned)rd_u32(l + 4),
                    (int)rd_u32(l + 8), (int)rd_u32(l + 12), (int)rd_u32(l + 16), (int)rd_u32(l + 20), rd_f32(l + 24),
                    rd_f32(l + 28), rd_f32(l + 32), rd_f32(l + 36), (unsigned)rd_u32(l + 40), rd_f32(l + 44), l[48], l[49],
                    l[50], l[51], (unsigned)rd_u32(l + 52));
        }
    }
    if (!t || !comp_init())
        return -1;
    hstate_t hs;
    save_host(&hs);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, (GLsizei)t->w, (GLsizei)t->h);
    for (int i = 0; i < NEN; i++)
        glDisable(en_caps[i]);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(comp_prog);
    glBindVertexArray(comp_vao);
    glBindBuffer(GL_ARRAY_BUFFER, comp_vbo);
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, 0);
    glUniform1i(comp_u_tex, 0);
    glBlendEquation(GL_FUNC_ADD);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *l = b + hdr + (size_t)i * COMPOSE_LAYER_SIZE;
        uint32_t cbh = rd_u32(l), mode = rd_u32(l + 4);
        int32_t dl = (int32_t)rd_u32(l + 8), dt = (int32_t)rd_u32(l + 12), dr = (int32_t)rd_u32(l + 16),
                db = (int32_t)rd_u32(l + 20);
        float cl = rd_f32(l + 24), ct = rd_f32(l + 28), cr = rd_f32(l + 32), cbm = rd_f32(l + 36);
        uint32_t blend = rd_u32(l + 40);
        float alpha = rd_f32(l + 44);
        const uint8_t *col = l + 48;
        uint32_t xf = rd_u32(l + 52);
        if (dr <= dl || db <= dt)
            continue;
        cb_t *src = NULL;
        if (mode == 2) { /* Device: a buffer */
            src = cb_find(cbh);
            if (!src)
                continue;
        } else if (mode != 3) { /* only SolidColor otherwise */
            continue;
        }
        /* The destination in normalised device coordinates: display row 0 is texture row 0 of the target. */
        float x0 = 2.0f * (float)dl / (float)t->w - 1.0f, x1 = 2.0f * (float)dr / (float)t->w - 1.0f;
        float y0 = 2.0f * (float)dt / (float)t->h - 1.0f, y1 = 2.0f * (float)db / (float)t->h - 1.0f;
        /* The source corners for the destination's top-left, top-right, bottom-left and bottom-right. */
        float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
        if (src) {
            u0 = cl / (float)src->w;
            u1 = cr / (float)src->w;
            v0 = ct / (float)src->h;
            v1 = cbm / (float)src->h;
        }
        float uv[4][2] = {{u0, v0}, {u1, v0}, {u0, v1}, {u1, v1}};
        if (xf & 1) /* FLIP_H */
            for (int k = 0; k < 4; k++)
                uv[k][0] = u0 + u1 - uv[k][0];
        if (xf & 2) /* FLIP_V */
            for (int k = 0; k < 4; k++)
                uv[k][1] = v0 + v1 - uv[k][1];
        if (xf & 4) { /* ROT_90 (clockwise): the destination's top-left shows the source's bottom-left */
            float r[4][2] = {{uv[2][0], uv[2][1]}, {uv[0][0], uv[0][1]}, {uv[3][0], uv[3][1]}, {uv[1][0], uv[1][1]}};
            memcpy(uv, r, sizeof uv);
        }
        float vtx[16] = {x0, y0, uv[0][0], uv[0][1], x1, y0, uv[1][0], uv[1][1],
                         x0, y1, uv[2][0], uv[2][1], x1, y1, uv[3][0], uv[3][1]};
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof vtx, vtx);
        if (src)
            glBindTexture(GL_TEXTURE_2D, src->tex);
        glUniform1i(comp_u_mode, src ? (blend == 1 ? 2 : blend == 3 ? 3 : 0) : 1);
        /* A solid color is not premultiplied: it is premultiplied here, so all modes blend as premultiplied. */
        float ca = col[3] / 255.0f;
        glUniform4f(comp_u_color, col[0] / 255.0f * ca, col[1] / 255.0f * ca, col[2] / 255.0f * ca, ca);
        if (blend == 1 && src) {
            glDisable(GL_BLEND);
            glUniform1f(comp_u_alpha, 1.0f);
        } else {
            glEnable(GL_BLEND);
            if (blend == 3) /* coverage: the buffer is not premultiplied */
                glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            else
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            glUniform1f(comp_u_alpha, alpha);
        }
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glDisable(GL_BLEND);
    restore_host(&hs);
    applied = NULL; /* the guest context's enables, blend, viewport and units come back on its next call */
    if (post)
        rc_FBPost(NULL, target);
    return 0;
}

static int32_t rc_Compose(void *ctx, uint32_t bufferSize, const void *buffer, uint32_t buffer_len)
{
    (void)ctx;
    return compose(buffer, bufferSize < buffer_len ? bufferSize : buffer_len, 1);
}

static int32_t rc_ComposeWithoutPost(void *ctx, uint32_t bufferSize, const void *buffer, uint32_t buffer_len)
{
    (void)ctx;
    return compose(buffer, bufferSize < buffer_len ? bufferSize : buffer_len, 0);
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
    case GL_SHADING_LANGUAGE_VERSION: return put_string(wgl_shading_version, buffer, buffer_len, bufferSize);
    case GL_EXTENSIONS: return put_string(wgl_extensions, buffer, buffer_len, bufferSize);
    default: return 0;
    }
}

static int32_t rc_QueryEGLString(void *ctx, uint32_t name, void *buffer, uint32_t buffer_len, int32_t bufferSize)
{
    (void)ctx;
    switch (name) {
    /* EGL_VENDOR and EGL_VERSION as gl_null.c answers them. Without a vendor the guest's eglDisplay keeps a static
     * string and frees it in eglTerminate (eglDisplay.cpp), which killed system_server in the browser (B17). */
    case 0x3053: return put_string("xiaoji", buffer, buffer_len, bufferSize);          /* EGL_VENDOR */
    case 0x3054: return put_string("1.4 xiaoji-webgl2", buffer, buffer_len, bufferSize); /* EGL_VERSION */
    case 0x3055: return put_string(wgl_egl_extensions, buffer, buffer_len, bufferSize); /* EGL_EXTENSIONS */
    case 0x308D: return put_string("OpenGL_ES", buffer, buffer_len, bufferSize);       /* EGL_CLIENT_APIS */
    default: return 0;
    }
}

/* ---------- GLES 2 and 3: the calls the guest sends ---------- */

/* State and binding calls: the guest's names, with the tracked state updated. */

static void w_glEnable(void *ctx, uint32_t cap)
{
    gctx_t *c = enter(ctx);
    int b = en_bit(cap);
    if (c && b >= 0)
        c->en |= 1u << b;
    glEnable(cap);
}
static void w_glDisable(void *ctx, uint32_t cap)
{
    gctx_t *c = enter(ctx);
    int b = en_bit(cap);
    if (c && b >= 0)
        c->en &= ~(1u << b);
    glDisable(cap);
}
static uint8_t w_glIsEnabled(void *ctx, uint32_t cap) { enter(ctx); return (uint8_t)glIsEnabled(cap); }
static void w_glBlendColor(void *ctx, float r, float g, float b, float a)
{
    gctx_t *c = enter(ctx);
    if (c) { c->bc[0] = r; c->bc[1] = g; c->bc[2] = b; c->bc[3] = a; }
    glBlendColor(r, g, b, a);
}
static void w_glBlendEquation(void *ctx, uint32_t mode)
{
    gctx_t *c = enter(ctx);
    if (c) c->beq[0] = c->beq[1] = mode;
    glBlendEquation(mode);
}
static void w_glBlendEquationSeparate(void *ctx, uint32_t m0, uint32_t m1)
{
    gctx_t *c = enter(ctx);
    if (c) { c->beq[0] = m0; c->beq[1] = m1; }
    glBlendEquationSeparate(m0, m1);
}
static void w_glBlendFunc(void *ctx, uint32_t s, uint32_t d)
{
    gctx_t *c = enter(ctx);
    if (c) { c->bf[0] = c->bf[2] = s; c->bf[1] = c->bf[3] = d; }
    glBlendFunc(s, d);
}
static void w_glBlendFuncSeparate(void *ctx, uint32_t a, uint32_t b, uint32_t c2, uint32_t d)
{
    gctx_t *c = enter(ctx);
    if (c) { c->bf[0] = a; c->bf[1] = b; c->bf[2] = c2; c->bf[3] = d; }
    glBlendFuncSeparate(a, b, c2, d);
}
static void w_glClearColor(void *ctx, float r, float g, float b, float a)
{
    gctx_t *c = enter(ctx);
    if (c) { c->cc[0] = r; c->cc[1] = g; c->cc[2] = b; c->cc[3] = a; }
    glClearColor(r, g, b, a);
}
static void w_glClearDepthf(void *ctx, float d)
{
    gctx_t *c = enter(ctx);
    if (c) c->cdepth = d;
    glClearDepthf(d);
}
static void w_glClearStencil(void *ctx, int32_t s)
{
    gctx_t *c = enter(ctx);
    if (c) c->cstencil = s;
    glClearStencil(s);
}
static void w_glClear(void *ctx, uint32_t mask) { enter(ctx); glClear(mask); }
static void w_glColorMask(void *ctx, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    gctx_t *c = enter(ctx);
    if (c) { c->cm[0] = r; c->cm[1] = g; c->cm[2] = b; c->cm[3] = a; }
    glColorMask(r, g, b, a);
}
static void w_glCullFace(void *ctx, uint32_t m)
{
    gctx_t *c = enter(ctx);
    if (c) c->cull = m;
    glCullFace(m);
}
static void w_glDepthFunc(void *ctx, uint32_t f)
{
    gctx_t *c = enter(ctx);
    if (c) c->dfunc = f;
    glDepthFunc(f);
}
static void w_glDepthMask(void *ctx, uint8_t f)
{
    gctx_t *c = enter(ctx);
    if (c) c->dmask = f;
    glDepthMask(f);
}
static void w_glDepthRangef(void *ctx, float n, float f) { enter(ctx); glDepthRangef(n, f); }
static void w_glFrontFace(void *ctx, uint32_t m)
{
    gctx_t *c = enter(ctx);
    if (c) c->front = m;
    glFrontFace(m);
}
static void w_glHint(void *ctx, uint32_t t, uint32_t m) { enter(ctx); glHint(t, m); }
static void w_glLineWidth(void *ctx, float w) { enter(ctx); glLineWidth(w); }
static void w_glPolygonOffset(void *ctx, float f, float u)
{
    gctx_t *c = enter(ctx);
    if (c) { c->poff[0] = f; c->poff[1] = u; }
    glPolygonOffset(f, u);
}
static void w_glSampleCoverage(void *ctx, float v, uint8_t inv) { enter(ctx); glSampleCoverage(v, inv); }
static void stencil_set(gctx_t *c, uint32_t face, int what, uint32_t a, uint32_t b, uint32_t d)
{
    for (int f = 0; f < 2; f++) {
        if (!(face == GL_FRONT_AND_BACK || (f == 0 && face == GL_FRONT) || (f == 1 && face == GL_BACK)))
            continue;
        if (what == 0) { c->sfunc[f] = a; c->sref[f] = (GLint)b; c->smask[f] = d; }
        else if (what == 1) c->swmask[f] = a;
        else { c->sop[f][0] = a; c->sop[f][1] = b; c->sop[f][2] = d; }
    }
}

static void w_glStencilFunc(void *ctx, uint32_t f, int32_t r, uint32_t m)
{
    gctx_t *c = enter(ctx);
    if (c) stencil_set(c, GL_FRONT_AND_BACK, 0, f, (uint32_t)r, m);
    glStencilFunc(f, r, m);
}
static void w_glStencilFuncSeparate(void *ctx, uint32_t face, uint32_t f, int32_t r, uint32_t m)
{
    gctx_t *c = enter(ctx);
    if (c) stencil_set(c, face, 0, f, (uint32_t)r, m);
    glStencilFuncSeparate(face, f, r, m);
}
static void w_glStencilMask(void *ctx, uint32_t m)
{
    gctx_t *c = enter(ctx);
    if (c) stencil_set(c, GL_FRONT_AND_BACK, 1, m, 0, 0);
    glStencilMask(m);
}
static void w_glStencilMaskSeparate(void *ctx, uint32_t face, uint32_t m)
{
    gctx_t *c = enter(ctx);
    if (c) stencil_set(c, face, 1, m, 0, 0);
    glStencilMaskSeparate(face, m);
}
static void w_glStencilOp(void *ctx, uint32_t a, uint32_t b, uint32_t c2)
{
    gctx_t *c = enter(ctx);
    if (c) stencil_set(c, GL_FRONT_AND_BACK, 2, a, b, c2);
    glStencilOp(a, b, c2);
}
static void w_glStencilOpSeparate(void *ctx, uint32_t face, uint32_t a, uint32_t b, uint32_t c2)
{
    gctx_t *c = enter(ctx);
    if (c) stencil_set(c, face, 2, a, b, c2);
    glStencilOpSeparate(face, a, b, c2);
}
static void w_glPixelStorei(void *ctx, uint32_t pname, int32_t param)
{
    gctx_t *c = enter(ctx);
    if (c) {
        if (pname == GL_UNPACK_ALIGNMENT) c->unpack_align = param;
        else if (pname == GL_PACK_ALIGNMENT) c->pack_align = param;
        else if (pname == GL_UNPACK_ROW_LENGTH) c->unpack_row = param;
        else if (pname == GL_UNPACK_SKIP_ROWS) c->unpack_skip_rows = param;
        else if (pname == GL_UNPACK_SKIP_PIXELS) c->unpack_skip_px = param;
    }
    glPixelStorei(pname, param);
}
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
    /* No compressed texture formats and no shader binary formats: the counts are 0, so the guest asks for no list. */
    if (pname == 0x86A2 || pname == 0x8DF9) {
        GLint zero = 0;
        put_ints(params, params_len, &zero, 1);
        return;
    }
    /* The arrays of those lists (more than the buffer of 16 below) are left to the null answers. */
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

/* Sampler objects (Skia uses them on ES 3.0, GrGLCaps.cpp): WebGL2 has them, and the names are the host's. The sampler
 * bindings are shared by the contexts (see the gaps in the design note). */
static void w_glGenSamplers(void *ctx, int32_t n, void *samplers, uint32_t len)
{
    enter(ctx);
    gen_into(glGenSamplers, n, samplers, len);
}

static void w_glDeleteSamplers(void *ctx, int32_t n, const void *samplers, uint32_t len)
{
    enter(ctx);
    delete_from(glDeleteSamplers, n, samplers, len);
}

static void w_glBindSampler(void *ctx, uint32_t unit, uint32_t sampler)
{
    gctx_t *c = enter(ctx);
    if (c && unit < 32) {
        c->sampler[unit] = sampler;
        if (unit + 1 > c->nunits)
            c->nunits = unit + 1;
    }
    glBindSampler(unit, sampler);
}

static void w_glSamplerParameteri(void *ctx, uint32_t sampler, uint32_t pname, int32_t param)
{
    enter(ctx);
    glSamplerParameteri(sampler, pname, param);
}

static void w_glSamplerParameterf(void *ctx, uint32_t sampler, uint32_t pname, float param)
{
    enter(ctx);
    glSamplerParameterf(sampler, pname, param);
}

/* Buffers. */

static void w_glGenBuffers(void *ctx, int32_t n, void *buffers, uint32_t len)
{
    enter(ctx);
    gen_into(glGenBuffers, n, buffers, len);
}

static void w_glDeleteBuffers(void *ctx, int32_t n, const void *buffers, uint32_t len)
{
    enter(ctx);
    delete_from(glDeleteBuffers, n, buffers, len);
}

static void w_glBindBuffer(void *ctx, uint32_t target, uint32_t buffer)
{
    gctx_t *c = enter(ctx);
    if (c) {
        if (target == GL_ARRAY_BUFFER) c->abuf = buffer;
        else if (target == GL_PIXEL_UNPACK_BUFFER) c->unpack_buf = buffer;
        else if (target == GL_PIXEL_PACK_BUFFER) c->pack_buf = buffer;
    }
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
    gen_into(glGenTextures, n, textures, len);
}

static void w_glDeleteTextures(void *ctx, int32_t n, const void *textures, uint32_t len)
{
    enter(ctx);
    if (n <= 0 || (uint64_t)n * 4 > len)
        return;
    GLuint *names = copy_in(textures, (size_t)n * 4);
    if (!names)
        return;
    for (int32_t i = 0; i < n; i++)
        for (int k = 0; k < NALIAS; k++)
            if (tex_alias[k].used && tex_alias[k].name == names[i])
                tex_alias[k].used = 0;
    glDeleteTextures(n, names);
    free(names);
}

static void w_glActiveTexture(void *ctx, uint32_t texture)
{
    gctx_t *c = enter(ctx);
    if (texture < GL_TEXTURE0 || texture - GL_TEXTURE0 >= 32)
        return;
    c->unit = texture - GL_TEXTURE0;
    if (c->unit + 1 > c->nunits)
        c->nunits = c->unit + 1;
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
    void *c = NULL;
    if (pixels && pixels_len && ((uintptr_t)pixels & 3)) {
        c = copy_in(pixels, pixels_len);
        pixels = c;
    }
    glTexImage2D(tgt(target), level, internalformat, width, height, border, format, type, pixels);
    free(c);
}

static void w_glTexSubImage2D(void *ctx, uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t width,
                              int32_t height, uint32_t format, uint32_t type, const void *pixels, uint32_t pixels_len)
{
    enter(ctx);
    void *c = NULL;
    if (pixels && pixels_len && ((uintptr_t)pixels & 3)) {
        c = copy_in(pixels, pixels_len);
        pixels = c;
    }
    glTexSubImage2D(tgt(target), level, xoffset, yoffset, width, height, format, type, pixels);
    free(c);
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
    gen_into(glGenFramebuffers, n, fbs, len);
}

static void w_glDeleteFramebuffers(void *ctx, int32_t n, const void *fbs, uint32_t len)
{
    enter(ctx);
    delete_from(glDeleteFramebuffers, n, fbs, len);
}

static void w_glBindFramebuffer(void *ctx, uint32_t target, uint32_t framebuffer)
{
    gctx_t *c = enter(ctx);
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
        c->fbo = framebuffer;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, host_fbo(c, framebuffer));
    }
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) {
        c->rfbo = framebuffer;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, host_rfbo(c, framebuffer));
    }
}

/* The calls G2 left to the null defaults that HWUI, Skia and RenderEngine use (B17). */

static void w_glCopyTexSubImage2D(void *ctx, uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset, int32_t x,
                                  int32_t y, int32_t width, int32_t height)
{
    enter(ctx);
    glCopyTexSubImage2D(tgt(target), level, xoffset, yoffset, x, y, width, height);
}

static void w_glCopyTexImage2D(void *ctx, uint32_t target, int32_t level, uint32_t internalformat, int32_t x, int32_t y,
                               int32_t width, int32_t height, int32_t border)
{
    enter(ctx);
    glCopyTexImage2D(tgt(target), level, internalformat, x, y, width, height, border);
}

static void w_glGetShaderPrecisionFormat(void *ctx, uint32_t shadertype, uint32_t precisiontype, void *range,
                                         uint32_t range_len, void *precision, uint32_t precision_len)
{
    enter(ctx);
    GLint r[2] = {0, 0}, p = 0;
    glGetShaderPrecisionFormat(shadertype, precisiontype, r, &p);
    if (range && range_len >= sizeof r)
        memcpy(range, r, sizeof r);
    if (precision && precision_len >= sizeof p)
        memcpy(precision, &p, sizeof p);
}

static void w_glClearBufferfv(void *ctx, uint32_t buffer, int32_t drawbuffer, const void *value, uint32_t value_len)
{
    enter(ctx);
    GLfloat v[4] = {0, 0, 0, 0};
    memcpy(v, value, value_len < sizeof v ? value_len : sizeof v);
    glClearBufferfv(buffer, drawbuffer, v);
}

static void w_glClearBufferiv(void *ctx, uint32_t buffer, int32_t drawbuffer, const void *value, uint32_t value_len)
{
    enter(ctx);
    GLint v[4] = {0, 0, 0, 0};
    memcpy(v, value, value_len < sizeof v ? value_len : sizeof v);
    glClearBufferiv(buffer, drawbuffer, v);
}

static void w_glClearBufferuiv(void *ctx, uint32_t buffer, int32_t drawbuffer, const void *value, uint32_t value_len)
{
    enter(ctx);
    GLuint v[4] = {0, 0, 0, 0};
    memcpy(v, value, value_len < sizeof v ? value_len : sizeof v);
    glClearBufferuiv(buffer, drawbuffer, v);
}

static void w_glClearBufferfi(void *ctx, uint32_t buffer, int32_t drawbuffer, float depth, int32_t stencil)
{
    enter(ctx);
    glClearBufferfi(buffer, drawbuffer, depth, stencil);
}

static void w_glVertexAttribDivisor(void *ctx, uint32_t index, uint32_t divisor)
{
    enter(ctx);
    glVertexAttribDivisor(index, divisor);
}

static void w_glDrawArraysInstanced(void *ctx, uint32_t mode, int32_t first, int32_t count, int32_t primcount)
{
    enter(ctx);
    glDrawArraysInstanced(mode, first, count, primcount);
}

/* The guest's framebuffer 0 is a real framebuffer here (the back buffer), so GL_BACK names its attachment 0. */
static GLenum fb_buffer(GLuint guest_fbo, GLenum b)
{
    return (!guest_fbo && b == GL_BACK) ? GL_COLOR_ATTACHMENT0 : b;
}

static void w_glDrawBuffers(void *ctx, int32_t n, const void *bufs, uint32_t bufs_len)
{
    gctx_t *c = enter(ctx);
    if (n <= 0 || n > 8 || (uint64_t)n * 4 > bufs_len)
        return;
    GLenum b[8];
    memcpy(b, bufs, (size_t)n * 4);
    for (int i = 0; i < n; i++)
        b[i] = fb_buffer(c ? c->fbo : 0, b[i]);
    glDrawBuffers(n, b);
}

static void w_glReadBuffer(void *ctx, uint32_t src)
{
    gctx_t *c = enter(ctx);
    glReadBuffer(fb_buffer(c ? c->rfbo : 0, src));
}

static void w_glBlitFramebuffer(void *ctx, int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1, int32_t dx0, int32_t dy0,
                                int32_t dx1, int32_t dy1, uint32_t mask, uint32_t filter)
{
    enter(ctx);
    glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}

/* Invalidation is a hint: the contents may be kept. Dropping it is correct (and avoids translating the default
 * framebuffer's GL_COLOR, GL_DEPTH and GL_STENCIL names). */
static void w_glInvalidateFramebuffer(void *ctx, uint32_t target, int32_t n, const void *att, uint32_t att_len)
{
    (void)target; (void)n; (void)att; (void)att_len;
    enter(ctx);
}

static void w_glInvalidateSubFramebuffer(void *ctx, uint32_t target, int32_t n, const void *att, uint32_t att_len,
                                         int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)target; (void)n; (void)att; (void)att_len; (void)x; (void)y; (void)w; (void)h;
    enter(ctx);
}

static void w_glFramebufferTextureLayer(void *ctx, uint32_t target, uint32_t attachment, uint32_t texture, int32_t level,
                                        int32_t layer)
{
    enter(ctx);
    glFramebufferTextureLayer(target, attachment, host_tex(texture), level, layer);
}

static void w_glRenderbufferStorageMultisample(void *ctx, uint32_t target, int32_t samples, uint32_t internalformat,
                                               int32_t width, int32_t height)
{
    enter(ctx);
    glRenderbufferStorageMultisample(target, samples, internalformat, width, height);
}

static void w_glTexParameterfv(void *ctx, uint32_t target, uint32_t pname, const void *params, uint32_t params_len)
{
    enter(ctx);
    GLfloat v[4] = {0, 0, 0, 0};
    memcpy(v, params, params_len < sizeof v ? params_len : sizeof v);
    glTexParameterfv(tgt(target), pname, v);
}

static void w_glTexParameteriv(void *ctx, uint32_t target, uint32_t pname, const void *params, uint32_t params_len)
{
    enter(ctx);
    GLint v[4] = {0, 0, 0, 0};
    memcpy(v, params, params_len < sizeof v ? params_len : sizeof v);
    glTexParameteriv(tgt(target), pname, v);
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
    gen_into(glGenRenderbuffers, n, rbs, len);
}

static void w_glDeleteRenderbuffers(void *ctx, int32_t n, const void *rbs, uint32_t len)
{
    enter(ctx);
    delete_from(glDeleteRenderbuffers, n, rbs, len);
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

static void w_glUniform1fv(void *ctx, int32_t loc, int32_t count, const void *v, uint32_t len)
{
    enter(ctx);
    uint32_t b = vec_bytes(count, 1, len);
    void *c = b ? copy_in(v, b) : NULL;
    if (c) {
        glUniform1fv(loc, count, c);
        free(c);
    }
}
static void w_glUniform2fv(void *ctx, int32_t loc, int32_t count, const void *v, uint32_t len)
{
    enter(ctx);
    uint32_t b = vec_bytes(count, 2, len);
    void *c = b ? copy_in(v, b) : NULL;
    if (c) {
        glUniform2fv(loc, count, c);
        free(c);
    }
}
static void w_glUniform3fv(void *ctx, int32_t loc, int32_t count, const void *v, uint32_t len)
{
    enter(ctx);
    uint32_t b = vec_bytes(count, 3, len);
    void *c = b ? copy_in(v, b) : NULL;
    if (c) {
        glUniform3fv(loc, count, c);
        free(c);
    }
}
static void w_glUniform4fv(void *ctx, int32_t loc, int32_t count, const void *v, uint32_t len)
{
    enter(ctx);
    uint32_t b = vec_bytes(count, 4, len);
    void *c = b ? copy_in(v, b) : NULL;
    if (c) {
        glUniform4fv(loc, count, c);
        free(c);
    }
}
static void w_glUniform1iv(void *ctx, int32_t loc, int32_t count, const void *v, uint32_t len)
{
    enter(ctx);
    uint32_t b = vec_bytes(count, 1, len);
    void *c = b ? copy_in(v, b) : NULL;
    if (c) {
        glUniform1iv(loc, count, c);
        free(c);
    }
}
static void w_glUniform2iv(void *ctx, int32_t loc, int32_t count, const void *v, uint32_t len)
{
    enter(ctx);
    uint32_t b = vec_bytes(count, 2, len);
    void *c = b ? copy_in(v, b) : NULL;
    if (c) {
        glUniform2iv(loc, count, c);
        free(c);
    }
}
static void w_glUniform3iv(void *ctx, int32_t loc, int32_t count, const void *v, uint32_t len)
{
    enter(ctx);
    uint32_t b = vec_bytes(count, 3, len);
    void *c = b ? copy_in(v, b) : NULL;
    if (c) {
        glUniform3iv(loc, count, c);
        free(c);
    }
}
static void w_glUniform4iv(void *ctx, int32_t loc, int32_t count, const void *v, uint32_t len)
{
    enter(ctx);
    uint32_t b = vec_bytes(count, 4, len);
    void *c = b ? copy_in(v, b) : NULL;
    if (c) {
        glUniform4iv(loc, count, c);
        free(c);
    }
}
static void w_glUniformMatrix2fv(void *ctx, int32_t loc, int32_t count, uint8_t t, const void *v, uint32_t len)
{
    enter(ctx);
    uint32_t b = vec_bytes(count, 4, len);
    void *c = b ? copy_in(v, b) : NULL;
    if (c) {
        glUniformMatrix2fv(loc, count, t, c);
        free(c);
    }
}
static void w_glUniformMatrix3fv(void *ctx, int32_t loc, int32_t count, uint8_t t, const void *v, uint32_t len)
{
    enter(ctx);
    uint32_t b = vec_bytes(count, 9, len);
    void *c = b ? copy_in(v, b) : NULL;
    if (c) {
        glUniformMatrix3fv(loc, count, t, c);
        free(c);
    }
}
static void w_glUniformMatrix4fv(void *ctx, int32_t loc, int32_t count, uint8_t t, const void *v, uint32_t len)
{
    enter(ctx);
    uint32_t b = vec_bytes(count, 16, len);
    void *c = b ? copy_in(v, b) : NULL;
    if (c) {
        glUniformMatrix4fv(loc, count, t, c);
        free(c);
    }
}

static void w_glVertexAttrib1f(void *ctx, uint32_t i, float x) { enter(ctx); glVertexAttrib1f(i, x); }
static void w_glVertexAttrib2f(void *ctx, uint32_t i, float x, float y) { enter(ctx); glVertexAttrib2f(i, x, y); }
static void w_glVertexAttrib3f(void *ctx, uint32_t i, float x, float y, float z) { enter(ctx); glVertexAttrib3f(i, x, y, z); }
static void w_glVertexAttrib4f(void *ctx, uint32_t i, float x, float y, float z, float w) { enter(ctx); glVertexAttrib4f(i, x, y, z, w); }
static void w_glVertexAttrib4fv(void *ctx, uint32_t i, const void *v, uint32_t len)
{
    enter(ctx);
    if (len < 16)
        return;
    GLfloat f[4];
    memcpy(f, v, sizeof f);
    glVertexAttrib4fv(i, f);
}

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
    gen_into(glGenVertexArrays, n, arrays, len);
}

static void w_glDeleteVertexArrays(void *ctx, int32_t n, const void *arrays, uint32_t len)
{
    enter(ctx);
    delete_from(glDeleteVertexArrays, n, arrays, len);
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

/* The not_api calls with data or out-parameters (gl_host.c, not_api_data), as gfxstream's GLESv2Decoder does them. */

/* emscripten's WebGL 2 library has it (getBufferSubData); the GLES 3 header does not declare it. */
extern void glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void *data);

/* A map copies the range out for a read, or for a write that keeps the old contents (GLESv2Decoder::s_glMapBufferRangeAEMU). */
static void w_glMapBufferRangeAEMU(void *ctx, uint32_t target, uint32_t offset, uint32_t length, uint32_t access,
                                   void *mapped)
{
    enter(ctx);
    if ((access & GL_MAP_READ_BIT) ||
        ((access & GL_MAP_WRITE_BIT) && !(access & (GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT))))
        glGetBufferSubData(target, offset, length, mapped);
}

/* An unmap of a write mapping writes the guest's copy of the range back (s_glUnmapBufferAEMU). */
static uint8_t w_glUnmapBufferAEMU(void *ctx, uint32_t target, uint32_t offset, uint32_t length, uint32_t access,
                                   const void *guest_buffer)
{
    enter(ctx);
    if ((access & GL_MAP_WRITE_BIT) && guest_buffer && length)
        glBufferSubData(target, offset, length, guest_buffer);
    return 1;
}

/* An explicit flush writes the flushed range; offset is absolute in the buffer (s_glFlushMappedBufferRangeAEMU). */
static void w_glFlushMappedBufferRangeAEMU(void *ctx, uint32_t target, uint32_t offset, uint32_t length, uint32_t access,
                                           const void *guest_buffer)
{
    (void)access;
    enter(ctx);
    if (length)
        glBufferSubData(target, offset, length, guest_buffer);
}

static uint8_t w_glBufferDataSyncAEMU(void *ctx, uint32_t target, uint32_t size, const void *data, uint32_t usage)
{
    enter(ctx);
    glBufferData(target, size, data, usage);
    return 1;
}

/* The names are packed as "name;name;..." (the guest's packVarNames). */
static void w_glGetUniformIndicesAEMU(void *ctx, uint32_t program, uint32_t count, const char *names, uint32_t packed_len,
                                      uint8_t *indices)
{
    enter(ctx);
    if (!count || count > 4096)
        return;
    char *buf = malloc((size_t)packed_len + 1);
    const char **v = calloc(count, sizeof *v);
    GLuint *out = malloc((size_t)count * sizeof *out);
    if (buf && v && out) {
        memcpy(buf, names, packed_len);
        buf[packed_len] = 0;
        uint32_t n = 0;
        char *p = buf;
        while (n < count && *p) {
            char *e = strchr(p, ';');
            if (e)
                *e = 0;
            v[n++] = p;
            if (!e)
                break;
            p = e + 1;
        }
        if (n == count) {
            glGetUniformIndices(program, (GLsizei)count, v, out);
            for (uint32_t i = 0; i < count; i++)
                for (int b = 0; b < 4; b++)
                    indices[i * 4 + b] = (uint8_t)(out[i] >> (8 * b));
        }
    }
    free(buf);
    free(v);
    free(out);
}

/* The client-index forms put the indices in the scratch element buffer, as glDrawElementsData does. */
static void draw_with_indices(const void *data, uint32_t datalen, void (*draw)(const void *arg), const void *arg)
{
    hstate_t hs;
    save_host(&hs);
    if (!scratch_ebo)
        glGenBuffers(1, &scratch_ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, scratch_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, datalen, data, GL_STREAM_DRAW);
    draw(arg);
    restore_host(&hs);
}

typedef struct {
    uint32_t mode, type, start, end;
    int32_t count, prim;
} drawargs_t;

static void draw_instanced0(const void *a)
{
    const drawargs_t *d = a;
    glDrawElementsInstanced(d->mode, d->count, d->type, NULL, d->prim);
}

static void draw_range0(const void *a)
{
    const drawargs_t *d = a;
    glDrawRangeElements(d->mode, d->start, d->end, d->count, d->type, NULL);
}

static void w_glDrawElementsInstancedDataAEMU(void *ctx, uint32_t mode, int32_t count, uint32_t type, const void *data,
                                              uint32_t datalen, int32_t primcount)
{
    enter(ctx);
    drawargs_t d = {mode, type, 0, 0, count, primcount};
    draw_with_indices(data, datalen, draw_instanced0, &d);
}

static void w_glDrawElementsInstancedOffsetAEMU(void *ctx, uint32_t mode, int32_t count, uint32_t type, uint32_t offset,
                                                int32_t primcount)
{
    enter(ctx);
    glDrawElementsInstanced(mode, count, type, (const void *)(uintptr_t)offset, primcount);
}

static void w_glDrawRangeElementsDataAEMU(void *ctx, uint32_t mode, uint32_t start, uint32_t end, int32_t count,
                                          uint32_t type, const void *data, uint32_t datalen)
{
    enter(ctx);
    drawargs_t d = {mode, type, start, end, count, 0};
    draw_with_indices(data, datalen, draw_range0, &d);
}

static void w_glDrawRangeElementsOffsetAEMU(void *ctx, uint32_t mode, uint32_t start, uint32_t end, int32_t count,
                                            uint32_t type, uint32_t offset)
{
    enter(ctx);
    glDrawRangeElements(mode, start, end, count, type, (const void *)(uintptr_t)offset);
}

static void w_glVertexAttribIPointerDataAEMU(void *ctx, uint32_t index, int32_t size, uint32_t type, int32_t stride,
                                             const void *data, uint32_t datalen)
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
    glVertexAttribIPointer(index, size, type, 0, NULL);
    restore_host(&hs);
}

static void w_glVertexAttribIPointerOffsetAEMU(void *ctx, uint32_t index, int32_t size, uint32_t type, int32_t stride,
                                               uint32_t offset)
{
    enter(ctx);
    glVertexAttribIPointer(index, size, type, stride, (const void *)(uintptr_t)offset);
}

/* The pixel-buffer forms: the offset is into the bound GL_PIXEL_UNPACK_BUFFER (or GL_PIXEL_PACK_BUFFER), which emscripten
 * passes to WebGL as an offset when such a buffer is bound. */
static void w_glTexImage2DOffsetAEMU(void *ctx, uint32_t target, int32_t level, int32_t internalformat, int32_t width,
                                     int32_t height, int32_t border, uint32_t format, uint32_t type, uint32_t offset)
{
    enter(ctx);
    glTexImage2D(tgt(target), level, internalformat, width, height, border, format, type, (const void *)(uintptr_t)offset);
}

static void w_glTexSubImage2DOffsetAEMU(void *ctx, uint32_t target, int32_t level, int32_t xoffset, int32_t yoffset,
                                        int32_t width, int32_t height, uint32_t format, uint32_t type, uint32_t offset)
{
    enter(ctx);
    glTexSubImage2D(tgt(target), level, xoffset, yoffset, width, height, format, type, (const void *)(uintptr_t)offset);
}

static void w_glReadPixelsOffsetAEMU(void *ctx, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format,
                                     uint32_t type, uint32_t offset)
{
    enter(ctx);
    glReadPixels(x, y, width, height, format, type, (void *)(uintptr_t)offset);
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
static uint32_t selftest_quad_px;  /* the quad's pixel on the surface, or ~0u when the draw did not run */
static uint32_t selftest_life_ok;  /* 1 when open and close count references, and the last close deletes */

/* ----- the self test's wire packets: built as the guest builds them, and fed through gl_host_decode ----- */
typedef struct {
    uint8_t *b;
    size_t n, cap;
} pkt_t;

static void pk_put(pkt_t *p, const void *d, size_t n)
{
    if (p->n + n > p->cap) {
        size_t nc = p->cap ? p->cap : 128;
        while (nc < p->n + n)
            nc *= 2;
        uint8_t *nb = realloc(p->b, nc);
        if (!nb)
            return;
        p->b = nb;
        p->cap = nc;
    }
    memcpy(p->b + p->n, d, n);
    p->n += n;
}

static void pk_u32(pkt_t *p, uint32_t v)
{
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    pk_put(p, b, 4);
}

static void pk_begin(pkt_t *p, uint32_t op)
{
    p->n = 0;
    pk_u32(p, op);
    pk_u32(p, 0); /* the total, set when the packet is sent */
}

/* Sends the packet to the GL host (the stream k), and returns the decoder's result. */
static int pk_send(void *k, pkt_t *p, uint32_t op, gl_reply *rep)
{
    uint32_t n = (uint32_t)p->n;
    p->b[4] = (uint8_t)n;
    p->b[5] = (uint8_t)(n >> 8);
    p->b[6] = (uint8_t)(n >> 16);
    p->b[7] = (uint8_t)(n >> 24);
    return gl_host_decode(gl_host_webgl(), k, op, p->b, p->n, rep);
}

/* The reply's first word (a name, or a location), and the reply freed. */
static uint32_t rep_u32(gl_reply *r)
{
    uint32_t v = 0;
    if (r->len >= 4)
        memcpy(&v, r->buf, 4);
    free(r->buf);
    memset(r, 0, sizeof *r);
    return v;
}

static int selftest_quad(void *k)
{
    static const char vs[] = "#version 300 es\nlayout(location = 0) in vec2 a_pos;\nuniform mat4 u_m;\n"
                             "void main() { gl_Position = u_m * vec4(a_pos, 0.0, 1.0); }\n";
    static const char fs[] = "#version 300 es\nprecision mediump float;\nout vec4 o;\n"
                             "void main() { o = vec4(0.0, 1.0, 0.0, 1.0); }\n";
    static const char uname[] = "u_m";
    static const float verts[8] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    static const float mat[16] = {0.5f, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; /* a column-major scale */
    static const uint8_t no = 0;
    pkt_t p = {0};
    gl_reply rep = {0};
    uint32_t vsh, fsh, prog, loc;
    int ok = 0;

#define PK_CHECK(e) do { if ((e) != 0) goto out; } while (0)
    pk_begin(&p, OP_glCreateShader);
    pk_u32(&p, 0x8B31); /* GL_VERTEX_SHADER */
    PK_CHECK(pk_send(k, &p, OP_glCreateShader, &rep));
    vsh = rep_u32(&rep);
    pk_begin(&p, OP_glCreateShader);
    pk_u32(&p, 0x8B30); /* GL_FRAGMENT_SHADER */
    PK_CHECK(pk_send(k, &p, OP_glCreateShader, &rep));
    fsh = rep_u32(&rep);
    pk_begin(&p, OP_glShaderString);
    pk_u32(&p, vsh);
    pk_u32(&p, (uint32_t)sizeof vs - 1);
    pk_put(&p, vs, sizeof vs - 1);
    pk_u32(&p, (uint32_t)sizeof vs - 1);
    PK_CHECK(pk_send(k, &p, OP_glShaderString, &rep));
    pk_begin(&p, OP_glShaderString);
    pk_u32(&p, fsh);
    pk_u32(&p, (uint32_t)sizeof fs - 1);
    pk_put(&p, fs, sizeof fs - 1);
    pk_u32(&p, (uint32_t)sizeof fs - 1);
    PK_CHECK(pk_send(k, &p, OP_glShaderString, &rep));
    pk_begin(&p, OP_glCompileShader);
    pk_u32(&p, vsh);
    PK_CHECK(pk_send(k, &p, OP_glCompileShader, &rep));
    pk_begin(&p, OP_glCompileShader);
    pk_u32(&p, fsh);
    PK_CHECK(pk_send(k, &p, OP_glCompileShader, &rep));
    pk_begin(&p, OP_glCreateProgram);
    PK_CHECK(pk_send(k, &p, OP_glCreateProgram, &rep));
    prog = rep_u32(&rep);
    pk_begin(&p, OP_glAttachShader);
    pk_u32(&p, prog);
    pk_u32(&p, vsh);
    PK_CHECK(pk_send(k, &p, OP_glAttachShader, &rep));
    pk_begin(&p, OP_glAttachShader);
    pk_u32(&p, prog);
    pk_u32(&p, fsh);
    PK_CHECK(pk_send(k, &p, OP_glAttachShader, &rep));
    pk_begin(&p, OP_glLinkProgram);
    pk_u32(&p, prog);
    PK_CHECK(pk_send(k, &p, OP_glLinkProgram, &rep));
    pk_begin(&p, OP_glUseProgram);
    pk_u32(&p, prog);
    PK_CHECK(pk_send(k, &p, OP_glUseProgram, &rep));
    pk_begin(&p, OP_glGetUniformLocation);
    pk_u32(&p, prog);
    pk_u32(&p, (uint32_t)sizeof uname - 1);
    pk_put(&p, uname, sizeof uname - 1);
    PK_CHECK(pk_send(k, &p, OP_glGetUniformLocation, &rep));
    loc = rep_u32(&rep);
    pk_begin(&p, OP_glEnableVertexAttribArray);
    pk_u32(&p, 0);
    PK_CHECK(pk_send(k, &p, OP_glEnableVertexAttribArray, &rep));
    /* glVertexAttribPointerData: index, size, type, normalized (1 byte), stride, the data (datalen) and datalen again */
    pk_begin(&p, OP_glVertexAttribPointerData);
    pk_u32(&p, 0);
    pk_u32(&p, 2);
    pk_u32(&p, 0x1406); /* GL_FLOAT */
    pk_put(&p, &no, 1);
    pk_u32(&p, 0);
    pk_u32(&p, sizeof verts);
    pk_put(&p, verts, sizeof verts);
    pk_u32(&p, sizeof verts);
    PK_CHECK(pk_send(k, &p, OP_glVertexAttribPointerData, &rep));
    /* glUniformMatrix4fv: the location, the count, transpose (1 byte), and the matrix: at an odd packet offset */
    pk_begin(&p, OP_glUniformMatrix4fv);
    pk_u32(&p, loc);
    pk_u32(&p, 1);
    pk_put(&p, &no, 1);
    pk_u32(&p, sizeof mat);
    pk_put(&p, mat, sizeof mat);
    PK_CHECK(pk_send(k, &p, OP_glUniformMatrix4fv, &rep));
    pk_begin(&p, OP_glDrawArrays);
    pk_u32(&p, 0x0005); /* GL_TRIANGLE_STRIP */
    pk_u32(&p, 0);
    pk_u32(&p, 4);
    PK_CHECK(pk_send(k, &p, OP_glDrawArrays, &rep));
    ok = 1;
out:
#undef PK_CHECK
    free(rep.buf);
    free(p.b);
    return ok;
}

int EMSCRIPTEN_KEEPALIVE xj_gl_selftest(void)
{
    const gl_host_t *h = gl_host_webgl();
    if (!h)
        return -1;
    void *k = &selftest_stream;
    const gles2_backend_t *g2 = h->gles2;
    uint32_t ctx = rc_CreateContext(NULL, 0, 0, 0x30000);
    uint32_t surf = rc_CreateWindowSurface(NULL, 0, XJ_DISPLAY_W, XJ_DISPLAY_H);
    uint32_t cb = rc_CreateColorBuffer(NULL, XJ_DISPLAY_W, XJ_DISPLAY_H, GL_RGBA);
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
    /* A quad through the wire path: two shaders, a link, a client array, a matrix uniform, a draw. */
    selftest_quad_px = ~0u;
    int quad_ok = selftest_quad(k);
    rc_FlushWindowColorBuffer(k, surf); /* eglSwapBuffers: the back buffer into the color buffer */
    if (quad_ok) {
        uint8_t q[4] = {0, 0, 0, 0};
        rc_ReadColorBuffer(k, cb, XJ_DISPLAY_W / 2, XJ_DISPLAY_H / 2 - 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q, sizeof q);
        selftest_quad_px = (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2];
    }
    /* Reference counts: create (1), open (2), close (1: still there), close (0: deleted). */
    uint32_t cbL = rc_CreateColorBuffer(NULL, 2, 2, GL_RGBA);
    rc_OpenColorBuffer2(NULL, cbL);
    rc_CloseColorBuffer(NULL, cbL);
    int alive = cb_find(cbL) != NULL;
    rc_CloseColorBuffer(NULL, cbL);
    selftest_life_ok = alive && !cb_find(cbL);
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

/* Host composition and the orientation of color buffers (B17). A 4x4 source written by the CPU (rcUpdateColorBuffer, top
 * row first: two red rows, then two blue ones) is composed onto an 8x8 target over its whole frame, opaque, then a
 * solid green layer over the target's right half, premultiplied at half alpha. The target must show red on top, blue
 * below, and the right half mixed with green. Then a window surface drawn by GL: blue, and red in GL's upper half
 * (y 4 to 7, the top of the picture); after the flush (eglSwapBuffers) the color buffer's row 0 must be red. Last, the
 * GLSL version string and the EGL vendor are advertised. Returns a bit per check (0x3F when all hold). */
uint32_t EMSCRIPTEN_KEEPALIVE xj_gl_selftest_compose(void)
{
    if (!gl_host_webgl())
        return 0;
    uint32_t src = rc_CreateColorBuffer(NULL, 4, 4, GL_RGBA), dst = rc_CreateColorBuffer(NULL, 8, 8, GL_RGBA);
    uint8_t px[64];
    for (int i = 0; i < 16; i++) {
        px[i * 4 + 0] = i < 8 ? 255 : 0;
        px[i * 4 + 1] = 0;
        px[i * 4 + 2] = i < 8 ? 0 : 255;
        px[i * 4 + 3] = 255;
    }
    rc_UpdateColorBuffer(NULL, src, 0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, px, sizeof px);
    uint8_t msg[12 + 2 * COMPOSE_LAYER_SIZE];
    memset(msg, 0, sizeof msg);
    uint32_t hdr[3] = {1, dst, 2};
    memcpy(msg, hdr, sizeof hdr);
    uint8_t *l = msg + 12;
    int32_t li[] = {(int32_t)src, 2, 0, 0, 8, 8};
    memcpy(l, li, sizeof li);
    float crop[4] = {0, 0, 4, 4};
    memcpy(l + 24, crop, sizeof crop);
    int32_t blend = 1;
    float alpha = 1.0f;
    memcpy(l + 40, &blend, 4);
    memcpy(l + 44, &alpha, 4);
    l += COMPOSE_LAYER_SIZE;
    int32_t li2[] = {0, 3, 4, 0, 8, 8};
    memcpy(l, li2, sizeof li2);
    blend = 2;
    alpha = 0.5f;
    memcpy(l + 40, &blend, 4);
    memcpy(l + 44, &alpha, 4);
    uint8_t green[4] = {0, 255, 0, 255};
    memcpy(l + 48, green, 4);
    uint32_t r = 0;
    if (rc_ComposeWithoutPost(NULL, sizeof msg, msg, sizeof msg) == 0)
        r |= 1;
    uint8_t top[4], bot[4], right[4];
    rc_ReadColorBuffer(NULL, dst, 1, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, top, 4);
    rc_ReadColorBuffer(NULL, dst, 1, 7, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bot, 4);
    rc_ReadColorBuffer(NULL, dst, 6, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, right, 4);
    if (top[0] > 250 && top[2] < 5)
        r |= 2;
    if (bot[2] > 250 && bot[0] < 5)
        r |= 4;
    if (right[1] > 120 && right[1] < 135 && right[0] > 120 && right[0] < 135)
        r |= 8;
    /* A GL window surface: a scissored clear of the top half of the picture (GL's upper half), then the flush. */
    static char k;
    uint32_t ctx = rc_CreateContext(NULL, 0, 0, 0x30000);
    uint32_t surf = rc_CreateWindowSurface(NULL, 0, 8, 8);
    uint32_t cb = rc_CreateColorBuffer(NULL, 8, 8, GL_RGBA);
    rc_SetWindowColorBuffer(NULL, surf, cb);
    rc_MakeCurrent(&k, ctx, surf, surf);
    const gles2_backend_t *g2 = gl_host_webgl()->gles2;
    g2->glClearColor(&k, 0, 0, 1, 1);
    g2->glClear(&k, GL_COLOR_BUFFER_BIT);
    g2->glEnable(&k, GL_SCISSOR_TEST);
    g2->glScissor(&k, 0, 4, 8, 4);
    g2->glClearColor(&k, 1, 0, 0, 1);
    g2->glClear(&k, GL_COLOR_BUFFER_BIT);
    rc_FlushWindowColorBuffer(&k, surf);
    uint8_t wtop[4];
    rc_ReadColorBuffer(NULL, cb, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, wtop, 4);
    if (wtop[0] > 250 && wtop[2] < 5)
        r |= 16;
    rc_MakeCurrent(&k, 0, 0, 0);
    rc_CloseColorBuffer(NULL, src);
    rc_CloseColorBuffer(NULL, dst);
    /* The GLSL version string, which Skia needs to make its context. */
    char glsl[64] = {0};
    char vendor[16] = {0};
    if (rc_GetGLString(NULL, GL_SHADING_LANGUAGE_VERSION, glsl, sizeof glsl - 1, sizeof glsl - 1) > 0 &&
        !strncmp(glsl, "OpenGL ES GLSL ES 3.00", 22) &&
        rc_QueryEGLString(NULL, 0x3053, vendor, sizeof vendor - 1, sizeof vendor - 1) > 0 && !strcmp(vendor, "xiaoji"))
        r |= 32;
    return r;
}

/* Per-context state (B17): two contexts on two streams. Context A enables blending with ONE, ONE, sets an unpack
 * alignment of 1 and binds an array buffer; context B keeps the defaults. Each switch must give the WebGL2 context the
 * state of the context made current on the stream. Returns a bit per check (0xFF when all hold). */
static char selftest_stream_a, selftest_stream_b;

uint32_t EMSCRIPTEN_KEEPALIVE xj_gl_selftest_state(void)
{
    const gl_host_t *h = gl_host_webgl();
    if (!h)
        return 0;
    const gles2_backend_t *g2 = h->gles2;
    void *ka = &selftest_stream_a, *kb = &selftest_stream_b;
    uint32_t ca = rc_CreateContext(NULL, 0, 0, 0x30000), cbx = rc_CreateContext(NULL, 0, 0, 0x30000);
    uint32_t sa = rc_CreateWindowSurface(NULL, 0, 4, 4), sb = rc_CreateWindowSurface(NULL, 0, 4, 4);
    rc_MakeCurrent(ka, ca, sa, sa);
    GLuint buf = 0;
    g2->glGenBuffers(ka, 1, &buf, sizeof buf);
    g2->glEnable(ka, GL_BLEND);
    g2->glBlendFunc(ka, GL_ONE, GL_ONE);
    g2->glPixelStorei(ka, GL_UNPACK_ALIGNMENT, 1);
    g2->glBindBuffer(ka, GL_ARRAY_BUFFER, buf);
    rc_MakeCurrent(kb, cbx, sb, sb);
    g2->glClearColor(kb, 0, 0, 0, 1); /* a call on stream B: context B is applied */
    GLint v = 0;
    uint32_t r = 0;
    r |= glIsEnabled(GL_BLEND) ? 0 : 1u;
    glGetIntegerv(GL_BLEND_DST_RGB, &v);
    r |= v == GL_ZERO ? 2u : 0;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &v);
    r |= v == 4 ? 4u : 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &v);
    r |= v == 0 ? 8u : 0;
    g2->glClearColor(ka, 0, 0, 0, 1); /* back on stream A */
    r |= glIsEnabled(GL_BLEND) ? 16u : 0;
    glGetIntegerv(GL_BLEND_DST_RGB, &v);
    r |= v == GL_ONE ? 32u : 0;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &v);
    r |= v == 1 ? 64u : 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &v);
    r |= (GLuint)v == buf ? 128u : 0;
    rc_MakeCurrent(ka, 0, 0, 0);
    rc_MakeCurrent(kb, 0, 0, 0);
    return r;
}

uint32_t EMSCRIPTEN_KEEPALIVE xj_gl_selftest_quad(void)
{
    return selftest_quad_px;
}

uint32_t EMSCRIPTEN_KEEPALIVE xj_gl_selftest_life(void)
{
    return selftest_life_ok;
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
    glViewport(0, 0, XJ_DISPLAY_W, XJ_DISPLAY_H);
    glScissor(0, 0, XJ_DISPLAY_W, XJ_DISPLAY_H);
    dflt = NULL;
    for (int i = 0; i < NCTX && !dflt; i++)
        if (!ctxs[i].used) {
            ctxs[i].used = 1;
            ctxs[i].handle = next_handle++;
            ctxs[i].vp[2] = XJ_DISPLAY_W;
            ctxs[i].vp[3] = XJ_DISPLAY_H;
            ctxs[i].sc[2] = XJ_DISPLAY_W;
            ctxs[i].sc[3] = XJ_DISPLAY_H;
            ctx_defaults(&ctxs[i]);
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
    rc_table.rcOpenColorBuffer = rc_OpenColorBuffer;
    rc_table.rcOpenColorBuffer2 = rc_OpenColorBuffer2;
    rc_table.rcReadColorBuffer = rc_ReadColorBuffer;
    rc_table.rcUpdateColorBuffer = rc_UpdateColorBuffer;
    rc_table.rcBindTexture = rc_BindTexture;
    rc_table.rcBindRenderbuffer = rc_BindRenderbuffer;
    rc_table.rcCreateClientImage = rc_CreateClientImage;
    rc_table.rcDestroyClientImage = rc_DestroyClientImage;
    rc_table.rcFBPost = rc_FBPost;
    rc_table.rcCompose = rc_Compose;
    rc_table.rcComposeWithoutPost = rc_ComposeWithoutPost;
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
    gles2_table.glCopyTexSubImage2D = w_glCopyTexSubImage2D;
    gles2_table.glCopyTexImage2D = w_glCopyTexImage2D;
    gles2_table.glGetShaderPrecisionFormat = w_glGetShaderPrecisionFormat;
    gles2_table.glClearBufferfv = w_glClearBufferfv;
    gles2_table.glClearBufferiv = w_glClearBufferiv;
    gles2_table.glClearBufferuiv = w_glClearBufferuiv;
    gles2_table.glClearBufferfi = w_glClearBufferfi;
    gles2_table.glVertexAttribDivisor = w_glVertexAttribDivisor;
    gles2_table.glDrawArraysInstanced = w_glDrawArraysInstanced;
    gles2_table.glDrawBuffers = w_glDrawBuffers;
    gles2_table.glReadBuffer = w_glReadBuffer;
    gles2_table.glBlitFramebuffer = w_glBlitFramebuffer;
    gles2_table.glInvalidateFramebuffer = w_glInvalidateFramebuffer;
    gles2_table.glInvalidateSubFramebuffer = w_glInvalidateSubFramebuffer;
    gles2_table.glFramebufferTextureLayer = w_glFramebufferTextureLayer;
    gles2_table.glRenderbufferStorageMultisample = w_glRenderbufferStorageMultisample;
    gles2_table.glTexParameterfv = w_glTexParameterfv;
    gles2_table.glTexParameteriv = w_glTexParameteriv;
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
    gles2_table.glGenSamplers = w_glGenSamplers;
    gles2_table.glDeleteSamplers = w_glDeleteSamplers;
    gles2_table.glBindSampler = w_glBindSampler;
    gles2_table.glSamplerParameteri = w_glSamplerParameteri;
    gles2_table.glSamplerParameterf = w_glSamplerParameterf;
    gles2_table.glGetActiveAttrib = w_glGetActiveAttrib;
    gles2_table.glGetActiveUniform = w_glGetActiveUniform;

    custom_table.glShaderString = w_glShaderString;
    custom_table.glFinishRoundTrip = w_glFinishRoundTrip;
    custom_table.glVertexAttribPointerData = w_glVertexAttribPointerData;
    custom_table.glVertexAttribPointerOffset = w_glVertexAttribPointerOffset;
    custom_table.glDrawElementsData = w_glDrawElementsData;
    custom_table.glDrawElementsOffset = w_glDrawElementsOffset;
    custom_table.glMapBufferRangeAEMU = w_glMapBufferRangeAEMU;
    custom_table.glUnmapBufferAEMU = w_glUnmapBufferAEMU;
    custom_table.glFlushMappedBufferRangeAEMU = w_glFlushMappedBufferRangeAEMU;
    custom_table.glBufferDataSyncAEMU = w_glBufferDataSyncAEMU;
    custom_table.glGetUniformIndicesAEMU = w_glGetUniformIndicesAEMU;
    custom_table.glDrawElementsInstancedDataAEMU = w_glDrawElementsInstancedDataAEMU;
    custom_table.glDrawElementsInstancedOffsetAEMU = w_glDrawElementsInstancedOffsetAEMU;
    custom_table.glDrawRangeElementsDataAEMU = w_glDrawRangeElementsDataAEMU;
    custom_table.glDrawRangeElementsOffsetAEMU = w_glDrawRangeElementsOffsetAEMU;
    custom_table.glVertexAttribIPointerDataAEMU = w_glVertexAttribIPointerDataAEMU;
    custom_table.glVertexAttribIPointerOffsetAEMU = w_glVertexAttribIPointerOffsetAEMU;
    custom_table.glTexImage2DOffsetAEMU = w_glTexImage2DOffsetAEMU;
    custom_table.glTexSubImage2DOffsetAEMU = w_glTexSubImage2DOffsetAEMU;
    custom_table.glReadPixelsOffsetAEMU = w_glReadPixelsOffsetAEMU;

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
