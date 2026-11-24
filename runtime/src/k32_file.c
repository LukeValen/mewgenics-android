/*
 * k32_file.c - kernel32 files, directories, find, mappings, console handles, paths.
 *
 * Path model: the game sees itself installed at C:\Game (GetModuleFileName,
 * current directory). C:\Game\... maps to w64_gamedir, every other C:\ path to
 * w64_userdir/C/..., and an absolute host path ("/...", which SDL_GetBasePath /
 * SDL_GetPrefPath hand out) is used as is. Lookups fall back to a
 * case-insensitive walk, as NTFS would match.
 */
#define _GNU_SOURCE
#include "w64.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

char w64_gamedir[1024] = ".";
char w64_userdir[1024] = "./user";
static const char *VGAME = "C:\\Game";

#define INVALID_HANDLE ((uint64_t)-1)
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NO_MORE_FILES 18
#define ERROR_FILE_EXISTS 80
#define ERROR_ALREADY_EXISTS 183
#define ERROR_DIR_NOT_EMPTY 145

/* ------------------------------------------------------------------ paths */
static int ieq_prefix(const char *s, const char *p)
{
    for (; *p; s++, p++) {
        char a = *s, b = *p;
        if (a == '\\') a = '/';
        if (b == '\\') b = '/';
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

/* resolve each component case-insensitively when the exact path does not exist */
static void ci_resolve(char *p)
{
    struct stat st;
    if (!*p || lstat(p, &st) == 0) return;
    char out[2048];
    size_t on = 0;
    const char *s = p;
    if (*s == '/') { out[on++] = '/'; s++; }
    out[on] = 0;
    while (*s) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        char comp[512];
        if (n >= sizeof comp) return;
        memcpy(comp, s, n); comp[n] = 0;
        char trial[2048];
        snprintf(trial, sizeof trial, "%s%s", out, comp);
        if (lstat(trial, &st) != 0) {
            DIR *d = opendir(on ? out : ".");
            int found = 0;
            if (d) {
                struct dirent *de;
                while ((de = readdir(d)))
                    if (!strcasecmp(de->d_name, comp)) { snprintf(comp, sizeof comp, "%s", de->d_name); found = 1; break; }
                closedir(d);
            }
            if (!found) return;     /* leave the original; caller gets ENOENT */
        }
        on += (size_t)snprintf(out + on, sizeof out - on, "%s%s", comp, e ? "/" : "");
        if (!e) break;
        s = e + 1;
    }
    snprintf(p, 1024, "%s", out);
}

void w64_host_path(const char *win, char *out, size_t cap)
{
    char tmp[1024];
    const char *s = win;
    if (!strncmp(s, "\\\\?\\", 4) || !strncmp(s, "//?/", 4)) s += 4;
    if (s[0] == '/' ) {
        snprintf(tmp, sizeof tmp, "%s", s);
    } else if (s[0] && s[1] == ':') {
        if (ieq_prefix(s, VGAME) && (s[7] == 0 || s[7] == '\\' || s[7] == '/'))
            snprintf(tmp, sizeof tmp, "%s%s", w64_gamedir, s + 7);
        else
            snprintf(tmp, sizeof tmp, "%s/%c%s", w64_userdir, (s[0] & ~32), s + 2);
    } else {
        snprintf(tmp, sizeof tmp, "%s/%s", w64_gamedir, s);
    }
    for (char *p = tmp; *p; p++) if (*p == '\\') *p = '/';
    /* collapse "//" and "/./" */
    char *w = tmp;
    for (char *r = tmp; *r; ) {
        if (r[0] == '/' && r[1] == '/') { r++; continue; }
        if (r[0] == '/' && r[1] == '.' && (r[2] == '/' || !r[2])) { r += 2; continue; }
        *w++ = *r++;
    }
    *w = 0;
    ci_resolve(tmp);
    snprintf(out, cap, "%s", tmp);
}

static void host_path_w(const uint16_t *wn, char *out, size_t cap)
{
    char *u = w16_to_utf8(wn, -1);
    w64_host_path(u, out, cap);
    free(u);
}

static void mkdirs_for(const char *path)
{
    char t[1024];
    snprintf(t, sizeof t, "%s", path);
    for (char *p = t + 1; *p; p++)
        if (*p == '/') { *p = 0; mkdir(t, 0777); *p = '/'; }
}

static uint32_t errno_to_win(int e)
{
    switch (e) {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EROFS: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_FILE_EXISTS;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    }
    return 31; /* ERROR_GEN_FAILURE */
}

/* ------------------------------------------------------------------ file objects */
typedef struct { int fd; int is_dir; int std; char path[1024]; } fobj_t;
#define STD_IN  0x00000000F0000010ull
#define STD_OUT 0x00000000F0000014ull
#define STD_ERR 0x00000000F0000018ull

static fobj_t g_std[3] = { { 0, 0, 1, "stdin" }, { 1, 0, 1, "stdout" }, { 2, 0, 1, "stderr" } };

static fobj_t *fobj(uint64_t h)
{
    if (h == STD_IN) return &g_std[0];
    if (h == STD_OUT) return &g_std[1];
    if (h == STD_ERR) return &g_std[2];
    return w64_handle_get(h, H_FILE);
}

static uint64_t ft_from_ts(struct timespec ts) { return 116444736000000000ull + (uint64_t)ts.tv_sec * 10000000ull + (uint64_t)ts.tv_nsec / 100; }

static uint32_t attrs_of(const struct stat *st, const char *name)
{
    uint32_t a = S_ISDIR(st->st_mode) ? 0x10 : 0x80;         /* DIRECTORY : NORMAL */
    if (name) { const char *b = strrchr(name, '/'); b = b ? b + 1 : name; if (b[0] == '.' && b[1] && b[1] != '.') a |= 2; }
    return a;
}

/* Memory-saving defaults for phones and tablets (W64_LOWMEM, on by default on Android): the game
 * keeps ~4.7 GB on the heap at full quality, above what Android lets a foreground app hold.
 * Its own settings texture_scale_* (bitmap resolution) and curve_resolution (vector tessellation)
 * bring that down; missing keys are added to settings.txt before the game reads it. */
static int lowmem(void)
{
    const char *e = getenv("W64_LOWMEM");
#ifdef __ANDROID__
    return !e || atoi(e) != 0;
#else
    return e && atoi(e) != 0;
#endif
}
static void settings_defaults(const char *path)
{
    /* values forced on every start; none of them is in the game's settings menu */
    static const char *keys[][2] = {
        { "texture_scale_big", "0.5" }, { "curve_resolution", "2.5" }, { "soundeffect_cache_size_mb", "64" },
        { "antialias", "0" }, { NULL, NULL }
    };
    static char buf[65536], out[66000];
    FILE *f = fopen(path, "rb");
    size_t n = 0, o = 0;
    if (f) { n = fread(buf, 1, sizeof buf - 1, f); fclose(f); }
    buf[n] = 0;
    int done[8] = { 0 }, changed = 0;
    const char *eol = strstr(buf, "\r\n") ? "\r\n" : "\n";
    for (char *line = buf; *line; ) {
        char *e = strchr(line, '\n');
        size_t len = e ? (size_t)(e - line + 1) : strlen(line);
        int k;
        for (k = 0; keys[k][0]; k++) {
            size_t kl = strlen(keys[k][0]);
            if (!strncmp(line, keys[k][0], kl) && line[kl] == ' ') break;
        }
        if (keys[k][0]) {
            char want[128];
            snprintf(want, sizeof want, "%s %s%s", keys[k][0], keys[k][1], eol);
            if (strncmp(line, want, len) || strlen(want) != len) changed = 1;
            o += (size_t)snprintf(out + o, sizeof out - o, "%s", want);
            done[k] = 1;
        } else if (o + len < sizeof out) { memcpy(out + o, line, len); o += len; }
        line += len;
    }
    if (o && out[o - 1] != '\n') o += (size_t)snprintf(out + o, sizeof out - o, "%s", eol);
    for (int k = 0; keys[k][0]; k++)
        if (!done[k]) { o += (size_t)snprintf(out + o, sizeof out - o, "%s %s%s", keys[k][0], keys[k][1], eol); changed = 1; }
    if (!changed) return;
    mkdirs_for(path);
    f = fopen(path, "wb");
    if (!f) return;
    fwrite(out, 1, o, f);
    fclose(f);
    w64_log("[lowmem] %s: texture_scale_big 0.5, curve_resolution 2.5, soundeffect_cache 64 MB, antialias 0", path);
}

static uint64_t open_file(const char *path, uint32_t access, uint32_t disp, uint32_t flags)
{
    size_t pl = strlen(path);
    if (lowmem() && pl > 13 && !strcmp(path + pl - 13, "/settings.txt") && !(access & 0x40000000)) settings_defaults(path);
    if (!strcasecmp(path + (strlen(path) > 4 ? strlen(path) - 4 : 0), "/nul") ) {
        int fd = open("/dev/null", O_RDWR);
        fobj_t *o = calloc(1, sizeof *o); o->fd = fd;
        return w64_handle_new(H_FILE, o);
    }
    struct stat st;
    int exists = stat(path, &st) == 0;
    if (exists && S_ISDIR(st.st_mode)) {
        if (!(flags & 0x02000000)) { w64_set_last_error(ERROR_ACCESS_DENIED); return INVALID_HANDLE; }   /* BACKUP_SEMANTICS */
        int fd = open(path, O_RDONLY | O_DIRECTORY);
        if (fd < 0) { w64_set_last_error(errno_to_win(errno)); return INVALID_HANDLE; }
        fobj_t *o = calloc(1, sizeof *o); o->fd = fd; o->is_dir = 1; snprintf(o->path, sizeof o->path, "%s", path);
        return w64_handle_new(H_FILE, o);
    }
    int wr = (access & 0x40000000) || (access & 0x10000000) || (access & 0x2) || (access & 0x4);
    int rd = (access & 0x80000000) || (access & 0x10000000) || (access & 0x1) || !wr;
    int of = (rd && wr) ? O_RDWR : wr ? O_WRONLY : O_RDONLY;
    switch (disp) {
    case 1: if (exists) { w64_set_last_error(ERROR_FILE_EXISTS); return INVALID_HANDLE; } of |= O_CREAT | O_EXCL; break;
    case 2: of |= O_CREAT | O_TRUNC; break;
    case 3: if (!exists) { w64_set_last_error(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE; } break;
    case 4: of |= O_CREAT; break;
    case 5: if (!exists) { w64_set_last_error(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE; } of |= O_TRUNC; break;
    }
    if (of & O_CREAT) mkdirs_for(path);
    int fd = open(path, of | O_CLOEXEC, 0666);
    if (fd < 0 && (of & O_ACCMODE) == O_RDWR && errno == EACCES) fd = open(path, (of & ~O_ACCMODE) | O_RDONLY | O_CLOEXEC, 0666);
    if (fd < 0) { w64_set_last_error(errno_to_win(errno)); return INVALID_HANDLE; }
    fobj_t *o = calloc(1, sizeof *o); o->fd = fd; snprintf(o->path, sizeof o->path, "%s", path);
    w64_set_last_error(exists && (disp == 2 || disp == 4) ? ERROR_ALREADY_EXISTS : 0);
    if (w64_trace) w64_log("[file] open %s -> fd %d", path, fd);
    return w64_handle_new(H_FILE, o);
}

static void CreateFileW_(CPU *c)
{
    char *u = w16_to_utf8(AW(0), -1);
    char p[1024];
    if (!strcmp(u, "CONOUT$")) { free(u); RET(STD_OUT); }
    if (!strcmp(u, "CONIN$")) { free(u); RET(STD_IN); }
    w64_host_path(u, p, sizeof p);
    uint64_t h = open_file(p, AU(1), AU(4), AU(5));
    if (h == INVALID_HANDLE && w64_trace) w64_log("[file] CreateFileW %s (%s) failed %u", u, p, w64_get_last_error());
    free(u);
    RET(h);
}
static void CreateFileA_(CPU *c)
{
    char p[1024];
    w64_host_path(AS(0), p, sizeof p);
    RET(open_file(p, AU(1), AU(4), AU(5)));
}

static void ReadFile_(CPU *c)
{
    fobj_t *o = fobj(A(0));
    if (!o) { w64_set_last_error(ERROR_INVALID_HANDLE); RET(0); }
    uint64_t ov = A(4);
    ssize_t n;
    if (ov) {
        off_t off = (off_t)(rd32(ov + 16) | ((uint64_t)rd32(ov + 20) << 32));
        n = pread(o->fd, AP(1), AU(2), off);
        if (n >= 0) { wr64(ov, 0); wr64(ov + 8, (uint64_t)n); }
    } else {
        n = read(o->fd, AP(1), AU(2));
    }
    if (n < 0) { w64_set_last_error(errno_to_win(errno)); if (A(3)) wr32(A(3), 0); RET(0); }
    if (A(3)) wr32(A(3), (uint32_t)n);
    if (ov && n == 0 && AU(2)) { w64_set_last_error(38 /* ERROR_HANDLE_EOF */); RET(0); }
    RET(1);
}
static void WriteFile_(CPU *c)
{
    fobj_t *o = fobj(A(0));
    if (!o) { w64_set_last_error(ERROR_INVALID_HANDLE); RET(0); }
    uint64_t ov = A(4);
    ssize_t n;
    if (o->std) {
        if (o->fd == 1 || o->fd == 2) { char b[4096]; size_t k = AU(2) < sizeof b - 1 ? AU(2) : sizeof b - 1; memcpy(b, AP(1), k); b[k] = 0; w64_log("[guest] %s", b); }
        n = AU(2);
    } else if (ov) {
        off_t off = (off_t)(rd32(ov + 16) | ((uint64_t)rd32(ov + 20) << 32));
        n = pwrite(o->fd, AP(1), AU(2), off);
    } else {
        n = write(o->fd, AP(1), AU(2));
    }
    if (n < 0) { w64_set_last_error(errno_to_win(errno)); RET(0); }
    if (A(3)) wr32(A(3), (uint32_t)n);
    RET(1);
}
static void SetFilePointer_(CPU *c)
{
    fobj_t *o = fobj(A(0));
    if (!o) RET(0xFFFFFFFFu);
    int64_t d = AI(1);
    if (A(2)) d = (int64_t)(((uint64_t)rd32(A(2)) << 32) | AU(1));
    off_t r = lseek(o->fd, d, AU(3) == 0 ? SEEK_SET : AU(3) == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) { w64_set_last_error(131); RET(0xFFFFFFFFu); }
    if (A(2)) wr32(A(2), (uint32_t)((uint64_t)r >> 32));
    w64_set_last_error(0);
    RET((uint32_t)r);
}
static void SetFilePointerEx_(CPU *c)
{
    fobj_t *o = fobj(A(0));
    if (!o) RET(0);
    off_t r = lseek(o->fd, (off_t)(int64_t)A(1), AU(3) == 0 ? SEEK_SET : AU(3) == 1 ? SEEK_CUR : SEEK_END);
    if (r < 0) { w64_set_last_error(131); RET(0); }
    if (A(2)) wr64(A(2), (uint64_t)r);
    RET(1);
}
static void GetFileSize_(CPU *c)
{
    fobj_t *o = fobj(A(0));
    struct stat st;
    if (!o || fstat(o->fd, &st)) RET(0xFFFFFFFFu);
    if (A(1)) wr32(A(1), (uint32_t)((uint64_t)st.st_size >> 32));
    RET((uint32_t)st.st_size);
}
static void GetFileSizeEx_(CPU *c)
{
    fobj_t *o = fobj(A(0));
    struct stat st;
    if (!o || fstat(o->fd, &st)) RET(0);
    wr64(A(1), (uint64_t)st.st_size);
    RET(1);
}
static void GetFileType_(CPU *c)
{
    fobj_t *o = fobj(A(0));
    if (!o) RET(0);
    RET(o->std ? 2 /* CHAR */ : 1 /* DISK */);
}
static void SetEndOfFile_(CPU *c)
{
    fobj_t *o = fobj(A(0));
    if (!o) RET(0);
    off_t p = lseek(o->fd, 0, SEEK_CUR);
    RET(ftruncate(o->fd, p) == 0);
}
static void FlushFileBuffers_(CPU *c) { fobj_t *o = fobj(A(0)); if (o && !o->std) fsync(o->fd); RET(1); }
static void GetFileTime_(CPU *c)
{
    fobj_t *o = fobj(A(0));
    struct stat st;
    if (!o || fstat(o->fd, &st)) RET(0);
    if (A(1)) wr64(A(1), ft_from_ts(st.st_ctim));
    if (A(2)) wr64(A(2), ft_from_ts(st.st_atim));
    if (A(3)) wr64(A(3), ft_from_ts(st.st_mtim));
    RET(1);
}
static void GetFileInformationByHandleEx_(CPU *c)
{
    fobj_t *o = fobj(A(0));
    struct stat st;
    if (!o || fstat(o->fd, &st)) { w64_set_last_error(ERROR_INVALID_HANDLE); RET(0); }
    uint32_t cls = AU(1);
    uint8_t *b = AP(2);
    uint32_t n = AU(3);
    memset(b, 0, n);
    if (cls == 0 && n >= 40) {           /* FileBasicInfo */
        uint64_t t;
        t = ft_from_ts(st.st_ctim); memcpy(b, &t, 8);
        t = ft_from_ts(st.st_atim); memcpy(b + 8, &t, 8);
        t = ft_from_ts(st.st_mtim); memcpy(b + 16, &t, 8); memcpy(b + 24, &t, 8);
        uint32_t a = attrs_of(&st, o->path); memcpy(b + 32, &a, 4);
        RET(1);
    }
    if (cls == 1 && n >= 24) {           /* FileStandardInfo */
        uint64_t sz = (uint64_t)st.st_size, al = (sz + 4095) & ~4095ull;
        uint32_t links = (uint32_t)st.st_nlink;
        memcpy(b, &al, 8); memcpy(b + 8, &sz, 8); memcpy(b + 16, &links, 4);
        b[21] = S_ISDIR(st.st_mode);
        RET(1);
    }
    if (cls == 9 && n >= 8) {            /* FileAttributeTagInfo */
        uint32_t a = attrs_of(&st, o->path); memcpy(b, &a, 4);
        RET(1);
    }
    if (cls == 18 && n >= 24) {          /* FileIdInfo */
        uint64_t vs = (uint64_t)st.st_dev, id = (uint64_t)st.st_ino;
        memcpy(b, &vs, 8); memcpy(b + 8, &id, 8);
        RET(1);
    }
    w64_set_last_error(87);
    RET(0);
}
static void SetFileInformationByHandle_(CPU *c)
{
    fobj_t *o = fobj(A(0));
    if (!o) RET(0);
    uint32_t cls = AU(1);
    uint8_t *b = AP(2);
    if (cls == 4 || cls == 21) {         /* FileDispositionInfo(Ex): delete */
        if (b[0] & 1) { if (o->is_dir) rmdir(o->path); else unlink(o->path); }
        RET(1);
    }
    if (cls == 3 || cls == 22) {         /* FileRenameInfo(Ex) */
        uint32_t len; memcpy(&len, b + 16, 4);
        char *u = w16_to_utf8((const uint16_t *)(b + 20), (int)(len / 2));
        char np[1024];
        w64_host_path(u, np, sizeof np);
        free(u);
        if (rename(o->path, np)) { w64_set_last_error(errno_to_win(errno)); RET(0); }
        snprintf(o->path, sizeof o->path, "%s", np);
        RET(1);
    }
    RET(1);
}

/* ------------------------------------------------------------------ mappings */
typedef struct { int fd; uint64_t size; uint32_t prot; } mapobj_t;
static void CreateFileMappingW_(CPU *c)
{
    fobj_t *o = fobj(A(0));
    mapobj_t *m = calloc(1, sizeof *m);
    m->fd = o ? o->fd : -1;
    m->size = ((uint64_t)AU(3) << 32) | AU(4);
    m->prot = AU(2);
    if (m->fd >= 0 && m->size) {
        struct stat st;
        if (!fstat(m->fd, &st) && (uint64_t)st.st_size < m->size && (m->prot & 0x04)) if (ftruncate(m->fd, (off_t)m->size)) {}
    }
    RET(w64_handle_new(H_MAPPING, m));
}
static void MapViewOfFile_(CPU *c)
{
    mapobj_t *m = w64_handle_get(A(0), H_MAPPING);
    if (!m) RET(0);
    uint64_t off = ((uint64_t)AU(2) << 32) | AU(3), n = A(4);
    if (!n) {
        if (m->fd >= 0) { struct stat st; fstat(m->fd, &st); n = (uint64_t)st.st_size - off; }
        else n = m->size;
    }
    int prot = PROT_READ | ((AU(1) & 2) ? PROT_WRITE : 0);
    void *p = m->fd >= 0 ? mmap(NULL, n, prot, (AU(1) & 1) ? MAP_PRIVATE : MAP_SHARED, m->fd, (off_t)off)
                         : mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    RET(p == MAP_FAILED ? 0 : (uintptr_t)p);
}
static void UnmapViewOfFile_(CPU *c) { RET(1); }    /* size unknown; views are few and small */
static void FlushViewOfFile_(CPU *c) { RET(1); }

/* ------------------------------------------------------------------ attributes, dirs */
static int fill_attr_data(const char *p, uint8_t *b)
{
    struct stat st;
    if (stat(p, &st)) return 0;
    uint32_t a = attrs_of(&st, p);
    uint64_t t;
    memcpy(b, &a, 4);
    t = ft_from_ts(st.st_ctim); memcpy(b + 4, &t, 8);
    t = ft_from_ts(st.st_atim); memcpy(b + 12, &t, 8);
    t = ft_from_ts(st.st_mtim); memcpy(b + 20, &t, 8);
    uint32_t hi = (uint32_t)((uint64_t)st.st_size >> 32), lo = (uint32_t)st.st_size;
    if (S_ISDIR(st.st_mode)) hi = lo = 0;
    memcpy(b + 28, &hi, 4); memcpy(b + 32, &lo, 4);
    return 1;
}
static void GetFileAttributesW_(CPU *c)
{
    char p[1024]; struct stat st;
    host_path_w(AW(0), p, sizeof p);
    if (stat(p, &st)) { w64_set_last_error(errno_to_win(errno)); RET(0xFFFFFFFFu); }
    RET(attrs_of(&st, p));
}
static void GetFileAttributesA_(CPU *c)
{
    char p[1024]; struct stat st;
    w64_host_path(AS(0), p, sizeof p);
    if (stat(p, &st)) { w64_set_last_error(errno_to_win(errno)); RET(0xFFFFFFFFu); }
    RET(attrs_of(&st, p));
}
static void GetFileAttributesExW_(CPU *c)
{
    char p[1024];
    host_path_w(AW(0), p, sizeof p);
    if (!fill_attr_data(p, AP(2))) { w64_set_last_error(errno_to_win(errno)); RET(0); }
    RET(1);
}
static void CreateDirectoryW_(CPU *c)
{
    char p[1024];
    host_path_w(AW(0), p, sizeof p);
    mkdirs_for(p);
    if (mkdir(p, 0777)) { w64_set_last_error(errno == EEXIST ? ERROR_ALREADY_EXISTS : errno_to_win(errno)); RET(0); }
    RET(1);
}
static void RemoveDirectoryW_(CPU *c)
{
    char p[1024];
    host_path_w(AW(0), p, sizeof p);
    if (rmdir(p)) { w64_set_last_error(errno_to_win(errno)); RET(0); }
    RET(1);
}
static void DeleteFileW_(CPU *c)
{
    char p[1024];
    host_path_w(AW(0), p, sizeof p);
    if (unlink(p)) { w64_set_last_error(errno_to_win(errno)); RET(0); }
    RET(1);
}
static void DeleteFileA_(CPU *c)
{
    char p[1024];
    w64_host_path(AS(0), p, sizeof p);
    if (unlink(p)) { w64_set_last_error(errno_to_win(errno)); RET(0); }
    RET(1);
}
static void MoveFileExW_(CPU *c)
{
    char a[1024], b[1024];
    host_path_w(AW(0), a, sizeof a);
    host_path_w(AW(1), b, sizeof b);
    if (!(AU(2) & 1) && access(b, F_OK) == 0) { w64_set_last_error(ERROR_ALREADY_EXISTS); RET(0); }
    if (rename(a, b)) { w64_set_last_error(errno_to_win(errno)); RET(0); }
    RET(1);
}
static void CopyFileExW_(CPU *c)
{
    char a[1024], b[1024];
    host_path_w(AW(0), a, sizeof a);
    host_path_w(AW(1), b, sizeof b);
    int in = open(a, O_RDONLY);
    if (in < 0) { w64_set_last_error(errno_to_win(errno)); RET(0); }
    if ((AU(5) & 1) && access(b, F_OK) == 0) { close(in); w64_set_last_error(ERROR_FILE_EXISTS); RET(0); }
    int out = open(b, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) { close(in); w64_set_last_error(errno_to_win(errno)); RET(0); }
    char buf[65536]; ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)n) != n) break;
    close(in); close(out);
    RET(1);
}

/* ------------------------------------------------------------------ find */
typedef struct { DIR *d; char dir[1024]; char pat[256]; } findobj_t;

static int wild(const char *p, const char *s)
{
    if (!strcmp(p, "*.*")) return 1;
    for (; *p; p++, s++) {
        if (*p == '*') { for (; *s; s++) if (wild(p + 1, s)) return 1; return wild(p + 1, s); }
        if (!*s) return 0;
        if (*p != '?' && ((*p | 32) != (*s | 32))) return 0;
    }
    return !*s;
}
static int find_fill(findobj_t *f, uint8_t *b)
{
    struct dirent *de;
    while ((de = readdir(f->d))) {
        if (!wild(f->pat, de->d_name)) continue;
        char full[2048];
        snprintf(full, sizeof full, "%s/%s", f->dir, de->d_name);
        memset(b, 0, 592);
        uint8_t ad[36];
        if (!fill_attr_data(full, ad)) continue;
        memcpy(b, ad, 36);
        uint16_t *wn = (uint16_t *)(b + 44);
        utf8_to_w16(de->d_name, wn, 260);
        return 1;
    }
    return 0;
}
static uint64_t find_first(const char *win, uint8_t *data)
{
    char p[1024];
    w64_host_path(win, p, sizeof p);
    findobj_t *f = calloc(1, sizeof *f);
    char *sl = strrchr(p, '/');
    if (sl) { *sl = 0; snprintf(f->dir, sizeof f->dir, "%s", p[0] ? p : "/"); snprintf(f->pat, sizeof f->pat, "%s", sl + 1); }
    else { snprintf(f->dir, sizeof f->dir, "."); snprintf(f->pat, sizeof f->pat, "%s", p); }
    /* the pattern component went through the case resolver too; take it from the original */
    const char *ow = win + strlen(win);
    while (ow > win && ow[-1] != '\\' && ow[-1] != '/') ow--;
    snprintf(f->pat, sizeof f->pat, "%s", ow);
    f->d = opendir(f->dir);
    if (!f->d) { free(f); w64_set_last_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE; }
    if (!find_fill(f, data)) { closedir(f->d); free(f); w64_set_last_error(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE; }
    return w64_handle_new(H_FIND, f);
}
static void FindFirstFileExW_(CPU *c) { char *u = w16_to_utf8(AW(0), -1); uint64_t h = find_first(u, AP(2)); free(u); RET(h); }
static void FindFirstFileW_(CPU *c) { char *u = w16_to_utf8(AW(0), -1); uint64_t h = find_first(u, AP(1)); free(u); RET(h); }
static void FindNextFileW_(CPU *c)
{
    findobj_t *f = w64_handle_get(A(0), H_FIND);
    if (!f) RET(0);
    if (!find_fill(f, AP(1))) { w64_set_last_error(ERROR_NO_MORE_FILES); RET(0); }
    RET(1);
}
static void FindClose_(CPU *c)
{
    findobj_t *f = w64_handle_get(A(0), H_FIND);
    if (f) { closedir(f->d); free(f); w64_handle_close(A(0)); }
    RET(1);
}

/* ------------------------------------------------------------------ CloseHandle */
int w64_close_waitable(uint64_t h);
static void CloseHandle_(CPU *c)
{
    uint64_t h = A(0);
    fobj_t *o = w64_handle_get(h, H_FILE);
    if (o) { close(o->fd); free(o); w64_handle_close(h); RET(1); }
    mapobj_t *m = w64_handle_get(h, H_MAPPING);
    if (m) { free(m); w64_handle_close(h); RET(1); }
    if (w64_close_waitable(h)) RET(1);
    if (w64_handle_type(h) != H_FREE) { w64_handle_close(h); RET(1); }
    RET(1);
}

/* ------------------------------------------------------------------ paths & dirs */
static uint16_t g_cwd_w[260];
static int put_w(uint64_t buf, uint32_t cap, const char *s)
{
    int need = utf8_to_w16(s, NULL, 0);
    if ((uint32_t)need > cap) return need;
    utf8_to_w16(s, (uint16_t *)(uintptr_t)buf, (int)cap);
    return need - 1;
}
static int put_a(uint64_t buf, uint32_t cap, const char *s)
{
    size_t n = strlen(s);
    if (n + 1 > cap) return (int)n + 1;
    memcpy((void *)(uintptr_t)buf, s, n + 1);
    return (int)n;
}
static void GetCurrentDirectoryW_(CPU *c) { (void)g_cwd_w; RET(put_w(A(1), AU(0), VGAME)); }
static void GetTempPathW_(CPU *c) { RET(put_w(A(1), AU(0), "C:\\Temp\\")); }
static void GetTempPathA_(CPU *c) { RET(put_a(A(1), AU(0), "C:\\Temp\\")); }

static void full_path(const char *in, char *out, size_t cap)
{
    char t[1024];
    if (in[0] && in[1] == ':') snprintf(t, sizeof t, "%s", in);
    else if (in[0] == '\\' || in[0] == '/') {
        if (in[0] == '/' && in[1] != '/') snprintf(t, sizeof t, "%s", in);     /* host absolute */
        else snprintf(t, sizeof t, "C:%s", in);
    } else snprintf(t, sizeof t, "%s\\%s", VGAME, in);
    /* normalise separators (keep host paths with '/') and resolve . and .. */
    int host = t[0] == '/';
    char *parts[256]; int np = 0;
    char buf[1024]; snprintf(buf, sizeof buf, "%s", host ? t + 1 : t + 3);
    char *save = NULL;
    for (char *s = strtok_r(buf, "\\/", &save); s; s = strtok_r(NULL, "\\/", &save)) {
        if (!strcmp(s, ".")) continue;
        if (!strcmp(s, "..")) { if (np) np--; continue; }
        if (np < 256) parts[np++] = s;
    }
    size_t o = 0;
    if (host) o += (size_t)snprintf(out + o, cap - o, "/");
    else o += (size_t)snprintf(out + o, cap - o, "%c:\\", t[0]);
    for (int i = 0; i < np && o < cap; i++) o += (size_t)snprintf(out + o, cap - o, "%s%s", parts[i], i + 1 < np ? (host ? "/" : "\\") : "");
    size_t l = strlen(in);
    if (l && (in[l - 1] == '\\' || in[l - 1] == '/') && o + 1 < cap && np) { out[o++] = host ? '/' : '\\'; out[o] = 0; }
}
static void GetFullPathNameW_(CPU *c)
{
    char *u = w16_to_utf8(AW(0), -1);
    char o[1024];
    full_path(u, o, sizeof o);
    free(u);
    int r = put_w(A(2), AU(1), o);
    if (A(3) && r < (int)AU(1)) {
        uint16_t *w = (uint16_t *)AP(2);
        int last = -1;
        for (int i = 0; w[i]; i++) if (w[i] == '\\' || w[i] == '/') last = i;
        wr64(A(3), w[last + 1] ? A(2) + 2 * (uint64_t)(last + 1) : 0);
    }
    RET(r);
}
static void GetFullPathNameA_(CPU *c)
{
    char o[1024];
    full_path(AS(0), o, sizeof o);
    int r = put_a(A(2), AU(1), o);
    if (A(3) && r < (int)AU(1)) {
        const char *s = AS(2), *l = strrchr(s, '\\');
        wr64(A(3), l && l[1] ? (uint64_t)(uintptr_t)(l + 1) : 0);
    }
    RET(r);
}
static void GetDiskFreeSpaceW_(CPU *c)
{
    if (A(1)) wr32(A(1), 8);
    if (A(2)) wr32(A(2), 512);
    if (A(3)) wr32(A(3), 0x01000000);
    if (A(4)) wr32(A(4), 0x02000000);
    RET(1);
}
static void GetLogicalDrives_(CPU *c) { RET(1u << 2); }

/* ------------------------------------------------------------------ console / std handles */
static void GetStdHandle_(CPU *c)
{
    int32_t n = AI(0);
    RET(n == -10 ? STD_IN : n == -11 ? STD_OUT : n == -12 ? STD_ERR : INVALID_HANDLE);
}
static void SetStdHandle_(CPU *c) { RET(1); }
static void GetConsoleMode_(CPU *c) { RET(0); }
static void GetConsoleOutputCP_(CPU *c) { RET(65001); }
static void AttachConsole_(CPU *c) { RET(0); }
static void SetConsoleCtrlHandler_(CPU *c) { RET(1); }
static void WriteConsoleW_(CPU *c)
{
    char *u = w16_to_utf8(AW(1), (int)AU(2));
    w64_log("[guest] %s", u);
    free(u);
    if (A(3)) wr32(A(3), AU(2));
    RET(1);
}
static void ReadConsoleW_(CPU *c) { if (A(3)) wr32(A(3), 0); RET(0); }
static void LockFile_(CPU *c) { RET(1); }
static void UnlockFile_(CPU *c) { RET(1); }
static void LockFileEx_(CPU *c) { RET(1); }
static void UnlockFileEx_(CPU *c) { RET(1); }
static void AreFileApisANSI_(CPU *c) { RET(1); }
static void DeviceIoControl_(CPU *c) { w64_set_last_error(1); RET(0); }
static void GetOverlappedResult_(CPU *c) { if (A(2)) wr32(A(2), (uint32_t)rd64(A(1) + 8)); RET(1); }
static void CancelIo_(CPU *c) { RET(1); }

static const w64_export_t k32_file_exports[] = {
    { "KERNEL32.dll", "CreateFileW", CreateFileW_ },
    { "KERNEL32.dll", "CreateFileA", CreateFileA_ },
    { "KERNEL32.dll", "ReadFile", ReadFile_ },
    { "KERNEL32.dll", "WriteFile", WriteFile_ },
    { "KERNEL32.dll", "SetFilePointer", SetFilePointer_ },
    { "KERNEL32.dll", "SetFilePointerEx", SetFilePointerEx_ },
    { "KERNEL32.dll", "GetFileSize", GetFileSize_ },
    { "KERNEL32.dll", "GetFileSizeEx", GetFileSizeEx_ },
    { "KERNEL32.dll", "GetFileType", GetFileType_ },
    { "KERNEL32.dll", "SetEndOfFile", SetEndOfFile_ },
    { "KERNEL32.dll", "FlushFileBuffers", FlushFileBuffers_ },
    { "KERNEL32.dll", "GetFileTime", GetFileTime_ },
    { "KERNEL32.dll", "GetFileInformationByHandleEx", GetFileInformationByHandleEx_ },
    { "KERNEL32.dll", "SetFileInformationByHandle", SetFileInformationByHandle_ },
    { "KERNEL32.dll", "CreateFileMappingW", CreateFileMappingW_ },
    { "KERNEL32.dll", "MapViewOfFile", MapViewOfFile_ },
    { "KERNEL32.dll", "UnmapViewOfFile", UnmapViewOfFile_ },
    { "KERNEL32.dll", "FlushViewOfFile", FlushViewOfFile_ },
    { "KERNEL32.dll", "GetFileAttributesW", GetFileAttributesW_ },
    { "KERNEL32.dll", "GetFileAttributesA", GetFileAttributesA_ },
    { "KERNEL32.dll", "GetFileAttributesExW", GetFileAttributesExW_ },
    { "KERNEL32.dll", "CreateDirectoryW", CreateDirectoryW_ },
    { "KERNEL32.dll", "RemoveDirectoryW", RemoveDirectoryW_ },
    { "KERNEL32.dll", "DeleteFileW", DeleteFileW_ },
    { "KERNEL32.dll", "DeleteFileA", DeleteFileA_ },
    { "KERNEL32.dll", "MoveFileExW", MoveFileExW_ },
    { "KERNEL32.dll", "CopyFileExW", CopyFileExW_ },
    { "KERNEL32.dll", "FindFirstFileExW", FindFirstFileExW_ },
    { "KERNEL32.dll", "FindFirstFileW", FindFirstFileW_ },
    { "KERNEL32.dll", "FindNextFileW", FindNextFileW_ },
    { "KERNEL32.dll", "FindClose", FindClose_ },
    { "KERNEL32.dll", "CloseHandle", CloseHandle_ },
    { "KERNEL32.dll", "GetCurrentDirectoryW", GetCurrentDirectoryW_ },
    { "KERNEL32.dll", "GetTempPathW", GetTempPathW_ },
    { "KERNEL32.dll", "GetTempPathA", GetTempPathA_ },
    { "KERNEL32.dll", "GetFullPathNameW", GetFullPathNameW_ },
    { "KERNEL32.dll", "GetFullPathNameA", GetFullPathNameA_ },
    { "KERNEL32.dll", "GetDiskFreeSpaceW", GetDiskFreeSpaceW_ },
    { "KERNEL32.dll", "GetDiskFreeSpaceA", GetDiskFreeSpaceW_ },
    { "KERNEL32.dll", "GetLogicalDrives", GetLogicalDrives_ },
    { "KERNEL32.dll", "GetStdHandle", GetStdHandle_ },
    { "KERNEL32.dll", "SetStdHandle", SetStdHandle_ },
    { "KERNEL32.dll", "GetConsoleMode", GetConsoleMode_ },
    { "KERNEL32.dll", "GetConsoleOutputCP", GetConsoleOutputCP_ },
    { "KERNEL32.dll", "AttachConsole", AttachConsole_ },
    { "KERNEL32.dll", "SetConsoleCtrlHandler", SetConsoleCtrlHandler_ },
    { "KERNEL32.dll", "WriteConsoleW", WriteConsoleW_ },
    { "KERNEL32.dll", "ReadConsoleW", ReadConsoleW_ },
    { "KERNEL32.dll", "LockFile", LockFile_ },
    { "KERNEL32.dll", "UnlockFile", UnlockFile_ },
    { "KERNEL32.dll", "LockFileEx", LockFileEx_ },
    { "KERNEL32.dll", "UnlockFileEx", UnlockFileEx_ },
    { "KERNEL32.dll", "AreFileApisANSI", AreFileApisANSI_ },
    { "KERNEL32.dll", "DeviceIoControl", DeviceIoControl_ },
    { "KERNEL32.dll", "GetOverlappedResult", GetOverlappedResult_ },
    { "KERNEL32.dll", "CancelIo", CancelIo_ },
    { 0 }
};

__attribute__((constructor)) static void k32_file_init(void) { w64_register(k32_file_exports); }
