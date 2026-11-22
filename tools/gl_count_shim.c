// GL call counter for the emulated GS's hardware renderer (measurement only; nothing in the game changes).
// The runtime loads its GL entry points through eglGetProcAddress, which it finds with dlsym on libEGL, so this
// LD_PRELOAD library interposes dlsym, hands out its own eglGetProcAddress and wraps a few calls with counters.
// Every 5 s it prints, per second, the calls and bytes it saw to stderr ("[glshim] ...").
//   cc -O2 -shared -fPIC -o gl_count_shim.so tools/gl_count_shim.c -ldl -lpthread
//   LD_PRELOAD=$PWD/gl_count_shim.so tools/play.sh ...
// Experiments (each breaks the picture; they only show what a call costs):
//   GLSHIM_NO_BARRIER=1    glTextureBarrier does nothing
//   GLSHIM_NO_READPIXELS=1 glReadPixels into client memory (not into a pixel buffer) does nothing
//   GLSHIM_NO_WAIT=1       glClientWaitSync returns at once (timeout 0)
//   GLSHIM_NO_TEXPARAM=1  glTexParameteri does nothing
//   GLSHIM_UNBIND_FEEDBACK=1  a draw not preceded by a glTextureBarrier, while its own colour target is bound to a texture
//                          unit (a feedback loop the shader does not read), has that unit unbound around it. Draws in a feedback
//                          loop are counted either way ("loop").
//   GLSHIM_SKIP_DRAWS_OVER=<n>  glDrawArrays with more than n vertices does nothing (0: every draw); GLSHIM_SKIP_DRAWS_UNDER=<n> the
//                          same for draws with fewer than n vertices. Draws of each size class are counted either way ("big" = over
//                          GLSHIM_BIG, default 3000 vertices).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLbitfield;
typedef void *GLsync;
typedef uint64_t GLuint64;

static void *(*real_dlsym)(void *, const char *);
static void *(*real_egl_gpa)(const char *);

static atomic_ullong n_barrier, n_draw, n_drawverts, n_read, n_readbytes, n_readpbo, n_wait, n_wait_ns, n_teximage, n_texbytes,
    n_texsub, n_texsubbytes, n_bindfb, n_useprog, n_clear, n_copyimg, n_blit, n_texparam, n_uniform, n_bufsub, n_bufsubbytes;
static int no_barrier, no_read, no_wait;
static int no_texparam, unbind_feedback, barrier_since_draw, active_unit;
static GLuint unit_tex[32], draw_fb, fb_color[4096];
static atomic_ullong n_loop;
static long skip_over = -1, skip_under = -1, big = 3000;
static atomic_ullong n_big, n_bigverts;
static GLint pack_buffer_bound; // tracked through glBindBuffer(GL_PIXEL_PACK_BUFFER)

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void report(void)
{
    static atomic_ullong last;
    uint64_t t = now_ns(), l = atomic_load(&last);
    if (l == 0)
    {
        atomic_store(&last, t);
        return;
    }
    if (t - l < 5000000000ull || !atomic_compare_exchange_strong(&last, &l, t))
        return;
    double s = (double)(t - l) / 1e9;
#define R(x) ((double)atomic_exchange(&x, 0) / s)
    fprintf(stderr,
            "[glshim] per s: draws %.0f (verts %.0f; big %.0f with %.0f verts; loop %.0f) barriers %.0f readpixels %.0f (%.1f MB, pbo %.0f) waits %.0f (%.1f ms) "
            "teximage %.0f (%.1f MB) texsub %.0f (%.1f MB) bufsub %.0f (%.1f MB) bindfb %.0f useprog %.0f clear %.0f copyimg %.0f blit %.0f "
            "texparam %.0f uniform %.0f\n",
            R(n_draw), R(n_drawverts), R(n_big), R(n_bigverts), R(n_loop), R(n_barrier), R(n_read), R(n_readbytes) / 1e6, R(n_readpbo), R(n_wait), R(n_wait_ns) / 1e6,
            R(n_teximage), R(n_texbytes) / 1e6, R(n_texsub), R(n_texsubbytes) / 1e6, R(n_bufsub), R(n_bufsubbytes) / 1e6, R(n_bindfb),
            R(n_useprog), R(n_clear), R(n_copyimg), R(n_blit), R(n_texparam), R(n_uniform));
#undef R
}

#define REAL(name, ret, args) static ret(*real_##name) args;
REAL(glTextureBarrier, void, (void))
REAL(glDrawArrays, void, (GLenum, GLint, GLsizei))
REAL(glReadPixels, void, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *))
REAL(glClientWaitSync, GLenum, (GLsync, GLbitfield, GLuint64))
REAL(glTexImage2D, void, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))
REAL(glTexSubImage2D, void, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *))
REAL(glBindFramebuffer, void, (GLenum, GLuint))
REAL(glUseProgram, void, (GLuint))
REAL(glClear, void, (GLbitfield))
REAL(glCopyImageSubData, void, (GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei))
REAL(glBlitFramebuffer, void, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))
REAL(glTexParameteri, void, (GLenum, GLenum, GLint))
REAL(glUniform1i, void, (GLint, GLint))
REAL(glUniform1f, void, (GLint, float))
REAL(glUniform2f, void, (GLint, float, float))
REAL(glUniform4i, void, (GLint, GLint, GLint, GLint, GLint))
REAL(glBindBuffer, void, (GLenum, GLuint))
REAL(glActiveTexture, void, (GLenum))
REAL(glBindTexture, void, (GLenum, GLuint))
REAL(glFramebufferTexture2D, void, (GLenum, GLenum, GLenum, GLuint, GLint))
REAL(glBufferSubData, void, (GLenum, intptr_t, intptr_t, const void *))

static void w_glTextureBarrier(void)
{
    barrier_since_draw = 1;
    atomic_fetch_add(&n_barrier, 1);
    if (!no_barrier)
        real_glTextureBarrier();
}
static void w_glDrawArrays(GLenum m, GLint f, GLsizei c)
{
    atomic_fetch_add(&n_draw, 1);
    atomic_fetch_add(&n_drawverts, (unsigned long long)c);
    if (c > big)
    {
        atomic_fetch_add(&n_big, 1);
        atomic_fetch_add(&n_bigverts, (unsigned long long)c);
    }
    report();
    GLuint target = draw_fb < 4096 ? fb_color[draw_fb] : 0;
    int loopUnits[32], loops = 0;
    if (target)
        for (int u = 0; u < 32; ++u)
            if (unit_tex[u] == target)
                loopUnits[loops++] = u;
    if (loops)
        atomic_fetch_add(&n_loop, 1);
    const int unbind = unbind_feedback && loops && !barrier_since_draw;
    barrier_since_draw = 0;
    if (unbind)
    {
        for (int i = 0; i < loops; ++i)
        {
            real_glActiveTexture(0x84C0 + loopUnits[i]);
            real_glBindTexture(0x0DE1, 0);
        }
        real_glActiveTexture(0x84C0 + active_unit);
    }
    if ((skip_over >= 0 && c > skip_over) || (skip_under >= 0 && c < skip_under))
    {
    }
    else
        real_glDrawArrays(m, f, c);
    if (unbind)
    {
        for (int i = 0; i < loops; ++i)
        {
            real_glActiveTexture(0x84C0 + loopUnits[i]);
            real_glBindTexture(0x0DE1, target);
        }
        real_glActiveTexture(0x84C0 + active_unit);
    }
}
static void w_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t, void *p)
{
    atomic_fetch_add(&n_read, 1);
    atomic_fetch_add(&n_readbytes, (unsigned long long)w * (unsigned long long)h * 4ull);
    if (pack_buffer_bound)
        atomic_fetch_add(&n_readpbo, 1);
    else if (no_read)
        return;
    real_glReadPixels(x, y, w, h, f, t, p);
}
static GLenum w_glClientWaitSync(GLsync s, GLbitfield f, GLuint64 timeout)
{
    atomic_fetch_add(&n_wait, 1);
    uint64_t t0 = now_ns();
    GLenum r = real_glClientWaitSync(s, f, no_wait ? 0 : timeout);
    atomic_fetch_add(&n_wait_ns, now_ns() - t0);
    return r;
}
static void w_glTexImage2D(GLenum a, GLint b, GLint c, GLsizei w, GLsizei h, GLint e, GLenum f, GLenum g, const void *p)
{
    atomic_fetch_add(&n_teximage, 1);
    if (p)
        atomic_fetch_add(&n_texbytes, (unsigned long long)w * (unsigned long long)h * 4ull);
    real_glTexImage2D(a, b, c, w, h, e, f, g, p);
}
static void w_glTexSubImage2D(GLenum a, GLint b, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum g, const void *p)
{
    atomic_fetch_add(&n_texsub, 1);
    atomic_fetch_add(&n_texsubbytes, (unsigned long long)w * (unsigned long long)h * 4ull);
    real_glTexSubImage2D(a, b, x, y, w, h, f, g, p);
}
static void w_glActiveTexture(GLenum u)
{
    active_unit = (int)(u - 0x84C0) & 31;
    real_glActiveTexture(u);
}
static void w_glBindTexture(GLenum t, GLuint tex)
{
    if (t == 0x0DE1)
        unit_tex[active_unit] = tex;
    real_glBindTexture(t, tex);
}
static void w_glFramebufferTexture2D(GLenum t, GLenum a, GLenum tt, GLuint tex, GLint l)
{
    if (a == 0x8CE0 && draw_fb < 4096)
        fb_color[draw_fb] = tex;
    real_glFramebufferTexture2D(t, a, tt, tex, l);
}
static void w_glBindFramebuffer(GLenum t, GLuint f)
{
    if (t == 0x8D40 || t == 0x8CA9)
        draw_fb = f;
    atomic_fetch_add(&n_bindfb, 1);
    real_glBindFramebuffer(t, f);
}
static void w_glUseProgram(GLuint p)
{
    atomic_fetch_add(&n_useprog, 1);
    real_glUseProgram(p);
}
static void w_glClear(GLbitfield m)
{
    atomic_fetch_add(&n_clear, 1);
    real_glClear(m);
}
static void w_glCopyImageSubData(GLuint a, GLenum b, GLint c, GLint d, GLint e, GLint f, GLuint g, GLenum h, GLint i, GLint j, GLint k, GLint l,
                                 GLsizei m, GLsizei n, GLsizei o)
{
    atomic_fetch_add(&n_copyimg, 1);
    real_glCopyImageSubData(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o);
}
static void w_glBlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h, GLbitfield m, GLenum fl)
{
    atomic_fetch_add(&n_blit, 1);
    real_glBlitFramebuffer(a, b, c, d, e, f, g, h, m, fl);
}
static void w_glTexParameteri(GLenum a, GLenum b, GLint c)
{
    atomic_fetch_add(&n_texparam, 1);
    if (!no_texparam)
        real_glTexParameteri(a, b, c);
}
static void w_glUniform1i(GLint l, GLint a) { atomic_fetch_add(&n_uniform, 1); real_glUniform1i(l, a); }
static void w_glUniform1f(GLint l, float a) { atomic_fetch_add(&n_uniform, 1); real_glUniform1f(l, a); }
static void w_glUniform2f(GLint l, float a, float b) { atomic_fetch_add(&n_uniform, 1); real_glUniform2f(l, a, b); }
static void w_glUniform4i(GLint l, GLint a, GLint b, GLint c, GLint d) { atomic_fetch_add(&n_uniform, 1); real_glUniform4i(l, a, b, c, d); }
static void w_glBindBuffer(GLenum t, GLuint b)
{
    if (t == 0x88EB)
        pack_buffer_bound = b != 0;
    real_glBindBuffer(t, b);
}
static void w_glBufferSubData(GLenum t, intptr_t o, intptr_t s, const void *d)
{
    atomic_fetch_add(&n_bufsub, 1);
    atomic_fetch_add(&n_bufsubbytes, (unsigned long long)s);
    real_glBufferSubData(t, o, s, d);
}

static void *wrap(const char *name, void *proc)
{
    if (!proc)
        return proc;
#define W(n)                              \
    if (strcmp(name, #n) == 0)            \
    {                                     \
        *(void **)&real_##n = proc;       \
        return (void *)&w_##n;            \
    }
    W(glTextureBarrier) W(glDrawArrays) W(glReadPixels) W(glClientWaitSync) W(glTexImage2D) W(glTexSubImage2D) W(glBindFramebuffer)
    W(glUseProgram) W(glClear) W(glCopyImageSubData) W(glBlitFramebuffer) W(glTexParameteri) W(glUniform1i) W(glUniform1f)
    W(glUniform2f) W(glUniform4i) W(glBindBuffer) W(glBufferSubData) W(glActiveTexture) W(glBindTexture) W(glFramebufferTexture2D)
#undef W
    return proc;
}

static void *my_egl_gpa(const char *name) { return wrap(name, real_egl_gpa(name)); }

static void init(void)
{
    if (real_dlsym)
        return;
    real_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
    if (!real_dlsym)
        real_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    no_barrier = getenv("GLSHIM_NO_BARRIER") != NULL;
    no_read = getenv("GLSHIM_NO_READPIXELS") != NULL;
    no_wait = getenv("GLSHIM_NO_WAIT") != NULL;
    no_texparam = getenv("GLSHIM_NO_TEXPARAM") != NULL;
    unbind_feedback = getenv("GLSHIM_UNBIND_FEEDBACK") != NULL;
    if (getenv("GLSHIM_SKIP_DRAWS_OVER"))
        skip_over = atol(getenv("GLSHIM_SKIP_DRAWS_OVER"));
    if (getenv("GLSHIM_SKIP_DRAWS_UNDER"))
        skip_under = atol(getenv("GLSHIM_SKIP_DRAWS_UNDER"));
    if (getenv("GLSHIM_BIG"))
        big = atol(getenv("GLSHIM_BIG"));
}

void *dlsym(void *handle, const char *name)
{
    init();
    void *p = real_dlsym(handle, name);
    if (strcmp(name, "eglGetProcAddress") == 0 && p)
    {
        real_egl_gpa = (void *(*)(const char *))p;
        return (void *)&my_egl_gpa;
    }
    if (strncmp(name, "gl", 2) == 0)
        return wrap(name, p);
    return p;
}
