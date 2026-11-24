/* The null GL backend (docs/graphics.md, 3.3): renderControl and GLES without a GPU. The decoders are generated
 * (build/gen); this file gives the answers that matter and leaves every other call to the generated no-op default
 * (gl_defaults.c), which allocates nothing, returns 0 and drops draws. Each default call is logged once
 * (gl_note_default), so the log lists what still needs an answer.
 *
 * Every pointer argument comes with its wire length (the guest's byte count): a write is bounded by it, and a
 * write that does not fit is not made. The values follow the host's (vulkan-cereal stream-servers/RenderControl.cpp)
 * where they are the same. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "gl_host.h"

extern const renderControl_backend_t renderControl_null_defaults;
extern const gles2_backend_t gles2_null_defaults;
extern const gles1_backend_t gles1_null_defaults;

/* renderControl.h names (renderControl_types.h, the EGL and GL enums). */
#define FB_WIDTH 1
#define FB_HEIGHT 2
#define FB_XDPI 3
#define FB_YDPI 4
#define FB_FPS 5
#define FB_MIN_SWAP_INTERVAL 6
#define FB_MAX_SWAP_INTERVAL 7
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_EXTENSIONS 0x3055
#define EGL_CLIENT_APIS 0x308D
#define EGL_NONE 0x3038
#define EGL_DONT_CARE (-1)

/* The display the engine presents: the size of the B4f NoOp composer, at the host's 72 dpi. The size is display.h's (the touchscreen reads it too). */
#define NULL_FB_WIDTH XJ_DISPLAY_W
#define NULL_FB_HEIGHT XJ_DISPLAY_H
#define NULL_RENDERER_VERSION 1

/* What the null host advertises. Only the features the design lists (docs/graphics.md, 3.2): GLES 3.0 and
 * native sync v2, and no host error checks. EGL_ANDROID_native_fence_sync is added because RenderEngine uses
 * native fences when they exist. EGL_KHR_surfaceless_context is not advertised: with it RenderEngine calls
 * eglMakeCurrent(no surface), which the guest's libEGL_emulation rejects (EGL_BAD_MATCH, egl.cpp), so the
 * stub pbuffer path is used instead (GLESRenderEngine.cpp:288-299). */
static const char gl_vendor[] = "xiaoji";
static const char gl_renderer[] = "xiaoji null GLES";
static const char gl_version[] = "OpenGL ES 3.0 xiaoji-null";
static const char gl_extensions[] =
    "GL_OES_EGL_image_external_essl3 "
    "ANDROID_EMU_gles_max_version_3_0 ANDROID_EMU_native_sync_v2 ANDROID_EMU_gles_no_host_error";
static const char egl_vendor[] = "xiaoji";
static const char egl_version[] = "1.4 xiaoji-null";
static const char egl_extensions[] = "EGL_ANDROID_native_fence_sync";
static const char egl_client_apis[] = "OpenGL_ES";

/* ---------- bounded writes ---------- */

/* Writes v at byte offset off of a buffer of len bytes, if it fits. */
static void put32(void *buf, uint32_t len, uint32_t off, uint32_t v)
{
    if (!buf || off > len || len - off < 4)
        return;
    uint8_t *p = buf;
    p[off] = (uint8_t)v;
    p[off + 1] = (uint8_t)(v >> 8);
    p[off + 2] = (uint8_t)(v >> 16);
    p[off + 3] = (uint8_t)(v >> 24);
}

static void put64(void *buf, uint32_t len, uint32_t off, uint64_t v)
{
    put32(buf, len, off, (uint32_t)v);
    put32(buf, len, off + 4, (uint32_t)(v >> 32));
}

/* The host's string convention: the length with its NUL. Too small a buffer gives -len and no copy. */
static int32_t put_string(const char *s, void *buffer, uint32_t buffer_len, int32_t bufferSize)
{
    int32_t len = (int32_t)strlen(s) + 1;
    if (!buffer || len > bufferSize || (uint32_t)len > buffer_len)
        return -len;
    memcpy(buffer, s, (size_t)len);
    return len;
}

/* ---------- renderControl: the answers ---------- */

#define NCFG_ATTRS 34
/* FbConfig.cpp kConfigAttributes, in order (the first four are read by index on the host). */
static const uint32_t cfg_ids[NCFG_ATTRS] = {
    0x3025, /* DEPTH_SIZE */       0x3026, /* STENCIL_SIZE */     0x3040, /* RENDERABLE_TYPE */
    0x3033, /* SURFACE_TYPE */     0x3028, /* CONFIG_ID */        0x3020, /* BUFFER_SIZE */
    0x3021, /* ALPHA_SIZE */       0x3022, /* BLUE_SIZE */        0x3023, /* GREEN_SIZE */
    0x3024, /* RED_SIZE */         0x3027, /* CONFIG_CAVEAT */    0x3029, /* LEVEL */
    0x302A, /* MAX_PBUFFER_HEIGHT */ 0x302B, /* MAX_PBUFFER_PIXELS */ 0x302C, /* MAX_PBUFFER_WIDTH */
    0x302D, /* NATIVE_RENDERABLE */ 0x302E, /* NATIVE_VISUAL_ID */ 0x302F, /* NATIVE_VISUAL_TYPE */
    0x3030, /* PRESERVED_RESOURCES */ 0x3031, /* SAMPLES */       0x3032, /* SAMPLE_BUFFERS */
    0x3034, /* TRANSPARENT_TYPE */ 0x3035, /* TRANSPARENT_BLUE_VALUE */ 0x3036, /* TRANSPARENT_GREEN_VALUE */
    0x3037, /* TRANSPARENT_RED_VALUE */ 0x3039, /* BIND_TO_TEXTURE_RGB */ 0x303A, /* BIND_TO_TEXTURE_RGBA */
    0x303B, /* MIN_SWAP_INTERVAL */ 0x303C, /* MAX_SWAP_INTERVAL */ 0x303D, /* LUMINANCE_SIZE */
    0x303E, /* ALPHA_MASK_SIZE */  0x303F, /* COLOR_BUFFER_TYPE */ 0x3142, /* RECORDABLE_ANDROID */
    0x3042, /* CONFORMANT */
};
static const int32_t cfg_vals[NCFG_ATTRS] = {
    24, 8, 0x44, 0x5, 1, 32, 8, 8, 8, 8,
    EGL_NONE, 0, 4096, 4096 * 4096, 4096, 0, 0, 0, 0, 0,
    0, EGL_NONE, 0, 0, 0, 1, 1, 0, 1, 0,
    8, 0x308E, 1, 0x44,
};

static int32_t fb_param(int32_t param)
{
    switch (param) {
    case FB_WIDTH: return NULL_FB_WIDTH;
    case FB_HEIGHT: return NULL_FB_HEIGHT;
    case FB_XDPI: return 72;
    case FB_YDPI: return 72;
    case FB_FPS: return 60;
    case FB_MIN_SWAP_INTERVAL: return 1;
    case FB_MAX_SWAP_INTERVAL: return 1;
    default: return 0;
    }
}

/* Does the one config meet a requirement of eglChooseConfig? Sizes are minimums, bit masks must be
 * contained, the id must match; other attributes are not checked. */
static int cfg_matches(const int32_t *attribs, uint32_t n)
{
    for (uint32_t i = 0; i + 1 < n && attribs[i] != EGL_NONE; i += 2) {
        int32_t key = attribs[i], want = attribs[i + 1];
        if (want == EGL_DONT_CARE)
            continue;
        int32_t have = -1;
        for (int k = 0; k < NCFG_ATTRS; k++)
            if ((int32_t)cfg_ids[k] == key)
                have = cfg_vals[k];
        if (have < 0)
            continue;
        switch (key) {
        case 0x3040: /* EGL_RENDERABLE_TYPE */
        case 0x3033: /* EGL_SURFACE_TYPE */
        case 0x3042: /* EGL_CONFORMANT */
            if ((have & want) != want)
                return 0;
            break;
        case 0x3028: /* EGL_CONFIG_ID */
            if (have != want)
                return 0;
            break;
        case 0x3020: case 0x3021: case 0x3022: case 0x3023: case 0x3024: case 0x3025:
        case 0x3026: case 0x3031: case 0x303D: case 0x303E:
            if (have < want)
                return 0;
            break;
        default:
            break;
        }
    }
    return 1;
}

static int32_t rc_GetRendererVersion(void *ctx)
{
    (void)ctx;
    return NULL_RENDERER_VERSION;
}

static int32_t rc_GetFBParam(void *ctx, int32_t param)
{
    (void)ctx;
    return fb_param(param);
}

static int32_t rc_GetEGLVersion(void *ctx, void *major, uint32_t major_len, void *minor, uint32_t minor_len)
{
    (void)ctx;
    put32(major, major_len, 0, 1);
    put32(minor, minor_len, 0, 4);
    return 1; /* EGL_TRUE */
}

static int32_t rc_QueryEGLString(void *ctx, uint32_t name, void *buffer, uint32_t buffer_len, int32_t bufferSize)
{
    (void)ctx;
    switch (name) {
    case EGL_VENDOR: return put_string(egl_vendor, buffer, buffer_len, bufferSize);
    case EGL_VERSION: return put_string(egl_version, buffer, buffer_len, bufferSize);
    case EGL_EXTENSIONS: return put_string(egl_extensions, buffer, buffer_len, bufferSize);
    case EGL_CLIENT_APIS: return put_string(egl_client_apis, buffer, buffer_len, bufferSize);
    default: return 0;
    }
}

static int32_t rc_GetGLString(void *ctx, uint32_t name, void *buffer, uint32_t buffer_len, int32_t bufferSize)
{
    (void)ctx;
    switch (name) {
    case GL_VENDOR: return put_string(gl_vendor, buffer, buffer_len, bufferSize);
    case GL_RENDERER: return put_string(gl_renderer, buffer, buffer_len, bufferSize);
    case GL_VERSION: return put_string(gl_version, buffer, buffer_len, bufferSize);
    case GL_EXTENSIONS: return put_string(gl_extensions, buffer, buffer_len, bufferSize);
    default: return 0;
    }
}

static int32_t rc_GetNumConfigs(void *ctx, void *numAttribs, uint32_t numAttribs_len)
{
    (void)ctx;
    put32(numAttribs, numAttribs_len, 0, NCFG_ATTRS);
    return 1;
}

/* The packing of FbConfig.cpp (packConfigs): the attribute ids, then one row of values for the config. */
static int32_t rc_GetConfigs(void *ctx, uint32_t bufSize, void *buffer, uint32_t buffer_len)
{
    (void)ctx;
    uint32_t needed = 2 * NCFG_ATTRS * 4;
    if (!buffer || bufSize < needed || buffer_len < needed)
        return -(int32_t)needed;
    for (uint32_t i = 0; i < NCFG_ATTRS; i++) {
        put32(buffer, buffer_len, 4 * i, cfg_ids[i]);
        put32(buffer, buffer_len, 4 * (NCFG_ATTRS + i), (uint32_t)cfg_vals[i]);
    }
    return 1;
}

static int32_t rc_ChooseConfig(void *ctx, const void *attribs, uint32_t attribs_len, uint32_t attribs_size,
                               void *configs, uint32_t configs_len, uint32_t configs_size)
{
    (void)ctx;
    /* attribs_size is in bytes (renderControl.attrib: len attribs attribs_size); the bytes are attribs_len. */
    int32_t one[2] = {EGL_NONE, 0};
    uint32_t bytes = attribs_size < attribs_len ? attribs_size : attribs_len;
    const int32_t *a = attribs && bytes >= 4 ? (const int32_t *)attribs : one;
    uint32_t na = attribs && bytes >= 4 ? bytes / 4 : 2;
    if (!cfg_matches(a, na))
        return 0;
    if (configs && configs_size > 0)
        put32(configs, configs_len, 0, 0); /* the one config, index 0 */
    return 1;
}

/* Handles: contexts, surfaces, color buffers, client images and syncs are numbers the guest keeps. */
static uint32_t next_handle = 1;
static uint64_t next_sync = 1;
static uint32_t frames_posted;

static uint32_t rc_CreateContext(void *ctx, uint32_t config, uint32_t share, uint32_t glVersion)
{
    (void)ctx; (void)config; (void)share; (void)glVersion;
    return next_handle++;
}

static void rc_DestroyContext(void *ctx, uint32_t context)
{
    (void)ctx; (void)context;
}

static uint32_t rc_CreateWindowSurface(void *ctx, uint32_t config, uint32_t width, uint32_t height)
{
    (void)ctx; (void)config; (void)width; (void)height;
    return next_handle++;
}

static void rc_DestroyWindowSurface(void *ctx, uint32_t windowSurface)
{
    (void)ctx; (void)windowSurface;
}

static int32_t rc_MakeCurrent(void *ctx, uint32_t context, uint32_t drawSurf, uint32_t readSurf)
{
    (void)ctx; (void)context; (void)drawSurf; (void)readSurf;
    return 1; /* EGL_TRUE */
}

/* Color buffers keep their pixels, so a CPU path (glReadPixels, rcReadColorBuffer) reads back what was written.
 * RGBA8888 only; other formats are refused (rcUpdateColorBuffer returns an error, rcReadColorBuffer copies nothing). */
#define NCB_MAX 256
#define CB_BPP 4
#define GL_RGBA_ 0x1908
#define GL_UNSIGNED_BYTE_ 0x1401
typedef struct {
    uint32_t handle;
    uint32_t width, height;
    uint8_t *pixels; /* width * height * CB_BPP bytes */
} colorbuffer_t;
static colorbuffer_t colorbuffers[NCB_MAX];
static int ncb;

static colorbuffer_t *cb_find(uint32_t handle)
{
    for (int i = 0; i < ncb; i++)
        if (colorbuffers[i].handle == handle)
            return &colorbuffers[i];
    return NULL;
}

static uint32_t cb_create(uint32_t width, uint32_t height)
{
    if (ncb >= NCB_MAX || !width || !height || (uint64_t)width * height > (1u << 24))
        return 0;
    uint8_t *px = calloc((size_t)width * height, CB_BPP);
    if (!px)
        return 0;
    colorbuffers[ncb].handle = next_handle++;
    colorbuffers[ncb].width = width;
    colorbuffers[ncb].height = height;
    colorbuffers[ncb].pixels = px;
    ncb++;
    return colorbuffers[ncb - 1].handle;
}

static uint32_t rc_CreateColorBuffer(void *ctx, uint32_t width, uint32_t height, uint32_t internalFormat)
{
    (void)ctx; (void)internalFormat;
    return cb_create(width, height);
}

static uint32_t rc_CreateColorBufferDMA(void *ctx, uint32_t width, uint32_t height, uint32_t internalFormat,
                                        int32_t frameworkFormat)
{
    (void)ctx; (void)internalFormat; (void)frameworkFormat;
    return cb_create(width, height);
}

static void rc_CloseColorBuffer(void *ctx, uint32_t colorbuffer)
{
    (void)ctx;
    colorbuffer_t *c = cb_find(colorbuffer);
    if (!c)
        return;
    free(c->pixels);
    *c = colorbuffers[--ncb];
}

/* A rectangle inside the color buffer. */
static int cb_rect_ok(const colorbuffer_t *c, int32_t x, int32_t y, int32_t w, int32_t h)
{
    return x >= 0 && y >= 0 && w > 0 && h > 0 && (uint32_t)x + (uint32_t)w <= c->width &&
           (uint32_t)y + (uint32_t)h <= c->height;
}

static void rc_ReadColorBuffer(void *ctx, uint32_t colorbuffer, int32_t x, int32_t y, int32_t width, int32_t height,
                               uint32_t format, uint32_t type, void *pixels, uint32_t pixels_len)
{
    (void)ctx;
    colorbuffer_t *c = cb_find(colorbuffer);
    if (!c || format != GL_RGBA_ || type != GL_UNSIGNED_BYTE_ || !cb_rect_ok(c, x, y, width, height))
        return;
    uint32_t row = (uint32_t)width * CB_BPP;
    for (int32_t r = 0; r < height; r++) {
        size_t src = ((size_t)(y + r) * c->width + (size_t)x) * CB_BPP;
        size_t dst = (size_t)r * row;
        if (dst + row > pixels_len)
            break;
        memcpy((uint8_t *)pixels + dst, c->pixels + src, row);
    }
}

static int32_t rc_UpdateColorBuffer(void *ctx, uint32_t colorbuffer, int32_t x, int32_t y, int32_t width, int32_t height,
                                    uint32_t format, uint32_t type, const void *pixels, uint32_t pixels_len)
{
    (void)ctx;
    colorbuffer_t *c = cb_find(colorbuffer);
    if (!c || format != GL_RGBA_ || type != GL_UNSIGNED_BYTE_ || !cb_rect_ok(c, x, y, width, height))
        return -1;
    uint32_t row = (uint32_t)width * CB_BPP;
    if ((uint64_t)row * (uint32_t)height > pixels_len)
        return -1;
    for (int32_t r = 0; r < height; r++) {
        size_t dst = ((size_t)(y + r) * c->width + (size_t)x) * CB_BPP;
        memcpy(c->pixels + dst, (const uint8_t *)pixels + (size_t)r * row, row);
    }
    return 0;
}

static uint32_t rc_CreateClientImage(void *ctx, uint32_t context, uint32_t target, uint32_t buffer)
{
    (void)ctx; (void)context; (void)target; (void)buffer;
    return next_handle++;
}

static void rc_FBPost(void *ctx, uint32_t colorBuffer)
{
    (void)ctx;
    frames_posted++;
    if (frames_posted <= 3)
        fprintf(stderr, "xiaoji: gl: frame %u posted (color buffer %u)\n", (unsigned)frames_posted, (unsigned)colorBuffer);
}

static void rc_CreateSyncKHR(void *ctx, uint32_t type, const void *attribs, uint32_t attribs_len, uint32_t num_attribs,
                             int32_t destroy_when_signaled, void *glsync_out, uint32_t glsync_out_len,
                             void *syncthread_out, uint32_t syncthread_out_len)
{
    (void)ctx; (void)type; (void)attribs; (void)attribs_len; (void)num_attribs; (void)destroy_when_signaled;
    put64(glsync_out, glsync_out_len, 0, next_sync++);
    put64(syncthread_out, syncthread_out_len, 0, next_sync++);
}

static int32_t rc_IsSyncSignaled(void *ctx, uint64_t sync)
{
    (void)ctx; (void)sync;
    return 1; /* the null backend finishes every command when it is issued */
}

static int32_t rc_ClientWaitSyncKHR(void *ctx, uint64_t sync, int32_t flags, uint64_t timeout)
{
    (void)ctx; (void)sync; (void)flags; (void)timeout;
    return 0x30F8; /* EGL_CONDITION_SATISFIED_KHR */
}

static int32_t rc_DestroySyncKHR(void *ctx, uint64_t sync)
{
    (void)ctx; (void)sync;
    return 1;
}

/* ---------- GLES 2/3: the state the framework reads, and the names it expects ---------- */

/* glGetIntegerv: the ES 3.0 limits and state a compositor reads. Values are written only where the guest's
 * buffer (params_len bytes) has room for them. A pname not listed writes nothing. */
static void gl_GetIntegerv(void *ctx, uint32_t pname, void *params, uint32_t params_len)
{
    (void)ctx;
    int32_t extensions = 1;
    for (const char *s = gl_extensions; *s; s++)
        if (*s == ' ')
            extensions++;
    int32_t v[4] = {0, 0, 0, 0};
    uint32_t n = 1;
    switch (pname) {
    case 0x0D33: v[0] = 4096; break;                 /* GL_MAX_TEXTURE_SIZE */
    case 0x851C: v[0] = 4096; break;                 /* GL_MAX_CUBE_MAP_TEXTURE_SIZE */
    case 0x8073: v[0] = 256; break;                  /* GL_MAX_3D_TEXTURE_SIZE */
    case 0x88FF: v[0] = 256; break;                  /* GL_MAX_ARRAY_TEXTURE_LAYERS */
    case 0x84E8: v[0] = 4096; break;                 /* GL_MAX_RENDERBUFFER_SIZE */
    case 0x0D3A: v[0] = 4096; v[1] = 4096; n = 2; break; /* GL_MAX_VIEWPORT_DIMS */
    case 0x8872: v[0] = 16; break;                   /* GL_MAX_TEXTURE_IMAGE_UNITS */
    case 0x8B4D: v[0] = 32; break;                   /* GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS */
    case 0x8B4C: v[0] = 16; break;                   /* GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS */
    case 0x8869: v[0] = 16; break;                   /* GL_MAX_VERTEX_ATTRIBS */
    case 0x8DFB: v[0] = 256; break;                  /* GL_MAX_VERTEX_UNIFORM_VECTORS */
    case 0x8DFD: v[0] = 224; break;                  /* GL_MAX_FRAGMENT_UNIFORM_VECTORS */
    case 0x8DFC: v[0] = 15; break;                   /* GL_MAX_VARYING_VECTORS */
    case 0x8824: v[0] = 4; break;                    /* GL_MAX_DRAW_BUFFERS */
    case 0x8CDF: v[0] = 4; break;                    /* GL_MAX_COLOR_ATTACHMENTS */
    case 0x8D57: v[0] = 4; break;                    /* GL_MAX_SAMPLES */
    case 0x821D: v[0] = extensions; break;           /* GL_NUM_EXTENSIONS */
    case 0x821B: v[0] = 3; break;                    /* GL_MAJOR_VERSION */
    case 0x821C: v[0] = 0; break;                    /* GL_MINOR_VERSION */
    case 0x0D52: case 0x0D53: case 0x0D54: case 0x0D55: v[0] = 8; break; /* RED, GREEN, BLUE, ALPHA bits */
    case 0x0D56: v[0] = 24; break;                   /* GL_DEPTH_BITS */
    case 0x0D57: v[0] = 8; break;                    /* GL_STENCIL_BITS */
    case 0x80A8: case 0x80A9: v[0] = 0; break;       /* GL_SAMPLE_BUFFERS, GL_SAMPLES */
    case 0x8B9A: v[0] = 0x1401; break;               /* GL_IMPLEMENTATION_COLOR_READ_TYPE (UNSIGNED_BYTE) */
    case 0x8B9B: v[0] = 0x1908; break;               /* GL_IMPLEMENTATION_COLOR_READ_FORMAT (RGBA) */
    case 0x0CF5: case 0x0D05: v[0] = 4; break;       /* GL_UNPACK_ALIGNMENT, GL_PACK_ALIGNMENT */
    case 0x0BA2: case 0x0C10: n = 4; break;          /* GL_VIEWPORT, GL_SCISSOR_BOX: zeros */
    default: return;
    }
    for (uint32_t i = 0; i < n; i++)
        put32(params, params_len, 4 * i, (uint32_t)v[i]);
}

static void gl_GetFloatv(void *ctx, uint32_t pname, void *params, uint32_t params_len)
{
    (void)ctx;
    if (pname == 0x0B21) { /* GL_LINE_WIDTH */
        float one = 1.0f;
        uint32_t bits;
        memcpy(&bits, &one, 4);
        put32(params, params_len, 0, bits);
    }
}

static void gl_GetBooleanv(void *ctx, uint32_t pname, void *params, uint32_t params_len)
{
    (void)ctx;
    if (pname == 0x0C11 && params && params_len >= 1) /* GL_SCISSOR_TEST: off */
        ((uint8_t *)params)[0] = 0;
}

/* Shader and program queries: compiling and linking succeed, with no info log. */
static void gl_GetShaderiv(void *ctx, uint32_t shader, uint32_t pname, void *params, uint32_t params_len)
{
    (void)ctx; (void)shader;
    if (pname == 0x8B81) /* GL_COMPILE_STATUS */
        put32(params, params_len, 0, 1);
    else if (pname == 0x8B84) /* GL_INFO_LOG_LENGTH */
        put32(params, params_len, 0, 0);
}

static void gl_GetProgramiv(void *ctx, uint32_t program, uint32_t pname, void *params, uint32_t params_len)
{
    (void)ctx; (void)program;
    if (pname == 0x8B82 || pname == 0x8B83) /* GL_LINK_STATUS, GL_VALIDATE_STATUS */
        put32(params, params_len, 0, 1);
    else if (pname == 0x8B84) /* GL_INFO_LOG_LENGTH */
        put32(params, params_len, 0, 0);
}

/* Names: every object created gets a new non-zero name. */
static uint32_t next_gl_name = 1;

static uint32_t gl_CreateShader(void *ctx, uint32_t type)
{
    (void)ctx; (void)type;
    return next_gl_name++;
}

static uint32_t gl_CreateProgram(void *ctx)
{
    (void)ctx;
    return next_gl_name++;
}

static void gen_names(int32_t n, void *names, uint32_t names_len)
{
    for (int32_t i = 0; i < n; i++)
        put32(names, names_len, 4 * (uint32_t)i, next_gl_name++);
}

static void gl_GenTextures(void *ctx, int32_t n, void *textures, uint32_t len) { (void)ctx; gen_names(n, textures, len); }
static void gl_GenBuffers(void *ctx, int32_t n, void *buffers, uint32_t len) { (void)ctx; gen_names(n, buffers, len); }
static void gl_GenFramebuffers(void *ctx, int32_t n, void *framebuffers, uint32_t len) { (void)ctx; gen_names(n, framebuffers, len); }
static void gl_GenRenderbuffers(void *ctx, int32_t n, void *renderbuffers, uint32_t len) { (void)ctx; gen_names(n, renderbuffers, len); }
static void gl_GenVertexArrays(void *ctx, int32_t n, void *arrays, uint32_t len) { (void)ctx; gen_names(n, arrays, len); }

static uint32_t gl_CheckFramebufferStatus(void *ctx, uint32_t target)
{
    (void)ctx; (void)target;
    return 0x8CD5; /* GL_FRAMEBUFFER_COMPLETE */
}

/* The glShaderString and glFinishRoundTrip calls the host decodes by hand (gl_host.c). */
static void null_glShaderString(void *ctx, uint32_t shader, const void *string, uint32_t len)
{
    (void)ctx; (void)shader; (void)string; (void)len; /* accepted; the text is dropped */
}

static int32_t null_glFinishRoundTrip(void *ctx)
{
    (void)ctx;
    return 0;
}

static renderControl_backend_t rc_table;
static gles2_backend_t gles2_table;
static gles1_backend_t gles1_table;
static gl_custom_backend_t custom_table;
static gl_host_t host_table;

const gl_host_t *gl_host_null(void)
{
    static int ready;
    if (!ready) {
        rc_table = renderControl_null_defaults;
        rc_table.rcGetRendererVersion = rc_GetRendererVersion;
        rc_table.rcGetFBParam = rc_GetFBParam;
        rc_table.rcGetEGLVersion = rc_GetEGLVersion;
        rc_table.rcQueryEGLString = rc_QueryEGLString;
        rc_table.rcGetGLString = rc_GetGLString;
        rc_table.rcGetNumConfigs = rc_GetNumConfigs;
        rc_table.rcGetConfigs = rc_GetConfigs;
        rc_table.rcChooseConfig = rc_ChooseConfig;
        rc_table.rcCreateContext = rc_CreateContext;
        rc_table.rcDestroyContext = rc_DestroyContext;
        rc_table.rcCreateWindowSurface = rc_CreateWindowSurface;
        rc_table.rcDestroyWindowSurface = rc_DestroyWindowSurface;
        rc_table.rcMakeCurrent = rc_MakeCurrent;
        rc_table.rcCreateColorBuffer = rc_CreateColorBuffer;
        rc_table.rcCreateColorBufferDMA = rc_CreateColorBufferDMA;
        rc_table.rcCloseColorBuffer = rc_CloseColorBuffer;
        rc_table.rcReadColorBuffer = rc_ReadColorBuffer;
        rc_table.rcUpdateColorBuffer = rc_UpdateColorBuffer;
        rc_table.rcCreateClientImage = rc_CreateClientImage;
        rc_table.rcFBPost = rc_FBPost;
        rc_table.rcCreateSyncKHR = rc_CreateSyncKHR;
        rc_table.rcIsSyncSignaled = rc_IsSyncSignaled;
        rc_table.rcClientWaitSyncKHR = rc_ClientWaitSyncKHR;
        rc_table.rcDestroySyncKHR = rc_DestroySyncKHR;
        gles2_table = gles2_null_defaults;
        gles2_table.glGetIntegerv = gl_GetIntegerv;
        gles2_table.glGetFloatv = gl_GetFloatv;
        gles2_table.glGetBooleanv = gl_GetBooleanv;
        gles2_table.glGetShaderiv = gl_GetShaderiv;
        gles2_table.glGetProgramiv = gl_GetProgramiv;
        gles2_table.glCreateShader = gl_CreateShader;
        gles2_table.glCreateProgram = gl_CreateProgram;
        gles2_table.glGenTextures = gl_GenTextures;
        gles2_table.glGenBuffers = gl_GenBuffers;
        gles2_table.glGenFramebuffers = gl_GenFramebuffers;
        gles2_table.glGenRenderbuffers = gl_GenRenderbuffers;
        gles2_table.glGenVertexArrays = gl_GenVertexArrays;
        gles2_table.glCheckFramebufferStatus = gl_CheckFramebufferStatus;
        gles1_table = gles1_null_defaults;
        custom_table.glShaderString = null_glShaderString;
        custom_table.glFinishRoundTrip = null_glFinishRoundTrip;
        host_table.rc = &rc_table;
        host_table.gles2 = &gles2_table;
        host_table.gles1 = &gles1_table;
        host_table.custom = &custom_table;
        ready = 1;
    }
    return &host_table;
}
