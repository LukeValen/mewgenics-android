#pragma once
#include "w64.h"

typedef struct { const char *name; w64_fn fn; } sdl_proc_t;
extern const sdl_proc_t sdl_procs[];
extern const unsigned sdl_proc_count;

#define SDLT_TRACE(n) do { if (w64_trace >= 2) w64_log("[sdl] %s", n); } while (0)

/* printf with Win64 argument passing: either the remaining register/stack
 * arguments of the current host-function call, or a Win64 va_list (a pointer
 * to consecutive 8-byte slots). */
typedef struct { CPU *c; int idx; uint64_t va; } w64_va;
static inline uint64_t w64_va_next(w64_va *v)
{
    if (v->c) return w64_arg(v->c, v->idx++);
    uint64_t r = rd64(v->va); v->va += 8; return r;
}
int w64_vformat(char *out, size_t cap, const char *fmt, w64_va *v);   /* returns full length like vsnprintf */

/* GL bridge */
uint64_t w64_gl_proc(const char *name);      /* sentinel for a GL entry point, 0 if unknown */
void w64_gl_on_context(void);                /* after a context became current */
extern int w64_gles;                         /* 1 = host context is OpenGL ES */
