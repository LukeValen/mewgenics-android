/*
 * main.c - Linux debug host: loads Mewgenics.exe, runs the recompiled code
 * against the host SDL3 + desktop OpenGL (or OpenGL ES with W64_GLES=1).
 *
 * usage: mewhost <game dir with resources.gpak> [Mewgenics.exe] [user dir]
 * env:   W64_TRACE=1|2|3, W64_LOGFILE, W64_SHOT_DIR, W64_SHOT_EVERY, W64_INPUT,
 *        W64_STEAM=fake|fail, W64_GLES=1, W64_ARGS, W64_ENV, W64_TIMEOUT (s)
 */
#define _GNU_SOURCE
#include "w64.h"
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

extern int w64_gles;
extern __thread uint8_t *w64_teb;
CPU *w64_cpu(void);

void w64_shutdown(int code)
{
    w64_log("[host] exit %d", code);
    fflush(stderr);
    _exit(code);
}

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    w64_log("[host] signal %d at address %p", sig, si->si_addr);
    if (w64_teb) w64_backtrace(w64_cpu());
    fflush(stderr);
    _exit(128 + sig);
}

static void *timeout_thread(void *arg)
{
    int secs = (int)(intptr_t)arg;
    sleep((unsigned)secs);
    w64_log("[host] timeout after %d s", secs);
    w64_shutdown(0);
    return NULL;
}

static const char *g_exe;
static void *game_thread(void *arg)
{
    (void)arg;
    if (w64_load(g_exe)) w64_shutdown(2);
    int r = w64_run_entry();
    w64_shutdown(r);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <game dir> [Mewgenics.exe] [user dir]\n", argv[0]);
        return 2;
    }
    realpath(argv[1], w64_gamedir);
    char exe[1024];
    if (argc > 2) snprintf(exe, sizeof exe, "%s", argv[2]);
    else snprintf(exe, sizeof exe, "%s/Mewgenics.exe", w64_gamedir);
    g_exe = exe;
    if (argc > 3) { mkdir(argv[3], 0777); realpath(argv[3], w64_userdir); }
    else { snprintf(w64_userdir, sizeof w64_userdir, "%s/.mewuser", getenv("HOME") ? getenv("HOME") : "/tmp"); mkdir(w64_userdir, 0777); }
    if (getenv("W64_TRACE")) w64_trace = atoi(getenv("W64_TRACE"));
    if (getenv("W64_GLES")) w64_gles = atoi(getenv("W64_GLES"));
    { extern int w64_glcheck; if (getenv("W64_GLCHECK")) w64_glcheck = 1; }
    if (chdir(w64_gamedir)) {}
    w64_log("[host] game %s, exe %s, user %s, gles %d", w64_gamedir, exe, w64_userdir, w64_gles);

    static uint8_t altstack[1 << 16];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    pthread_t th;
    if (getenv("W64_TIMEOUT")) pthread_create(&th, NULL, timeout_thread, (void *)(intptr_t)atoi(getenv("W64_TIMEOUT")));
    /* The lifted code keeps every guest call as a host C frame: run it on a large stack. */
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 512u << 20);
    pthread_create(&th, &at, game_thread, NULL);
    pthread_join(th, NULL);
    return 0;
}
