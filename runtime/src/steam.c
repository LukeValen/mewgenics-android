/*
 * steam.c - steam_api64.dll. No Steam client exists on the target.
 *
 * W64_STEAM=fail : SteamAPI init reports "no Steam client" (the game decides).
 * W64_STEAM=fake (default): init succeeds and every interface is a fake object
 *   whose virtual methods log once and return 0, with a few methods answered
 *   with plausible values (logged-on user, persona name, app id, language).
 */
#include "w64.h"
#include <pthread.h>
#include <time.h>

const char *w64_current_hostfn(void);

static int steam_fake(void)
{
    const char *m = getenv("W64_STEAM");
    return !m || strcmp(m, "fail");
}

static void SteamAPI_RestartAppIfNecessary_(CPU *c) { RET(0); }
static void SteamInternal_SteamAPI_Init_(CPU *c)
{
    char vers[1024] = "";
    if (A(0)) {
        size_t o = 0;
        for (const char *p = AS(0); *p && o < sizeof vers - 80; p += strlen(p) + 1)
            o += (size_t)snprintf(vers + o, sizeof vers - o, "%s ", p);
    }
    w64_log("[steam] SteamInternal_SteamAPI_Init(%s) -> %s", vers, steam_fake() ? "ok (fake)" : "no client");
    if (steam_fake()) RET(0);                       /* k_ESteamAPIInitResult_OK */
    if (A(1)) {
        static char msg[1024] = "Steam is not available on this device";
        memcpy(AP(1), msg, strlen(msg) + 1);
    }
    RET(2);                                         /* NoSteamClient */
}
static void SteamAPI_GetHSteamUser_(CPU *c) { RET(1); }
static void SteamAPI_RunCallbacks_(CPU *c) { RETV(); }
static void SteamAPI_Shutdown_(CPU *c) { RETV(); }

/* CSteamAPIContext init data: { void (*pFn)(void *ctx); uintp counter; ctx... } */
static void SteamInternal_ContextInit_(CPU *c)
{
    uint64_t d = A(0);
    if (rd64(d + 8) != 1) {
        wr64(d + 8, 1);
        w64_call(c, rd64(d), 1, d + 16);
    }
    RET(d + 16);
}

/* ---- fake interfaces ---- */
#define NMETH 160
typedef struct { char ver[64]; uint64_t vtbl[NMETH]; uint64_t self_vptr; } iface_t;
static iface_t *g_if[64];
static int g_nif;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t g_seen[1024];
static int g_nseen;

static void method(CPU *c)
{
    const char *n = w64_current_hostfn();       /* "SteamUser023#5" */
    uint64_t key = (uint64_t)(uintptr_t)n;
    int dup = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_nseen; i++) if (g_seen[i] == key) { dup = 1; break; }
    if (!dup && g_nseen < 1024) g_seen[g_nseen++] = key;
    pthread_mutex_unlock(&g_mu);
    const char *h = strchr(n, '#');
    int k = h ? atoi(h + 1) : -1;
    size_t pl = h ? (size_t)(h - n) : strlen(n);
    char ifn[64];
    snprintf(ifn, sizeof ifn, "%.*s", (int)pl, n);
    /* answers by interface + vtable index (Steamworks SDK 1.5x layouts) */
    const uint64_t steamid = 0x0110000100000001ull;
    uint64_t r = 0;
    int sret = 0;                         /* method returns CSteamID through the hidden pointer in rdx */
    if (!strncmp(ifn, "SteamUser", 9)) {
        if (k == 0 || k == 1) r = 1;                          /* GetHSteamUser, BLoggedOn */
        else if (k == 2) sret = 1;                            /* GetSteamID */
        else if (k == 18) r = 0;                              /* UserHasLicenseForApp: k_EUserHasLicenseResultHasLicense */
        else if (k == 24 || k == 23) r = 1;
    } else if (!strncmp(ifn, "SteamFriends", 12)) {
        if (k == 0) r = (uintptr_t)"Player";                  /* GetPersonaName */
        else if (k == 2) r = 1;                               /* GetPersonaState: online */
    } else if (!strncmp(ifn, "SteamUtils", 10)) {
        if (k == 2) r = 1;                                    /* GetConnectedUniverse: public */
        else if (k == 3) r = (uint64_t)time(NULL);            /* GetServerRealTime */
        else if (k == 4) r = (uintptr_t)"US";                 /* GetIPCountry */
        else if (k == 7) r = 255;                             /* GetCurrentBatteryPower: AC */
        else if (k == 8) r = 686060;                          /* GetAppID */
        else if (k == 21) r = (uintptr_t)"english";           /* GetSteamUILanguage */
    } else if (!strncmp(ifn, "STEAMAPPS_INTERFACE_VERSION", 27) || !strncmp(ifn, "SteamApps", 9)) {
        if (k == 0 || k == 6) r = 1;                          /* BIsSubscribed, BIsSubscribedApp */
        else if (k == 4) r = (uintptr_t)"english";            /* GetCurrentGameLanguage */
        else if (k == 5) r = (uintptr_t)"english";            /* GetAvailableGameLanguages */
        else if (k == 15) r = 0;                              /* GetCurrentBetaName: false */
        else if (k == 19) r = 1;                              /* BIsAppInstalled */
        else if (k == 20) sret = 1;                           /* GetAppOwner */
    }
    if (sret) { wr64(c->rdx, steamid); r = c->rdx; }
    if (!dup) w64_log("[steam] %s (rdx %#llx r8 %#llx) -> %#llx%s", n, (unsigned long long)c->rdx, (unsigned long long)c->r8,
                      (unsigned long long)r, sret ? " (CSteamID via rdx)" : "");
    RET(r);
}

static void SteamInternal_FindOrCreateUserInterface_(CPU *c)
{
    const char *ver = A(1) ? AS(1) : "?";
    if (!steam_fake()) RET(0);
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_nif; i++) if (!strcmp(g_if[i]->ver, ver)) { pthread_mutex_unlock(&g_mu); RET((uintptr_t)&g_if[i]->self_vptr); }
    iface_t *f = calloc(1, sizeof *f);
    snprintf(f->ver, sizeof f->ver, "%s", ver);
    for (int k = 0; k < NMETH; k++) {
        char *nm = malloc(96);
        snprintf(nm, 96, "%s#%d", ver, k);
        f->vtbl[k] = w64_hostfn(nm, method);
    }
    f->self_vptr = (uint64_t)(uintptr_t)f->vtbl;
    if (g_nif < 64) g_if[g_nif++] = f;
    pthread_mutex_unlock(&g_mu);
    w64_log("[steam] interface %s -> fake", ver);
    RET((uintptr_t)&f->self_vptr);
}

static const w64_export_t steam_exports[] = {
    { "steam_api64.dll", "SteamAPI_RestartAppIfNecessary", SteamAPI_RestartAppIfNecessary_ },
    { "steam_api64.dll", "SteamInternal_SteamAPI_Init", SteamInternal_SteamAPI_Init_ },
    { "steam_api64.dll", "SteamAPI_GetHSteamUser", SteamAPI_GetHSteamUser_ },
    { "steam_api64.dll", "SteamAPI_RunCallbacks", SteamAPI_RunCallbacks_ },
    { "steam_api64.dll", "SteamAPI_Shutdown", SteamAPI_Shutdown_ },
    { "steam_api64.dll", "SteamInternal_ContextInit", SteamInternal_ContextInit_ },
    { "steam_api64.dll", "SteamInternal_FindOrCreateUserInterface", SteamInternal_FindOrCreateUserInterface_ },
    { 0 }
};
__attribute__((constructor)) static void steam_init(void) { w64_register(steam_exports); }
