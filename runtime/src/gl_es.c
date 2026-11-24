/*
 * gl_es.c - desktop GL 3.2 core on an OpenGL ES 3.1+ context.
 *
 * Every function here is registered for both modes and falls through to the
 * plain thunk on a desktop context. On ES:
 *  - shaders: GLSL 150 -> ESSL 3.10 (glsl_xlate.cpp), type from glCreateShader
 *  - calls ES lacks: glPolygonMode/glPointSize/glBindFragDataLocation (no-op),
 *    glClearDepth/glDepthRange (float variants), glDrawBuffer -> glDrawBuffers,
 *    glMapBuffer -> glMapBufferRange, caps ES rejects in glEnable/glDisable
 *  - texture formats: unsized/BGRA/desktop-only combinations -> ES ones
 */
#include "sdl_bridge.h"
#include <pthread.h>
#define BCDEC_IMPLEMENTATION
#include "../../third_party/stb/bcdec.h"

void *gl_host_proc(const char *name);
char *w64_glsl_to_essl(const char *src, int gl_type, int es_version);
typedef struct { const char *name; w64_fn fn; } gl_thunk_t;
extern const gl_thunk_t gl_thunks[];
extern const unsigned gl_thunk_count;

static w64_fn thunk(const char *name)
{
    int lo = 0, hi = (int)gl_thunk_count - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2, r = strcmp(name, gl_thunks[m].name);
        if (!r) return gl_thunks[m].fn;
        if (r < 0) hi = m - 1; else lo = m + 1;
    }
    return NULL;
}
#define DESKTOP(name) do { static w64_fn f_; if (!f_) f_ = thunk(name); f_(c); return; } while (0)
#define HOST(var, name, type) static __typeof__(type) var; if (!var) var = (__typeof__(type))gl_host_proc(name)

/* ---------------------------------------------------------------- shaders */
#define MAX_SH 65536
static uint16_t g_sh_type[MAX_SH];
static void es_glCreateShader(CPU *c)
{
    HOST(p, "glCreateShader", uint32_t (*)(uint32_t));
    uint32_t t = AU(0), id = p(t);
    if (id < MAX_SH) g_sh_type[id] = (uint16_t)t;
    RET(id);
}
char *w64_gl_translate_shader(const char *src, uint32_t shader)
{
    int type = shader < MAX_SH ? g_sh_type[shader] : 0;
    char *r = w64_glsl_to_essl(src, type ? type : 0x8B31, 310);
    if (!r) {
        w64_log("[gl-es] shader %u (type %#x) not translated; source follows\n%s", shader, type, src);
        return NULL;
    }
    if (getenv("W64_SHADER_DUMP")) {
        static int n;
        char fn[1024];
        snprintf(fn, sizeof fn, "%s/es_%03d_%u.glsl", getenv("W64_SHADER_DUMP"), n++, shader);
        FILE *f = fopen(fn, "w"); if (f) { fputs(r, f); fclose(f); }
    }
    return r;
}

/* ---------------------------------------------------------------- no-ops and float variants */
static void es_glPolygonMode(CPU *c) { if (!w64_gles) DESKTOP("glPolygonMode"); RETV(); }
static void es_glPointSize(CPU *c) { if (!w64_gles) DESKTOP("glPointSize"); RETV(); }
static void es_glBindFragDataLocation(CPU *c) { if (!w64_gles) DESKTOP("glBindFragDataLocation"); RETV(); }
static void es_glClearDepth(CPU *c)
{
    if (!w64_gles) DESKTOP("glClearDepth");
    HOST(p, "glClearDepthf", void (*)(float));
    p((float)AD(0));
    RETV();
}
static void es_glDepthRange(CPU *c)
{
    if (!w64_gles) DESKTOP("glDepthRange");
    HOST(p, "glDepthRangef", void (*)(float, float));
    p((float)AD(0), (float)AD(1));
    RETV();
}
static void es_glDrawBuffer(CPU *c)
{
    if (!w64_gles) DESKTOP("glDrawBuffer");
    HOST(p, "glDrawBuffers", void (*)(int32_t, const uint32_t *));
    uint32_t m = AU(0);
    if (m == 0x0404 /* GL_FRONT */ || m == 0x0408 /* FRONT_AND_BACK */) m = 0x0405;   /* GL_BACK */
    p(1, &m);
    RETV();
}
static void es_glMapBuffer(CPU *c)
{
    if (!w64_gles) DESKTOP("glMapBuffer");
    HOST(sz, "glGetBufferParameteri64v", void (*)(uint32_t, uint32_t, int64_t *));
    HOST(mr, "glMapBufferRange", void *(*)(uint32_t, intptr_t, intptr_t, uint32_t));
    uint32_t t = AU(0), acc = AU(1);
    int64_t n = 0;
    sz(t, 0x8764 /* GL_BUFFER_SIZE */, &n);
    uint32_t bits = acc == 0x88B8 ? 0x1 : acc == 0x88B9 ? 0x2 : 0x3;   /* READ_ONLY / WRITE_ONLY / READ_WRITE */
    RET((uintptr_t)mr(t, 0, (intptr_t)n, bits));
}
static int es_cap_ok(uint32_t cap)
{
    switch (cap) {
    case 0x8642: /* PROGRAM_POINT_SIZE */ case 0x8DB9: /* FRAMEBUFFER_SRGB */ case 0x0B20: /* LINE_SMOOTH */
    case 0x0B41: /* POLYGON_SMOOTH */ case 0x809D: /* MULTISAMPLE */ case 0x884F: /* TEXTURE_CUBE_MAP_SEAMLESS */
    case 0x864F: /* DEPTH_CLAMP */ case 0x8861: /* POINT_SPRITE */ case 0x0B10: /* POINT_SMOOTH */
    case 0x2A02: /* POLYGON_OFFSET_LINE */ case 0x2A01: /* POLYGON_OFFSET_POINT */ case 0x0BF2: /* COLOR_LOGIC_OP */
        return 0;
    }
    return 1;
}
static uint32_t es_cap(uint32_t cap) { return cap == 0x8F9D /* PRIMITIVE_RESTART */ ? 0x8D69 : cap; }
static void es_glEnable(CPU *c)
{
    if (!w64_gles) DESKTOP("glEnable");
    HOST(p, "glEnable", void (*)(uint32_t));
    if (es_cap_ok(AU(0))) p(es_cap(AU(0)));
    RETV();
}
static void es_glDisable(CPU *c)
{
    if (!w64_gles) DESKTOP("glDisable");
    HOST(p, "glDisable", void (*)(uint32_t));
    if (es_cap_ok(AU(0))) p(es_cap(AU(0)));
    RETV();
}

/* ---------------------------------------------------------------- S3TC fallback */
/* The game compresses its texture atlases to DXT5 itself and creates them the desktop way:
 * glTexImage3D with a compressed internal format and no data, then glCompressedTexSubImage3D.
 * OpenGL ES does not allow compressed formats in glTexImage*, so even on drivers that expose
 * S3TC (Adreno 840 in the Galaxy Z Fold8) the atlas gets no storage and the battle floor and
 * scenery are missing. On ES the textures are therefore always stored as RGBA8 and the blocks
 * decoded on upload (verified on Adreno 730 and 840). W64_S3TC=1 tries the driver's S3TC. */
static int g_s3tc = -1;
static int has_s3tc(void)
{
    if (g_s3tc >= 0) return g_s3tc;
    HOST(gi, "glGetIntegerv", void (*)(uint32_t, int32_t *));
    HOST(gs, "glGetStringi", const char *(*)(uint32_t, uint32_t));
    int32_t n = 0;
    gi(0x821D /* GL_NUM_EXTENSIONS */, &n);
    g_s3tc = 0;
    for (int32_t i = 0; i < n; i++) {
        const char *e = gs(0x1F03, (uint32_t)i);
        if (e && (!strcmp(e, "GL_EXT_texture_compression_s3tc") || !strcmp(e, "GL_NV_texture_compression_s3tc"))) g_s3tc = 1;
    }
    int drv = g_s3tc;
    if (!getenv("W64_S3TC") || getenv("W64_NO_S3TC")) g_s3tc = 0;
    w64_log("[gl-es] S3TC %s by the driver; DXT textures %s", drv ? "supported" : "not supported",
            g_s3tc ? "passed through (W64_S3TC)" : "decoded to RGBA8");
    return g_s3tc;
}
static int is_dxt(uint32_t f) { return (f >= 0x83F0 && f <= 0x83F3) || (f >= 0x8C4C && f <= 0x8C4F); }
static int dxt_bytes(uint32_t f) { return (f == 0x83F0 || f == 0x83F1 || f == 0x8C4C || f == 0x8C4D) ? 8 : 16; }
static uint32_t pbo_bound(void)
{
    HOST(gi, "glGetIntegerv", void (*)(uint32_t, int32_t *));
    int32_t b = 0;
    gi(0x88EF /* GL_PIXEL_UNPACK_BUFFER_BINDING */, &b);
    return (uint32_t)b;
}
/* Source bytes of an upload: client memory, or mapped from the bound unpack buffer. Returns a
 * pointer valid until unmap_src(). */
static const uint8_t *map_src(const void *data, size_t n, uint32_t *pbo)
{
    *pbo = pbo_bound();
    if (!*pbo) return data;
    HOST(mr, "glMapBufferRange", void *(*)(uint32_t, intptr_t, intptr_t, uint32_t));
    return mr(0x88EC /* GL_PIXEL_UNPACK_BUFFER */, (intptr_t)(uintptr_t)data, (intptr_t)n, 0x1 /* MAP_READ */);
}
static void unmap_src(uint32_t pbo)
{
    if (!pbo) return;
    HOST(um, "glUnmapBuffer", uint8_t (*)(uint32_t));
    HOST(bb, "glBindBuffer", void (*)(uint32_t, uint32_t));
    um(0x88EC);
    bb(0x88EC, 0);
}
static void rebind_pbo(uint32_t pbo)
{
    if (!pbo) return;
    HOST(bb, "glBindBuffer", void (*)(uint32_t, uint32_t));
    bb(0x88EC, pbo);
}
/* decode w x h (x d) of DXT blocks into a tightly packed RGBA8 buffer */
static uint8_t *dxt_decode(uint32_t fmt, const uint8_t *src, int w, int h, int d)
{
    int bw = (w + 3) / 4, bh = (h + 3) / 4, pw = bw * 4, ph = bh * 4, bs = dxt_bytes(fmt);
    uint8_t *tmp = malloc((size_t)pw * ph * 4);
    uint8_t *out = malloc((size_t)w * h * 4 * (size_t)(d > 0 ? d : 1));
    for (int z = 0; z < (d > 0 ? d : 1); z++) {
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                const uint8_t *b = src + ((size_t)z * bw * bh + (size_t)by * bw + bx) * bs;
                uint8_t *dst = tmp + ((size_t)by * 4 * pw + (size_t)bx * 4) * 4;
                if (bs == 8) bcdec_bc1(b, dst, pw * 4);
                else if (fmt == 0x83F2 || fmt == 0x8C4E) bcdec_bc2(b, dst, pw * 4);
                else bcdec_bc3(b, dst, pw * 4);
            }
        for (int y = 0; y < h; y++)
            memcpy(out + ((size_t)z * h + y) * w * 4, tmp + (size_t)y * pw * 4, (size_t)w * 4);
    }
    free(tmp);
    return out;
}
static void es_glCompressedTexSubImage3D(CPU *c)
{
    if (!w64_gles || has_s3tc() || !is_dxt(AU(8))) DESKTOP("glCompressedTexSubImage3D");
    HOST(p, "glTexSubImage3D", void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *));
    uint32_t tgt = AU(0), fmt = AU(8);
    int32_t lvl = AI(1), x = AI(2), y = AI(3), z = AI(4), w = AI(5), h = AI(6), d = AI(7), size = AI(9);
    uint32_t pbo;
    const uint8_t *src = map_src(AP(10), (size_t)size, &pbo);
    if (!src) { w64_log("[gl-es] compressed upload: cannot read source"); rebind_pbo(pbo); RETV(); }
    uint8_t *rgba = dxt_decode(fmt, src, w, h, d);
    unmap_src(pbo);
    p(tgt, lvl, x, y, z, w, h, d, 0x1908, 0x1401, rgba);
    rebind_pbo(pbo);
    free(rgba);
    RETV();
}
static void es_glCompressedTexSubImage2D(CPU *c)
{
    if (!w64_gles || has_s3tc() || !is_dxt(AU(6))) DESKTOP("glCompressedTexSubImage2D");
    HOST(p, "glTexSubImage2D", void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *));
    uint32_t tgt = AU(0), fmt = AU(6);
    int32_t lvl = AI(1), x = AI(2), y = AI(3), w = AI(4), h = AI(5), size = AI(7);
    uint32_t pbo;
    const uint8_t *src = map_src(AP(8), (size_t)size, &pbo);
    if (!src) { rebind_pbo(pbo); RETV(); }
    uint8_t *rgba = dxt_decode(fmt, src, w, h, 1);
    unmap_src(pbo);
    p(tgt, lvl, x, y, w, h, 0x1908, 0x1401, rgba);
    rebind_pbo(pbo);
    free(rgba);
    RETV();
}
static void es_glCompressedTexImage2D(CPU *c)
{
    if (!w64_gles || has_s3tc() || !is_dxt(AU(2))) DESKTOP("glCompressedTexImage2D");
    HOST(p, "glTexImage2D", void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *));
    uint32_t tgt = AU(0), fmt = AU(2);
    int32_t lvl = AI(1), w = AI(3), h = AI(4), size = AI(6);
    uint32_t pbo;
    const uint8_t *src = AP(7) || pbo_bound() ? map_src(AP(7), (size_t)size, &pbo) : NULL;
    uint8_t *rgba = src ? dxt_decode(fmt, src, w, h, 1) : NULL;
    if (src) unmap_src(pbo);
    p(tgt, lvl, 0x8058, w, h, 0, 0x1908, 0x1401, rgba);
    if (src) rebind_pbo(pbo);
    free(rgba);
    RETV();
}
static void es_glCompressedTexImage3D(CPU *c)
{
    if (!w64_gles || has_s3tc() || !is_dxt(AU(2))) DESKTOP("glCompressedTexImage3D");
    HOST(p, "glTexImage3D", void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *));
    uint32_t tgt = AU(0), fmt = AU(2);
    int32_t lvl = AI(1), w = AI(3), h = AI(4), d = AI(5), size = AI(7);
    uint32_t pbo;
    const uint8_t *src = AP(8) || pbo_bound() ? map_src(AP(8), (size_t)size, &pbo) : NULL;
    uint8_t *rgba = src ? dxt_decode(fmt, src, w, h, d) : NULL;
    if (src) unmap_src(pbo);
    p(tgt, lvl, 0x8058, w, h, d, 0, 0x1908, 0x1401, rgba);
    if (src) rebind_pbo(pbo);
    free(rgba);
    RETV();
}
static void es_glTexStorage3D(CPU *c)
{
    if (!w64_gles || has_s3tc() || !is_dxt(AU(2))) DESKTOP("glTexStorage3D");
    HOST(p, "glTexStorage3D", void (*)(uint32_t, int32_t, uint32_t, int32_t, int32_t, int32_t));
    w64_log("[gl-es] glTexStorage3D DXT %#x %dx%dx%d -> RGBA8", AU(2), AI(3), AI(4), AI(5));
    p(AU(0), AI(1), 0x8058, AI(3), AI(4), AI(5));
    RETV();
}
static void es_glTexStorage2D(CPU *c)
{
    if (!w64_gles || has_s3tc() || !is_dxt(AU(2))) DESKTOP("glTexStorage2D");
    HOST(p, "glTexStorage2D", void (*)(uint32_t, int32_t, uint32_t, int32_t, int32_t));
    p(AU(0), AI(1), 0x8058, AI(3), AI(4));
    RETV();
}

/* ---------------------------------------------------------------- blits */
/* ES rejects depth/stencil bits in a blit whose buffers differ in format (the window surface
 * usually has none) where desktop GL ignores them; log the rest once. */
static void es_glBlitFramebuffer(CPU *c)
{
    if (!w64_gles) DESKTOP("glBlitFramebuffer");
    HOST(p, "glBlitFramebuffer", void (*)(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t));
    HOST(gi, "glGetIntegerv", void (*)(uint32_t, int32_t *));
    HOST(ge, "glGetError", uint32_t (*)(void));
    uint32_t mask = AU(8), filter = AU(9);
    int32_t drawfb = 0, readfb = 0;
    gi(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &drawfb);
    gi(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &readfb);
    if (drawfb == 0 || readfb == 0) mask &= 0x4000;          /* default framebuffer: colour only */
    if (filter == 0x2601 && (mask & 0x500)) filter = 0x2600; /* LINEAR not allowed for depth/stencil */
    ge();
    p(AI(0), AI(1), AI(2), AI(3), AI(4), AI(5), AI(6), AI(7), mask, filter);
    uint32_t e = ge();
    if (e == 0x502 && mask == 0x4000) {
        /* multisampled source: ES resolves only between identical rectangles. Resolve into a
         * single-sample colour buffer first, then copy with offset/scale from there. */
        HOST(gf, "glGenFramebuffers", void (*)(int32_t, uint32_t *));
        HOST(gr, "glGenRenderbuffers", void (*)(int32_t, uint32_t *));
        HOST(br, "glBindRenderbuffer", void (*)(uint32_t, uint32_t));
        HOST(rs, "glRenderbufferStorage", void (*)(uint32_t, uint32_t, int32_t, int32_t));
        HOST(bf, "glBindFramebuffer", void (*)(uint32_t, uint32_t));
        HOST(fr, "glFramebufferRenderbuffer", void (*)(uint32_t, uint32_t, uint32_t, uint32_t));
        static uint32_t tfb, trb;
        static int32_t tw, th;
        int32_t x1 = AI(0) > AI(2) ? AI(0) : AI(2), y1 = AI(1) > AI(3) ? AI(1) : AI(3);
        if (!tfb) { gf(1, &tfb); gr(1, &trb); }
        if (x1 > tw || y1 > th) {
            tw = x1 > tw ? x1 : tw; th = y1 > th ? y1 : th;
            br(0x8D41, trb);
            rs(0x8D41, 0x8058 /* RGBA8 */, tw, th);
            bf(0x8CA9, tfb);
            fr(0x8CA9, 0x8CE0, 0x8D41, trb);
        }
        bf(0x8CA9 /* DRAW_FRAMEBUFFER */, tfb);
        p(AI(0), AI(1), AI(2), AI(3), AI(0), AI(1), AI(2), AI(3), 0x4000, 0x2600);
        bf(0x8CA8 /* READ_FRAMEBUFFER */, tfb);
        bf(0x8CA9, (uint32_t)drawfb);
        p(AI(0), AI(1), AI(2), AI(3), AI(4), AI(5), AI(6), AI(7), 0x4000, filter);
        bf(0x8CA8, (uint32_t)readfb);
        e = ge();
    }
    static int logged;
    if (e && logged++ < 5)
        w64_log("[gl-es] glBlitFramebuffer read %d draw %d src %d,%d-%d,%d dst %d,%d-%d,%d mask %#x filter %#x -> %#x",
                readfb, drawfb, AI(0), AI(1), AI(2), AI(3), AI(4), AI(5), AI(6), AI(7), mask, filter, e);
    RETV();
}

/* ---------------------------------------------------------------- textures */
static void swap_rb(uint8_t *p, size_t npx) { for (size_t i = 0; i < npx; i++) { uint8_t t = p[4 * i]; p[4 * i] = p[4 * i + 2]; p[4 * i + 2] = t; } }

/* internalformat/format/type -> ES-valid; returns 1 if BGRA data must be swizzled */
static int es_tex_fix(int32_t *ifmt, uint32_t *fmt, uint32_t *type)
{
    int sw = 0;
    if (is_dxt((uint32_t)*ifmt) && !has_s3tc()) { *ifmt = 0x8058; *fmt = 0x1908; *type = 0x1401; }
    if (*fmt == 0x80E1 /* BGRA */) { *fmt = 0x1908; sw = 1; }
    if (*type == 0x8367 /* UNSIGNED_INT_8_8_8_8_REV */) *type = 0x1401;
    switch (*ifmt) {
    case 0x1903: /* RED */ *ifmt = *type == 0x1406 ? 0x822E /* R32F */ : 0x8229; /* R8 */ break;
    case 0x8227: /* RG */ *ifmt = *type == 0x1406 ? 0x8230 : 0x822B; break;
    case 0x1907: /* RGB */ if (*type == 0x1406) *ifmt = 0x8815; /* RGB32F */ break;
    case 0x1908: /* RGBA */ if (*type == 0x1406) *ifmt = 0x8814; /* RGBA32F */ else if (*type == 0x140B) *ifmt = 0x881A; break;
    case 0x1902: /* DEPTH_COMPONENT */ *ifmt = *type == 0x1406 ? 0x8CAC /* DEPTH32F */ : 0x81A6; /* DEPTH24 */
                 if (*type == 0x1401) *type = 0x1405; break;
    case 0x81A5: /* DEPTH_COMPONENT16 */ if (*type == 0x1406) *ifmt = 0x8CAC; break;
    case 0x81A7: /* DEPTH_COMPONENT32 */ *ifmt = 0x8CAC; *type = 0x1406; break;
    case 0x84F9: /* DEPTH_STENCIL */ *ifmt = 0x88F0; /* DEPTH24_STENCIL8 */ *type = 0x84FA; break;
    case 0x8058: /* RGBA8 */ if (*type != 0x1401) *type = 0x1401; break;
    case 0x8051: /* RGB8 */ break;
    case 0x8C41: /* SRGB8 */ case 0x8C43: /* SRGB8_ALPHA8 */ break;
    }
    if (*ifmt == 0x88F0) { *fmt = 0x84F9; *type = 0x84FA; }
    if (*ifmt == 0x81A6 || *ifmt == 0x81A5 || *ifmt == 0x8CAC) *fmt = 0x1902;
    return sw;
}
static void log_tex_once(const char *fn, int32_t ifmt, uint32_t fmt, uint32_t type)
{
    static uint64_t seen[64];
    static int n;
    uint64_t k = ((uint64_t)(uint32_t)ifmt << 32) ^ ((uint64_t)fmt << 16) ^ type;
    for (int i = 0; i < n; i++) if (seen[i] == k) return;
    if (n < 64) seen[n++] = k;
    w64_log("[gl-es] %s internal %#x format %#x type %#x", fn, ifmt, fmt, type);
}
static void es_glTexImage2D(CPU *c)
{
    if (!w64_gles) DESKTOP("glTexImage2D");
    HOST(p, "glTexImage2D", void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *));
    uint32_t tgt = AU(0), fmt = AU(6), type = AU(7);
    int32_t lvl = AI(1), ifmt = AI(2), w = AI(3), h = AI(4), b = AI(5);
    const void *data = AP(8);
    log_tex_once("glTexImage2D", ifmt, fmt, type);
    int sw = es_tex_fix(&ifmt, &fmt, &type);
    if (sw && data && type == 0x1401 && !pbo_bound()) {
        size_t n = (size_t)w * (size_t)h;
        uint8_t *t = malloc(n * 4);
        memcpy(t, data, n * 4);
        swap_rb(t, n);
        p(tgt, lvl, ifmt, w, h, b, fmt, type, t);
        free(t);
    } else p(tgt, lvl, ifmt, w, h, b, fmt, type, data);
    RETV();
}
static void es_glTexSubImage2D(CPU *c)
{
    if (!w64_gles) DESKTOP("glTexSubImage2D");
    HOST(p, "glTexSubImage2D", void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *));
    uint32_t tgt = AU(0), fmt = AU(6), type = AU(7);
    int32_t lvl = AI(1), x = AI(2), y = AI(3), w = AI(4), h = AI(5), dummy = 0;
    const void *data = AP(8);
    int sw = es_tex_fix(&dummy, &fmt, &type);
    if (sw && data && type == 0x1401 && !pbo_bound()) {
        size_t n = (size_t)w * (size_t)h;
        uint8_t *t = malloc(n * 4);
        memcpy(t, data, n * 4);
        swap_rb(t, n);
        p(tgt, lvl, x, y, w, h, fmt, type, t);
        free(t);
    } else p(tgt, lvl, x, y, w, h, fmt, type, data);
    RETV();
}
static void es_glTexImage3D(CPU *c)
{
    if (!w64_gles) DESKTOP("glTexImage3D");
    HOST(p, "glTexImage3D", void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *));
    uint32_t tgt = AU(0), fmt = AU(7), type = AU(8);
    int32_t lvl = AI(1), ifmt = AI(2), w = AI(3), h = AI(4), d = AI(5), b = AI(6);
    const void *data = AP(9);
    log_tex_once("glTexImage3D", ifmt, fmt, type);
    if (getenv("W64_TEXLOG")) w64_log("[gl-es] glTexImage3D %dx%dx%d internal %#x", w, h, d, ifmt);
    int sw = es_tex_fix(&ifmt, &fmt, &type);
    if (sw && data && type == 0x1401 && !pbo_bound()) {
        size_t n = (size_t)w * (size_t)h * (size_t)d;
        uint8_t *t = malloc(n * 4);
        memcpy(t, data, n * 4);
        swap_rb(t, n);
        p(tgt, lvl, ifmt, w, h, d, b, fmt, type, t);
        free(t);
    } else p(tgt, lvl, ifmt, w, h, d, b, fmt, type, data);
    RETV();
}
static void es_glTexSubImage3D(CPU *c)
{
    if (!w64_gles) DESKTOP("glTexSubImage3D");
    HOST(p, "glTexSubImage3D", void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *));
    uint32_t tgt = AU(0), fmt = AU(8), type = AU(9);
    int32_t lvl = AI(1), x = AI(2), y = AI(3), z = AI(4), w = AI(5), h = AI(6), d = AI(7), dummy = 0;
    const void *data = AP(10);
    int sw = es_tex_fix(&dummy, &fmt, &type);
    if (sw && data && type == 0x1401 && !pbo_bound()) {
        size_t n = (size_t)w * (size_t)h * (size_t)d;
        uint8_t *t = malloc(n * 4);
        memcpy(t, data, n * 4);
        swap_rb(t, n);
        p(tgt, lvl, x, y, z, w, h, d, fmt, type, t);
        free(t);
    } else p(tgt, lvl, x, y, z, w, h, d, fmt, type, data);
    RETV();
}
static void es_glTexParameteri(CPU *c)
{
    if (!w64_gles) DESKTOP("glTexParameteri");
    HOST(p, "glTexParameteri", void (*)(uint32_t, uint32_t, int32_t));
    uint32_t pn = AU(1);
    int32_t v = AI(2);
    if (v == 0x2900 /* GL_CLAMP */) v = 0x812F;
    if (pn == 0x8501 /* TEXTURE_LOD_BIAS */) RETV();
    p(AU(0), pn, v);
    RETV();
}

/* W64_GLSTAT=1: glCopyTexSubImage2D calls and copied pixels per frame (5 s averages) */
uint64_t w64_frame_count(void);
static void es_glCopyTexSubImage2D(CPU *c)
{
    HOST(p, "glCopyTexSubImage2D", void (*)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t));
    static int stat = -1;
    if (stat < 0) stat = getenv("W64_GLSTAT") != NULL;
    int32_t w = AI(6), h = AI(7);
    if (stat) {
        static uint64_t n, px, f0, maxw, maxh;
        uint64_t f = w64_frame_count();
        if (!f0) f0 = f;
        n++; px += (uint64_t)w * (uint64_t)h;
        if ((uint64_t)w * h > maxw * maxh) { maxw = (uint64_t)w; maxh = (uint64_t)h; }
        if (f - f0 >= 300) {
            w64_log("[glstat] glCopyTexSubImage2D %.1f calls/frame, %.2f Mpx/frame, largest %llux%llu",
                    (double)n / (double)(f - f0), (double)px / 1e6 / (double)(f - f0), (unsigned long long)maxw, (unsigned long long)maxh);
            n = px = maxw = maxh = 0; f0 = f;
        }
    }
    p(AU(0), AI(1), AI(2), AI(3), AI(4), AI(5), w, h);
    RETV();
}

static const w64_export_t es_exports[] = {
    { "OPENGL32.dll", "glCopyTexSubImage2D", es_glCopyTexSubImage2D },
    { "OPENGL32.dll", "glCreateShader", es_glCreateShader },
    { "OPENGL32.dll", "glPolygonMode", es_glPolygonMode },
    { "OPENGL32.dll", "glPointSize", es_glPointSize },
    { "OPENGL32.dll", "glBindFragDataLocation", es_glBindFragDataLocation },
    { "OPENGL32.dll", "glClearDepth", es_glClearDepth },
    { "OPENGL32.dll", "glDepthRange", es_glDepthRange },
    { "OPENGL32.dll", "glDrawBuffer", es_glDrawBuffer },
    { "OPENGL32.dll", "glMapBuffer", es_glMapBuffer },
    { "OPENGL32.dll", "glEnable", es_glEnable },
    { "OPENGL32.dll", "glDisable", es_glDisable },
    { "OPENGL32.dll", "glTexImage2D", es_glTexImage2D },
    { "OPENGL32.dll", "glTexSubImage2D", es_glTexSubImage2D },
    { "OPENGL32.dll", "glTexImage3D", es_glTexImage3D },
    { "OPENGL32.dll", "glTexSubImage3D", es_glTexSubImage3D },
    { "OPENGL32.dll", "glTexParameteri", es_glTexParameteri },
    { "OPENGL32.dll", "glCompressedTexSubImage3D", es_glCompressedTexSubImage3D },
    { "OPENGL32.dll", "glCompressedTexSubImage2D", es_glCompressedTexSubImage2D },
    { "OPENGL32.dll", "glCompressedTexImage2D", es_glCompressedTexImage2D },
    { "OPENGL32.dll", "glCompressedTexImage3D", es_glCompressedTexImage3D },
    { "OPENGL32.dll", "glTexStorage3D", es_glTexStorage3D },
    { "OPENGL32.dll", "glTexStorage2D", es_glTexStorage2D },
    { "OPENGL32.dll", "glBlitFramebuffer", es_glBlitFramebuffer },
    { 0 }
};

/* registered before gl_bridge's import table so these win in w64_find */
__attribute__((constructor(101))) static void gl_es_init(void) { w64_register(es_exports); }
