/*
 * sdl_bridge.c - the SDL3 boundary.
 *
 * Mewgenics links SDL 3.2.12 statically, with SDL's dynamic API compiled in:
 * every public SDL function is a stub that jumps through a table, and the
 * first call fills that table - from a library named by SDL3_DYNAMIC_API if it
 * exports SDL_DYNAPI_entry. The runtime sets SDL3_DYNAMIC_API=w64sdl.dll,
 * answers LoadLibraryA/GetProcAddress for it, and SDL_DYNAPI_entry below fills
 * the guest's table with host functions that call the host's real SDL3.
 * SDL's own Win32 code in the image therefore never runs.
 */
#define _GNU_SOURCE
#include "sdl_bridge.h"
#include <SDL3/SDL.h>
#include <sys/stat.h>
#include <math.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/native_window.h>
#endif

uint64_t w64_load_library(const char *name);
uint64_t w64_get_proc(uint64_t h, const char *name);

const char *w64_current_hostfn(void);
static void miss_generic(CPU *c);

static void SDL_DYNAPI_entry_(CPU *c)
{
    uint32_t ver = AU(0), size = AU(2);
    uint64_t tab = A(1);
    unsigned n = size / 8;
    w64_log("[sdl] SDL_DYNAPI_entry(api %u, table %#llx, %u slots; host has %u)", ver, (unsigned long long)tab, n, sdl_proc_count);
    if (n > sdl_proc_count) n = sdl_proc_count;
    int manual = 0, autos = 0, missing = 0;
    for (unsigned i = 0; i < n; i++) {
        const char *name = sdl_procs[i].name;
        w64_fn f = w64_find("w64sdl.dll", name);
        if (f) manual++;
        else if ((f = sdl_procs[i].fn)) autos++;
        else { f = miss_generic; missing++; }
        wr64(tab + 8 * (uint64_t)i, w64_hostfn(name, f));
    }
    w64_log("[sdl] jump table: %d manual, %d automatic, %d without thunk", manual, autos, missing);
    RET(0);
}

static void miss_generic(CPU *c)
{
    w64_fatal(c, "SDL function without a thunk: %s", w64_current_hostfn());
}

/* ------------------------------------------------------------------ app entry */
static void SDL_RunApp_(CPU *c)
{
    int argc = AI(0);
    uint64_t argv = A(1), mainf = A(2);
    static char *args[2];
    if (!argv) {
        args[0] = strdup("C:\\Game\\Mewgenics.exe");
        args[1] = NULL;
        argc = 1;
        argv = (uint64_t)(uintptr_t)args;
    }
    w64_log("[sdl] SDL_RunApp -> main %#llx (argc %d)", (unsigned long long)mainf, argc);
    uint64_t r = w64_call(c, mainf, 2, (uint64_t)argc, argv);
    w64_log("[sdl] main returned %d", (int)r);
    RET((uint32_t)r);
}

/* ------------------------------------------------------------------ audio callback */
typedef struct { uint64_t fn, ud; } gcb_t;
static void SDLCALL audio_cb(void *ud, SDL_AudioStream *s, int add, int total)
{
    gcb_t *g = ud;
    static uint64_t n;
    if (++n == 1 || n % 2000 == 0) w64_log("[audio] callback #%llu (add %d total %d)", (unsigned long long)n, add, total);
    w64_call(w64_cpu(), g->fn, 4, g->ud, (uint64_t)(uintptr_t)s, (uint64_t)(uint32_t)add, (uint64_t)(uint32_t)total);
}
static void SDL_OpenAudioDeviceStream_(CPU *c)
{
    SDL_AudioDeviceID dev = (SDL_AudioDeviceID)A(0);
    const SDL_AudioSpec *spec = AP(1);
    uint64_t fn = A(2), ud = A(3);
    if (spec) w64_log("[sdl] OpenAudioDeviceStream dev %u fmt %#x ch %d rate %d cb %#llx", dev, spec->format, spec->channels, spec->freq, (unsigned long long)fn);
    SDL_AudioStream *s;
    if (fn) {
        gcb_t *g = malloc(sizeof *g);
        g->fn = fn; g->ud = ud;
        s = SDL_OpenAudioDeviceStream(dev, spec, audio_cb, g);
    } else {
        s = SDL_OpenAudioDeviceStream(dev, spec, NULL, NULL);
    }
    if (!s) w64_log("[sdl] OpenAudioDeviceStream failed: %s", SDL_GetError());
    RET((uintptr_t)s);
}

/* Audio level probe: peak/RMS of what the game feeds its stream, logged every 10 s, so a
 * device run shows real (non-silent) audio without anyone listening. */
static void SDL_PutAudioStreamData_(CPU *c)
{
    SDL_AudioStream *s = AP(0);
    const void *buf = AP(1);
    int len = AI(2);
    static uint64_t last_ns, frames;
    static double sum2;
    static float peak;
    SDL_AudioSpec in;
    if (buf && len > 0 && SDL_GetAudioStreamFormat(s, &in, NULL) && in.format == SDL_AUDIO_F32) {
        const float *f = buf;
        int n = len / 4;
        for (int i = 0; i < n; i++) { float a = f[i] < 0 ? -f[i] : f[i]; if (a > peak) peak = a; sum2 += (double)f[i] * f[i]; }
        frames += (uint64_t)n;
    }
    uint64_t now = SDL_GetTicksNS();
    if (now - last_ns > 10000000000ull && frames) {
        w64_log("[audio] level: peak %.3f rms %.4f over %llu samples", peak, frames ? sqrt(sum2 / (double)frames) : 0.0, (unsigned long long)frames);
        last_ns = now; peak = 0; sum2 = 0; frames = 0;
    }
    RET(SDL_PutAudioStreamData(s, buf, len));
}

/* ------------------------------------------------------------------ shared objects */
static void SDL_LoadObject_(CPU *c) { RET(w64_load_library(AS(0))); }
static void SDL_LoadFunction_(CPU *c) { RET(w64_get_proc(A(0), AS(1))); }
static void SDL_UnloadObject_(CPU *c) { RETV(); }

/* ------------------------------------------------------------------ paths */
static void SDL_GetBasePath_(CPU *c)
{
    static char *p;
    if (!p) { size_t n = strlen(w64_gamedir) + 2; p = malloc(n); snprintf(p, n, "%s/", w64_gamedir); }
    RET((uintptr_t)p);
}
static void mkdir_p(const char *path)
{
    char t[1024];
    snprintf(t, sizeof t, "%s", path);
    for (char *q = t + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(t, 0777); *q = '/'; }
    mkdir(t, 0777);
}
static void SDL_GetPrefPath_(CPU *c)
{
    const char *org = A(0) ? AS(0) : "", *app = A(1) ? AS(1) : "";
    char p[1024];
    snprintf(p, sizeof p, "%s/pref/%s%s%s/", w64_userdir, org, *org ? "/" : "", app);
    mkdir_p(p);
    w64_log("[sdl] SDL_GetPrefPath(%s, %s) -> %s", org, app, p);
    RET((uintptr_t)SDL_strdup(p));
}
static void SDL_IOFromFile_(CPU *c)
{
    const char *f = AS(0), *mode = AS(1);
    char p[1024];
    w64_host_path(f, p, sizeof p);
    SDL_IOStream *io = SDL_IOFromFile(p, mode);
    if (w64_trace || !io) w64_log("[sdl] SDL_IOFromFile(%s, %s) -> %s%s", f, mode, io ? "ok" : "failed: ", io ? "" : SDL_GetError());
    RET((uintptr_t)io);
}

/* ------------------------------------------------------------------ window */
/* On Android the game window is always fullscreen (immersive, no system bars); the game's own
 * windowed/borderless settings would otherwise leave the status and navigation bars visible. */
static int force_fullscreen(void)
{
#ifdef __ANDROID__
    return 1;
#else
    return getenv("W64_FULLSCREEN") != NULL;
#endif
}
/* Reduced render size on Android (W64_RENDER_SIZE from MewActivity, which also fixed the
 * SurfaceView size so the compositor scales it up): SDL resets the buffer geometry to the
 * window size whenever it (re)creates the EGL surface, so it is set again here - after the
 * window is created and, checked every 60 frames, after a surface was recreated (resume). */
void w64_apply_render_size(SDL_Window *w)
{
#ifdef __ANDROID__
    static int rw = -1, rh;
    if (rw < 0) { const char *rs = getenv("W64_RENDER_SIZE"); rw = 0; if (rs && sscanf(rs, "%dx%d", &rw, &rh) != 2) rw = 0; }
    if (rw <= 0 || !w) return;
    ANativeWindow *nw = (ANativeWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
    if (!nw) return;
    /* ANativeWindow_getWidth reports the SurfaceView's fixed size even while SDL's (0, 0)
     * geometry makes EGL allocate display-sized buffers, so apply once per native window */
    static ANativeWindow *done;
    if (nw == done) return;
    done = nw;
    int cw = ANativeWindow_getWidth(nw), ch = ANativeWindow_getHeight(nw);
    int r = ANativeWindow_setBuffersGeometry(nw, rw, rh, 0);
    w64_log("[sdl] surface buffers %dx%d -> %dx%d (%d)", cw, ch, rw, rh, r);
#else
    (void)w;
#endif
}

static void SDL_CreateWindow_(CPU *c)
{
    SDL_WindowFlags fl = (SDL_WindowFlags)A(3);
    if (force_fullscreen()) fl |= SDL_WINDOW_FULLSCREEN;
    SDL_Window *w = SDL_CreateWindow(AS(0), AI(1), AI(2), fl);
    w64_log("[sdl] CreateWindow %dx%d flags %#llx -> %p", AI(1), AI(2), (unsigned long long)fl, (void *)w);
    w64_apply_render_size(w);
    RET((uintptr_t)w);
}
static void SDL_SetWindowFullscreen_(CPU *c)
{
    bool on = (bool)(uint8_t)A(1);
    if (force_fullscreen()) on = true;
    RET(SDL_SetWindowFullscreen(AP(0), on));
}

/* ------------------------------------------------------------------ display modes */
/* The game derives its fixed update rate from the display refresh rate. Xvfb reports 0 Hz
 * (update rate 0 -> no game updates at all) and phones run at 90/120 Hz (twice the CPU for the
 * lifted game logic), so the guest always sees 60 Hz. */
static __thread SDL_DisplayMode t_mode[2];
static const SDL_DisplayMode *fix_mode(const SDL_DisplayMode *m, int k)
{
    if (!m) return NULL;
    t_mode[k] = *m;
    /* Android with a reduced render surface (MewActivity sets W64_RENDER_SIZE): the display
     * "is" that size for the game, like the window it gets */
    static int rw = -1, rh;
    if (rw < 0) { const char *rs = getenv("W64_RENDER_SIZE"); rw = 0; if (rs && sscanf(rs, "%dx%d", &rw, &rh) != 2) rw = 0;
                  if (rw) w64_log("[sdl] render size %dx%d (display %dx%d)", rw, rh, m->w, m->h); }
    if (rw > 0) { t_mode[k].w = rw; t_mode[k].h = rh; t_mode[k].pixel_density = 1.0f; }
    if (t_mode[k].refresh_rate != 60.0f) {
        static int once;
        if (!once++) w64_log("[sdl] display refresh %.2f Hz reported to the game as 60 Hz", m->refresh_rate);
        t_mode[k].refresh_rate = 60.0f;
        t_mode[k].refresh_rate_numerator = 60;
        t_mode[k].refresh_rate_denominator = 1;
    }
    return &t_mode[k];
}
static void SDL_GetCurrentDisplayMode_(CPU *c) { RET((uintptr_t)fix_mode(SDL_GetCurrentDisplayMode((SDL_DisplayID)A(0)), 0)); }
static void SDL_GetDesktopDisplayMode_(CPU *c) { RET((uintptr_t)fix_mode(SDL_GetDesktopDisplayMode((SDL_DisplayID)A(0)), 1)); }

/* ------------------------------------------------------------------ events */
/* Touch (W64_TOUCH=1, always on Android): SDL turns the first finger into the left mouse
 * button. A second finger landing within TAP_MS of the first turns the gesture into a RIGHT
 * click instead (the left press is held back that long so it can be withdrawn). The Android
 * back key becomes Escape (the game's pause menu). */
#define TAP_MS 140
static int g_touch = -1;
/* Queue of synthesized events, each with a not-before time: a tap's press is held until the
 * pointer has hovered its target for a while, and its release comes a few frames after the
 * press, so the game sees hover -> press -> release in separate frames as with a mouse. */
#define HOVER_NS   0ull         /* no hover delay: first tap on a tile previews, second confirms (the game's own behaviour) */
#define PRESS_NS   (60ull * 1000000ull)
static SDL_Event g_q[16];
static uint64_t g_q_at[16];
static int g_qn;
static SDL_Event g_pending;
static int g_has_pending, g_right, g_fingers, g_swallow_left_up;
static uint64_t g_pending_ns, g_last_motion_ns, g_q_last_at;
static void q_push_at(const SDL_Event *e, uint64_t at)
{
    if (g_qn >= 16) return;
    if (at < g_q_last_at) at = g_q_last_at;          /* keep order */
    g_q[g_qn] = *e; g_q_at[g_qn] = at; g_qn++; g_q_last_at = at;
}
static void q_push(const SDL_Event *e) { q_push_at(e, 0); }
static int q_pop(SDL_Event *e)
{
    if (!g_qn || SDL_GetTicksNS() < g_q_at[0]) return 0;
    *e = g_q[0];
    memmove(g_q, g_q + 1, sizeof g_q[0] * (size_t)(g_qn - 1));
    memmove(g_q_at, g_q_at + 1, sizeof g_q_at[0] * (size_t)(g_qn - 1));
    g_qn--;
    if (!g_qn) g_q_last_at = 0;
    if ((e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) && e->button.which == SDL_TOUCH_MOUSEID) e->button.which = 1;
    return 1;
}

static int touch_filter_inner(SDL_Event *e);
static int touch_filter(SDL_Event *e)      /* 1 = deliver e, 0 = swallow */
{
    int r = touch_filter_inner(e);
    /* the game may skip mouse events tagged as touch-emulated: present them as a plain mouse */
    if (r && (e->type == SDL_EVENT_MOUSE_MOTION) && e->motion.which == SDL_TOUCH_MOUSEID) e->motion.which = 1;
    if (r && (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP) && e->button.which == SDL_TOUCH_MOUSEID) e->button.which = 1;
    return r;
}
static int touch_filter_inner(SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_FINGER_DOWN:
        g_fingers++;
        if (g_fingers == 2 && g_has_pending) {
            SDL_Event r = g_pending;
            r.button.button = SDL_BUTTON_RIGHT;
            g_has_pending = 0; g_right = 1; g_swallow_left_up = 1;
            w64_log("[touch] two-finger tap -> right button at %.0f,%.0f", r.button.x, r.button.y);
            q_push(&r);
        }
        return 1;
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        if (g_fingers > 0) g_fingers--;
        if (g_right && g_fingers == 0) {
            SDL_Event r;
            memset(&r, 0, sizeof r);
            r.type = SDL_EVENT_MOUSE_BUTTON_UP;
            r.button.timestamp = SDL_GetTicksNS();
            r.button.windowID = e->tfinger.windowID;
            r.button.which = SDL_TOUCH_MOUSEID;
            r.button.button = SDL_BUTTON_RIGHT;
            r.button.clicks = 1;
            SDL_GetMouseState(&r.button.x, &r.button.y);
            g_right = 0;
            w64_log("[touch] right button up");
            q_push(&r);
        }
        return 1;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.which == SDL_TOUCH_MOUSEID && e->button.button == SDL_BUTTON_LEFT) {
            if (g_right) return 0;
            g_pending = *e; g_has_pending = 1; g_pending_ns = SDL_GetTicksNS();
            return 0;
        }
        return 1;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.which == SDL_TOUCH_MOUSEID && e->button.button == SDL_BUTTON_LEFT) {
            if (g_swallow_left_up) { g_swallow_left_up = 0; return 0; }
            uint64_t now = SDL_GetTicksNS();
            if (g_has_pending) {
                uint64_t at = g_last_motion_ns + HOVER_NS > now ? g_last_motion_ns + HOVER_NS : now;
                q_push_at(&g_pending, at); g_has_pending = 0;
                q_push_at(e, at + PRESS_NS);
                return 0;
            }
            if (g_qn) { q_push_at(e, g_q_last_at + PRESS_NS); return 0; }   /* press still queued */
        }
        return 1;
    case SDL_EVENT_MOUSE_MOTION:
        if (e->motion.which == SDL_TOUCH_MOUSEID) {
            g_last_motion_ns = SDL_GetTicksNS();
            if (g_has_pending) {
                /* finger moving while its press is still held back: a drag. Release the press
                 * first (at the touch-down point) once it moved clearly away, so the game sees
                 * press -> motion -> release like a mouse drag; small jitter stays queued. */
                int ww = 1280, wh = 720;
                SDL_Window *w = SDL_GetWindowFromID(e->motion.windowID);
                if (w) SDL_GetWindowSize(w, &ww, &wh);
                float dx = e->motion.x - g_pending.button.x, dy = e->motion.y - g_pending.button.y;
                float lim = (float)ww * 0.02f;
                if (dx * dx + dy * dy > lim * lim) {
                    /* the press goes out now, the first drag motion one frame later: the game
                     * picks up what is under the pointer in the frame of the press */
                    uint64_t now = SDL_GetTicksNS();
                    q_push_at(&g_pending, now); g_has_pending = 0;
                    q_push_at(e, now + 4000000ull);
                    return 0;
                }
                return 0;                       /* jitter before the press: drop */
            }
        }
        if (g_qn) { q_push(e); return 0; }      /* keep motion behind queued presses */
        return 1;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (e->key.key == SDLK_AC_BACK) { e->key.key = SDLK_ESCAPE; e->key.scancode = SDL_SCANCODE_ESCAPE; }
        return 1;
    }
    return 1;
}

static uint64_t g_polls, g_events;
uint64_t w64_frame_count(void);

/* With touch, SDL's own mouse state runs ahead of the events the game has received (presses
 * are held back and reordered above). The game also polls SDL_GetMouseState, so it gets the
 * position and buttons of the mouse events it was actually given. */
static int g_mtrack;
static float g_mx, g_my;
static SDL_MouseButtonFlags g_mbtn;
static void track_mouse(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION: g_mx = e->motion.x; g_my = e->motion.y; g_mtrack = 1; break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: g_mx = e->button.x; g_my = e->button.y; g_mbtn |= SDL_BUTTON_MASK(e->button.button); g_mtrack = 1; break;
    case SDL_EVENT_MOUSE_BUTTON_UP: g_mx = e->button.x; g_my = e->button.y; g_mbtn &= ~SDL_BUTTON_MASK(e->button.button); g_mtrack = 1; break;
    }
}
static void SDL_GetMouseState_(CPU *c)
{
    float *x = AP(0), *y = AP(1);
    if (!g_touch || !g_mtrack) RET(SDL_GetMouseState(x, y));
    if (x) *x = g_mx;
    if (y) *y = g_my;
    RET(g_mbtn);
}
static void SDL_PollEvent_(CPU *c)
{
    SDL_Event *e = AP(0), tmp;
    if (!e) e = &tmp;
    if (g_touch < 0) {
#ifdef __ANDROID__
        g_touch = 1;
#else
        g_touch = getenv("W64_TOUCH") != NULL;
#endif
    }
    bool r = false;
    if (q_pop(e)) r = true;
    else if (g_has_pending && SDL_GetTicksNS() - g_pending_ns > (uint64_t)TAP_MS * 1000000ull) {
        *e = g_pending; g_has_pending = 0; r = true;      /* finger held: a press (drag) after TAP_MS */
        if (e->button.which == SDL_TOUCH_MOUSEID) e->button.which = 1;
    } else {
        while (SDL_PollEvent(e)) {
            if (!g_touch || touch_filter(e)) { r = true; break; }
            if (q_pop(e)) { r = true; break; }
        }
    }
    g_polls++;
    if (r) {
        g_events++;
        if (g_touch) track_mouse(e);
        if (w64_trace) w64_log("[ev] type %#x", e->type);
        if (e->type >= SDL_EVENT_TERMINATING && e->type <= SDL_EVENT_DID_ENTER_FOREGROUND) w64_log("[ev] app lifecycle event %#x", e->type);
        if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && e->button.button == SDL_BUTTON_RIGHT) w64_log("[ev] right button down at %.0f,%.0f", e->button.x, e->button.y);
        static int evlog = -1;
        if (evlog < 0) evlog = getenv("W64_EVLOG") != NULL;
        if (evlog && (e->type == SDL_EVENT_MOUSE_MOTION || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP))
            w64_log("[evlog] frame %llu t %.3f type %#x %.0f,%.0f btn %d",
                    (unsigned long long)w64_frame_count(), SDL_GetTicksNS() / 1e9, e->type,
                    e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.x : e->button.x,
                    e->type == SDL_EVENT_MOUSE_MOTION ? e->motion.y : e->button.y,
                    e->type == SDL_EVENT_MOUSE_MOTION ? 0 : e->button.button);
        if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_ESCAPE) w64_log("[ev] escape key");
    }
    if (g_polls == 1) w64_log("[ev] first SDL_PollEvent (touch mapping %s)", g_touch ? "on" : "off");
    RET(r);
}

/* ------------------------------------------------------------------ time */
/* Windows-shaped performance counter (10 MHz, small values): host counters in
 * nanoseconds since boot overflow `counter * 1000000 / freq` style code. */
uint64_t w64_qpc(void);
static void SDL_GetPerformanceCounter_(CPU *c) { RET(w64_qpc()); }
static void SDL_GetPerformanceFrequency_(CPU *c) { RET(10000000ull); }

/* ------------------------------------------------------------------ CPU / platform */
static void ret_true(CPU *c) { RET(1); }
static void ret_false(CPU *c) { RET(0); }
static void SDL_GetPlatform_(CPU *c) { RET((uintptr_t)"Windows"); }
static void SDL_GetCPUCacheLineSize_(CPU *c) { RET(64); }
static void SDL_GetSIMDAlignment_(CPU *c) { RET(16); }

/* ------------------------------------------------------------------ GL */
int w64_gles;
static void SDL_GL_SetAttribute_(CPU *c)
{
    SDL_GLAttr a = (SDL_GLAttr)A(0);
    int v = AI(1);
    if (w64_gles) {
        if (a == SDL_GL_CONTEXT_PROFILE_MASK) v = SDL_GL_CONTEXT_PROFILE_ES;
        else if (a == SDL_GL_CONTEXT_MAJOR_VERSION) v = 3;
        else if (a == SDL_GL_CONTEXT_MINOR_VERSION) v = 2;
        else if (a == SDL_GL_CONTEXT_FLAGS) v &= ~SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG;
    }
    w64_log("[sdl] GL_SetAttribute(%d, %d -> %d)", (int)a, AI(1), v);
    RET(SDL_GL_SetAttribute(a, v));
}
static void SDL_GL_CreateContext_(CPU *c)
{
    SDL_Window *w = AP(0);
    SDL_GLContext ctx = SDL_GL_CreateContext(w);
    if (!ctx && w64_gles) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        ctx = SDL_GL_CreateContext(w);
    }
    w64_log("[sdl] GL_CreateContext -> %p %s", (void *)ctx, ctx ? "" : SDL_GetError());
    if (ctx) w64_gl_on_context();
    RET((uintptr_t)ctx);
}
static void SDL_GL_MakeCurrent_(CPU *c)
{
    bool r = SDL_GL_MakeCurrent(AP(0), (SDL_GLContext)AP(1));
    if (r && A(1)) w64_gl_on_context();
    RET(r);
}
static void SDL_GL_GetProcAddress_(CPU *c) { RET(w64_gl_proc(AS(0))); }
static void SDL_EGL_GetProcAddress_(CPU *c) { RET(w64_gl_proc(AS(0))); }

/* ------------------------------------------------------------------ message boxes */
static void SDL_ShowSimpleMessageBox_(CPU *c)
{
    w64_log("[msgbox] %s: %s", A(1) ? AS(1) : "", A(2) ? AS(2) : "");
    RET(1);
}
static void SDL_ShowMessageBox_(CPU *c)
{
    const SDL_MessageBoxData *d = AP(0);
    int id = 0;
    w64_log("[msgbox] %s: %s", d->title ? d->title : "", d->message ? d->message : "");
    for (int i = 0; i < d->numbuttons; i++) {
        w64_log("[msgbox]   button %d '%s'%s", d->buttons[i].buttonID, d->buttons[i].text,
                (d->buttons[i].flags & SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT) ? " (default)" : "");
        if (i == 0 || (d->buttons[i].flags & SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT)) id = d->buttons[i].buttonID;
    }
    if (A(1)) wr32(A(1), (uint32_t)id);
    RET(1);
}

/* ------------------------------------------------------------------ varargs / va_list */
static void SDL_LogMessageV_(CPU *c)
{
    char buf[4096];
    w64_va v = { NULL, 0, A(3) };
    w64_vformat(buf, sizeof buf, AS(2), &v);
    w64_log("[sdl-log %d/%d] %s", AI(0), AI(1), buf);
    RETV();
}
static void SDL_LogMessage_(CPU *c)
{
    char buf[4096];
    w64_va v = { c, 3, 0 };
    w64_vformat(buf, sizeof buf, AS(2), &v);
    w64_log("[sdl-log %d/%d] %s", AI(0), AI(1), buf);
    RETV();
}
static void SDL_Log_(CPU *c)
{
    char buf[4096];
    w64_va v = { c, 1, 0 };
    w64_vformat(buf, sizeof buf, AS(0), &v);
    w64_log("[sdl-log] %s", buf);
    RETV();
}
static void SDL_vsnprintf_(CPU *c)
{
    w64_va v = { NULL, 0, A(3) };
    RET((uint32_t)w64_vformat((char *)AP(0), (size_t)A(1), AS(2), &v));
}
static void SDL_snprintf_(CPU *c)
{
    w64_va v = { c, 3, 0 };
    RET((uint32_t)w64_vformat((char *)AP(0), (size_t)A(1), AS(2), &v));
}
static void SDL_vasprintf_(CPU *c)
{
    w64_va v = { NULL, 0, A(2) }, v2 = v;
    int n = w64_vformat(NULL, 0, AS(1), &v);
    char *s = SDL_malloc((size_t)n + 1);
    w64_vformat(s, (size_t)n + 1, AS(1), &v2);
    wr64(A(0), (uint64_t)(uintptr_t)s);
    RET((uint32_t)n);
}
static void SDL_asprintf_(CPU *c)
{
    w64_va v = { c, 2, 0 }, v2 = v;
    int n = w64_vformat(NULL, 0, AS(1), &v);
    char *s = SDL_malloc((size_t)n + 1);
    w64_vformat(s, (size_t)n + 1, AS(1), &v2);
    wr64(A(0), (uint64_t)(uintptr_t)s);
    RET((uint32_t)n);
}
static void SDL_SetError_(CPU *c)
{
    char buf[1024];
    w64_va v = { c, 1, 0 };
    w64_vformat(buf, sizeof buf, AS(0), &v);
    SDL_SetError("%s", buf);
    RET(0);
}
static void SDL_SetErrorV_(CPU *c)
{
    char buf[1024];
    w64_va v = { NULL, 0, A(1) };
    w64_vformat(buf, sizeof buf, AS(0), &v);
    SDL_SetError("%s", buf);
    RET(0);
}

/* ------------------------------------------------------------------ long (32-bit on Win64) */
static void SDL_strtol_(CPU *c)
{
    long long v = SDL_strtoll(AS(0), (char **)AP(1), AI(2));
    if (v > 0x7FFFFFFFLL) v = 0x7FFFFFFFLL;
    if (v < -0x80000000LL) v = -0x80000000LL;
    RET((uint32_t)(int32_t)v);
}
static void SDL_strtoul_(CPU *c)
{
    unsigned long long v = SDL_strtoull(AS(0), (char **)AP(1), AI(2));
    if (v > 0xFFFFFFFFull) v = 0xFFFFFFFFull;
    RET((uint32_t)v);
}
static void SDL_ltoa_(CPU *c) { RET((uintptr_t)SDL_lltoa((long long)AI(0), (char *)AP(1), AI(2))); }
static void SDL_ultoa_(CPU *c) { RET((uintptr_t)SDL_ulltoa((unsigned long long)AU(0), (char *)AP(1), AI(2))); }

/* ------------------------------------------------------------------ wchar_t (16-bit on Win64) */
static size_t w_len(const uint16_t *s) { return w16len(s); }
static void SDL_wcslen_(CPU *c) { RET(w_len(AW(0))); }
static void SDL_wcsnlen_(CPU *c) { const uint16_t *s = AW(0); size_t m = (size_t)A(1), n = 0; while (n < m && s[n]) n++; RET(n); }
static void SDL_wcslcpy_(CPU *c)
{
    uint16_t *d = AP(0); const uint16_t *s = AW(1); size_t m = (size_t)A(2), n = w_len(s);
    if (m) { size_t k = n < m - 1 ? n : m - 1; memcpy(d, s, k * 2); d[k] = 0; }
    RET(n);
}
static void SDL_wcslcat_(CPU *c)
{
    uint16_t *d = AP(0); const uint16_t *s = AW(1); size_t m = (size_t)A(2), dl = w_len(d), sl = w_len(s);
    if (dl < m) { size_t k = sl < m - dl - 1 ? sl : m - dl - 1; memcpy(d + dl, s, k * 2); d[dl + k] = 0; }
    RET(dl + sl);
}
static void SDL_wcsdup_(CPU *c) { const uint16_t *s = AW(0); size_t n = w_len(s); uint16_t *d = SDL_malloc((n + 1) * 2); memcpy(d, s, (n + 1) * 2); RET((uintptr_t)d); }
static int w_cmp(const uint16_t *a, const uint16_t *b, size_t m, int ci)
{
    for (size_t i = 0; i < m; i++) {
        uint16_t x = a[i], y = b[i];
        if (ci) { if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; }
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}
static void SDL_wcscmp_(CPU *c) { RET((uint32_t)w_cmp(AW(0), AW(1), (size_t)-1, 0)); }
static void SDL_wcsncmp_(CPU *c) { RET((uint32_t)w_cmp(AW(0), AW(1), (size_t)A(2), 0)); }
static void SDL_wcscasecmp_(CPU *c) { RET((uint32_t)w_cmp(AW(0), AW(1), (size_t)-1, 1)); }
static void SDL_wcsncasecmp_(CPU *c) { RET((uint32_t)w_cmp(AW(0), AW(1), (size_t)A(2), 1)); }
static void SDL_wcsstr_(CPU *c)
{
    const uint16_t *h = AW(0), *nd = AW(1);
    size_t nl = w_len(nd);
    for (; *h; h++) if (!w_cmp(h, nd, nl, 0)) RET((uintptr_t)h);
    RET(nl ? 0 : A(0));
}

/* ------------------------------------------------------------------ qsort / bsearch with guest comparators */
static __thread uint64_t t_cmp_fn, t_cmp_ud;
static __thread int t_cmp_r;
static int SDLCALL cmp_tramp(const void *a, const void *b)
{
    return (int)(int32_t)w64_call(w64_cpu(), t_cmp_fn, 2, (uint64_t)(uintptr_t)a, (uint64_t)(uintptr_t)b);
}
static int SDLCALL cmp_tramp_r(void *ud, const void *a, const void *b)
{
    (void)ud;
    return (int)(int32_t)w64_call(w64_cpu(), t_cmp_fn, 3, t_cmp_ud, (uint64_t)(uintptr_t)a, (uint64_t)(uintptr_t)b);
}
static void SDL_qsort_(CPU *c)
{
    uint64_t sv = t_cmp_fn; t_cmp_fn = A(3);
    SDL_qsort(AP(0), (size_t)A(1), (size_t)A(2), cmp_tramp);
    t_cmp_fn = sv;
    RETV();
}
static void SDL_qsort_r_(CPU *c)
{
    uint64_t sv = t_cmp_fn, su = t_cmp_ud; t_cmp_fn = A(3); t_cmp_ud = A(4);
    SDL_qsort_r(AP(0), (size_t)A(1), (size_t)A(2), cmp_tramp_r, NULL);
    t_cmp_fn = sv; t_cmp_ud = su;
    RETV();
}
static void SDL_bsearch_(CPU *c)
{
    uint64_t sv = t_cmp_fn; t_cmp_fn = A(4);
    void *r = SDL_bsearch(AP(0), AP(1), (size_t)A(2), (size_t)A(3), cmp_tramp);
    t_cmp_fn = sv;
    RET((uintptr_t)r);
}

static const w64_export_t sdl_exports[] = {
    { "w64sdl.dll", "SDL_DYNAPI_entry", SDL_DYNAPI_entry_ },
    { "w64sdl.dll", "SDL_RunApp", SDL_RunApp_ },
    { "w64sdl.dll", "SDL_OpenAudioDeviceStream", SDL_OpenAudioDeviceStream_ },
    { "w64sdl.dll", "SDL_PutAudioStreamData", SDL_PutAudioStreamData_ },
    { "w64sdl.dll", "SDL_LoadObject", SDL_LoadObject_ },
    { "w64sdl.dll", "SDL_LoadFunction", SDL_LoadFunction_ },
    { "w64sdl.dll", "SDL_UnloadObject", SDL_UnloadObject_ },
    { "w64sdl.dll", "SDL_GetBasePath", SDL_GetBasePath_ },
    { "w64sdl.dll", "SDL_GetPrefPath", SDL_GetPrefPath_ },
    { "w64sdl.dll", "SDL_IOFromFile", SDL_IOFromFile_ },
    { "w64sdl.dll", "SDL_GetPlatform", SDL_GetPlatform_ },
    { "w64sdl.dll", "SDL_PollEvent", SDL_PollEvent_ },
    { "w64sdl.dll", "SDL_GetMouseState", SDL_GetMouseState_ },
    { "w64sdl.dll", "SDL_CreateWindow", SDL_CreateWindow_ },
    { "w64sdl.dll", "SDL_SetWindowFullscreen", SDL_SetWindowFullscreen_ },
    { "w64sdl.dll", "SDL_GetCurrentDisplayMode", SDL_GetCurrentDisplayMode_ },
    { "w64sdl.dll", "SDL_GetDesktopDisplayMode", SDL_GetDesktopDisplayMode_ },
    { "w64sdl.dll", "SDL_GetPerformanceCounter", SDL_GetPerformanceCounter_ },
    { "w64sdl.dll", "SDL_GetPerformanceFrequency", SDL_GetPerformanceFrequency_ },
    { "w64sdl.dll", "SDL_HasMMX", ret_true },
    { "w64sdl.dll", "SDL_HasSSE", ret_true },
    { "w64sdl.dll", "SDL_HasSSE2", ret_true },
    { "w64sdl.dll", "SDL_HasSSE3", ret_false },
    { "w64sdl.dll", "SDL_HasSSE41", ret_false },
    { "w64sdl.dll", "SDL_HasSSE42", ret_false },
    { "w64sdl.dll", "SDL_HasAVX", ret_false },
    { "w64sdl.dll", "SDL_HasAVX2", ret_false },
    { "w64sdl.dll", "SDL_HasAVX512F", ret_false },
    { "w64sdl.dll", "SDL_HasAltiVec", ret_false },
    { "w64sdl.dll", "SDL_HasARMSIMD", ret_false },
    { "w64sdl.dll", "SDL_HasNEON", ret_false },
    { "w64sdl.dll", "SDL_HasLSX", ret_false },
    { "w64sdl.dll", "SDL_HasLASX", ret_false },
    { "w64sdl.dll", "SDL_GetCPUCacheLineSize", SDL_GetCPUCacheLineSize_ },
    { "w64sdl.dll", "SDL_GetSIMDAlignment", SDL_GetSIMDAlignment_ },
    { "w64sdl.dll", "SDL_GL_SetAttribute", SDL_GL_SetAttribute_ },
    { "w64sdl.dll", "SDL_GL_CreateContext", SDL_GL_CreateContext_ },
    { "w64sdl.dll", "SDL_GL_MakeCurrent", SDL_GL_MakeCurrent_ },
    { "w64sdl.dll", "SDL_GL_GetProcAddress", SDL_GL_GetProcAddress_ },
    { "w64sdl.dll", "SDL_EGL_GetProcAddress", SDL_EGL_GetProcAddress_ },
    { "w64sdl.dll", "SDL_ShowSimpleMessageBox", SDL_ShowSimpleMessageBox_ },
    { "w64sdl.dll", "SDL_ShowMessageBox", SDL_ShowMessageBox_ },
    { "w64sdl.dll", "SDL_LogMessageV", SDL_LogMessageV_ },
    { "w64sdl.dll", "SDL_LogMessage", SDL_LogMessage_ },
    { "w64sdl.dll", "SDL_Log", SDL_Log_ },
    { "w64sdl.dll", "SDL_vsnprintf", SDL_vsnprintf_ },
    { "w64sdl.dll", "SDL_snprintf", SDL_snprintf_ },
    { "w64sdl.dll", "SDL_vasprintf", SDL_vasprintf_ },
    { "w64sdl.dll", "SDL_asprintf", SDL_asprintf_ },
    { "w64sdl.dll", "SDL_SetError", SDL_SetError_ },
    { "w64sdl.dll", "SDL_SetErrorV", SDL_SetErrorV_ },
    { "w64sdl.dll", "SDL_strtol", SDL_strtol_ },
    { "w64sdl.dll", "SDL_strtoul", SDL_strtoul_ },
    { "w64sdl.dll", "SDL_ltoa", SDL_ltoa_ },
    { "w64sdl.dll", "SDL_ultoa", SDL_ultoa_ },
    { "w64sdl.dll", "SDL_wcslen", SDL_wcslen_ },
    { "w64sdl.dll", "SDL_wcsnlen", SDL_wcsnlen_ },
    { "w64sdl.dll", "SDL_wcslcpy", SDL_wcslcpy_ },
    { "w64sdl.dll", "SDL_wcslcat", SDL_wcslcat_ },
    { "w64sdl.dll", "SDL_wcsdup", SDL_wcsdup_ },
    { "w64sdl.dll", "SDL_wcscmp", SDL_wcscmp_ },
    { "w64sdl.dll", "SDL_wcsncmp", SDL_wcsncmp_ },
    { "w64sdl.dll", "SDL_wcscasecmp", SDL_wcscasecmp_ },
    { "w64sdl.dll", "SDL_wcsncasecmp", SDL_wcsncasecmp_ },
    { "w64sdl.dll", "SDL_wcsstr", SDL_wcsstr_ },
    { "w64sdl.dll", "SDL_qsort", SDL_qsort_ },
    { "w64sdl.dll", "SDL_qsort_r", SDL_qsort_r_ },
    { "w64sdl.dll", "SDL_bsearch", SDL_bsearch_ },
    { 0 }
};

__attribute__((constructor)) static void sdl_bridge_init(void) { w64_register(sdl_exports); }
