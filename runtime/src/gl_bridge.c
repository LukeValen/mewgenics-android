/*
 * gl_bridge.c - OpenGL boundary: opengl32 imports, wglGetProcAddress, the few
 * GL entry points that need more than argument marshaling, and frame capture.
 *
 * The game renders with desktop GL 3.2 core (GLSL 150) through GLEW. On a
 * desktop host the calls go straight to the host driver. On OpenGL ES hosts
 * (Android) gl_es.c rewrites shaders and the desktop-only calls.
 */
#define _GNU_SOURCE
#include "sdl_bridge.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { const char *name; w64_fn fn; } gl_thunk_t;
extern const gl_thunk_t gl_thunks[];
extern const unsigned gl_thunk_count;

const char *w64_current_hostfn(void);
void *gl_host_proc(const char *name)
{
    void *p = (void *)SDL_GL_GetProcAddress(name);
    return p;
}

static w64_fn gl_thunk(const char *name)
{
    int lo = 0, hi = (int)gl_thunk_count - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2, r = strcmp(name, gl_thunks[m].name);
        if (!r) return gl_thunks[m].fn;
        if (r < 0) hi = m - 1; else lo = m + 1;
    }
    return NULL;
}

uint64_t w64_gl_proc(const char *name)
{
    uint64_t r = w64_hostfn_named("OPENGL32.dll", name);     /* hand-written first */
    if (r) return r;
    w64_fn f = gl_thunk(name);
    if (!f || !gl_host_proc(name)) {
        if (w64_trace >= 2) w64_log("[gl] no entry point %s", name);
        return 0;
    }
    return w64_hostfn(name, f);
}

int w64_glcheck;
void gl_check(const char *n)
{
    static uint32_t (*ge)(void);
    static const char *seen[256];
    static int ns;
    if (!ge) ge = (uint32_t (*)(void))gl_host_proc("glGetError");
    uint32_t e = ge();
    if (!e) return;
    for (int i = 0; i < ns; i++) if (seen[i] == n) return;
    if (ns < 256) seen[ns++] = n;
    w64_log("[gl-err] %s -> %#x", n, e);
}

static void wglGetProcAddress_(CPU *c) { RET(w64_gl_proc(AS(0))); }
static void wglGetCurrentDC_(CPU *c) { RET(0x1234); }

/* ------------------------------------------------------------------ strings */
typedef const uint8_t *(*PFN_glGetString)(uint32_t);
typedef const uint8_t *(*PFN_glGetStringi)(uint32_t, uint32_t);
static void glGetString_(CPU *c)
{
    static PFN_glGetString p;
    if (!p) p = (PFN_glGetString)gl_host_proc("glGetString");
    uint32_t name = AU(0);
    if (w64_gles) {
        if (name == 0x1F02) RET((uintptr_t)"3.3.0 (W64 on OpenGL ES)");          /* GL_VERSION */
        if (name == 0x8B8C) RET((uintptr_t)"3.30");                              /* GL_SHADING_LANGUAGE_VERSION */
    }
    const uint8_t *s = p(name);
    if (w64_trace) w64_log("[gl] glGetString(%#x) = %s", name, s ? (const char *)s : "(null)");
    RET((uintptr_t)s);
}
/* Extensions the guest must not see: W64_HIDE_EXT (comma list) plus, on ES hosts, nothing
 * extra - an ES driver reports ES extension names the desktop game does not look for. */
static int ext_hidden(const char *e)
{
    const char *h = getenv("W64_HIDE_EXT");
    if (!h || !e) return 0;
    size_t n = strlen(e);
    for (const char *p = h; (p = strstr(p, e)); p += n)
        if ((p == h || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) return 1;
    return 0;
}
static void glGetStringi_(CPU *c)
{
    static PFN_glGetStringi p;
    if (!p) p = (PFN_glGetStringi)gl_host_proc("glGetStringi");
    const uint8_t *s = p(AU(0), AU(1));
    if (AU(0) == 0x1F03 /* GL_EXTENSIONS */ && ext_hidden((const char *)s)) {
        if (w64_trace) w64_log("[gl] hiding %s", s);
        s = (const uint8_t *)"GL_W64_hidden_extension";
    }
    RET((uintptr_t)s);
}

/* ------------------------------------------------------------------ shaders */
typedef void (*PFN_glShaderSource)(uint32_t, int32_t, const char *const *, const int32_t *);
char *w64_gl_translate_shader(const char *src, uint32_t shader);   /* gl_es.c; NULL = unchanged */
static void glShaderSource_(CPU *c)
{
    static PFN_glShaderSource p;
    if (!p) p = (PFN_glShaderSource)gl_host_proc("glShaderSource");
    uint32_t sh = AU(0);
    int32_t n = AI(1);
    const char *const *strs = AP(2);
    const int32_t *lens = AP(3);
    size_t tot = 1;
    for (int i = 0; i < n; i++) tot += lens && lens[i] >= 0 ? (size_t)lens[i] : strlen(strs[i]);
    char *all = malloc(tot), *q = all;
    for (int i = 0; i < n; i++) {
        size_t l = lens && lens[i] >= 0 ? (size_t)lens[i] : strlen(strs[i]);
        memcpy(q, strs[i], l); q += l;
    }
    *q = 0;
    static int dumped;
    const char *dd = getenv("W64_SHADER_DUMP");
    if (dd && dumped < 400) {
        char fn[1024];
        snprintf(fn, sizeof fn, "%s/shader_%03d_%u.glsl", dd, dumped++, sh);
        FILE *f = fopen(fn, "w"); if (f) { fputs(all, f); fclose(f); }
    }
    char *t = w64_gles ? w64_gl_translate_shader(all, sh) : NULL;
    const char *src = t ? t : all;
    p(sh, 1, &src, NULL);
    free(t);
    free(all);
    RETV();
}

/* ------------------------------------------------------------------ debug callback */
typedef void (*GLDEBUGPROC_h)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t, const char *, const void *);
typedef void (*PFN_glDebugMessageCallback)(GLDEBUGPROC_h, const void *);
static uint64_t g_dbg_fn, g_dbg_ud;
static void dbg_tramp(uint32_t src, uint32_t type, uint32_t id, uint32_t sev, int32_t len, const char *msg, const void *ud)
{
    (void)ud;
    if (w64_trace) w64_log("[gl-debug] %s", msg);
    if (g_dbg_fn) {
        uint64_t a[7] = { src, type, id, sev, (uint64_t)(uint32_t)len, (uint64_t)(uintptr_t)msg, g_dbg_ud };
        w64_callv(w64_cpu(), g_dbg_fn, 7, a, 0);
    }
}
static void glDebugMessageCallback_(CPU *c)
{
    const char *nm = w64_current_hostfn();
    PFN_glDebugMessageCallback p = (PFN_glDebugMessageCallback)gl_host_proc(nm);
    g_dbg_fn = A(0); g_dbg_ud = A(1);
    if (p) p(g_dbg_fn ? dbg_tramp : NULL, NULL);
    RETV();
}

/* ------------------------------------------------------------------ frame capture + swap */
typedef void (*PFN_glReadPixels)(int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void *);
typedef void (*PFN_glGetIntegerv)(uint32_t, int32_t *);
typedef void (*PFN_glBindFramebuffer)(uint32_t, uint32_t);
static uint64_t g_frames, g_last_shot_ns, g_t0;
uint64_t w64_frame_count(void) { return g_frames; }

static void capture(SDL_Window *w, const char *path)
{
    static PFN_glReadPixels rp;
    static PFN_glGetIntegerv gi;
    static PFN_glBindFramebuffer bf;
    if (!rp) { rp = (PFN_glReadPixels)gl_host_proc("glReadPixels"); gi = (PFN_glGetIntegerv)gl_host_proc("glGetIntegerv"); bf = (PFN_glBindFramebuffer)gl_host_proc("glBindFramebuffer"); }
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(w, &pw, &ph);
    if (pw <= 0 || ph <= 0) return;
    int32_t fbo = 0, pack = 0;
    gi(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &fbo);
    gi(0x88ED /* GL_PIXEL_PACK_BUFFER_BINDING */, &pack);
    bf(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
    uint8_t *px = malloc((size_t)pw * ph * 4);
    rp(0, 0, pw, ph, 0x1908 /* GL_RGBA */, 0x1401, px);
    bf(0x8CA8, (uint32_t)fbo);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", pw, ph);
        uint8_t *row = malloc((size_t)pw * 3);
        for (int y = ph - 1; y >= 0; y--) {
            for (int x = 0; x < pw; x++) memcpy(row + 3 * x, px + 4 * ((size_t)y * pw + x), 3);
            fwrite(row, 3, (size_t)pw, f);
        }
        free(row);
        fclose(f);
    }
    free(px);
}

void w64_input_tick(SDL_Window *w, uint64_t frame, double secs);   /* host input script */
static void SDL_GL_SwapWindow_(CPU *c)
{
    SDL_Window *w = AP(0);
    uint64_t now = w64_now_ns();
    if (!g_t0) g_t0 = now;
    g_frames++;
    const char *dir = getenv("W64_SHOT_DIR");
    if (dir) {
        double every = getenv("W64_SHOT_EVERY") ? atof(getenv("W64_SHOT_EVERY")) : 5.0;
        if (!g_last_shot_ns || (double)(now - g_last_shot_ns) / 1e9 >= every) {
            char p[1024];
            snprintf(p, sizeof p, "%s/f%06llu_%05.1fs.ppm", dir, (unsigned long long)g_frames, (double)(now - g_t0) / 1e9);
            mkdir(dir, 0777);
            capture(w, p);
            g_last_shot_ns = now;
            w64_log("[shot] %s", p);
        }
    }
    /* on-demand capture: create $W64_SHOT_DIR/req, get $W64_SHOT_DIR/req.ppm */
    if (dir && (g_frames % 10) == 0) {
        char rq[1024], out[1024];
        snprintf(rq, sizeof rq, "%s/req", dir);
        if (access(rq, F_OK) == 0) {
            snprintf(out, sizeof out, "%s/req.ppm.tmp", dir);
            capture(w, out);
            char fin[1024];
            snprintf(fin, sizeof fin, "%s/req.ppm", dir);
            rename(out, fin);
            unlink(rq);
        }
    }
    static uint64_t fps_t, fps_n;
    fps_n++;
    if (!fps_t) fps_t = now;
    if (now - fps_t >= 5000000000ull) { w64_log("[fps] %.1f (frame %llu)", fps_n * 1e9 / (double)(now - fps_t), (unsigned long long)g_frames); fps_t = now; fps_n = 0; }
    { void w64_slotstat_report(uint64_t); static uint64_t f0; if (g_frames - f0 >= 300) { w64_slotstat_report(g_frames - f0); f0 = g_frames; } }
    { void w64_apply_render_size(SDL_Window *); if ((g_frames % 60) == 1) w64_apply_render_size(w); }
    w64_input_tick(w, g_frames, (double)(now - g_t0) / 1e9);
    RET(SDL_GL_SwapWindow(w));
}

/* ------------------------------------------------------------------ opengl32 imports */
static const char *g_gl11[] = {
    "glDisable", "glCopyTexSubImage2D", "glGetIntegerv", "glStencilMask", "glClearColor", "glClear", "glLineWidth",
    "glPointSize", "glTexParameteri", "glBindTexture", "glDeleteTextures", "glEnable", "glPixelStorei", "glTexImage2D",
    "glViewport", "glBlendFunc", "glDrawArrays", "glPolygonMode", "glColorMask", "glDepthMask", "glStencilFunc",
    "glStencilOp", "glScissor", "glGenTextures", NULL
};
static w64_export_t g_gl_exports[64];

static const w64_export_t gl_manual[] = {
    { "OPENGL32.dll", "wglGetProcAddress", wglGetProcAddress_ },
    { "OPENGL32.dll", "wglGetCurrentDC", wglGetCurrentDC_ },
    { "OPENGL32.dll", "glGetString", glGetString_ },
    { "OPENGL32.dll", "glGetStringi", glGetStringi_ },
    { "OPENGL32.dll", "glShaderSource", glShaderSource_ },
    { "OPENGL32.dll", "glDebugMessageCallback", glDebugMessageCallback_ },
    { "OPENGL32.dll", "glDebugMessageCallbackARB", glDebugMessageCallback_ },
    { "OPENGL32.dll", "glDebugMessageCallbackKHR", glDebugMessageCallback_ },
    { "w64sdl.dll", "SDL_GL_SwapWindow", SDL_GL_SwapWindow_ },
    { 0 }
};



__attribute__((constructor)) static void gl_bridge_init(void)
{
    w64_register(gl_manual);
    int k = 0;
    for (int i = 0; g_gl11[i]; i++) {
        w64_fn f = gl_thunk(g_gl11[i]);
        if (f) { g_gl_exports[k].dll = "OPENGL32.dll"; g_gl_exports[k].name = g_gl11[i]; g_gl_exports[k].fn = f; k++; }
    }
    g_gl_exports[k].name = NULL;
    w64_register(g_gl_exports);
}

void w64_gl_on_context(void)
{
    static int once;
    if (once++) return;
    PFN_glGetString p = (PFN_glGetString)gl_host_proc("glGetString");
    if (p) w64_log("[gl] context: %s | %s | %s | GLSL %s", p(0x1F00), p(0x1F01), p(0x1F02), p(0x8B8C));
}
