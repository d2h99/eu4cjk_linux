// GL texture-upload audit for EU4 (LD_PRELOAD, no game modification)
// Covers both PLT imports (symbol preemption) and GLEW-style runtime
// resolution (glXGetProcAddress interception). Logs large uploads only.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

static FILE *g_log;
__attribute__((constructor)) static void init(void) {
    g_log = fopen("glaudit.log", "w");
    if (g_log) setvbuf(g_log, NULL, _IOLBF, 0);
}

typedef void (*fn_glCompressedTexImage2D)(unsigned, int, int, int, int, int, int, const void *);
typedef void (*fn_glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
typedef void (*fn_glTexStorage2D)(unsigned, int, int, int, int);
typedef void (*fn_glCompressedTexSubImage2D)(unsigned, int, int, int, int, int, unsigned, int, const void *);
typedef void (*fn_glTexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);

static void *real(const char *n) { return dlsym(RTLD_NEXT, n); }

static void logup(const char *tag, long tgt, int w, int h, long p1, long p2) {
    if (!g_log) return;
    fprintf(g_log, "%s t=0x%lx w=%d h=%d a=0x%lx b=0x%lx\n", tag, tgt, w, h, p1, p2);
}

void glCompressedTexImage2D(unsigned t, int lvl, int ifmt, int w, int h, int b, int sz, const void *d) {
    if (w >= 100 || h >= 100) logup("[COMP]", t, w, h, ifmt, sz);
    ((fn_glCompressedTexImage2D)real("glCompressedTexImage2D"))(t, lvl, ifmt, w, h, b, sz, d);
}

void glTexImage2D(unsigned t, int lvl, int ifmt, int w, int h, int b, unsigned fmt, unsigned typ, const void *d) {
    if (w >= 100 || h >= 100) logup("[UNCP]", t, w, h, ifmt, (fmt << 16) | typ);
    ((fn_glTexImage2D)real("glTexImage2D"))(t, lvl, ifmt, w, h, b, fmt, typ, d);
}

void glTexStorage2D(unsigned t, int lvls, int ifmt, int w, int h) {
    if (w >= 100 || h >= 100) logup("[STOR]", t, w, h, ifmt, lvls);
    ((fn_glTexStorage2D)real("glTexStorage2D"))(t, lvls, ifmt, w, h);
}

void glCompressedTexSubImage2D(unsigned t, int lvl, int xo, int yo, int w, int h, unsigned fmt, int sz, const void *d) {
    if (w >= 100 || h >= 100) logup("[CSUB]", t, w, h, fmt, sz);
    ((fn_glCompressedTexSubImage2D)real("glCompressedTexSubImage2D"))(t, lvl, xo, yo, w, h, fmt, sz, d);
}

void glTexSubImage2D(unsigned t, int lvl, int xo, int yo, int w, int h, unsigned fmt, unsigned typ, const void *d) {
    if (w >= 100 || h >= 100) logup("[USUB]", t, w, h, fmt, (typ << 16) | lvl);
    ((fn_glTexSubImage2D)real("glTexSubImage2D"))(t, lvl, xo, yo, w, h, fmt, typ, d);
}

typedef void *(*PFN_glXGetProcAddress)(const unsigned char *);

void *glXGetProcAddress(const unsigned char *name) {
    static PFN_glXGetProcAddress realfn;
    if (!realfn) realfn = (PFN_glXGetProcAddress)dlsym(RTLD_NEXT, "glXGetProcAddress");
    const char *n = (const char *)name;
    if (g_log && (strncmp(n, "glTex", 5) == 0 || strncmp(n, "glCompressed", 12) == 0))
        fprintf(g_log, "[RESLV] %s\n", n);
    return realfn(name);
}

void *glXGetProcAddressARB(const unsigned char *name) {
    static PFN_glXGetProcAddress realfn;
    if (!realfn) realfn = (PFN_glXGetProcAddress)dlsym(RTLD_NEXT, "glXGetProcAddressARB");
    const char *n = (const char *)name;
    if (g_log && (strncmp(n, "glTex", 5) == 0 || strncmp(n, "glCompressed", 12) == 0))
        fprintf(g_log, "[RESLV-ARB] %s\n", n);
    return realfn(name);
}
