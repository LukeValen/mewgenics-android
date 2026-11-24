/*
 * w32_stubs.c - user32, gdi32, imm32, shell32, ole32, oleaut32, advapi32,
 * setupapi, version.
 *
 * Nearly all of these imports belong to SDL's Win32 backend, which never runs
 * (SDL is replaced at the dynapi table). What the game itself calls gets a
 * real answer below; everything else logs its name once and returns 0.
 */
#include "w64.h"
#include <pthread.h>

const char *w64_current_hostfn(void);

static void quiet0(CPU *c)
{
    static const char *seen[512];
    static int n;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    const char *nm = w64_current_hostfn();
    int dup = 0;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < n; i++) if (seen[i] == nm) { dup = 1; break; }
    if (!dup && n < 512) seen[n++] = nm;
    pthread_mutex_unlock(&mu);
    if (!dup) w64_log("[w32] %s -> 0 (stub)", nm);
    RET(0);
}

static void MessageBoxA_(CPU *c)
{
    w64_log("[msgbox] %s: %s", A(2) ? AS(2) : "", A(1) ? AS(1) : "");
    RET(1);   /* IDOK */
}
static void GetSystemMetrics_(CPU *c)
{
    int i = AI(0);
    RET(i == 0 ? 1920 : i == 1 ? 1080 : i == 80 ? 1 : 0);   /* SM_CXSCREEN, SM_CYSCREEN, SM_CMONITORS */
}
static void SHGetFolderPathW_(CPU *c)
{
    int csidl = AI(1) & 0xFF;
    const char *p = csidl == 0x1a ? "C:\\Users\\Player\\AppData\\Roaming" :
                    csidl == 0x1c ? "C:\\Users\\Player\\AppData\\Local" :
                    csidl == 0x05 ? "C:\\Users\\Player\\Documents" :
                    csidl == 0x28 ? "C:\\Users\\Player" : "C:\\Users\\Player\\AppData\\Roaming";
    utf8_to_w16(p, (uint16_t *)AP(4), 260);
    w64_log("[w32] SHGetFolderPathW(%#x) -> %s", csidl, p);
    RET(0);
}
static void ShellExecuteA_(CPU *c) { w64_log("[w32] ShellExecuteA %s %s (ignored)", A(1) ? AS(1) : "", A(2) ? AS(2) : ""); RET(42); }
static void ShellExecuteW_(CPU *c)
{
    char *a = A(1) ? w16_to_utf8(AW(1), -1) : strdup(""), *b = A(2) ? w16_to_utf8(AW(2), -1) : strdup("");
    w64_log("[w32] ShellExecuteW %s %s (ignored)", a, b);
    free(a); free(b);
    RET(42);
}
static void CommandLineToArgvW_(CPU *c)
{
    /* single argument: the executable */
    uint64_t *v = calloc(4, 8);
    uint16_t *s = utf8_to_w16_dup("C:\\Game\\Mewgenics.exe");
    v[0] = (uint64_t)(uintptr_t)s;
    if (A(1)) wr32(A(1), 1);
    RET((uintptr_t)v);
}
static void CoInitializeEx_(CPU *c) { RET(0); }
static void CoCreateInstance_(CPU *c) { if (A(4)) wr64(A(4), 0); w64_log("[w32] CoCreateInstance (refused)"); RET(0x80040154u); /* REGDB_E_CLASSNOTREG */ }
static void RegOpenKeyExW_(CPU *c) { if (A(4)) wr64(A(4), 0); RET(2); }
static void RegQueryValueExW_(CPU *c) { RET(2); }
static void GetFileVersionInfoSizeA_(CPU *c) { RET(0); }

static const w64_export_t named[] = {
    { "USER32.dll", "MessageBoxA", MessageBoxA_ },
    { "USER32.dll", "GetSystemMetrics", GetSystemMetrics_ },
    { "SHELL32.dll", "SHGetFolderPathW", SHGetFolderPathW_ },
    { "SHELL32.dll", "ShellExecuteA", ShellExecuteA_ },
    { "SHELL32.dll", "ShellExecuteW", ShellExecuteW_ },
    { "SHELL32.dll", "CommandLineToArgvW", CommandLineToArgvW_ },
    { "ole32.dll", "CoInitializeEx", CoInitializeEx_ },
    { "ole32.dll", "OleInitialize", CoInitializeEx_ },
    { "ole32.dll", "CoCreateInstance", CoCreateInstance_ },
    { "ADVAPI32.dll", "RegOpenKeyExW", RegOpenKeyExW_ },
    { "ADVAPI32.dll", "RegQueryValueExW", RegQueryValueExW_ },
    { "VERSION.dll", "GetFileVersionInfoSizeA", GetFileVersionInfoSizeA_ },
    { 0 }
};

/* every remaining import of these DLLs -> quiet0 */
static const char *stub_dlls[] = { "USER32.dll", "GDI32.dll", "IMM32.dll", "SHELL32.dll", "ole32.dll", "OLEAUT32.dll",
                                   "ADVAPI32.dll", "SETUPAPI.dll", "VERSION.dll", NULL };
static w64_export_t g_stubs[600];

__attribute__((constructor)) static void w32_stubs_init(void)
{
    w64_register(named);
    int k = 0;
    for (uint32_t i = 0; i < HLE_COUNT && k < 599; i++) {
        const hle_import_t *im = &recomp_imports[i];
        for (int d = 0; stub_dlls[d]; d++)
            if (!strcmp(im->dll, stub_dlls[d])) { g_stubs[k].dll = im->dll; g_stubs[k].name = im->name; g_stubs[k].fn = quiet0; k++; break; }
    }
    g_stubs[k].name = NULL;
    w64_register(g_stubs);
}
