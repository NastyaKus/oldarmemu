/*
 * Adreno 200 drawing on the host GPU (OpenGL ES 3 through EGL).
 *
 * Included at the end of adreno_draw.c: it shares that file's decoders
 * (vertex shading on the CPU, texture constants, formats, tiling, resolve
 * packing).  Selected with ADRENO_BACKEND:
 *   sw    the software rasterizer (default, and the reference)
 *   gl    pixels on the host GPU
 *   both  both; every resolve of the GPU path is compared with the
 *         software one and differences are logged (debugging)
 *
 * Model.  GMEM is a set of GL render targets, one per row stride and
 * pixel size: a GMEM pixel (x, y) of a pass with stride W is texel
 * (x, y) of the W-wide target.  Draws (EDRAM mode 4) run the vertex
 * shader on the CPU (run_vs) and a GLSL translation of the pixel shader
 * on the GPU.  Resolves (mode 6) read the GMEM area back through a pixel
 * buffer without waiting; the pixels are written to guest memory before
 * the next CP write to memory or register that the CPU may look at
 * (timestamps, scratch registers, end of the ring pass), like the
 * real GPU's ordering.  Textures are decoded from guest memory and kept
 * while a hash of their memory is unchanged.
 *
 * EGL and GLES are loaded with dlopen: no link dependency, and without a
 * usable GPU the software rasterizer simply stays in charge.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <dlfcn.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

/* generated list: every EGL/GL entry point the backend calls */
#define GL_EGL_FNS(X) \
    X(eglGetDisplay) \
    X(eglInitialize) \
    X(eglBindAPI) \
    X(eglChooseConfig) \
    X(eglCreateContext) \
    X(eglCreatePbufferSurface) \
    X(eglMakeCurrent) \
    X(eglGetError) \
    X(eglQueryString) \
    X(eglGetProcAddress)
#define GL_GL_FNS(X) \
    X(glGetString) \
    X(glGetError) \
    X(glGenTextures) \
    X(glBindTexture) \
    X(glTexImage2D) \
    X(glTexSubImage2D) \
    X(glTexParameteri) \
    X(glPixelStorei) \
    X(glGenFramebuffers) \
    X(glBindFramebuffer) \
    X(glFramebufferTexture2D) \
    X(glFramebufferRenderbuffer) \
    X(glGenRenderbuffers) \
    X(glBindRenderbuffer) \
    X(glRenderbufferStorage) \
    X(glCheckFramebufferStatus) \
    X(glViewport) \
    X(glScissor) \
    X(glEnable) \
    X(glDisable) \
    X(glClearColor) \
    X(glClearDepthf) \
    X(glClear) \
    X(glBlendFuncSeparate) \
    X(glBlendEquationSeparate) \
    X(glBlendColor) \
    X(glColorMask) \
    X(glDepthFunc) \
    X(glDepthMask) \
    X(glCreateShader) \
    X(glShaderSource) \
    X(glCompileShader) \
    X(glGetShaderiv) \
    X(glGetShaderInfoLog) \
    X(glCreateProgram) \
    X(glAttachShader) \
    X(glBindAttribLocation) \
    X(glLinkProgram) \
    X(glGetProgramiv) \
    X(glGetProgramInfoLog) \
    X(glUseProgram) \
    X(glGetUniformLocation) \
    X(glUniform4fv) \
    X(glUniform2f) \
    X(glUniform1i) \
    X(glUniform1f) \
    X(glUniform4f) \
    X(glGenBuffers) \
    X(glBindBuffer) \
    X(glBufferData) \
    X(glVertexAttribPointer) \
    X(glEnableVertexAttribArray) \
    X(glDisableVertexAttribArray) \
    X(glDrawArrays) \
    X(glReadPixels) \
    X(glMapBufferRange) \
    X(glUnmapBuffer) \
    X(glActiveTexture) \
    X(glGenVertexArrays) \
    X(glBindVertexArray) \
    X(glDeleteTextures) \
    X(glDeleteFramebuffers) \
    X(glDeleteRenderbuffers) \
    X(glDeleteShader) \
    X(glFlush) \
    X(glFinish)

#define GL_DECL(n) static __typeof__(&n) p_##n;
GL_EGL_FNS(GL_DECL)
GL_GL_FNS(GL_DECL)


static bool gl_load_libs(void)
{
    static const char *egl_names[] = { "libEGL.so", "libEGL.so.1", NULL };
    static const char *gl_names[] = {
        "libGLESv3.so", "libGLESv2.so", "libGLESv2.so.2", NULL
    };
    void *he = NULL, *hg = NULL;

    for (int i = 0; egl_names[i] && !he; i++) {
        he = dlopen(egl_names[i], RTLD_NOW | RTLD_LOCAL);
    }
    for (int i = 0; gl_names[i] && !hg; i++) {
        hg = dlopen(gl_names[i], RTLD_NOW | RTLD_LOCAL);
    }
    if (!he || !hg) {
        error_report("adreno: GL backend: cannot load EGL/GLES (%s)", dlerror());
        return false;
    }
#define GL_LOAD_EGL(n) \
    if (!(p_##n = (__typeof__(p_##n))dlsym(he, #n))) { \
        error_report("adreno: GL backend: no %s", #n); return false; }
#define GL_LOAD_GL(n) \
    if (!(p_##n = (__typeof__(p_##n))dlsym(hg, #n))) { \
        error_report("adreno: GL backend: no %s", #n); return false; }
    GL_EGL_FNS(GL_LOAD_EGL)
    GL_GL_FNS(GL_LOAD_GL)
#undef GL_LOAD_EGL
#undef GL_LOAD_GL
    return true;
}

/* from here on the names call the loaded pointers */
#define eglGetDisplay p_eglGetDisplay
#define eglInitialize p_eglInitialize
#define eglBindAPI p_eglBindAPI
#define eglChooseConfig p_eglChooseConfig
#define eglCreateContext p_eglCreateContext
#define eglCreatePbufferSurface p_eglCreatePbufferSurface
#define eglMakeCurrent p_eglMakeCurrent
#define eglGetError p_eglGetError
#define eglQueryString p_eglQueryString
#define eglGetProcAddress p_eglGetProcAddress
#define glGetString p_glGetString
#define glGetError p_glGetError
#define glGenTextures p_glGenTextures
#define glBindTexture p_glBindTexture
#define glTexImage2D p_glTexImage2D
#define glTexSubImage2D p_glTexSubImage2D
#define glTexParameteri p_glTexParameteri
#define glPixelStorei p_glPixelStorei
#define glGenFramebuffers p_glGenFramebuffers
#define glBindFramebuffer p_glBindFramebuffer
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glFramebufferRenderbuffer p_glFramebufferRenderbuffer
#define glGenRenderbuffers p_glGenRenderbuffers
#define glBindRenderbuffer p_glBindRenderbuffer
#define glRenderbufferStorage p_glRenderbufferStorage
#define glCheckFramebufferStatus p_glCheckFramebufferStatus
#define glViewport p_glViewport
#define glScissor p_glScissor
#define glEnable p_glEnable
#define glDisable p_glDisable
#define glClearColor p_glClearColor
#define glClearDepthf p_glClearDepthf
#define glClear p_glClear
#define glBlendFuncSeparate p_glBlendFuncSeparate
#define glBlendEquationSeparate p_glBlendEquationSeparate
#define glBlendColor p_glBlendColor
#define glColorMask p_glColorMask
#define glDepthFunc p_glDepthFunc
#define glDepthMask p_glDepthMask
#define glCreateShader p_glCreateShader
#define glShaderSource p_glShaderSource
#define glCompileShader p_glCompileShader
#define glGetShaderiv p_glGetShaderiv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glCreateProgram p_glCreateProgram
#define glAttachShader p_glAttachShader
#define glBindAttribLocation p_glBindAttribLocation
#define glLinkProgram p_glLinkProgram
#define glGetProgramiv p_glGetProgramiv
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glUseProgram p_glUseProgram
#define glGetUniformLocation p_glGetUniformLocation
#define glUniform4fv p_glUniform4fv
#define glUniform2f p_glUniform2f
#define glUniform1i p_glUniform1i
#define glUniform1f p_glUniform1f
#define glUniform4f p_glUniform4f
#define glGenBuffers p_glGenBuffers
#define glBindBuffer p_glBindBuffer
#define glBufferData p_glBufferData
#define glVertexAttribPointer p_glVertexAttribPointer
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glDisableVertexAttribArray p_glDisableVertexAttribArray
#define glDrawArrays p_glDrawArrays
#define glReadPixels p_glReadPixels
#define glMapBufferRange p_glMapBufferRange
#define glUnmapBuffer p_glUnmapBuffer
#define glActiveTexture p_glActiveTexture
#define glGenVertexArrays p_glGenVertexArrays
#define glBindVertexArray p_glBindVertexArray
#define glDeleteTextures p_glDeleteTextures
#define glDeleteFramebuffers p_glDeleteFramebuffers
#define glDeleteRenderbuffers p_glDeleteRenderbuffers
#define glDeleteShader p_glDeleteShader
#define glFlush p_glFlush
#define glFinish p_glFinish

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x40
#endif

static int gl_mode = -1;
int adreno_gl_pending;                  /* resolves read back, not yet stored */
static uint32_t gl_epoch = 1;           /* ring passes: textures rehashed once each */
static uint64_t gl_tick;                /* LRU clock */
static GLuint gl_vao, gl_vbo;

/* ---------------- context ---------------- */

static bool gl_init(void)
{
    EGLDisplay d = EGL_NO_DISPLAY;
    EGLint ma, mi, n = 0;
    EGLConfig cfg;

    if (!gl_load_libs()) {
        return false;
    }
    const char *cext = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (gpd && cext && strstr(cext, "EGL_MESA_platform_surfaceless")) {
        d = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    }
    if (d == EGL_NO_DISPLAY) {
        d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, &ma, &mi)) {
        error_report("adreno: GL backend: no EGL display (%x)", eglGetError());
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ca[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    if (!eglChooseConfig(d, ca, &cfg, 1, &n) || !n) {
        error_report("adreno: GL backend: no GLES 3 config (%x)", eglGetError());
        return false;
    }
    const EGLint cx[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(d, cfg, EGL_NO_CONTEXT, cx);
    if (ctx == EGL_NO_CONTEXT) {
        error_report("adreno: GL backend: no GLES 3 context (%x)", eglGetError());
        return false;
    }
    EGLSurface surf = EGL_NO_SURFACE;
    const char *dext = eglQueryString(d, EGL_EXTENSIONS);
    if (!dext || !strstr(dext, "EGL_KHR_surfaceless_context")) {
        const EGLint pa[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
        surf = eglCreatePbufferSurface(d, cfg, pa);
    }
    if (!eglMakeCurrent(d, surf, surf, ctx)) {
        error_report("adreno: GL backend: cannot make the context current (%x)",
                     eglGetError());
        return false;
    }
    info_report("adreno: GL backend on %s, %s", (const char *)glGetString(GL_RENDERER),
                (const char *)glGetString(GL_VERSION));
    glGenVertexArrays(1, &gl_vao);
    glBindVertexArray(gl_vao);
    glGenBuffers(1, &gl_vbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    return true;
}

/* the backend in use; the context is created by the GPU thread's first draw */
static int gl_backend(void)
{
    if (gl_mode < 0) {
        const char *e = getenv("ADRENO_BACKEND");
        gl_mode = !e ? GLB_OFF : !strcmp(e, "gl") ? GLB_ON :
                  !strcmp(e, "both") ? GLB_BOTH : GLB_OFF;
        if (gl_mode != GLB_OFF && !gl_init()) {
            warn_report("adreno: GL backend unavailable, drawing in software");
            gl_mode = GLB_OFF;
        }
    }
    return gl_mode;
}

void adreno_gl_new_pass(void)
{
    gl_epoch++;
}

/* ---------------- GMEM render targets ---------------- */

typedef struct GmemRT {
    uint32_t tw, th, bpp;
    GLuint tex, depth, fbo;
    uint64_t used;
} GmemRT;

#define GL_NRT 8
static GmemRT gl_rt[GL_NRT];

/* GMEM seen with a row stride of tw pixels of bpp bytes */
static GmemRT *gl_gmem_rt(uint32_t tw, uint32_t bpp)
{
    GmemRT *r = NULL;

    tw = MAX(tw, 1u);
    for (int i = 0; i < GL_NRT; i++) {
        if (gl_rt[i].fbo && gl_rt[i].tw == tw && gl_rt[i].bpp == bpp) {
            gl_rt[i].used = ++gl_tick;
            return &gl_rt[i];
        }
        if (!r || gl_rt[i].used < r->used) {
            r = &gl_rt[i];
        }
    }
    if (r->fbo) {
        glDeleteFramebuffers(1, &r->fbo);
        glDeleteTextures(1, &r->tex);
        glDeleteRenderbuffers(1, &r->depth);
    }
    r->tw = tw;
    r->bpp = bpp;
    r->th = MIN(MAX(ADRENO_GMEM_SIZE / (tw * bpp), 1u), 4096u);
    r->used = ++gl_tick;
    glGenTextures(1, &r->tex);
    glBindTexture(GL_TEXTURE_2D, r->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, r->tw, r->th, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenRenderbuffers(1, &r->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, r->depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, r->tw, r->th);
    glGenFramebuffers(1, &r->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           r->tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, r->depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        warn_report("adreno: GL: incomplete GMEM target %ux%u", r->tw, r->th);
    }
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glDepthMask(1);
    glClearColor(0, 0, 0, 0);
    glClearDepthf(1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    return r;
}

/* where GMEM byte offset cbase lands in a target: texel (*x, *y) */
static void gl_gmem_origin(const GmemRT *r, uint32_t cbase, uint32_t *x, uint32_t *y)
{
    uint32_t row = r->tw * r->bpp;
    cbase &= ADRENO_GMEM_SIZE - 1;
    *y = cbase / row;
    *x = (cbase % row) / r->bpp;
}

/* ---------------- textures from guest memory ---------------- */

/* fast 64-bit hash of guest memory (4 lanes), unmapped pages count as holes */
static uint64_t gl_hash_mem(AdrenoState *s, uint32_t va, uint32_t len)
{
    uint64_t h[4] = { 0x9e3779b97f4a7c15ull, 0xc2b2ae3d27d4eb4full,
                      0x165667b19e3779f9ull, 0x27d4eb2f165667c5ull };
    const uint64_t k = 0x9fb21c651e98df25ull;

    while (len) {
        uint32_t n = MIN(len, 4096 - (va & 4095));
        const uint8_t *p = adreno_host_page(s, va);
        if (!p) {
            h[0] = (h[0] ^ va) * k;
        } else {
            p += va & 4095;
            uint32_t i = 0;
            for (; i + 32 <= n; i += 32) {
                for (int l = 0; l < 4; l++) {
                    uint64_t v;
                    memcpy(&v, p + i + 8 * l, 8);
                    h[l] = (h[l] ^ v) * k;
                    h[l] ^= h[l] >> 29;
                }
            }
            for (; i < n; i++) {
                h[0] = (h[0] ^ p[i]) * k;
            }
        }
        va += n;
        len -= n;
    }
    return h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7) ^ len;
}

typedef struct GlTex {
    uint32_t base, fmt, w, h, pitch, span;
    bool tiled, sign;
    uint64_t hash, used;
    uint32_t epoch;
    GLuint tex;
} GlTex;

#define GL_NTEX 96
static GlTex gl_tex[GL_NTEX];

static bool gl_fmt_block(uint32_t fmt)
{
    return fmt >= 46 && fmt <= 48;      /* ETC1 */
}

/* guest memory bytes a texture occupies */
static uint32_t gl_tex_span(const TexInfo *ti)
{
    if (gl_fmt_block(ti->fmt)) {
        return ((ti->pitch + 3) / 4) * ((ti->h + 3) / 4) * 8;
    }
    if (ti->tiled) {
        return ((ti->pitch + 31) & ~31u) * ((ti->h + 31) & ~31u) * MAX(ti->bpp, 1u);
    }
    return ti->pitch * ti->h * MAX(ti->bpp, 1u);
}

static uint8_t gl_u8(float v)
{
    v = v * 255.0f + 0.5f;
    return v <= 0.0f ? 0 : v >= 255.0f ? 255 : (uint8_t)v;
}

/* decode a texture into RGBA8 texels (before its swizzle) */
static uint8_t *gl_decode_tex(AdrenoState *s, const TexInfo *ti, uint32_t w, uint32_t h)
{
    uint8_t *out = g_malloc(w * h * 4);
    uint32_t fmt = ti->fmt;
    bool fast = !ti->tiled && !ti->sign && ti->norm &&
                (fmt == 6 || fmt == 14 || fmt == 4 || fmt == 15 || fmt == 2 ||
                 fmt == 8 || fmt == 9);

    if (fast) {
        uint32_t rowb = ti->pitch * ti->bpp;
        g_autofree uint8_t *row = g_malloc(rowb);
        for (uint32_t y = 0; y < h; y++) {
            uint8_t *o = out + y * w * 4;
            adreno_read(s, ti->base + y * rowb, row, rowb);
            for (uint32_t x = 0; x < w; x++, o += 4) {
                const uint8_t *q = row + x * ti->bpp;
                uint32_t v;
                switch (fmt) {
                case 6: case 14:
                    memcpy(o, q, 4);
                    break;
                case 4:
                    v = q[0] | (q[1] << 8);
                    o[0] = ((v & 31) * 255 + 15) / 31;
                    o[1] = (((v >> 5) & 63) * 255 + 31) / 63;
                    o[2] = (((v >> 11) & 31) * 255 + 15) / 31;
                    o[3] = 255;
                    break;
                case 15:
                    v = q[0] | (q[1] << 8);
                    o[0] = (v & 15) * 17;
                    o[1] = ((v >> 4) & 15) * 17;
                    o[2] = ((v >> 8) & 15) * 17;
                    o[3] = ((v >> 12) & 15) * 17;
                    break;
                default:                /* 8 bit */
                    o[0] = q[0];
                    o[1] = o[2] = 0;
                    o[3] = 255;
                    break;
                }
            }
        }
        return out;
    }
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            float v[4] = { 0, 0, 0, 1 };
            texel(s, &s->mc, ti, x, y, v);
            uint8_t *o = out + (y * w + x) * 4;
            for (int i = 0; i < 4; i++) {
                o[i] = gl_u8(v[i]);
            }
        }
    }
    return out;
}

/* the GL texture for fetch constant ti, (re)uploaded when its memory changed */
static GLuint gl_texture(AdrenoState *s, const TexInfo *ti)
{
    GlTex *e = NULL, *lru = NULL;
    uint32_t w = MIN(ti->w, 2048u), h = MIN(ti->h, 2048u);

    if (!ti->bpp && !gl_fmt_block(ti->fmt)) {
        return 0;
    }
    for (int i = 0; i < GL_NTEX; i++) {
        GlTex *t = &gl_tex[i];
        if (t->tex && t->base == ti->base && t->fmt == ti->fmt && t->w == w &&
            t->h == h && t->pitch == ti->pitch && t->tiled == ti->tiled &&
            t->sign == ti->sign) {
            e = t;
            break;
        }
        if (!lru || t->used < lru->used) {
            lru = t;
        }
    }
    bool fresh = !e;
    if (!e) {
        e = lru;
        if (!e->tex) {
            glGenTextures(1, &e->tex);
        }
        e->base = ti->base;
        e->fmt = ti->fmt;
        e->w = w;
        e->h = h;
        e->pitch = ti->pitch;
        e->tiled = ti->tiled;
        e->sign = ti->sign;
        e->span = gl_tex_span(ti);
        e->epoch = 0;
    }
    e->used = ++gl_tick;
    if (e->epoch != gl_epoch) {
        uint64_t hv = gl_hash_mem(s, ti->base, e->span);
        e->epoch = gl_epoch;
        if (fresh || hv != e->hash) {
            g_autofree uint8_t *px = gl_decode_tex(s, ti, w, h);
            e->hash = hv;
            glBindTexture(GL_TEXTURE_2D, e->tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, px);
        }
    }
    return e->tex;
}

/* drop cached textures overlapping guest memory the GPU just wrote */
static void gl_tex_touch(uint32_t va, uint32_t len)
{
    for (int i = 0; i < GL_NTEX; i++) {
        GlTex *t = &gl_tex[i];
        if (t->tex && va < t->base + t->span && t->base < va + len) {
            t->epoch = 0;               /* rehash at the next use */
        }
    }
}


/* ---------------- pixel shaders: a2xx -> GLSL ES 3.00 ---------------- */

/*
 * The control flow is walked at translation time with the draw's boolean
 * and loop constants (which are part of the cache key): loops unroll, so
 * the loop index aL is a constant everywhere, and only predicates and the
 * address register a0 stay dynamic.
 */
#define GL_NCONST   224                 /* uniform vectors every GLES 3 GPU has */

typedef struct Gen {
    AdrenoState *s;
    GString *b;
    int aL;
    int nconst;
    bool dyn_const;
    uint32_t samplers;
    int8_t filt[32];                    /* -1: the fetch constant's filter */
    uint32_t ninstr;
    const char *fail;
} Gen;

static const char gl_xyzw[] = "xyzw";

static void gen_swz(GString *o, uint32_t swz)
{
    g_string_append_c(o, '.');
    for (int j = 0; j < 4; j++) {
        g_string_append_c(o, gl_xyzw[((swz >> (2 * j)) + j) & 3]);
    }
}

static void gen_const(Gen *g, GString *o, int idx, bool dyn)
{
    if (dyn) {
        g->dyn_const = true;
        g_string_append_printf(o, "cst(%d + a0)", idx);
    } else if (idx < 0 || idx >= GL_NCONST) {
        g_string_append(o, "vec4(0.0)");
    } else {
        g->nconst = MAX(g->nconst, idx + 1);
        g_string_append_printf(o, "C[%d]", idx);
    }
}

static int gen_temp(Gen *g, uint32_t r, bool rel)
{
    return ((r & 0x3f) + (rel ? g->aL : 0)) & 63;
}

static void gen_mask_store(GString *b, const char *dst, uint32_t mask, const char *src,
                           bool scalar)
{
    if (!mask) {
        return;
    }
    g_string_append_printf(b, "%s.", dst);
    for (int i = 0; i < 4; i++) {
        if (mask & (1u << i)) {
            g_string_append_c(b, gl_xyzw[i]);
        }
    }
    if (scalar) {
        g_string_append_printf(b, " = vec4(%s).", src);
    } else {
        g_string_append_printf(b, " = %s.", src);
    }
    for (int i = 0; i < 4; i++) {
        if (mask & (1u << i)) {
            g_string_append_c(b, gl_xyzw[i]);
        }
    }
    g_string_append(b, ";\n");
}

static void gen_alu(Gen *g, const uint32_t *w)
{
    GString *b = g->b;
    uint32_t d0 = w[0], d1 = w[1], d2 = w[2];
    uint32_t vdst = d0 & 0x3f, vdst_rel = (d0 >> 6) & 1, abs_c = (d0 >> 7) & 1;
    uint32_t sdst = (d0 >> 8) & 0x3f, sdst_rel = (d0 >> 14) & 1;
    bool exp = (d0 >> 15) & 1;
    uint32_t vmask = (d0 >> 16) & 0xf, smask = (d0 >> 20) & 0xf;
    bool vclamp = (d0 >> 24) & 1, sclamp = (d0 >> 25) & 1;
    uint32_t sop = (d0 >> 26) & 0x3f;
    uint32_t swz[3] = { (d1 >> 16) & 0xff, (d1 >> 8) & 0xff, d1 & 0xff };
    bool neg[3] = { (d1 >> 26) & 1, (d1 >> 25) & 1, (d1 >> 24) & 1 };
    bool pred_cond = (d1 >> 27) & 1, predicated = (d1 >> 28) & 1;
    bool c_a0 = (d1 >> 29) & 1, c1_addr = (d1 >> 30) & 1, c0_addr = (d1 >> 31) & 1;
    uint32_t sreg[3] = { (d2 >> 16) & 0xff, (d2 >> 8) & 0xff, d2 & 0xff };
    uint32_t vop = (d2 >> 24) & 0x1f;
    bool istemp[3] = { (d2 >> 31) & 1, (d2 >> 30) & 1, (d2 >> 29) & 1 };

    g_string_append(b, predicated ? (pred_cond ? "if (p) {\n" : "if (!p) {\n") : "{\n");

    /* operands a, b, c */
    for (int i = 0; i < 3; i++) {
        GString *o = g_string_new("");
        if (istemp[i]) {
            g_string_append_printf(o, "r%d", gen_temp(g, sreg[i], sreg[i] & 0x40));
        } else {
            bool addressed = i == 0 ? c0_addr :
                             i == 1 ? (istemp[0] ? c0_addr : c1_addr) :
                             ((istemp[0] && istemp[1]) ? c0_addr : c1_addr);
            bool dyn = addressed && c_a0;
            gen_const(g, o, sreg[i] + (addressed && !c_a0 ? g->aL : 0), dyn);
        }
        gen_swz(o, swz[i]);
        bool absv = istemp[i] ? (sreg[i] & 0x80) : abs_c;
        g_string_append_printf(b, "vec4 %c = %s%s%s%s%s;\n", "abc"[i],
                               neg[i] ? "-" : "", absv ? "abs(" : "", o->str,
                               absv ? ")" : "", "");
        g_string_free(o, true);
    }

    uint32_t vwm = vmask, swm = smask;
    if (exp) {
        vwm &= ~smask;
        swm &= ~vmask;
    }
    g_string_append(b, "vec4 vr = vec4(0.0);\n");
    if (vwm || vop >= 20) {
        static const char *simple[] = {
            [0] = "a + b", [1] = "a * b", [2] = "max(a, b)", [3] = "min(a, b)",
            [4] = "vec4(equal(a, b))", [5] = "vec4(greaterThan(a, b))",
            [6] = "vec4(greaterThanEqual(a, b))", [7] = "vec4(notEqual(a, b))",
            [8] = "fract(a)", [9] = "trunc(a)", [10] = "floor(a)",
            [11] = "a * b + c",
            [12] = "mix(c, b, vec4(equal(a, vec4(0.0))))",
            [13] = "mix(c, b, vec4(greaterThanEqual(a, vec4(0.0))))",
            [14] = "mix(c, b, vec4(greaterThan(a, vec4(0.0))))",
            [15] = "vec4(dot(a, b))", [16] = "vec4(dot(a.xyz, b.xyz))",
            [17] = "vec4(a.x * b.x + a.y * b.y + c.x)",
            [18] = "cube(a)",
            [19] = "vec4(max(max(a.x, a.y), max(a.z, a.w)))",
            [28] = "vec4(1.0, a.y * b.y, a.z, b.w)",
        };
        if (vop < ARRAY_SIZE(simple) && simple[vop]) {
            g_string_append_printf(b, "vr = %s;\n", simple[vop]);
        } else if (vop >= 20 && vop <= 23) {
            static const char *cnd[] = { "==", "!=", ">", ">=" };
            const char *c = cnd[vop - 20];
            g_string_append_printf(b,
                "p = a.w == 0.0 && b.w %s 0.0;\n"
                "vr = vec4((a.x == 0.0 && b.x %s 0.0) ? 0.0 : a.x + 1.0);\n", c, c);
        } else if (vop >= 24 && vop <= 27) {
            static const char *k[] = { "equal", "greaterThan", "greaterThanEqual",
                                       "notEqual" };
            g_string_append_printf(b, "if (any(%s(a, b))) discard;\n", k[vop - 24]);
        } else if (vop == 29) {
            g_string_append(b, "a0 = int(floor(clamp(a.w, -256.0, 255.0) + 0.5));\n"
                               "vr = max(a, b);\n");
        }
    }

    /* scalar */
    if (sop >= 42 && sop <= 47) {
        uint32_t sw = swz[2];
        uint32_t rr = (sop & 1) | (istemp[2] << 1) | (sw & 0x3c);
        GString *o = g_string_new("");
        gen_const(g, o, sreg[2] + (c1_addr && !c_a0 ? g->aL : 0), c1_addr && c_a0);
        g_string_append_printf(b, "float x = %s%s%s.%c%s;\nfloat y = %sr%d.%c;\n",
                               neg[2] ? "-" : "", abs_c ? "abs(" : "", o->str,
                               gl_xyzw[((sw >> 6) + 3) & 3], abs_c ? ")" : "",
                               neg[2] ? "-" : "", (int)(rr & 63), gl_xyzw[sw & 3]);
        g_string_free(o, true);
    } else {
        g_string_append(b, "float x = c.w, y = c.x;\n");
    }
    const char *e = NULL;
    switch (sop) {
    case 0: case 44: case 45: e = "x + y"; break;
    case 1: e = "x + ps"; break;
    case 2: case 42: case 43: e = "x * y"; break;
    case 3: e = "x * ps"; break;
    case 4: e = "(ps == -FMAX || isinf(ps) || isnan(ps) || isinf(y) || y <= 0.0)"
                " ? -FMAX : x * ps"; break;
    case 5: e = "x >= y ? x : y"; break;
    case 6: e = "x < y ? x : y"; break;
    case 7: e = "float(x == 0.0)"; break;
    case 8: e = "float(x > 0.0)"; break;
    case 9: e = "float(x >= 0.0)"; break;
    case 10: e = "float(x != 0.0)"; break;
    case 11: e = "fract(x)"; break;
    case 12: e = "trunc(x)"; break;
    case 13: e = "floor(x)"; break;
    case 14: e = "exp2(x)"; break;
    case 15: e = "x <= 0.0 ? -FMAX : log2(x)"; break;
    case 16: e = "log2(x)"; break;
    case 17: e = "x == 0.0 ? FMAX : 1.0 / x"; break;
    case 18: e = "x == 0.0 ? 0.0 : 1.0 / x"; break;
    case 19: e = "1.0 / x"; break;
    case 20: e = "x == 0.0 ? FMAX : inversesqrt(x)"; break;
    case 21: e = "x == 0.0 ? 0.0 : inversesqrt(x)"; break;
    case 22: e = "inversesqrt(x)"; break;
    case 23: case 24:
        g_string_append_printf(b, "a0 = int(floor(clamp(x, -256.0, 255.0)%s));\n",
                               sop == 23 ? " + 0.5" : "");
        e = "x >= y ? x : y";
        break;
    case 25: case 46: case 47: e = "x - y"; break;
    case 26: e = "x - ps"; break;
    case 27: g_string_append(b, "p = x == 0.0;\n"); e = "p ? 0.0 : 1.0"; break;
    case 28: g_string_append(b, "p = x != 0.0;\n"); e = "p ? 0.0 : 1.0"; break;
    case 29: g_string_append(b, "p = x > 0.0;\n"); e = "p ? 0.0 : 1.0"; break;
    case 30: g_string_append(b, "p = x >= 0.0;\n"); e = "p ? 0.0 : 1.0"; break;
    case 31: g_string_append(b, "p = x == 1.0;\n");
        e = "p ? 0.0 : (x == 0.0 ? 1.0 : x)"; break;
    case 32: g_string_append(b, "p = x - 1.0 <= 0.0;\n"); e = "p ? 0.0 : x - 1.0"; break;
    case 33: g_string_append(b, "p = false;\n"); e = "FMAX"; break;
    case 34: g_string_append(b, "p = x == 0.0;\n"); e = "p ? 0.0 : x"; break;
    case 35: g_string_append(b, "if (x == 0.0) discard;\n"); e = "0.0"; break;
    case 36: g_string_append(b, "if (x > 0.0) discard;\n"); e = "0.0"; break;
    case 37: g_string_append(b, "if (x >= 0.0) discard;\n"); e = "0.0"; break;
    case 38: g_string_append(b, "if (x != 0.0) discard;\n"); e = "0.0"; break;
    case 39: g_string_append(b, "if (x == 1.0) discard;\n"); e = "0.0"; break;
    case 40: e = "sqrt(x)"; break;
    case 48: e = "sin(x)"; break;
    case 49: e = "cos(x)"; break;
    default: e = NULL; break;           /* RETAIN_PREV and unknown: unchanged */
    }
    if (e) {
        g_string_append_printf(b, "ps = %s;\n", e);
    }
    if (vclamp) {
        g_string_append(b, "vr = clamp(vr, 0.0, 1.0);\n");
    }
    g_string_append_printf(b, "float sr = %s;\n", sclamp ? "clamp(ps, 0.0, 1.0)" : "ps");

    if (exp) {
        if (vdst == 0) {                /* colour 0; other exports unused here */
            uint32_t one = vmask & smask;
            uint32_t zero = sdst_rel ? (0xf & ~(vmask | smask)) : 0;
            gen_mask_store(b, "e0", vwm, "vr", false);
            gen_mask_store(b, "e0", swm, "sr", true);
            gen_mask_store(b, "e0", one & ~vwm & ~swm, "1.0", true);
            gen_mask_store(b, "e0", zero & ~vwm & ~swm & ~one, "0.0", true);
        }
    } else {
        char d[16];
        snprintf(d, sizeof(d), "r%d", gen_temp(g, vdst, vdst_rel));
        gen_mask_store(b, d, vwm, "vr", false);
        snprintf(d, sizeof(d), "r%d", gen_temp(g, sdst, sdst_rel));
        gen_mask_store(b, d, swm, "sr", true);
    }
    g_string_append(b, "}\n");
}

static void gen_fetch(Gen *g, const uint32_t *w)
{
    GString *b = g->b;
    uint32_t d0 = w[0], d1 = w[1];
    uint32_t opc = d0 & 0x1f;
    uint32_t src = (d0 >> 5) & 0x3f, src_rel = (d0 >> 11) & 1;
    uint32_t dst = (d0 >> 12) & 0x3f, dst_rel = (d0 >> 18) & 1;
    uint32_t cidx = (d0 >> 20) & 0x1f;
    bool denorm = (d0 >> 25) & 1;
    uint32_t sswz = (d0 >> 26) & 0x3f;
    uint32_t dswz = d1 & 0xfff;
    int mag = (d1 >> 12) & 3;
    bool pselect = (w[1] >> 31) & 1, pcond = (w[2] >> 31) & 1;

    if (opc == 0) {
        g->fail = "vertex fetch in a pixel shader";
        return;
    }
    g_string_append(b, pselect ? (pcond ? "if (p) {\n" : "if (!p) {\n") : "{\n");
    if (opc == 1) {
        int sr = gen_temp(g, src, src_rel);
        g->samplers |= 1u << cidx;
        if (g->filt[cidx] < 0 && mag != 3) {
            g->filt[cidx] = mag;
        }
        char sz[16] = "";
        if (denorm) {                   /* unnormalised coordinates */
            snprintf(sz, sizeof(sz), " / S%u", cidx);
        }
        g_string_append_printf(b, "vec4 t = texture(T%u, vec2(r%d.%c, r%d.%c)%s);\n",
                               cidx, sr, gl_xyzw[sswz & 3], sr,
                               gl_xyzw[(sswz >> 2) & 3], sz);
    } else {
        g_string_append(b, "vec4 t = vec4(0.0, 0.0, 0.0, 1.0);\n");
    }
    int dr = gen_temp(g, dst, dst_rel);
    for (int i = 0; i < 4; i++) {
        uint32_t c = (dswz >> (3 * i)) & 7;
        if (c == 7) {
            continue;
        }
        if (c < 4) {
            g_string_append_printf(b, "r%d.%c = t.%c;\n", dr, gl_xyzw[i], gl_xyzw[c]);
        } else {
            g_string_append_printf(b, "r%d.%c = %s;\n", dr, gl_xyzw[i],
                                   c == 5 ? "1.0" : "0.0");
        }
    }
    g_string_append(b, "}\n");
}

/* walk the control flow from base, emitting the instructions run */
static void gen_program(Gen *g, uint32_t base)
{
    AdrenoState *s = g->s;
    const uint32_t *prog = &s->istore[base * 3];
    uint32_t limit = ADRENO_ISTORE_DWORDS / 3 - base;
    int call_stack[4], call_depth = 0;
    int loop_cnt[4], loop_depth = 0;

    for (uint32_t pc = 0, steps = 0; pc < 512 && !g->fail; steps++) {
        uint32_t lo, hi;
        if (steps >= 4096 || g->ninstr > 2048) {
            g->fail = "program too long";
            return;
        }
        cf_get(prog, pc, &lo, &hi);
        uint64_t cf = lo | ((uint64_t)hi << 32);
        uint32_t opc = (cf >> 44) & 0xf;
        uint32_t next = pc + 1;

        switch (opc) {
        case 0: case 12: case 15:
            break;
        case 1: case 2: case 3: case 4: case 5: case 6: case 13: case 14: {
            uint32_t addr = cf & 0x1ff, count = (cf >> 12) & 7;
            uint32_t serial = (cf >> 16) & 0xfff;
            uint32_t baddr = (cf >> 34) & 0xff;
            bool cond = (cf >> 42) & 1;
            bool run = true, dyn = false;
            if (opc == 3 || opc == 4 || opc == 13 || opc == 14) {
                run = bool_const(s, baddr) == cond;
            } else if (opc == 5 || opc == 6) {
                dyn = true;
            }
            if (run) {
                if (dyn) {
                    g_string_append(g->b, cond ? "if (p) {\n" : "if (!p) {\n");
                }
                for (uint32_t i = 0; i < count && addr + i < limit; i++) {
                    const uint32_t *w = &prog[3 * (addr + i)];
                    g->ninstr++;
                    if ((serial >> (2 * i)) & 1) {
                        gen_fetch(g, w);
                    } else {
                        gen_alu(g, w);
                    }
                }
                if (dyn) {
                    g_string_append(g->b, "}\n");
                }
            }
            if (opc == 2 || opc == 4 || opc == 6 || opc == 14) {
                return;
            }
            break;
        }
        case 7: {
            uint32_t lc = s->regs[ADRENO_REG_LOOP + (((cf >> 16) & 0x1f) & 31)];
            int count = lc & 0xff;
            if (loop_depth >= 4 || count == 0) {
                next = cf & 0x3ff;
                break;
            }
            loop_cnt[loop_depth++] = count;
            g->aL = (lc >> 8) & 0xff;
            break;
        }
        case 8: {
            uint32_t lc = s->regs[ADRENO_REG_LOOP + (((cf >> 16) & 0x1f) & 31)];
            if (!loop_depth) {
                break;
            }
            g->aL += (int8_t)(lc >> 16);
            if (--loop_cnt[loop_depth - 1] > 0) {
                next = cf & 0x3ff;
            } else {
                loop_depth--;
            }
            break;
        }
        case 9: case 11: {
            bool force = (cf >> 13) & 1, predj = (cf >> 14) & 1;
            bool cond = (cf >> 42) & 1;
            if (predj && !force) {
                g->fail = "jump on a predicate";
                return;
            }
            if (force || bool_const(s, (cf >> 34) & 0xff) == cond) {
                if (opc == 9) {
                    if (call_depth >= 4) {
                        break;
                    }
                    call_stack[call_depth++] = pc + 1;
                }
                next = cf & 0x3ff;
            }
            break;
        }
        case 10:
            if (call_depth) {
                next = call_stack[--call_depth];
            }
            break;
        default:
            return;
        }
        pc = next;
    }
}

typedef struct GlProg {
    uint64_t key;
    uint32_t np;
    GLuint prog;
    bool bad;
    GLint locC, locFC, locAREF;
    GLint locT[32], locS[32];
    uint32_t samplers;
    int8_t filt[32];
    int nconst;
    uint64_t used;
} GlProg;

#define GL_NPROG 160
static GlProg gl_prog[GL_NPROG];

static GLuint gl_compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;

    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        qemu_log("adreno: GL shader compile failed: %s\n%s\n", log, src);
        error_report("adreno: GL shader compile failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static uint64_t gl_mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

/* translated program for the draw's pixel shader and state */
static GlProg *gl_get_prog(AdrenoState *s, const DrawState *ds)
{
    uint32_t base = ds->ps_base;
    uint32_t n = MIN((uint32_t)ADRENO_ISTORE_DWORDS - base * 3, 1536u);
    bool atest = (ds->cctl >> 3) & 1;
    uint32_t afunc = atest ? ds->cctl & 7 : 7;
    uint64_t key = 0x12345;
    GlProg *e = NULL, *lru = NULL;

    for (uint32_t i = 0; i < n; i++) {
        key = (key ^ s->istore[base * 3 + i]) * 0x100000001b3ull;
    }
    for (int i = 0; i < 8; i++) {
        key = gl_mix(key, s->regs[ADRENO_REG_BOOL + i]);
    }
    for (int i = 0; i < 32; i++) {
        key = gl_mix(key, s->regs[ADRENO_REG_LOOP + i]);
    }
    key = gl_mix(key, ds->nparams | ds->param_gen << 8 | ds->param_gen_pos << 16 |
                      (uint64_t)afunc << 32);
    for (int i = 0; i < GL_NPROG; i++) {
        if (gl_prog[i].used && gl_prog[i].key == key) {
            gl_prog[i].used = ++gl_tick;
            return gl_prog[i].bad ? NULL : &gl_prog[i];
        }
        if (!lru || gl_prog[i].used < lru->used) {
            lru = &gl_prog[i];
        }
    }
    e = lru;
    if (e->prog) {
        /* programs are small; the GL objects of evicted ones are kept */
        e->prog = 0;
    }
    memset(e, 0, sizeof(*e));
    e->key = key;
    e->used = ++gl_tick;
    e->np = ds->nparams;

    Gen g = { .s = s, .b = g_string_new("") };
    memset(g.filt, -1, sizeof(g.filt));
    gen_program(&g, base);
    if (g.fail) {
        static int warned;
        if (warned++ < 20) {
            warn_report("adreno: GL: pixel shader not translated: %s", g.fail);
        }
        g_string_free(g.b, true);
        e->bad = true;
        return NULL;
    }

    GString *fs = g_string_new("#version 300 es\nprecision highp float;\n"
                               "precision highp int;\n#define FMAX 3.402823e38\n");
    for (uint32_t i = 0; i < ds->nparams; i++) {
        g_string_append_printf(fs, "in vec4 v%u;\n", i);
    }
    int nc = g.dyn_const ? GL_NCONST : g.nconst;
    if (nc) {
        g_string_append_printf(fs, "uniform vec4 C[%d];\n", nc);
    }
    if (g.dyn_const) {
        g_string_append_printf(fs, "vec4 cst(int i) { return i < 0 || i >= %d ?"
                               " vec4(0.0) : C[i]; }\n", nc);
    }
    for (int i = 0; i < 32; i++) {
        if (g.samplers & (1u << i)) {
            g_string_append_printf(fs, "uniform sampler2D T%d;\nuniform vec2 S%d;\n", i, i);
        }
    }
    g_string_append(fs,
        "uniform vec4 FC;\nuniform float AREF;\nout vec4 o;\n"
        "vec4 cube(vec4 a) {\n"
        "  float x = a.z, y = a.w, z = a.x;\n"
        "  float ax = abs(x), ay = abs(y), az = abs(z); vec4 r;\n"
        "  if (az >= ax && az >= ay) r = vec4(-y, z < 0.0 ? -x : x, z, z < 0.0 ? 5.0 : 4.0);\n"
        "  else if (ay >= ax) r = vec4(y < 0.0 ? -z : z, x, y, y < 0.0 ? 3.0 : 2.0);\n"
        "  else r = vec4(-y, x < 0.0 ? z : -z, x, x < 0.0 ? 1.0 : 0.0);\n"
        "  r.z *= 2.0; return r;\n}\n"
        "void main() {\n");
    for (int i = 0; i < 64; i++) {
        if ((uint32_t)i < ds->nparams) {
            g_string_append_printf(fs, "vec4 r%d = v%d;\n", i, i);
        } else {
            g_string_append_printf(fs, "vec4 r%d = vec4(0.0);\n", i);
        }
    }
    if (ds->param_gen) {
        g_string_append_printf(fs, "r%u = vec4((gl_FragCoord.x - FC.x) * FC.z,"
                               " gl_FragCoord.y - FC.y, 0.0, 0.0);\n",
                               ds->param_gen_pos & 63);
    }
    g_string_append(fs, "vec4 e0 = vec4(0.0);\nfloat ps = 0.0;\nbool p = false;\n"
                        "int a0 = 0;\n");
    g_string_append(fs, g.b->str);
    static const char *aop[] = { NULL, "<", "==", "<=", ">", "!=", ">=", NULL };
    if (afunc == 0) {
        g_string_append(fs, "discard;\n");
    } else if (aop[afunc]) {
        g_string_append_printf(fs, "if (!(e0.w %s AREF)) discard;\n", aop[afunc]);
    }
    g_string_append(fs, "o = e0;\n}\n");
    g_string_free(g.b, true);

    GString *vs = g_string_new("#version 300 es\nin vec4 P;\n");
    for (uint32_t i = 0; i < ds->nparams; i++) {
        g_string_append_printf(vs, "in vec4 A%u;\nout vec4 v%u;\n", i, i);
    }
    g_string_append(vs, "void main() {\n  gl_Position = P;\n");
    for (uint32_t i = 0; i < ds->nparams; i++) {
        g_string_append_printf(vs, "  v%u = A%u;\n", i, i);
    }
    g_string_append(vs, "}\n");

    GLuint v = gl_compile(GL_VERTEX_SHADER, vs->str);
    GLuint f = v ? gl_compile(GL_FRAGMENT_SHADER, fs->str) : 0;
    g_string_free(vs, true);
    if (getenv("ADRENO_GLSL_DUMP")) {
        qemu_log("adreno: GL ps %016" PRIx64 ":\n%s\n", key, fs->str);
    }
    g_string_free(fs, true);
    if (!v || !f) {
        e->bad = true;
        return NULL;
    }
    GLuint pr = glCreateProgram();
    glAttachShader(pr, v);
    glAttachShader(pr, f);
    glBindAttribLocation(pr, 0, "P");
    for (uint32_t i = 0; i < ds->nparams; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "A%u", i);
        glBindAttribLocation(pr, i + 1, nm);
    }
    glLinkProgram(pr);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(pr, sizeof(log), NULL, log);
        error_report("adreno: GL program link failed: %s", log);
        e->bad = true;
        return NULL;
    }
    e->prog = pr;
    e->samplers = g.samplers;
    memcpy(e->filt, g.filt, sizeof(e->filt));
    e->nconst = nc;
    e->locC = glGetUniformLocation(pr, "C");
    e->locFC = glGetUniformLocation(pr, "FC");
    e->locAREF = glGetUniformLocation(pr, "AREF");
    for (int i = 0; i < 32; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "T%d", i);
        e->locT[i] = glGetUniformLocation(pr, nm);
        snprintf(nm, sizeof(nm), "S%d", i);
        e->locS[i] = glGetUniformLocation(pr, nm);
    }
    return e;
}


/* ---------------- draws ---------------- */

static GLenum gl_blend_factor(uint32_t f)
{
    static const GLenum m[] = {
        [0] = GL_ZERO, [1] = GL_ONE, [4] = GL_SRC_COLOR,
        [5] = GL_ONE_MINUS_SRC_COLOR, [6] = GL_SRC_ALPHA,
        [7] = GL_ONE_MINUS_SRC_ALPHA, [8] = GL_DST_COLOR,
        [9] = GL_ONE_MINUS_DST_COLOR, [10] = GL_DST_ALPHA,
        [11] = GL_ONE_MINUS_DST_ALPHA, [12] = GL_CONSTANT_COLOR,
        [13] = GL_ONE_MINUS_CONSTANT_COLOR, [14] = GL_CONSTANT_ALPHA,
        [15] = GL_ONE_MINUS_CONSTANT_ALPHA, [16] = GL_SRC_ALPHA_SATURATE,
    };
    return f < ARRAY_SIZE(m) && (m[f] || f == 0) ? m[f] : GL_ONE;
}

static GLenum gl_blend_eq(uint32_t op)
{
    switch (op) {
    case 1: return GL_FUNC_SUBTRACT;
    case 2: return GL_MIN;
    case 3: return GL_MAX;
    case 4: return GL_FUNC_REVERSE_SUBTRACT;
    default: return GL_FUNC_ADD;
    }
}

static void gl_state(AdrenoState *s, const DrawState *ds)
{
    bool blend = !((ds->cctl >> 5) & 1) || ((ds->blend >> 29) & 1);
    uint32_t b = ds->blend;

    if (blend && b != 0x00010001) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(gl_blend_factor(b & 0x1f), gl_blend_factor((b >> 8) & 0x1f),
                            gl_blend_factor((b >> 16) & 0x1f),
                            gl_blend_factor((b >> 24) & 0x1f));
        glBlendEquationSeparate(gl_blend_eq((b >> 5) & 7), gl_blend_eq((b >> 21) & 7));
        glBlendColor(f_of(s->regs[A2XX_RB_BLEND_RED]),
                     f_of(s->regs[A2XX_RB_BLEND_RED + 1]),
                     f_of(s->regs[A2XX_RB_BLEND_RED + 2]),
                     f_of(s->regs[A2XX_RB_BLEND_RED + 3]));
    } else {
        glDisable(GL_BLEND);
    }
    glColorMask(ds->cmask & 1, (ds->cmask >> 1) & 1, (ds->cmask >> 2) & 1,
                (ds->cmask >> 3) & 1);
    if (ds->zctl & 2) {
        static const GLenum f[] = { GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL,
                                    GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(f[(ds->zctl >> 4) & 7]);
        glDepthMask((ds->zctl >> 2) & 1);
    } else {
        glDisable(GL_DEPTH_TEST);
        glDepthMask(0);
    }
    glDisable(GL_CULL_FACE);
    glDisable(GL_DITHER);
}

typedef struct GlVerts {
    float *v;
    uint32_t n, cap, stride;            /* floats per vertex */
    float sx, ox, sy, oy;               /* window -> NDC */
    bool persp;
} GlVerts;

static void gl_push_vtx(GlVerts *g, const PVtx *p, uint32_t np)
{
    if (g->n + 1 > g->cap) {
        g->cap = MAX(g->cap * 2, 96u);
        g->v = g_renew(float, g->v, g->cap * g->stride);
    }
    float *o = g->v + g->n * g->stride;
    float w = g->persp && p->rw != 0.0f ? 1.0f / p->rw : 1.0f;
    o[0] = (p->x * g->sx + g->ox) * w;
    o[1] = (p->y * g->sy + g->oy) * w;
    o[2] = (p->z * 2.0f - 1.0f) * w;
    o[3] = w;
    for (uint32_t i = 0; i < np; i++) {
        memcpy(o + 4 + 4 * i, p->p[i].f, 16);
    }
    g->n++;
}

/* a triangle, culled as by raster_tri */
static void gl_push_tri(GlVerts *g, const DrawState *ds, const PVtx *a, const PVtx *b,
                        const PVtx *c, bool nocull)
{
    float area = edge(a->x, a->y, b->x, b->y, c->x, c->y);

    if (area == 0.0f || !isfinite(area)) {
        return;
    }
    if (!nocull) {
        uint32_t sc = ds->s->regs[A2XX_PA_SU_SC_MODE_CNTL];
        bool ccw_front = !((sc >> 2) & 1);
        bool is_front = (area < 0) == ccw_front;
        if ((is_front && (sc & 1)) || (!is_front && (sc & 2))) {
            return;
        }
    }
    gl_push_vtx(g, a, ds->nparams);
    gl_push_vtx(g, b, ds->nparams);
    gl_push_vtx(g, c, ds->nparams);
}

static void gl_push_rect(GlVerts *g, const DrawState *ds, const PVtx *v)
{
    for (int c = 0; c < 3; c++) {
        const PVtx *o = &v[c], *a = &v[(c + 1) % 3], *b = &v[(c + 2) % 3];
        bool right = (fabsf(a->x - o->x) < 1e-3f && fabsf(b->y - o->y) < 1e-3f) ||
                     (fabsf(a->y - o->y) < 1e-3f && fabsf(b->x - o->x) < 1e-3f);
        if (right) {
            PVtx d = *a;
            d.x = a->x + b->x - o->x;
            d.y = a->y + b->y - o->y;
            d.z = a->z + b->z - o->z;
            d.rw = a->rw + b->rw - o->rw;
            for (uint32_t i = 0; i < ds->nparams; i++) {
                for (int k = 0; k < 4; k++) {
                    d.p[i].f[k] = a->p[i].f[k] + b->p[i].f[k] - o->p[i].f[k];
                }
            }
            gl_push_tri(g, ds, o, a, b, true);
            gl_push_tri(g, ds, a, &d, b, true);
            return;
        }
    }
    gl_push_tri(g, ds, &v[0], &v[1], &v[2], true);
}

static void gl_build(GlVerts *g, const DrawState *ds, uint32_t prim, PVtx **vs,
                     uint32_t count)
{
    switch (prim) {
    case 4:
        for (uint32_t i = 0; i + 2 < count; i += 3) {
            gl_push_tri(g, ds, vs[i], vs[i + 1], vs[i + 2], false);
        }
        break;
    case 5:
        for (uint32_t i = 1; i + 1 < count; i++) {
            gl_push_tri(g, ds, vs[0], vs[i], vs[i + 1], false);
        }
        break;
    case 6:
        for (uint32_t i = 0; i + 2 < count; i++) {
            if (i & 1) {
                gl_push_tri(g, ds, vs[i + 1], vs[i], vs[i + 2], false);
            } else {
                gl_push_tri(g, ds, vs[i], vs[i + 1], vs[i + 2], false);
            }
        }
        break;
    case 8:
        for (uint32_t i = 0; i + 2 < count; i += 3) {
            PVtx r3[3] = { *vs[i], *vs[i + 1], *vs[i + 2] };
            gl_push_rect(g, ds, r3);
        }
        break;
    }
}

static void gl_bind_textures(AdrenoState *s, const GlProg *pr)
{
    int unit = 0;

    if (adreno_gl_pending && pr->samplers) {
        adreno_gl_sync(s);              /* textures may be what was just resolved */
    }
    for (int n = 0; n < 32; n++) {
        if (!(pr->samplers & (1u << n))) {
            continue;
        }
        TexInfo ti;
        tex_info(s, n, &ti);
        GLuint t = gl_texture(s, &ti);
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, t);
        if (t) {
            int f = pr->filt[n] >= 0 ? pr->filt[n] : ti.mag;
            GLenum gf = f == 0 ? GL_NEAREST : GL_LINEAR;
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gf);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gf);
            for (int a = 0; a < 2; a++) {
                uint32_t m = a ? ti.clamp_y : ti.clamp_x;
                GLenum gw = m == 0 ? GL_REPEAT : m == 1 ? GL_MIRRORED_REPEAT :
                            GL_CLAMP_TO_EDGE;
                glTexParameteri(GL_TEXTURE_2D, a ? GL_TEXTURE_WRAP_T : GL_TEXTURE_WRAP_S,
                                gw);
            }
            static const GLenum sw[4] = { GL_TEXTURE_SWIZZLE_R, GL_TEXTURE_SWIZZLE_G,
                                          GL_TEXTURE_SWIZZLE_B, GL_TEXTURE_SWIZZLE_A };
            for (int i = 0; i < 4; i++) {
                uint32_t c = ti.swz[i];
                glTexParameteri(GL_TEXTURE_2D, sw[i], c < 4 ? (GLint)(GL_RED + c) :
                                c == 5 ? GL_ONE : GL_ZERO);
            }
        }
        glUniform1i(pr->locT[n], unit);
        glUniform2f(pr->locS[n], ti.w, ti.h);
        unit++;
    }
}

static void gl_draw_render(AdrenoState *s, DrawState *ds, uint32_t prim, PVtx **vs,
                           uint32_t count)
{
    GlProg *pr = gl_get_prog(s, ds);
    if (!pr) {
        return;
    }
    uint32_t ns = ds->samples, bpp = fmt_bpp_rb(ds->cfmt);
    GmemRT *rt = gl_gmem_rt(ds->pitch * ns, bpp);
    uint32_t x0, y0;
    gl_gmem_origin(rt, ds->cbase, &x0, &y0);

    GlVerts g = { .stride = 4 + 4 * ds->nparams, .persp = ds->persp };
    g.sx = 2.0f * ns / rt->tw;
    g.ox = 2.0f * x0 / rt->tw - 1.0f;
    g.sy = 2.0f / rt->th;
    g.oy = 2.0f * y0 / rt->th - 1.0f;
    gl_build(&g, ds, prim, vs, count);
    if (!g.n) {
        g_free(g.v);
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    glViewport(0, 0, rt->tw, rt->th);
    glEnable(GL_SCISSOR_TEST);
    glScissor(ds->sx0 * ns + x0, ds->sy0 + y0, (ds->sx1 - ds->sx0) * ns,
              ds->sy1 - ds->sy0);
    gl_state(s, ds);
    glUseProgram(pr->prog);

    if (pr->nconst) {
        uint32_t c = s->regs[A2XX_SQ_PS_CONST];
        uint32_t base = c & 0x1ff, size = (c >> 12) & 0x1ff;
        float cv[GL_NCONST * 4];
        for (int i = 0; i < pr->nconst; i++) {
            for (int k = 0; k < 4; k++) {
                cv[i * 4 + k] = ((uint32_t)i > size || base + i >= ADRENO_ALU_CONSTS) ?
                    0.0f : f_of(s->regs[ADRENO_REG_ALU + (base + i) * 4 + k]);
            }
        }
        glUniform4fv(pr->locC, pr->nconst, cv);
    }
    glUniform4f(pr->locFC, x0, y0, 1.0f / ns, 0);
    glUniform1f(pr->locAREF, ds->alpha_ref);
    gl_bind_textures(s, pr);

    glBindBuffer(GL_ARRAY_BUFFER, gl_vbo);
    glBufferData(GL_ARRAY_BUFFER, g.n * g.stride * 4, g.v, GL_STREAM_DRAW);
    for (uint32_t i = 0; i < 17; i++) {
        if (i <= ds->nparams) {
            glEnableVertexAttribArray(i);
            glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, g.stride * 4,
                                  (void *)(uintptr_t)(i * 16));
        } else {
            glDisableVertexAttribArray(i);
        }
    }
    glDrawArrays(GL_TRIANGLES, 0, g.n);
    g_free(g.v);
}

/* ---------------- resolves ---------------- */

typedef struct GlPend {
    GLuint pbo;
    uint32_t w, h, x, y;                /* GMEM area */
    uint32_t cp_dest, cp_pitch, cp_info, cp_dx, cp_dy;
    uint64_t draw;
} GlPend;

#define GL_NPEND 64
static GlPend gl_pend[GL_NPEND];
static GLuint gl_pbo_pool[GL_NPEND];
static uint64_t gl_cmp_px, gl_cmp_bad, gl_cmp_resolves, gl_cmp_bad_resolves;

/* store (or, in "both" mode, compare) one read back resolve */
static void gl_store(AdrenoState *s, const GlPend *pe, const uint8_t *px)
{
    uint32_t dfmt = (pe->cp_info >> 4) & 0xf, dswap = (pe->cp_info >> 8) & 3;
    uint32_t wmask = (pe->cp_info >> 14) & 0xf;
    bool linear = (pe->cp_info >> 3) & 1;
    int dbpp = fmt_bpp_rb(dfmt);
    bool cmp = gl_mode == GLB_BOTH;
    g_autofree uint8_t *row = g_malloc(pe->w * 16);
    g_autofree uint8_t *cur = g_malloc(pe->w * 16);
    uint64_t bad = 0;

    for (uint32_t y = 0; y < pe->h; y++) {
        const uint8_t *src = px + y * pe->w * 4;
        uint32_t dy = pe->y + y + pe->cp_dy;
        for (uint32_t x = 0; x < pe->w; x++) {
            uint8_t *d = row + x * dbpp;
            uint32_t dx = pe->x + x + pe->cp_dx;
            uint32_t addr = linear ? pe->cp_dest + (dy * pe->cp_pitch + dx) * dbpp :
                            pe->cp_dest + tiled_offset(dx, dy, pe->cp_pitch, dbpp);
            if (dfmt == 5 && dswap == 0 && wmask == 0xf) {
                memcpy(d, src + x * 4, 4);
            } else {
                float c[4];
                for (int i = 0; i < 4; i++) {
                    c[i] = src[x * 4 + i] * (1.0f / 255);
                }
                if (wmask != 0xf || cmp) {
                    adreno_read(s, addr, d, dbpp);
                }
                rb_pack(dfmt, dswap, c, d, wmask);
            }
            if (!linear) {
                if (cmp) {
                    uint8_t m[16];
                    adreno_read(s, addr, m, dbpp);
                    for (int i = 0; i < dbpp; i++) {
                        if (abs(m[i] - d[i]) > 3) {
                            bad++;
                            break;
                        }
                    }
                } else {
                    adreno_write(s, addr, d, dbpp);
                }
            }
        }
        if (linear) {
            uint32_t addr = pe->cp_dest + (dy * pe->cp_pitch + pe->x + pe->cp_dx) * dbpp;
            if (cmp) {
                adreno_read(s, addr, cur, pe->w * dbpp);
                for (uint32_t x = 0; x < pe->w; x++) {
                    for (int i = 0; i < dbpp; i++) {
                        if (abs(cur[x * dbpp + i] - row[x * dbpp + i]) > 3) {
                            bad++;
                            break;
                        }
                    }
                }
            } else {
                adreno_write(s, addr, row, pe->w * dbpp);
            }
        }
    }
    if (!cmp) {
        uint32_t first = pe->cp_dest + (pe->y + pe->cp_dy) * pe->cp_pitch * dbpp;
        gl_tex_touch(first, pe->h * pe->cp_pitch * dbpp + pe->w * dbpp);
        return;
    }
    gl_cmp_px += pe->w * pe->h;
    gl_cmp_bad += bad;
    gl_cmp_resolves++;
    if (bad * 200 > pe->w * pe->h) {
        static int logged;
        gl_cmp_bad_resolves++;
        if (logged++ < 40) {
            fprintf(stderr, "adreno: GL/SW differ: draw %" PRIu64 " resolve %ux%u at gmem"
                     " %u,%u to %08x (fmt %u): %" PRIu64 " px\n", pe->draw, pe->w,
                     pe->h, pe->x, pe->y, pe->cp_dest, dfmt, bad);
        }
    }
    static int64_t last;
    int64_t now = get_clock();
    if (now - last > 5000000000LL) {
        last = now;
        fprintf(stderr, "adreno: GL/SW: %" PRIu64 " of %" PRIu64 " resolves differ, %"
                 PRIu64 " of %" PRIu64 " px\n", gl_cmp_bad_resolves, gl_cmp_resolves,
                 gl_cmp_bad, gl_cmp_px);
    }
}

void adreno_gl_sync(AdrenoState *s)
{
    for (int i = 0; i < adreno_gl_pending; i++) {
        GlPend *pe = &gl_pend[i];
        uint32_t size = pe->w * pe->h * 4;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, pe->pbo);
        const uint8_t *px = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, size,
                                             GL_MAP_READ_BIT);
        if (px) {
            gl_store(s, pe, px);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    adreno_gl_pending = 0;
}

static void gl_draw_resolve(AdrenoState *s, DrawState *ds, PVtx **vs, uint32_t count)
{
    float minx = INFINITY, miny = INFINITY, maxx = -INFINITY, maxy = -INFINITY;

    for (uint32_t i = 0; i < count; i++) {
        minx = MIN(minx, vs[i]->x);
        miny = MIN(miny, vs[i]->y);
        maxx = MAX(maxx, vs[i]->x);
        maxy = MAX(maxy, vs[i]->y);
    }
    if (!isfinite(minx) || !isfinite(maxx) || !isfinite(miny) || !isfinite(maxy)) {
        return;
    }
    int xa = MAX(ds->sx0, (int)floorf(minx + 0.5f));
    int xb = MIN(ds->sx1, (int)floorf(maxx + 0.5f));
    int ya = MAX(ds->sy0, (int)floorf(miny + 0.5f));
    int yb = MIN(ds->sy1, (int)floorf(maxy + 0.5f));
    if (xa >= xb || ya >= yb) {
        return;
    }
    if (adreno_gl_pending == GL_NPEND) {
        adreno_gl_sync(s);
    }
    uint32_t bpp = fmt_bpp_rb(ds->cfmt);
    GmemRT *rt = gl_gmem_rt(ds->pitch, bpp);
    uint32_t x0, y0;
    gl_gmem_origin(rt, ds->cbase, &x0, &y0);

    GlPend *pe = &gl_pend[adreno_gl_pending];
    if (!gl_pbo_pool[adreno_gl_pending]) {
        glGenBuffers(1, &gl_pbo_pool[adreno_gl_pending]);
    }
    pe->pbo = gl_pbo_pool[adreno_gl_pending];
    pe->w = xb - xa;
    pe->h = yb - ya;
    pe->x = xa;
    pe->y = ya;
    pe->cp_dest = ds->cp_dest;
    pe->cp_pitch = ds->cp_pitch;
    pe->cp_info = ds->cp_info;
    pe->cp_dx = ds->cp_dx;
    pe->cp_dy = ds->cp_dy;
    pe->draw = s->draws;
    adreno_gl_pending++;

    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pe->pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, pe->w * pe->h * 4, NULL, GL_STREAM_READ);
    glReadPixels(xa + x0, ya + y0, pe->w, pe->h, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    if (ds->cp_ctl & 0xf0) {            /* copy and clear */
        uint32_t cc = s->regs[A2XX_CLEAR_COLOR];
        glEnable(GL_SCISSOR_TEST);
        glScissor(xa + x0, ya + y0, pe->w, pe->h);
        glColorMask(1, 1, 1, 1);
        if (bpp == 4) {
            glClearColor((cc & 0xff) / 255.0f, ((cc >> 8) & 0xff) / 255.0f,
                         ((cc >> 16) & 0xff) / 255.0f, (cc >> 24) / 255.0f);
        } else {
            glClearColor(0, 0, 0, 0);
        }
        glClear(GL_COLOR_BUFFER_BIT);
    }
    if (gl_mode == GLB_BOTH) {
        adreno_gl_sync(s);
    }
}

/*
 * A draw for the GL backend.  Returns true when it took the draw (the
 * software rasterizer is then skipped unless both run).
 */
static bool gl_draw(AdrenoState *s, DrawState *ds, uint32_t prim, PVtx **vs,
                    uint32_t count)
{
    if (ds->sysmem) {
        /* rendering straight into memory: the software rasterizer does it,
         * on memory that must hold every earlier resolve */
        if (adreno_gl_pending) {
            adreno_gl_sync(s);
        }
        return false;
    }
    if (ds->mode == 4) {
        gl_draw_render(s, ds, prim, vs, count);
    } else if (ds->mode == 6) {
        gl_draw_resolve(s, ds, vs, count);
    }
    return gl_mode == GLB_ON;
}
