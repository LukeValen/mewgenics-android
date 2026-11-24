/*
 * core.c - loader, dispatch, host-function registry, TEB/TLS, host->guest calls.
 */
#define _GNU_SOURCE
#include "w64.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/log.h>
#endif

int64_t g_image_delta;
__thread uint8_t *w64_teb;
uint64_t w64_image_base, w64_image_size, w64_entry;
int w64_trace;
extern uint64_t es3_eh_image_base;

/* ------------------------------------------------------------------ logging */
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_logf;

void w64_log(const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_log_mu);
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_INFO, "mew", buf);
#endif
    fputs(buf, stderr);
    if (buf[0] && buf[strlen(buf) - 1] != '\n') fputc('\n', stderr);
    if (!g_logf) {
        const char *p = getenv("W64_LOGFILE");
        if (p && *p) g_logf = fopen(p, "a");
    }
    if (g_logf) { fputs(buf, g_logf); if (buf[0] && buf[strlen(buf) - 1] != '\n') fputc('\n', g_logf); fflush(g_logf); }
    pthread_mutex_unlock(&g_log_mu);
}

/* Return addresses on the guest stack, filtered to the image: an approximate
 * guest call chain (no frame pointers needed). */
void w64_backtrace(CPU *c)
{
    if (!c) return;
    w64_log("  rip %#llx rsp %#llx rax %#llx rcx %#llx rdx %#llx r8 %#llx r9 %#llx",
            (unsigned long long)c->rip, (unsigned long long)c->rsp, (unsigned long long)c->rax,
            (unsigned long long)c->rcx, (unsigned long long)c->rdx, (unsigned long long)c->r8,
            (unsigned long long)c->r9);
    uint64_t base = w64_teb ? rd64((uint64_t)(uintptr_t)w64_teb + TEB_STACK_BASE) : 0;
    int n = 0;
    for (uint64_t p = c->rsp; p + 8 <= base && n < 24 && p < c->rsp + 0x8000; p += 8) {
        uint64_t v = rd64(p);
        if (v >= w64_image_base && v < w64_image_base + w64_image_size && v > w64_image_base + 0x1000) {
            w64_log("  [rsp+%#llx] %#llx", (unsigned long long)(p - c->rsp), (unsigned long long)v);
            n++;
        }
    }
}

void w64_fatal(CPU *c, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    w64_log("FATAL: %s", buf);
    w64_backtrace(c);
    fflush(stderr);
    _exit(3);
}

static pthread_mutex_t g_todo_mu = PTHREAD_MUTEX_INITIALIZER;
void w64_todo(uint64_t va, const char *text)
{
    static uint64_t seen[256];
    static int nseen;
    pthread_mutex_lock(&g_todo_mu);
    int dup = 0;
    for (int i = 0; i < nseen; i++) if (seen[i] == va) { dup = 1; break; }
    if (!dup && nseen < 256) seen[nseen++] = va;
    pthread_mutex_unlock(&g_todo_mu);
    if (!dup) w64_log("[todo] %#llx: %s", (unsigned long long)va, text);
    if (getenv("W64_TODO_FATAL")) w64_fatal(w64_cpu(), "untranslated instruction at %#llx", (unsigned long long)va);
}

/* ------------------------------------------------------------------ strings */
size_t w16len(const uint16_t *s) { size_t n = 0; if (s) while (s[n]) n++; return n; }

char *w16_to_utf8(const uint16_t *s, int len)
{
    if (!s) return strdup("");
    if (len < 0) len = (int)w16len(s);
    char *o = malloc((size_t)len * 3 + 1), *p = o;
    for (int i = 0; i < len; i++) {
        uint32_t u = s[i];
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < len && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            u = 0x10000 + ((u - 0xD800) << 10) + (s[i + 1] - 0xDC00); i++;
        }
        if (u < 0x80) *p++ = (char)u;
        else if (u < 0x800) { *p++ = (char)(0xC0 | (u >> 6)); *p++ = (char)(0x80 | (u & 63)); }
        else if (u < 0x10000) { *p++ = (char)(0xE0 | (u >> 12)); *p++ = (char)(0x80 | ((u >> 6) & 63)); *p++ = (char)(0x80 | (u & 63)); }
        else { *p++ = (char)(0xF0 | (u >> 18)); *p++ = (char)(0x80 | ((u >> 12) & 63)); *p++ = (char)(0x80 | ((u >> 6) & 63)); *p++ = (char)(0x80 | (u & 63)); }
    }
    *p = 0;
    return o;
}

/* writes at most cap units (incl. NUL when it fits); returns units needed incl. NUL */
int utf8_to_w16(const char *s, uint16_t *out, int cap)
{
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (*p) {
        uint32_t u = *p++;
        if (u >= 0xF0 && p[0] && p[1] && p[2]) { u = ((u & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 3; }
        else if (u >= 0xE0 && p[0] && p[1]) { u = ((u & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
        else if (u >= 0xC0 && p[0]) { u = ((u & 31) << 6) | (p[0] & 63); p += 1; }
        if (u >= 0x10000) {
            u -= 0x10000;
            if (out && n < cap) out[n] = (uint16_t)(0xD800 + (u >> 10));
            n++;
            if (out && n < cap) out[n] = (uint16_t)(0xDC00 + (u & 0x3FF));
            n++;
        } else {
            if (out && n < cap) out[n] = (uint16_t)u;
            n++;
        }
    }
    if (out && n < cap) out[n] = 0;
    return n + 1;
}

uint16_t *utf8_to_w16_dup(const char *s)
{
    int n = utf8_to_w16(s, NULL, 0);
    uint16_t *o = malloc((size_t)n * 2);
    utf8_to_w16(s, o, n);
    return o;
}

uint64_t w64_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------ registry */
#define REG_MAX 4096
static const w64_export_t *g_reg[REG_MAX];
static int g_nreg;

void w64_register(const w64_export_t *tab)
{
    for (; tab->name; tab++) if (g_nreg < REG_MAX) g_reg[g_nreg++] = tab;
}

static int dll_eq(const char *a, const char *b)
{
    /* "KERNEL32.dll" == "kernel32" == "kernel32.DLL" */
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x == '.' ) x = 0;
        if (y == '.') y = 0;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

w64_fn w64_find(const char *dll, const char *name)
{
    for (int i = 0; i < g_nreg; i++)
        if (!strcmp(g_reg[i]->name, name) && (!dll || !g_reg[i]->dll || dll_eq(g_reg[i]->dll, dll)))
            return g_reg[i]->fn;
    return NULL;
}

typedef struct { w64_fn fn; const char *dll, *name; } slot_t;
static slot_t g_slots[W64_SENT_SLOTS];
static uint32_t g_nslots = HLE_COUNT;
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;

static void unimpl_import(CPU *c);

uint64_t w64_hostfn(const char *name, w64_fn fn)
{
    pthread_mutex_lock(&g_slot_mu);
    for (uint32_t i = HLE_COUNT; i < g_nslots; i++)
        if (g_slots[i].fn == fn && g_slots[i].name == name) { pthread_mutex_unlock(&g_slot_mu); return HLE_SENTINEL(i); }
    if (g_nslots >= W64_SENT_SLOTS) { pthread_mutex_unlock(&g_slot_mu); w64_fatal(NULL, "out of host-function slots"); }
    uint32_t i = g_nslots++;
    g_slots[i].fn = fn; g_slots[i].name = name; g_slots[i].dll = "host";
    pthread_mutex_unlock(&g_slot_mu);
    return HLE_SENTINEL(i);
}

uint64_t w64_hostfn_named(const char *dll, const char *name)
{
    w64_fn f = w64_find(dll, name);
    if (!f) return 0;
    pthread_mutex_lock(&g_slot_mu);
    for (uint32_t i = 0; i < g_nslots; i++)
        if (g_slots[i].fn == f && g_slots[i].name && !strcmp(g_slots[i].name, name)) { pthread_mutex_unlock(&g_slot_mu); return HLE_SENTINEL(i); }
    pthread_mutex_unlock(&g_slot_mu);
    char *nm = strdup(name);
    return w64_hostfn(nm, f);
}

const char *w64_hostfn_name(uint64_t va)
{
    if (!W64_IS_SENTINEL(va)) return NULL;
    uint64_t i = (va - HLE_BASE) / HLE_STRIDE;
    return i < g_nslots ? g_slots[i].name : NULL;
}

static __thread uint64_t t_cur_slot;
/* thread registry for the watchdog (W64_WATCH=<seconds>) */
typedef struct { CPU *c; uint8_t *teb; volatile int64_t slot; uint64_t tid; } treg_t;
static treg_t g_threads[256];
static int g_nthreads, g_live_threads;
static __thread treg_t *t_reg;
const char *w64_current_hostfn(void) { return g_slots[t_cur_slot].name ? g_slots[t_cur_slot].name : "?"; }
static void unimpl_import(CPU *c)
{
    slot_t *s = &g_slots[t_cur_slot];
    w64_fatal(c, "unimplemented import %s!%s", s->dll ? s->dll : "?", s->name ? s->name : "?");
}

/* ------------------------------------------------------------------ dispatch */
#define HT_BITS 18
#define HT_SIZE (1u << HT_BITS)
static struct { uint64_t va; void (*fn)(CPU *); } *g_ht;

static inline uint32_t ht_hash(uint64_t va) { return (uint32_t)((va * 0x9E3779B97F4A7C15ull) >> (64 - HT_BITS)); }

/* Host replacements for guest functions (runtime/src/hle_libs.c). The address must also be
 * listed in config/keep_dispatch.txt so callers go through dispatch(). */
typedef struct { uint64_t va; w64_fn fn; const char *name; } override_t;
static override_t g_ovr[64];
static int g_novr;
void w64_add_override(uint64_t va, w64_fn fn, const char *name)
{
    if (g_novr < 64) { g_ovr[g_novr].va = va; g_ovr[g_novr].fn = fn; g_ovr[g_novr].name = name; g_novr++; }
}

static void ht_build(void)
{
    g_ht = calloc(HT_SIZE, sizeof *g_ht);
    for (uint32_t i = 0; i < recomp_dispatch_count; i++) {
        uint32_t h = ht_hash(recomp_dispatch_table[i].va);
        while (g_ht[h].va) h = (h + 1) & (HT_SIZE - 1);
        g_ht[h].va = recomp_dispatch_table[i].va;
        g_ht[h].fn = recomp_dispatch_table[i].fn;
    }
    int off = getenv("W64_NO_OVERRIDES") != NULL;
    for (int k = 0; k < g_novr && !off; k++) {
        uint32_t h = ht_hash(g_ovr[k].va);
        while (g_ht[h].va && g_ht[h].va != g_ovr[k].va) h = (h + 1) & (HT_SIZE - 1);
        g_ht[h].va = g_ovr[k].va;
        g_ht[h].fn = g_ovr[k].fn;
        w64_log("[ovr] %#llx -> host %s", (unsigned long long)g_ovr[k].va, g_ovr[k].name);
    }
}

static void (*ht_lookup(uint64_t va))(CPU *)
{
    uint32_t h = ht_hash(va);
    while (g_ht[h].va) {
        if (g_ht[h].va == va) return g_ht[h].fn;
        h = (h + 1) & (HT_SIZE - 1);
    }
    return NULL;
}

/* W64_GLSTAT=1: calls per import/host function, reported per frame from SDL_GL_SwapWindow */
static uint32_t *g_slot_calls;
static int g_slot_stat = -1;
void w64_slotstat_report(uint64_t frames)
{
    if (!g_slot_calls || !frames) return;
    int top[16] = { 0 }; uint32_t topn[16] = { 0 };
    for (int i = 0; i < (int)W64_SENT_SLOTS; i++) {
        uint32_t n = g_slot_calls[i];
        if (!n) continue;
        for (int k = 0; k < 16; k++)
            if (n > topn[k]) {
                memmove(&top[k + 1], &top[k], (15 - k) * sizeof top[0]); memmove(&topn[k + 1], &topn[k], (15 - k) * sizeof topn[0]);
                top[k] = i; topn[k] = n; break;
            }
    }
    char buf[1024]; size_t o = 0;
    for (int k = 0; k < 16 && topn[k]; k++)
        o += (size_t)snprintf(buf + o, sizeof buf - o, " %s=%.1f", g_slots[top[k]].name ? g_slots[top[k]].name : "?", (double)topn[k] / (double)frames);
    w64_log("[glstat] calls/frame:%s", buf);
    memset(g_slot_calls, 0, W64_SENT_SLOTS * sizeof *g_slot_calls);
}

void dispatch(CPU *c, uint64_t target)
{
    uint64_t k = target - HLE_BASE;
    if (k < (uint64_t)W64_SENT_SLOTS * HLE_STRIDE) {
        uint64_t i = k / HLE_STRIDE;
        slot_t *s = &g_slots[i];
        if (g_slot_stat) {
            if (g_slot_stat < 0) { g_slot_stat = getenv("W64_GLSTAT") != NULL; if (g_slot_stat) g_slot_calls = calloc(W64_SENT_SLOTS, sizeof *g_slot_calls); }
            if (g_slot_calls) __atomic_fetch_add(&g_slot_calls[i], 1, __ATOMIC_RELAXED);
        }
        if (w64_trace && i < HLE_COUNT) w64_log("[imp] %s", s->name);
        t_cur_slot = i;
        if (!s->fn) unimpl_import(c);
        if (t_reg) { int64_t sv = t_reg->slot; t_reg->slot = (int64_t)i; s->fn(c); t_reg->slot = sv; }
        else s->fn(c);
        return;
    }
    void (*fn)(CPU *) = ht_lookup(target);
    if (fn) { fn(c); return; }
    /* An import/dynapi thunk (`jmp [rip+disp]`) that is only ever used through
     * its address never made it into the catalog: follow the pointer. */
    if (target >= w64_image_base && target + 7 < w64_image_base + w64_image_size) {
        const uint8_t *b = (const uint8_t *)(uintptr_t)target;
        int rex = b[0] == 0x48;
        if (b[rex] == 0xFF && b[rex + 1] == 0x25) {
            int32_t disp;
            memcpy(&disp, b + rex + 2, 4);
            uint64_t slot = target + (uint64_t)(rex + 6) + (uint64_t)(int64_t)disp;
            dispatch(c, rd64(slot));
            return;
        }
    }
    if (target == 0) w64_fatal(c, "call to NULL");
    w64_fatal(c, "dispatch to unknown guest address %#llx", (unsigned long long)target);
}

void dispatch_jmp(CPU *c, uint64_t target) { dispatch(c, target); }

/* eh64.c bookkeeping: the shadow call-stack depth is not tracked here. */
int  es3_cs_depth_get(void) { return 0; }
void es3_cs_depth_set(int d) { (void)d; }

/* ------------------------------------------------------------------ handles */
typedef struct { int type; void *obj; } hent_t;
#define HMAX 65536
static hent_t g_handles[HMAX];
static pthread_mutex_t g_h_mu = PTHREAD_MUTEX_INITIALIZER;

uint64_t w64_handle_new(int type, void *obj)
{
    pthread_mutex_lock(&g_h_mu);
    for (int i = 16; i < HMAX; i++)
        if (g_handles[i].type == H_FREE) {
            g_handles[i].type = type; g_handles[i].obj = obj;
            pthread_mutex_unlock(&g_h_mu);
            return (uint64_t)i * 4;
        }
    pthread_mutex_unlock(&g_h_mu);
    return 0;
}

static int h_index(uint64_t h) { return (h & 3) || h / 4 >= HMAX ? -1 : (int)(h / 4); }

void *w64_handle_get(uint64_t h, int type)
{
    int i = h_index(h);
    if (i < 0 || g_handles[i].type == H_FREE) return NULL;
    if (type && g_handles[i].type != type) return NULL;
    return g_handles[i].obj;
}

int w64_handle_type(uint64_t h) { int i = h_index(h); return i < 0 ? H_FREE : g_handles[i].type; }

void w64_handle_close(uint64_t h)
{
    int i = h_index(h);
    if (i < 0) return;
    pthread_mutex_lock(&g_h_mu);
    g_handles[i].type = H_FREE; g_handles[i].obj = NULL;
    pthread_mutex_unlock(&g_h_mu);
}

/* ------------------------------------------------------------------ TEB / TLS */
static uint8_t *g_peb;
static uint64_t g_tls_start, g_tls_end, g_tls_zero, g_tls_index_va, g_tls_callbacks;
static __thread CPU *t_cpu;
static atomic_uint g_next_tid = 0x1000;
__thread uint64_t w64_preset_tid;
uint64_t w64_alloc_tid(void) { return atomic_fetch_add(&g_next_tid, 4); }

void w64_set_last_error(uint32_t e) { if (w64_teb) memcpy(w64_teb + TEB_LAST_ERROR, &e, 4); }
uint32_t w64_get_last_error(void) { uint32_t e = 0; if (w64_teb) memcpy(&e, w64_teb + TEB_LAST_ERROR, 4); return e; }

static void run_tls_callbacks(CPU *c, uint32_t reason)
{
    if (!g_tls_callbacks) return;
    for (uint64_t p = g_tls_callbacks; rd64(p); p += 8)
        w64_call(c, rd64(p), 3, w64_image_base, (uint64_t)reason, 0ull);
}

/* Guest stack + TEB + implicit TLS block for the calling host thread. */
void w64_thread_attach(CPU *c)
{
    size_t stack_sz = 16u << 20;
    uint8_t *stk = mmap(NULL, stack_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (stk == MAP_FAILED) w64_fatal(NULL, "guest stack mmap failed");
    uint8_t *teb = calloc(1, TEB_SIZE);
    uint64_t hi = (uint64_t)(uintptr_t)(stk + stack_sz), lo = (uint64_t)(uintptr_t)stk;
    memcpy(teb + TEB_STACK_BASE, &hi, 8);
    memcpy(teb + TEB_STACK_LIMIT, &lo, 8);
    uint64_t self = (uint64_t)(uintptr_t)teb, peb = (uint64_t)(uintptr_t)g_peb;
    memcpy(teb + TEB_SELF, &self, 8);
    memcpy(teb + TEB_PEB, &peb, 8);
    uint64_t pid = 0x100, tid = w64_preset_tid ? w64_preset_tid : atomic_fetch_add(&g_next_tid, 4);
    memcpy(teb + TEB_PID, &pid, 8);
    memcpy(teb + TEB_TID, &tid, 8);
    if (g_tls_end > g_tls_start || g_tls_zero) {
        size_t raw = (size_t)(g_tls_end - g_tls_start);
        uint8_t *blk = calloc(1, raw + g_tls_zero + 64);
        memcpy(blk, (void *)(uintptr_t)g_tls_start, raw);
        uint64_t *arr = calloc(8, sizeof *arr);
        arr[0] = (uint64_t)(uintptr_t)blk;
        uint64_t ap = (uint64_t)(uintptr_t)arr;
        memcpy(teb + TEB_TLS_PTR, &ap, 8);
    }
    w64_teb = teb;
    memset(c, 0, sizeof *c);
    c->rsp = hi - 0x100;
    c->mxcsr = 0x1F80;
    t_cpu = c;
    pthread_mutex_lock(&g_slot_mu);
    t_reg = NULL;
    for (int i = 0; i < g_nthreads && !t_reg; i++) if (!g_threads[i].c) t_reg = &g_threads[i];   /* entry of an exited thread */
    if (!t_reg && g_nthreads < 256) t_reg = &g_threads[g_nthreads++];
    if (t_reg) { t_reg->c = c; t_reg->teb = teb; t_reg->slot = -1; t_reg->tid = tid; }
    g_live_threads++;
    pthread_mutex_unlock(&g_slot_mu);
}

CPU *w64_cpu(void)
{
    if (!t_cpu) {
        CPU *c = calloc(1, sizeof(CPU));
        w64_thread_attach(c);
        run_tls_callbacks(c, 2 /* DLL_THREAD_ATTACH */);
    }
    return t_cpu;
}

/* Undo w64_thread_attach when a guest thread ends (CreateThread / thread pool work): guest stack,
 * TEB (+ TLS expansion slots), implicit TLS block and the CPU struct. The game starts short-lived
 * threads all the time; keeping them would grow memory for as long as the game runs. */
void w64_thread_detach(void)
{
    CPU *c = t_cpu;
    uint8_t *teb = w64_teb;
    if (!c || !teb) return;
    pthread_mutex_lock(&g_slot_mu);
    if (t_reg) { t_reg->c = NULL; t_reg->teb = NULL; t_reg->slot = -1; t_reg->tid = 0; }
    g_live_threads--;
    pthread_mutex_unlock(&g_slot_mu);
    t_reg = NULL;
    uint64_t hi, lo, tlsp, ex;
    memcpy(&hi, teb + TEB_STACK_BASE, 8);
    memcpy(&lo, teb + TEB_STACK_LIMIT, 8);
    memcpy(&tlsp, teb + TEB_TLS_PTR, 8);
    memcpy(&ex, teb + TEB_TLS_EXP, 8);
    if (tlsp) { uint64_t *arr = (uint64_t *)(uintptr_t)tlsp; free((void *)(uintptr_t)arr[0]); free(arr); }
    if (ex) free((void *)(uintptr_t)ex);
    w64_teb = NULL;
    t_cpu = NULL;
    munmap((void *)(uintptr_t)lo, (size_t)(hi - lo));
    free(teb);
    free(c);
}

uint64_t w64_thread_id(void) { return w64_teb ? rd64((uint64_t)(uintptr_t)w64_teb + TEB_TID) : 0; }

/* ------------------------------------------------------------------ host -> guest */
#define W64_RET_MAGIC 0x00000000E5E2FFF0ull

uint64_t w64_callv(CPU *c, uint64_t fn, int n, const uint64_t *args, unsigned float_mask)
{
    uint64_t save_rsp = c->rsp, save_rip = c->rip;
    uint64_t sp = (c->rsp - 0x40) & ~15ull;
    int nst = n > 4 ? n - 4 : 0;
    sp -= (uint64_t)(0x20 + 8 * nst);
    sp &= ~15ull;
    for (int k = 0; k < nst; k++) wr64(sp + 0x20 + 8 * (uint64_t)k, args[4 + k]);
    uint64_t *r[4] = { &c->rcx, &c->rdx, &c->r8, &c->r9 };
    for (int k = 0; k < n && k < 4; k++) {
        *r[k] = args[k];
        if (float_mask & (1u << k)) { c->xmm[k].u64[0] = args[k]; c->xmm[k].u64[1] = 0; }
    }
    c->rsp = sp;
    push64(c, W64_RET_MAGIC);
    dispatch(c, fn);
    if (c->rsp != sp)
        w64_log("[call] %#llx returned with rsp off by %lld", (unsigned long long)fn, (long long)(c->rsp - sp));
    c->rsp = save_rsp;
    c->rip = save_rip;
    return c->rax;
}

uint64_t w64_call(CPU *c, uint64_t fn, int n, ...)
{
    uint64_t a[16] = { 0 };
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n && i < 16; i++) a[i] = va_arg(ap, uint64_t);
    va_end(ap);
    return w64_callv(c, fn, n, a, 0);
}

/* ------------------------------------------------------------------ loader */
static uint32_t u32at(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t u16at(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint64_t u64at(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

int w64_load(const char *exe_path)
{
    int fd = open(exe_path, O_RDONLY);
    if (fd < 0) { w64_log("cannot open %s: %s", exe_path, strerror(errno)); return -1; }
    struct stat st;
    fstat(fd, &st);
    uint8_t *f = malloc((size_t)st.st_size);
    if (read(fd, f, (size_t)st.st_size) != st.st_size) { close(fd); return -1; }
    close(fd);

    uint32_t pe = u32at(f + 0x3c);
    if (memcmp(f + pe, "PE\0\0", 4) || u16at(f + pe + 24) != 0x20b) { w64_log("not a PE32+ image"); return -1; }
    uint16_t nsec = u16at(f + pe + 6), optsz = u16at(f + pe + 20);
    const uint8_t *opt = f + pe + 24;
    uint64_t base = u64at(opt + 24);
    uint32_t soi = u32at(opt + 56), hdrs = u32at(opt + 60), entry = u32at(opt + 16);

    /* Reserve the sentinel range so no host allocation can land there. */
    void *sres = mmap((void *)(uintptr_t)(HLE_BASE & ~0xFFFFull), (size_t)W64_SENT_SLOTS * HLE_STRIDE + 0x10000,
                      PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (sres == MAP_FAILED) w64_log("warning: could not reserve the sentinel range (%s)", strerror(errno));

    void *img = mmap((void *)(uintptr_t)base, soi, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (img == MAP_FAILED || (uint64_t)(uintptr_t)img != base) {
        w64_log("cannot map the image at its preferred base %#llx: %s", (unsigned long long)base, strerror(errno));
        return -1;
    }
    memcpy(img, f, hdrs);
    const uint8_t *sh = opt + optsz;
    for (int i = 0; i < nsec; i++, sh += 40) {
        uint32_t vsz = u32at(sh + 8), va = u32at(sh + 12), rsz = u32at(sh + 16), raw = u32at(sh + 20);
        uint32_t n = rsz < vsz || !vsz ? rsz : vsz;
        if (raw && n) memcpy((uint8_t *)img + va, f + raw, n);
    }
    w64_image_base = base;
    w64_image_size = soi;
    w64_entry = base + entry;
    es3_eh_image_base = base;
    g_image_delta = 0;

    /* IAT -> sentinels */
    for (uint32_t i = 0; i < HLE_COUNT; i++) {
        const hle_import_t *im = &recomp_imports[i];
        g_slots[i].dll = im->dll;
        g_slots[i].name = im->name;
        g_slots[i].fn = w64_find(im->dll, im->name);
        wr64(im->slot, HLE_SENTINEL(i));
    }
    int missing = 0;
    for (uint32_t i = 0; i < HLE_COUNT; i++) if (!g_slots[i].fn) missing++;
    w64_log("[load] %s at %#llx, %u bytes, entry %#llx, %u imports (%d not implemented)",
            exe_path, (unsigned long long)base, soi, (unsigned long long)w64_entry, HLE_COUNT, missing);
    if (getenv("W64_LIST_MISSING"))
        for (uint32_t i = 0; i < HLE_COUNT; i++)
            if (!g_slots[i].fn) w64_log("  missing %s!%s", g_slots[i].dll, g_slots[i].name);

    /* TLS directory (data dir 9) */
    uint32_t tls_rva = u32at(opt + 112 + 9 * 8);
    if (tls_rva) {
        const uint8_t *t = (const uint8_t *)img + tls_rva;
        g_tls_start = u64at(t);
        g_tls_end = u64at(t + 8);
        g_tls_index_va = u64at(t + 16);
        g_tls_callbacks = u64at(t + 24);
        g_tls_zero = u32at(t + 32);
        wr32(g_tls_index_va, 0);
    }

    /* PEB: the fields the static CRT and common library code read directly */
    g_peb = calloc(1, 0x1000);
    memcpy(g_peb + 0x10, &base, 8);          /* ImageBaseAddress */
    {
        uint8_t *pp = calloc(1, 0x800);      /* RTL_USER_PROCESS_PARAMETERS */
        uint32_t ml = 0x800;
        memcpy(pp, &ml, 4); memcpy(pp + 4, &ml, 4);
        static uint16_t img[] = u"C:\\Game\\Mewgenics.exe", cmd[] = u"\"C:\\Game\\Mewgenics.exe\"", cwd[] = u"C:\\Game\\";
        uint16_t l;
        uint64_t q;
        l = (uint16_t)(sizeof img - 2); memcpy(pp + 0x60, &l, 2); l += 2; memcpy(pp + 0x62, &l, 2); q = (uint64_t)(uintptr_t)img; memcpy(pp + 0x68, &q, 8);
        l = (uint16_t)(sizeof cmd - 2); memcpy(pp + 0x70, &l, 2); l += 2; memcpy(pp + 0x72, &l, 2); q = (uint64_t)(uintptr_t)cmd; memcpy(pp + 0x78, &q, 8);
        l = (uint16_t)(sizeof cwd - 2); memcpy(pp + 0x38, &l, 2); l += 2; memcpy(pp + 0x3A, &l, 2); q = (uint64_t)(uintptr_t)cwd; memcpy(pp + 0x40, &q, 8);
        q = (uint64_t)(uintptr_t)pp; memcpy(g_peb + 0x20, &q, 8);
        uint8_t *ldr = calloc(1, 0x100);     /* PEB_LDR_DATA with empty module lists */
        uint32_t len = 0x58, init = 1;
        memcpy(ldr, &len, 4); memcpy(ldr + 4, &init, 4);
        for (int k = 0; k < 3; k++) { q = (uint64_t)(uintptr_t)(ldr + 0x10 + 16 * k); memcpy(ldr + 0x10 + 16 * k, &q, 8); memcpy(ldr + 0x18 + 16 * k, &q, 8); }
        q = (uint64_t)(uintptr_t)ldr; memcpy(g_peb + 0x18, &q, 8);
        q = 0x150000ull; memcpy(g_peb + 0x30, &q, 8);   /* ProcessHeap */
        uint32_t v;
        v = 10; memcpy(g_peb + 0x118, &v, 4);           /* OSMajorVersion */
        v = 0; memcpy(g_peb + 0x11C, &v, 4);
        uint16_t b = 19045; memcpy(g_peb + 0x120, &b, 2);
        v = 2; memcpy(g_peb + 0x124, &v, 4);            /* OSPlatformId */
        v = 8; memcpy(g_peb + 0xB8, &v, 4);             /* NumberOfProcessors */
    }
    ht_build();
    free(f);
    return 0;
}

static void *watchdog(void *arg)
{
    int secs = (int)(intptr_t)arg;
    for (;;) {
        sleep((unsigned)secs);
        { void w64_live_dump(void); if (getenv("W64_ALLOCLOG")) w64_live_dump(); }
        pthread_mutex_lock(&g_slot_mu);
        w64_log("[watch] ---- %d threads ----", g_live_threads);
        for (int i = 0; i < g_nthreads; i++) {
            treg_t *t = &g_threads[i];
            CPU *c = t->c;
            if (!c) continue;
            char buf[512];
            size_t o = 0;
            uint64_t base = rd64((uint64_t)(uintptr_t)t->teb + TEB_STACK_BASE);
            int n = 0;
            for (uint64_t p = c->rsp; p + 8 <= base && n < 8 && p < c->rsp + 0x4000; p += 8) {
                uint64_t v = rd64(p);
                if (v > w64_image_base + 0x1000 && v < w64_image_base + w64_image_size && v < 0x140e3f000ull) {
                    o += (size_t)snprintf(buf + o, sizeof buf - o, " %llx", (unsigned long long)v); n++;
                }
            }
            buf[o] = 0;
            w64_log("[watch] tid %#llx rip %#llx in %s |%s", (unsigned long long)t->tid, (unsigned long long)c->rip,
                    t->slot >= 0 ? (g_slots[t->slot].name ? g_slots[t->slot].name : "?") : "-", buf);
        }
        pthread_mutex_unlock(&g_slot_mu);
    }
    return NULL;
}

/* ---- sampling profiler (W64_PROF=1): guest rip of every thread at ~1 kHz ---- */
static uint64_t func_of(uint64_t rip)
{
    int lo = 0, hi = (int)recomp_dispatch_count - 1, best = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (recomp_dispatch_table[m].va <= rip) { best = m; lo = m + 1; } else hi = m - 1;
    }
    return best >= 0 ? recomp_dispatch_table[best].va : 0;
}
#define PROF_N 8192
static struct { uint64_t va; uint64_t n; } g_prof[PROF_N];
static void prof_add(uint64_t f)
{
    uint32_t h = (uint32_t)((f * 0x9E3779B97F4A7C15ull) >> 51) & (PROF_N - 1);
    for (int k = 0; k < PROF_N; k++, h = (h + 1) & (PROF_N - 1)) {
        if (g_prof[h].va == f) { g_prof[h].n++; return; }
        if (!g_prof[h].va) { g_prof[h].va = f; g_prof[h].n = 1; return; }
    }
}
static void *profiler(void *arg)
{
    /* every 20 s: top guest functions and host imports (by name) of that window, per mille of
     * the samples of all threads; idle waits inside imports count too, so read them as "where
     * the threads are", not only "what burns CPU" */
    (void)arg;
    uint64_t last = w64_now_ns(), total = 0;
    for (;;) {
        usleep(1000);
        pthread_mutex_lock(&g_slot_mu);
        for (int i = 0; i < g_nthreads; i++) {
            treg_t *t = &g_threads[i];
            if (!t->c) continue;
            int64_t sl = t->slot;
            if (sl >= 0) { prof_add(HLE_SENTINEL(sl)); total++; continue; }   /* inside an import */
            uint64_t rip = t->c->rip;
            if (!rip) continue;
            prof_add(func_of(rip)); total++;
        }
        pthread_mutex_unlock(&g_slot_mu);
        if (w64_now_ns() - last > 20000000000ull) {
            last = w64_now_ns();
            uint64_t top[20] = { 0 }, topn[20] = { 0 };
            for (int h = 0; h < PROF_N; h++) {
                if (!g_prof[h].va) continue;
                for (int k = 0; k < 20; k++)
                    if (g_prof[h].n > topn[k]) {
                        memmove(&top[k + 1], &top[k], (19 - k) * 8); memmove(&topn[k + 1], &topn[k], (19 - k) * 8);
                        top[k] = g_prof[h].va; topn[k] = g_prof[h].n; break;
                    }
            }
            w64_log("[prof] %llu samples (%d threads live, %d registry entries)", (unsigned long long)total, g_live_threads, g_nthreads);
            for (int k = 0; k < 20 && top[k]; k++) {
                uint64_t va = top[k];
                const char *nm = NULL;
                if (va >= HLE_SENTINEL(0) && va < HLE_SENTINEL(W64_SENT_SLOTS)) nm = g_slots[(va - HLE_SENTINEL(0)) / 8].name;
                w64_log("[prof] %5.1f%% %#llx %s", 100.0 * (double)topn[k] / (double)(total ? total : 1),
                        (unsigned long long)va, nm ? nm : "");
            }
            memset(g_prof, 0, sizeof g_prof);
            total = 0;
        }
    }
    return NULL;
}

int w64_run_entry(void)
{
    if (getenv("W64_PROF")) { pthread_t th; pthread_create(&th, NULL, profiler, NULL); }
    if (getenv("W64_WATCH")) { pthread_t th; pthread_create(&th, NULL, watchdog, (void *)(intptr_t)atoi(getenv("W64_WATCH"))); }
    CPU *c = calloc(1, sizeof(CPU));
    w64_thread_attach(c);
    run_tls_callbacks(c, 1 /* DLL_PROCESS_ATTACH */);
    w64_log("[run] entry %#llx", (unsigned long long)w64_entry);
    uint64_t r = w64_call(c, w64_entry, 0);
    w64_log("[run] entry returned %#llx", (unsigned long long)r);
    return (int)r;
}
