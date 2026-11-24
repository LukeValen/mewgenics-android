/*
 * android_main.c - SDL3 entry on Android. SDLActivity calls SDL_main on its SDLThread with
 * the arguments MewActivity.getArguments() returns: <game dir> <user dir>.
 *
 * Debug switches come from the system property debug.mew.env ("W64_TRACE=1;W64_GLCHECK=1").
 */
#define _GNU_SOURCE
#include "w64.h"
#include <SDL3/SDL.h>
#include <android/log.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

extern int w64_gles, w64_glcheck;

void w64_shutdown(int code)
{
    w64_log("[android] exit %d", code);
    _exit(code);
}

static void env_from_property(void)
{
    char v[PROP_VALUE_MAX] = "";
    __system_property_get("debug.mew.env", v);
    if (!v[0]) return;
    char *save = NULL;
    for (char *t = strtok_r(v, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = 0;
        setenv(t, eq + 1, 1);
        __android_log_print(ANDROID_LOG_INFO, "mew", "env %s=%s", t, eq + 1);
    }
}

static char g_exe[1100];
static int g_rc;
static void *game_thread(void *arg)
{
    (void)arg;
    if (w64_load(g_exe)) { w64_log("[android] cannot load %s", g_exe); g_rc = 2; return NULL; }
    g_rc = w64_run_entry();
    return NULL;
}

__attribute__((visibility("default"))) int SDL_main(int argc, char *argv[])
{
    env_from_property();
    snprintf(w64_gamedir, sizeof w64_gamedir, "%s", argc > 1 ? argv[1] : "/sdcard/Android/data/ch.plum.mewgenics/files/game");
    snprintf(w64_userdir, sizeof w64_userdir, "%s", argc > 2 ? argv[2] : "/sdcard/Android/data/ch.plum.mewgenics/files/user");
    mkdir(w64_userdir, 0777);
    char logp[1200];
    snprintf(logp, sizeof logp, "%s/runtime.log", w64_userdir);
    unlink(logp);
    setenv("W64_LOGFILE", logp, 0);
    if (getenv("W64_TRACE")) w64_trace = atoi(getenv("W64_TRACE"));
    if (getenv("W64_GLCHECK")) w64_glcheck = 1;
    w64_gles = 1;
    if (chdir(w64_gamedir)) {}
    snprintf(g_exe, sizeof g_exe, "%s/Mewgenics.exe", w64_gamedir);
    w64_log("[android] game %s user %s", w64_gamedir, w64_userdir);
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    /* the lifted code keeps every guest call as a host frame: big stack */
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256u << 20);
    pthread_t th;
    if (pthread_create(&th, &at, game_thread, NULL)) { w64_log("[android] pthread_create failed"); return 1; }
    pthread_join(th, NULL);
    w64_log("[android] game returned %d", g_rc);
    return g_rc;
}
