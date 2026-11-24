/*
 * input_script.c - scripted input for unattended host/device runs.
 *
 * W64_INPUT="3:key:Return,6.5:click:640:360,9:move:100:200,12:key:Escape"
 * times in seconds since the first frame; click/move in window coordinates.
 */
#include "w64.h"
#include <SDL3/SDL.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

typedef struct { double t; int kind; char key[32]; float x, y; int done; } ev_t;
static ev_t g_ev[256];
static int g_nev = -1;

/* 1 key, 2 click, 3 move, 4 rclick, 5 tap, 6 tap2 (two-finger tap); held/drag primitives:
 * 7 down / 8 up (mouse left), 9 tdown / 10 tmove / 11 tup (one finger, as Android delivers it) */
static int kind_of(const char *k)
{
    static const char *names[] = { "", "key", "click", "move", "rclick", "tap", "tap2",
                                   "down", "up", "tdown", "tmove", "tup" };
    for (int i = 1; i < (int)(sizeof names / sizeof names[0]); i++)
        if (!strcmp(k, names[i])) return i;
    return 0;
}

static void parse(void)
{
    g_nev = 0;
    const char *s = getenv("W64_INPUT");
    if (!s) return;
    char *d = strdup(s), *save = NULL;
    for (char *t = strtok_r(d, ",", &save); t && g_nev < 256; t = strtok_r(NULL, ",", &save)) {
        ev_t *e = &g_ev[g_nev];
        char kind[16] = { 0 };
        memset(e, 0, sizeof *e);
        if (sscanf(t, "%lf:%15[^:]:%31[^,]", &e->t, kind, e->key) < 2) continue;
        if (!(e->kind = kind_of(kind))) continue;
        if (e->kind != 1) sscanf(e->key, "%f:%f", &e->x, &e->y);
        g_nev++;
    }
    free(d);
}

static void push_key(SDL_Window *w, const char *name, int down)
{
    SDL_Keycode k = SDL_GetKeyFromName(name);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.timestamp = SDL_GetTicksNS();
    e.key.windowID = SDL_GetWindowID(w);
    e.key.key = k;
    e.key.scancode = SDL_GetScancodeFromKey(k, NULL);
    e.key.down = down;
    SDL_PushEvent(&e);
}

static void push_mouse(SDL_Window *w, float x, float y, int button, int down)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    SDL_WarpMouseInWindow(w, x, y);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.timestamp = SDL_GetTicksNS();
    e.motion.windowID = SDL_GetWindowID(w);
    e.motion.x = x; e.motion.y = y;
    SDL_PushEvent(&e);
    if (!button) return;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.timestamp = SDL_GetTicksNS();
    e.button.windowID = SDL_GetWindowID(w);
    e.button.button = (Uint8)button;
    e.button.down = down;
    e.button.clicks = 1;
    e.button.x = x; e.button.y = y;
    SDL_PushEvent(&e);
}

/* what SDL on Android produces for a finger: FINGER_* plus synthetic touch-mouse events */
static void push_finger(SDL_Window *w, int id, float x, float y, int down)
{
    int ww = 1, wh = 1;
    SDL_GetWindowSize(w, &ww, &wh);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_FINGER_DOWN : SDL_EVENT_FINGER_UP;
    e.tfinger.timestamp = SDL_GetTicksNS();
    e.tfinger.touchID = 1; e.tfinger.fingerID = (SDL_FingerID)id;
    e.tfinger.x = x / (float)ww; e.tfinger.y = y / (float)wh;
    e.tfinger.windowID = SDL_GetWindowID(w);
    SDL_PushEvent(&e);
}
static void push_touch_mouse(SDL_Window *w, float x, float y, int down)
{
    SDL_Event e;
    SDL_WarpMouseInWindow(w, x, y);     /* Android's touch-mouse emulation moves SDL's mouse state too */
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.timestamp = SDL_GetTicksNS(); e.motion.windowID = SDL_GetWindowID(w);
    e.motion.which = SDL_TOUCH_MOUSEID; e.motion.x = x; e.motion.y = y;
    if (down) SDL_PushEvent(&e);
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.timestamp = SDL_GetTicksNS(); e.button.windowID = SDL_GetWindowID(w);
    e.button.which = SDL_TOUCH_MOUSEID; e.button.button = SDL_BUTTON_LEFT; e.button.down = down;
    e.button.clicks = 1; e.button.x = x; e.button.y = y;
    SDL_PushEvent(&e);
}

/* finger drag step: FINGER_MOTION plus the touch-mouse motion (left button held) */
static void push_touch_motion(SDL_Window *w, int id, float x, float y)
{
    int ww = 1, wh = 1;
    SDL_GetWindowSize(w, &ww, &wh);
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_FINGER_MOTION;
    e.tfinger.timestamp = SDL_GetTicksNS();
    e.tfinger.touchID = 1; e.tfinger.fingerID = (SDL_FingerID)id;
    e.tfinger.x = x / (float)ww; e.tfinger.y = y / (float)wh;
    e.tfinger.windowID = SDL_GetWindowID(w);
    SDL_PushEvent(&e);
    SDL_WarpMouseInWindow(w, x, y);
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.timestamp = SDL_GetTicksNS(); e.motion.windowID = SDL_GetWindowID(w);
    e.motion.which = SDL_TOUCH_MOUSEID; e.motion.state = SDL_BUTTON_LMASK;
    e.motion.x = x; e.motion.y = y;
    SDL_PushEvent(&e);
}

/* Test channel on a device: setprop debug.mew.inject "<n>:tap2:x:y" (also tap / key:Name); every
 * new <n> injects one event 0.2 s later, through the same SDL queue real input goes through. */
static void poll_inject(double secs)
{
#ifdef __ANDROID__
    static char last[PROP_VALUE_MAX];
    static int primed;
    char v[PROP_VALUE_MAX] = "";
    __system_property_get("debug.mew.inject", v);
    if (!primed) {                      /* a value left over from an earlier run is not a new event */
        primed = 1;
        snprintf(last, sizeof last, "%s", v);
        return;
    }
    if (!v[0] || !strcmp(v, last)) return;
    snprintf(last, sizeof last, "%s", v);
    const char *c = strchr(v, ':');
    if (!c || g_nev >= 255 || g_nev < 0) return;
    ev_t *e = &g_ev[g_nev];
    char kind[16] = { 0 };
    memset(e, 0, sizeof *e);
    if (sscanf(c + 1, "%15[^:]:%31s", kind, e->key) < 1) return;
    e->t = secs + 0.2;
    if (!(e->kind = kind_of(kind))) return;
    if (e->kind != 1) sscanf(e->key, "%f:%f", &e->x, &e->y);
    w64_log("[inject] %s %s", kind, e->key);
    g_nev++;
#else
    /* host: W64_INJECT_FILE, one command per line appended over time: "click x y", "rclick x y",
     * "tap x y", "tap2 x y", "move x y", "key Name" */
    static long off;
    const char *fn = getenv("W64_INJECT_FILE");
    if (!fn) return;
    FILE *f = fopen(fn, "r");
    if (!f) return;
    fseek(f, off, SEEK_SET);
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (!strchr(line, '\n')) break;                 /* incomplete line: read later */
        off = ftell(f);
        if (g_nev >= 255 || g_nev < 0) continue;
        ev_t *e = &g_ev[g_nev];
        char kind[16] = { 0 }, a[32] = { 0 }, b[32] = { 0 };
        memset(e, 0, sizeof *e);
        if (sscanf(line, "%15s %31s %31s", kind, a, b) < 2) continue;
        e->t = secs + 0.05;
        if (!strcmp(kind, "key")) { e->kind = 1; snprintf(e->key, sizeof e->key, "%s", a); }
        else {
            e->x = (float)atof(a); e->y = (float)atof(b);
            snprintf(e->key, sizeof e->key, "%s:%s", a, b);
            if ((e->kind = kind_of(kind)) <= 1) continue;
        }
        w64_log("[inject] %s", line);
        g_nev++;
    }
    fclose(f);
#endif
}

void w64_input_tick(SDL_Window *w, uint64_t frame, double secs)
{
    if (g_nev < 0) parse();
    if ((frame % 15) == 0) poll_inject(secs);
    for (int i = 0; i < g_nev; i++) {
        ev_t *e = &g_ev[i];
        if (e->done >= 2 || secs < e->t) continue;
        if (e->done == 0 && e->kind >= 7) {          /* one-shot held/drag primitives */
            w64_log("[input] %.1fs kind %d %s", secs, e->kind, e->key);
            if (e->kind == 7 || e->kind == 8) push_mouse(w, e->x, e->y, SDL_BUTTON_LEFT, e->kind == 7);
            else if (e->kind == 9) { push_finger(w, 1, e->x, e->y, 1); push_touch_mouse(w, e->x, e->y, 1); }
            else if (e->kind == 10) push_touch_motion(w, 1, e->x, e->y);
            else { push_finger(w, 1, e->x, e->y, 0); push_touch_mouse(w, e->x, e->y, 0); }
            e->done = 2;
            continue;
        }
        if (e->done == 0) {
            w64_log("[input] %.1fs %s %s", secs, e->kind == 1 ? "key" : e->kind == 3 ? "move" : "click", e->key);
            if (e->kind == 1) push_key(w, e->key, 1);
            else if (e->kind == 5) { push_finger(w, 1, e->x, e->y, 1); push_touch_mouse(w, e->x, e->y, 1); }
            else if (e->kind == 6) { push_finger(w, 1, e->x, e->y, 1); push_touch_mouse(w, e->x, e->y, 1); push_finger(w, 2, e->x + 80, e->y, 1); }
            else if (e->kind == 2) push_mouse(w, e->x, e->y, SDL_BUTTON_LEFT, 1);
            else if (e->kind == 4) push_mouse(w, e->x, e->y, SDL_BUTTON_RIGHT, 1);
            else { push_mouse(w, e->x, e->y, 0, 0); e->done = 2; continue; }
            e->done = 1;
        } else if (secs >= e->t + 0.12) {
            if (e->kind == 1) push_key(w, e->key, 0);
            else if (e->kind == 5) { push_finger(w, 1, e->x, e->y, 0); push_touch_mouse(w, e->x, e->y, 0); }
            else if (e->kind == 6) { push_finger(w, 2, e->x + 80, e->y, 0); push_finger(w, 1, e->x, e->y, 0); push_touch_mouse(w, e->x, e->y, 0); }
            else push_mouse(w, e->x, e->y, e->kind == 4 ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT, 0);
            e->done = 2;
        }
    }
    /* drop finished events so long injected sessions never run out of slots */
    int n = 0;
    for (int i = 0; i < g_nev; i++)
        if (g_ev[i].done < 2) g_ev[n++] = g_ev[i];
    g_nev = n;
}
