/* imports.c -- .so import resolution for libchrono.so / libc++_shared.so
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * The C++ ABI (std::/__cxa_/RTTI) is deliberately absent here -- it resolves
 * against libc++_shared.so. This table provides the libc subset (shimmed where
 * bionic/newlib differ), GLES2 (mesa), eglGetProcAddress, OpenSL ES and the
 * AAsset NDK API.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <math.h>
#include <malloc.h>
#include <locale.h>
#include <setjmp.h>
#include <pthread.h>
#include <time.h>
#include <wchar.h>
#include <errno.h>
#include <dirent.h>
#include <sys/time.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <switch.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "opensles.h"
#include "asset.h"
#include "imports.h"

// ---------------------------------------------------------------------------
// stack protector: libchrono imports __stack_chk_guard as a data symbol AND the
// NDK compiler also reads a canary from TLS (tpidr_el0 + 0x28), so we satisfy
// both (the symbol here, the TLS slot via tls_setup_guard()).
// ---------------------------------------------------------------------------

uint64_t __stack_chk_guard_fake = 0x0123456789ABCDEFull;

static void __stack_chk_fail_fake(void) {
  debugPrintf("*** stack smashing detected ***\n");
  abort();
}

FILE *stderr_fake = (FILE *)0x1337;

static int pthread_ret_to_bionic(int ret) {
  // Android libc++ compares pthread status codes against bionic errno values.
  // devkit/newlib uses a different ETIMEDOUT number, which turns a normal
  // condition_variable timeout into an uncaught std::system_error.
  const int bionic_etimedout = 110;
  if (ret == ETIMEDOUT)
    return bionic_etimedout;
  return ret;
}

// ---------------------------------------------------------------------------
// pthread wrappers: bionic allocates the opaque types inline and zero-inits
// them, so we lazily back them with heap-allocated newlib objects stashed
// through the caller's pointer slot.
// ---------------------------------------------------------------------------

static int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *mutexattr) {
  pthread_mutex_t *m = calloc(1, sizeof(pthread_mutex_t));
  if (!m) return -1;
  const int recursive = (mutexattr && *mutexattr == 1);
  int ret;
  if (recursive) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    ret = pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
  } else {
    ret = pthread_mutex_init(m, NULL);
  }
  if (ret != 0) { free(m); return -1; }
  *uid = m;
  return 0;
}

static int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && *uid && (uintptr_t)*uid > 0x8000) {
    pthread_mutex_destroy(*uid);
    free(*uid);
    *uid = NULL;
  }
  return 0;
}

// bionic statically initialises PTHREAD_MUTEX_INITIALIZER to all-zero, and
// PTHREAD_RECURSIVE_MUTEX_INITIALIZER to a small magic constant; lazily create
// the real object on first use.
static int ensure_mutex(pthread_mutex_t **uid) {
  if (!*uid) return pthread_mutex_init_fake(uid, NULL);
  if ((uintptr_t)*uid < 0x8000) { int attr = 1; return pthread_mutex_init_fake(uid, &attr); }
  return 0;
}

static int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  int ret = ensure_mutex(uid);
  if (ret < 0) return ret;
  return pthread_mutex_lock(*uid);
}
static int pthread_mutex_trylock_fake(pthread_mutex_t **uid) {
  int ret = ensure_mutex(uid);
  if (ret < 0) return ret;
  return pthread_mutex_trylock(*uid);
}
static int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  int ret = ensure_mutex(uid);
  if (ret < 0) return ret;
  return pthread_mutex_unlock(*uid);
}

static int pthread_cond_init_fake(pthread_cond_t **cnd, const int *condattr) {
  (void)condattr;
  pthread_cond_t *c = calloc(1, sizeof(pthread_cond_t));
  if (!c) return -1;
  if (pthread_cond_init(c, NULL) != 0) { free(c); return -1; }
  *cnd = c;
  return 0;
}
static int ensure_cond(pthread_cond_t **cnd) { return *cnd ? 0 : pthread_cond_init_fake(cnd, NULL); }
static int pthread_cond_broadcast_fake(pthread_cond_t **cnd) { if (ensure_cond(cnd) < 0) return -1; return pthread_cond_broadcast(*cnd); }
static int pthread_cond_signal_fake(pthread_cond_t **cnd) { if (ensure_cond(cnd) < 0) return -1; return pthread_cond_signal(*cnd); }
static int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  if (cnd && *cnd) { pthread_cond_destroy(*cnd); free(*cnd); *cnd = NULL; }
  return 0;
}
static int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  if (ensure_cond(cnd) < 0) return -1;
  if (ensure_mutex(mtx) < 0) return -1;
  return pthread_cond_wait(*cnd, *mtx);
}
static int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  if (ensure_cond(cnd) < 0) return -1;
  if (ensure_mutex(mtx) < 0) return -1;
  return pthread_ret_to_bionic(pthread_cond_timedwait(*cnd, *mtx, t));
}

// bionic pthread_once_t is a zero-initialised int. A correct pthread_once must
// run the init exactly once AND block every other caller until it has finished,
// or singletons race (a second caller could use a half-constructed object).
// State machine: 0 = not started, 1 = in progress, 2 = done.
static int pthread_once_fake(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine) return -1;
  if (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) == 2)
    return 0;
  if (__sync_bool_compare_and_swap(once_control, 0, 1)) {
    (*init_routine)();
    __atomic_store_n(once_control, 2, __ATOMIC_RELEASE);
  } else {
    while (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) != 2)
      svcSleepThread(1000); // 1us; brief wait for the initialising thread
  }
  return 0;
}

static int pthread_mutexattr_init_fake(int *attr) { if (attr) *attr = 0; return 0; }
static int pthread_mutexattr_settype_fake(int *attr, int type) { if (attr) *attr = type; return 0; }

// each engine thread gets tpidr_el0 pointed at a stack-guard block before it
// runs any guarded engine code (see tls_setup_guard)
typedef struct { void *(*entry)(void *); void *arg; } ThreadStart;

static void *thread_trampoline(void *p) {
  ThreadStart ts = *(ThreadStart *)p;
  free(p);
  tls_setup_guard();
  return ts.entry(ts.arg);
}

static int pthread_create_fake(pthread_t *thread, const void *unused, void *entry, void *arg) {
  (void)unused; // ignore the (incompatible) bionic attr
  ThreadStart *ts = malloc(sizeof(*ts));
  if (!ts) return -1;
  ts->entry = (void *(*)(void *))entry;
  ts->arg = arg;
  return pthread_create(thread, NULL, thread_trampoline, ts);
}

// devkitA64's pthread_detach returns ENOSYS; std::thread::detach turns that into
// an uncaught std::system_error -> terminate (the sead sound engine detaches a
// worker on battle teardown). Detach best-effort and always report success.
static int pthread_detach_fake(pthread_t thread) {
  pthread_detach(thread);
  return 0;
}

static int pthread_attr_setschedparam_fake(void *a, const void *p) { (void)a; (void)p; return 0; }

// ---------------------------------------------------------------------------
// small misc shims
// ---------------------------------------------------------------------------

static int getpid_fake(void) { return 1; }
static int sched_yield_fake(void) { svcSleepThread(0); return 0; }

// bionic and newlib number the POSIX clocks differently (bionic CLOCK_REALTIME=0,
// newlib=1), so libchrono's clock_gettime(CLOCK_REALTIME) hits an unsupported
// newlib id -> EINVAL -> std::chrono throws. Remap the realtime ids and fall back
// so a failure is never returned to the engine.
static int clock_gettime_fake(int clk, struct timespec *tp) {
  if (!tp) return 0;
  clockid_t want = (clk == 0 || clk == 5) ? CLOCK_REALTIME : (clockid_t)clk;
  if (clock_gettime(want, tp) == 0) return 0;
  if (clock_gettime(CLOCK_REALTIME, tp) == 0) return 0;
  if (clock_gettime(CLOCK_MONOTONIC, tp) == 0) return 0;
  const u64 f = armGetSystemTickFreq() ? armGetSystemTickFreq() : 19200000ull;
  const u64 t = armGetSystemTick();
  tp->tv_sec = (time_t)(t / f);
  tp->tv_nsec = (long)(((t % f) * 1000000000ull) / f);
  return 0;
}

// ---------------------------------------------------------------------------
// frame pacing
//
// Frame-time jitter (Director's own per-frame delta swinging several ms
// frame-to-frame) is handled properly by the fixed_timestep patch group in
// patches.h, which overwrites Director::drawScene / calculateDeltaTime's
// _deltaTime store directly with a constant 1/60 -- a single, deterministic
// intervention at the one place that actually needs it.
//
// This used to also be "fixed" here, at the gettimeofday() import level, with
// an EMA-smoothed virtual clock. That approach was wired as the replacement
// for every gettimeofday() call the engine makes -- not just Director's --
// which is the problem: its smoothing state (virt/last_real/smooth) advances
// on *any* call arriving >=2ms after the previous one, from *any* caller. If
// more than one subsystem calls gettimeofday() independently within the same
// real frame (e.g. a cutscene/audio-visual timing path, separate from
// Director's own per-frame sample), each such call ticks the shared virtual
// clock forward again, so it runs measurably faster than real time. That's
// why the intro's game-rendered sequence (timed against this clock) used to
// finish before its background song (played on a real, unaffected audio
// clock) -- not a tuning problem, a structural one with a shared clock and
// multiple uncoordinated callers.
//
// Now a plain passthrough: real time in, real time out. Director's own dt is
// handled by the binary patch instead.
static int gettimeofday_paced(struct timeval *tv, void *tz) {
  return gettimeofday(tv, tz);
}

// dlsym: cocos probes GL/EGL extensions through it. Resolve via eglGetProcAddress
// first, then fall back to our own import table.
DynLibFunction *so_find_import(DynLibFunction *funcs, int num_funcs, const char *name);
static void *dlsym_fake(void *handle, const char *name) {
  (void)handle;
  void *p = (void *)eglGetProcAddress(name);
  if (p) return p;
  DynLibFunction *f = so_find_import(dynlib_functions, (int)dynlib_numfunctions, name);
  return f ? (void *)f->func : NULL;
}
static void *dlopen_fake(const char *name, int flag) { (void)name; (void)flag; return (void *)1; }
static int dlclose_fake(void *h) { (void)h; return 0; }
static const char *dlerror_fake(void) { return NULL; }

// mmap/munmap are not implemented by newlib/libnx; report failure so callers
// fall back to read()/fread() (cocos FileUtils does).
static void *mmap_fake(void *addr, size_t len, int prot, int flags, int fd, long off) {
  (void)addr; (void)len; (void)prot; (void)flags; (void)fd; (void)off;
  return (void *)-1; // MAP_FAILED
}
static int munmap_fake(void *addr, size_t len) { (void)addr; (void)len; return 0; }

// fdopendir is absent from devkitA64 newlib; libc++ only needs it for
// std::filesystem directory iteration, which the game does not use.
static void *fdopendir_fake(int fd) { (void)fd; return NULL; }

// GL_OES_mapbuffer is an extension: resolve lazily through eglGetProcAddress so
// the table entry is valid even though mesa exports it only as a proc address.
static void *gl_MapBufferOES(GLenum target, GLenum access) {
  static PFNGLMAPBUFFEROESPROC fn = NULL;
  if (!fn) fn = (PFNGLMAPBUFFEROESPROC)eglGetProcAddress("glMapBufferOES");
  return fn ? fn(target, access) : NULL;
}
static GLboolean gl_UnmapBufferOES(GLenum target) {
  static PFNGLUNMAPBUFFEROESPROC fn = NULL;
  if (!fn) fn = (PFNGLUNMAPBUFFEROESPROC)eglGetProcAddress("glUnmapBufferOES");
  return fn ? fn(target) : GL_FALSE;
}

// cocos RenderTexture (FieldMap::RewriteBg) can request a 0-sized FBO texture
// before map data is ready; the mesa/nouveau driver then NULL-derefs in
// st_update_renderbuffer_surface. Clamp degenerate allocations to 1x1.
// ---------------------------------------------------------------------------
// FORCE_NEAREST (config.force_nearest) -- kill ALL bilinear sampling at the
// GL boundary. A 1080p framebuffer screenshot proved the output is a
// bilinear-class upscale (every pixel a gradient; no flat 3px art-pixel runs)
// even with the remove_bilinear_filter binary patch active. Cause: that patch
// covers texture-CREATION sites inside libchrono, but (a) GL's DEFAULT mag
// filter is GL_LINEAR, so any texture that never receives an explicit
// glTexParameteri (e.g. the 1024x1024 field composite buffers, created
// outside the patched paths) samples LINEAR by default, and (b) runtime
// setAntiAliasTexParameters-style calls can re-enable LINEAR later. Filtered
// upscaling of pixel art at a non-integer effective ratio is exactly the
// observed "squares aren't square / some wider some tighter / grey blended
// edges" artifact. Enforcing NEAREST here, on every texture and every filter
// call, is total: no creation path or runtime change can escape the wrapper.
// Trade-off: intentionally-smoothed stretched backgrounds become crisp/blocky.
// ---------------------------------------------------------------------------
static void force_nearest_params(GLenum target) {
  glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}
static GLint nearest_filter(GLint param) {
  switch (param) {
    case GL_LINEAR:                 return GL_NEAREST;
    case GL_LINEAR_MIPMAP_NEAREST:
    case GL_LINEAR_MIPMAP_LINEAR:
    case GL_NEAREST_MIPMAP_LINEAR:  return GL_NEAREST_MIPMAP_NEAREST;
    default:                        return param;
  }
}
static void glTexParameteri_nearest(GLenum target, GLenum pname, GLint param) {
  if (config.force_nearest &&
      (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER))
    param = nearest_filter(param);
  glTexParameteri(target, pname, param);
}
static void glTexParameterf_nearest(GLenum target, GLenum pname, GLfloat param) {
  if (config.force_nearest &&
      (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER))
    param = (GLfloat)nearest_filter((GLint)param);
  glTexParameterf(target, pname, param);
}

static void glTexImage2D_guard(GLenum target, GLint level, GLint internalformat,
    GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
    const void *pixels) {
  if (width <= 0 || height <= 0) {
    width = width <= 0 ? 1 : width;
    height = height <= 0 ? 1 : height;
    pixels = NULL; // the supplied buffer no longer matches the clamped size
  }
  glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
  // Stamp NEAREST at creation so textures relying on GL default filtering
  // (default MAG filter is GL_LINEAR!) can never sample LINEAR.
  if (config.force_nearest && level == 0 && target == GL_TEXTURE_2D)
    force_nearest_params(target);
}

// ---------------------------------------------------------------------------
// DIAG_BIGQUAD -- capture EVERY large drawn quad (not just fractional-flagged
// ones like earlier probes) with its texture identity and per-axis texel->point
// scale, to identify WHICH quad the visible non-square pixels live on. Defined
// up here because the texture tracker below is shared with DIAG_VERTS.
// ---------------------------------------------------------------------------
#define DIAG_BIGQUAD 0

// ---------------------------------------------------------------------------
// Tiny bound-texture-size tracker, used by the DIAG_VERTS and DIAG_BIGQUAD
// blocks so UV fractionality / texel scales can be checked against the REAL
// bound texture's pixel size instead of guessing (why early UV measurements
// were unreliable). Compiled only when a consumer is active (avoids
// -Wunused-function warnings otherwise).
// ---------------------------------------------------------------------------
#if DIAG_VERTS || DIAG_BIGQUAD
#define TEXTRACK_N 128
static GLuint  s_tt_id[TEXTRACK_N];
static GLsizei s_tt_w[TEXTRACK_N];
static GLsizei s_tt_h[TEXTRACK_N];
static int     s_tt_next = 0;
static GLuint  s_cur_tex2d = 0;

static void texTrack_set(GLuint id, GLsizei w, GLsizei h) {
  for (int i = 0; i < TEXTRACK_N; i++) {
    if (s_tt_id[i] == id) { s_tt_w[i] = w; s_tt_h[i] = h; return; }
  }
  int i = s_tt_next; s_tt_next = (s_tt_next + 1) % TEXTRACK_N;
  s_tt_id[i] = id; s_tt_w[i] = w; s_tt_h[i] = h;
}

static int texTrack_get(GLuint id, GLsizei *w, GLsizei *h) {
  for (int i = 0; i < TEXTRACK_N; i++) {
    if (s_tt_id[i] == id) { *w = s_tt_w[i]; *h = s_tt_h[i]; return 1; }
  }
  return 0;
}

static void glBindTexture_track(GLenum target, GLuint texture) {
  if (target == GL_TEXTURE_2D) s_cur_tex2d = texture;
  glBindTexture(target, texture);
}

static void glTexImage2D_guard_tracked(GLenum target, GLint level, GLint internalformat,
    GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type,
    const void *pixels) {
  if (width <= 0 || height <= 0) {
    width = width <= 0 ? 1 : width;
    height = height <= 0 ? 1 : height;
    pixels = NULL;
  }
  if (target == GL_TEXTURE_2D && level == 0) texTrack_set(s_cur_tex2d, width, height);
  glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
}
#endif /* DIAG_VERTS || DIAG_BIGQUAD */

// ---------------------------------------------------------------------------
// DIAG_VERTS -- measure whether each rendered quad's ACTUAL ON-SCREEN SIZE
// (bounding box of its 4 vertices) is a whole number of pixels, and whether
// that matches an integer multiple of the source texel rect it samples.
// This is the direct test of "is this tile/sprite rendered at a consistent
// integer pixel size" -- the literal question behind "some squares wider
// than others". Self-contained: writes gl_diag.log next to config.ini.
//
// cocos2d sprites upload quads as V3F_C4B_T2F: 4 verts x 24 bytes each =
//   pos.x pos.y pos.z (3 float, 12B) | color RGBA (4 u8, 4B) | u v (2 float, 8B).
//
//   quads: uploads=.. total=.. frac_width=.. frac_height=..
//     QUADSIZE tex=N(WxH) onscreen=WxH src=WxH pos(x,y) scale=SxS
//
// frac_width/frac_height counts how many quads have a non-integer on-screen
// width/height. The "scale" field is onscreen-size / source-texel-size --
// if that's not a clean whole number for real game art (not tiny UI/text
// textures), that IS the bug, with the exact factor in hand.
// ---------------------------------------------------------------------------
#define PIXEL_SNAP_TEST 0
#define DIAG_VERTS 0
#if DIAG_VERTS
static void vd_log(const char *fmt, ...) {
  FILE *f = fopen("gl_diag.log", "a");
  if (!f) return;
  va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
  fclose(f);
}

static unsigned s_vd_uploads = 0;
static unsigned s_vd_total_quads = 0;
static unsigned s_vd_frac_pos = 0;
static unsigned s_vd_frac_uv  = 0;
static int      s_vd_examples = 0;

static int vd_is_frac(float v) {
  float r = v - (float)floor((double)v);
  return (r > 0.01f && r < 0.99f);
}

static void vd_scan(const void *data, long size) {
  if (!data || size < 24) return;
  const unsigned char *p = (const unsigned char *)data;
  long nverts = size / 24;
  if (nverts < 4) return;
  if (nverts > 4096) nverts = 4096;

  GLsizei texW = 0, texH = 0;
  int haveTex = texTrack_get(s_cur_tex2d, &texW, &texH);
  if (!haveTex || texW <= 0 || texH <= 0) return;

  long nquads = nverts / 4;
  for (long q = 0; q < nquads; q++) {
    float x[4], y[4], u[4], vv[4];
    for (int i = 0; i < 4; i++) {
      const unsigned char *v = p + (q * 4 + i) * 24;
      memcpy(&x[i],  v + 0,  4);
      memcpy(&y[i],  v + 4,  4);
      memcpy(&u[i],  v + 16, 4);
      memcpy(&vv[i], v + 20, 4);
    }
    float minX = x[0], maxX = x[0], minY = y[0], maxY = y[0];
    float minU = u[0], maxU = u[0], minV = vv[0], maxV = vv[0];
    for (int i = 1; i < 4; i++) {
      if (x[i]  < minX) minX = x[i];  if (x[i]  > maxX) maxX = x[i];
      if (y[i]  < minY) minY = y[i];  if (y[i]  > maxY) maxY = y[i];
      if (u[i]  < minU) minU = u[i];  if (u[i]  > maxU) maxU = u[i];
      if (vv[i] < minV) minV = vv[i]; if (vv[i] > maxV) maxV = vv[i];
    }
    if (minX < -2600 || maxX > 2600 || minY < -2600 || maxY > 2600) continue;

    float qw = maxX - minX, qh = maxY - minY;
    if (qw < 0.5f || qh < 0.5f) continue; // degenerate/zero-area quad

    s_vd_total_quads++;
    int wfrac = vd_is_frac(qw);
    int hfrac = vd_is_frac(qh);
    if (wfrac) s_vd_frac_pos++; // repurposed: "quad WIDTH not a whole pixel count"
    if (hfrac) s_vd_frac_uv++;  // repurposed: "quad HEIGHT not a whole pixel count"

    // Source rect size in real texel space -- tells us how many source
    // texels this quad is supposed to represent, so we can see if the
    // on-screen size (qw/qh) matches an integer multiple of it.
    float srcW = (maxU - minU) * (float)texW;
    float srcH = (maxV - minV) * (float)texH;

    if ((wfrac || hfrac) && s_vd_examples < 20) {
      s_vd_examples++;
      vd_log("  QUADSIZE tex=%u(%dx%d) onscreen=%.3fx%.3f src=%.2fx%.2f pos(%.1f,%.1f) scale=%.5fx%.5f\n",
             s_cur_tex2d, (int)texW, (int)texH, qw, qh, srcW, srcH, minX, minY,
             srcW > 0.001f ? qw / srcW : 0.0f, srcH > 0.001f ? qh / srcH : 0.0f);
    }
  }
}

static void vd_tick(void) {
  s_vd_uploads++;
  if (s_vd_uploads % 120 == 0) {
    vd_log("quads: uploads=%u total=%u frac_width=%u frac_height=%u\n",
           s_vd_uploads, s_vd_total_quads, s_vd_frac_pos, s_vd_frac_uv);
    s_vd_examples = 0;
  }
}

#if !PIXEL_SNAP_TEST
static void glBufferData_diag(GLenum target, GLsizeiptr size,
                              const void *data, GLenum usage) {
  if (target == GL_ARRAY_BUFFER && data && size >= 24)
    vd_scan(data, (long)size);
  vd_tick();
  glBufferData(target, size, data, usage);
}

static void glBufferSubData_diag(GLenum target, GLintptr offset,
                                 GLsizeiptr size, const void *data) {
  if (target == GL_ARRAY_BUFFER && data && size >= 24)
    vd_scan(data, (long)size);
  vd_tick();
  glBufferSubData(target, offset, size, data);
}
#endif
#endif

// ---------------------------------------------------------------------------
// PIXEL_SNAP_TEST -- decisive diagnostic patch, NOT a proposed permanent fix.
//
// gl_diag.log proved a large fraction of sprite/tile vertex positions reach
// the GPU with a non-integer screen coordinate, even though every code path
// traced by hand in libchrono.so computes clean integers. Rather than keep
// hunting for the one place a fraction enters, this intercepts the same
// vertex uploads DIAG_VERTS already reads and forcibly rounds every vertex's
// X/Y to the nearest whole pixel before it reaches the GPU -- regardless of
// which code path produced the fraction.
//
// This is a blunt instrument on purpose: if the shimmer disappears with this
// on, that's decisive proof the cause is fractional vertex geometry (broadly,
// not tied to one function), and a real, scoped fix becomes worth writing.
// If the shimmer is UNCHANGED, fractional vertex positions are not (solely)
// the cause, and the search should move to texture sampling / rasterization
// instead. Either answer ends a category of guessing.
//
// Known side effects while testing (expected, not corruption):
//   - Anything that was using its half-pixel offset on purpose (odd-width
//     sprites center-anchored) snaps by <1px -- usually invisible.
//   - Text/UI kerning may look very slightly different.
//   - Uses a static scratch buffer (single GL submission thread; gl_threaded
//     mode still serializes actual GL calls onto one thread in this engine).
// ---------------------------------------------------------------------------
#if PIXEL_SNAP_TEST
// 65536 verts (16384 quads, 1.5MB) -- comfortably covers any single
// glBufferData/glBufferSubData call this engine is likely to make, including
// the field's full batched tile layer. The previous 4096-vert (1024-quad)
// cap was silently exceeded by real field content, so snap_positions() was
// returning the ORIGINAL untouched buffer for exactly the geometry that
// matters -- title/menu screens (small buffers) looked "fixed" in the log,
// gameplay never was. That's why the in-game test showed no change: it
// never actually ran on the field.
static unsigned char s_snap_scratch[65536 * 24];

static int snap_has_frac(const unsigned char *p, long nverts) {
  for (long i = 0; i < nverts; i++) {
    const unsigned char *v = p + i * 24;
    float px, py;
    memcpy(&px, v + 0, 4);
    memcpy(&py, v + 4, 4);
    if (px >= -8000 && px <= 8000 && px != roundf(px)) return 1;
    if (py >= -8000 && py <= 8000 && py != roundf(py)) return 1;
  }
  return 0;
}

// Returns a pointer to (possibly) modified data, or the original pointer if
// this buffer doesn't look like a V3F_C4B_T2F sprite-quad upload, is too
// large for the scratch buffer (logged so we'd KNOW if that's happening
// again), or already has no fractional positions (fast path -- skips the
// copy entirely, which is most menu/UI/static geometry).
static const void *snap_positions(const void *data, long size) {
  if (!data || size < 24) return data;
  long nverts = size / 24;
  if (nverts < 3) return data;

  const unsigned char *src = (const unsigned char *)data;
  float px0, py0;
  memcpy(&px0, src + 0, 4);
  memcpy(&py0, src + 4, 4);
  if (px0 < -8000 || px0 > 8000 || py0 < -8000 || py0 > 8000) return data;

  if (!snap_has_frac(src, nverts)) return data;  // already clean, skip copy

  if (size > (long)sizeof(s_snap_scratch)) {
    #if DIAG_VERTS
    static int warned = 0;
    if (!warned) { warned = 1; vd_log("snap: buffer too large to snap (%ld bytes, %ld verts) -- NOT snapped\n", size, nverts); }
    #endif
    return data;
  }

  memcpy(s_snap_scratch, data, (size_t)size);
  unsigned char *dst = s_snap_scratch;
  for (long i = 0; i < nverts; i++) {
    unsigned char *v = dst + i * 24;
    float px, py;
    memcpy(&px, v + 0, 4);
    memcpy(&py, v + 4, 4);
    if (px >= -8000 && px <= 8000) px = roundf(px);
    if (py >= -8000 && py <= 8000) py = roundf(py);
    memcpy(v + 0, &px, 4);
    memcpy(v + 4, &py, 4);
  }
  return s_snap_scratch;
}

static void glBufferData_snap(GLenum target, GLsizeiptr size,
                              const void *data, GLenum usage) {
  const void *use = data;
  if (target == GL_ARRAY_BUFFER && data && size >= 24)
    use = snap_positions(data, (long)size);
#if DIAG_VERTS
  // Scan the buffer we're ACTUALLY uploading (post-snap), not the original
  // input -- otherwise the log can never show whether snapping engaged.
  if (target == GL_ARRAY_BUFFER && use && size >= 24)
    vd_scan(use, (long)size);
  vd_tick();
#endif
  glBufferData(target, size, use, usage);
}

static void glBufferSubData_snap(GLenum target, GLintptr offset,
                                 GLsizeiptr size, const void *data) {
  const void *use = data;
  if (target == GL_ARRAY_BUFFER && data && size >= 24)
    use = snap_positions(data, (long)size);
#if DIAG_VERTS
  if (target == GL_ARRAY_BUFFER && use && size >= 24)
    vd_scan(use, (long)size);
  vd_tick();
#endif
  glBufferSubData(target, offset, size, use);
}
#endif

// ---------------------------------------------------------------------------
// DIAG_MVP -- measure the REAL per-axis screen scale the GPU actually applies,
// straight from the MVP matrices the engine uploads, paired with whatever
// glViewport is active at that moment. This is ground truth: no inference
// about design resolution / contentScaleFactor / RT sizing, just "how many
// screen pixels per design-unit does this draw actually get, on X vs Y".
//
// Self-contained: writes gl_mvp.log next to config.ini. Dedups on
// (viewport, ratio) so a render-texture pass (different viewport) is never
// hidden behind the main screen pass even if both happen to be uniform.
//
//   mvp #N: vp=WxH  scaleX=.. scaleY=..  RATIO=x.xxxxx (+/-N.NN% wide) [NON-INT: X and/or Y]
//
// RATIO != 1 on a given viewport line means X and Y disagree (an aspect
// error, baked in at that draw stage). (NON-INT) on either axis flags a
// uniform-but-non-whole-number scale, which is the other way to get
// "some columns wider than others" under nearest-neighbour sampling, even
// when X and Y agree with each other.
// ---------------------------------------------------------------------------
#define DIAG_MVP 1
#if DIAG_MVP
static void mvp_log(const char *fmt, ...) {
  FILE *f = fopen("gl_mvp.log", "a");
  if (!f) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fclose(f);
}

static GLint  s_mvp_vp_x = 0, s_mvp_vp_y = 0;
static GLsizei s_mvp_vp_w = 0, s_mvp_vp_h = 0;

#define MVP_SEEN_N 64
static struct { GLsizei w, h; float ratio; int used; } s_mvp_seen[MVP_SEEN_N];
static int s_mvp_seen_n = 0;
static int s_mvp_count = 0;

static void glViewport_mvp(GLint x, GLint y, GLsizei w, GLsizei h) {
  s_mvp_vp_x = x; s_mvp_vp_y = y; s_mvp_vp_w = w; s_mvp_vp_h = h;
  glViewport(x, y, w, h);
}

static void glUniformMatrix4fv_mvp(GLint location, GLsizei count, GLboolean transpose,
                                    const GLfloat *value) {
  glUniformMatrix4fv(location, count, transpose, value);

  if (!value || count < 1 || s_mvp_vp_w <= 0 || s_mvp_vp_h <= 0) return;

  // Column-major 4x4: m[0] = X-scale term, m[5] = Y-scale term (standard
  // orthographic/projection layout used throughout cocos2d-x). Skip
  // anything that isn't a plausible 2D ortho scale (identity-ish, or
  // clearly a rotation/other matrix) to keep the log to real projections.
  float m0 = value[0], m5 = value[5];
  if (m0 == 0.0f || m5 == 0.0f) return;
  if (fabsf(m0) > 100.0f || fabsf(m5) > 100.0f) return; // not a sane ortho scale

  float scaleX = fabsf(m0) * (float)s_mvp_vp_w / 2.0f;
  float scaleY = fabsf(m5) * (float)s_mvp_vp_h / 2.0f;
  if (scaleX < 0.001f || scaleY < 0.001f) return;

  float ratio = scaleX / scaleY;

  // Dedup on (viewport size, ratio rounded) so distinct render passes each
  // get their own line, but repeats of the same pass don't spam the log.
  float ratio_key = roundf(ratio * 100000.0f) / 100000.0f;
  for (int i = 0; i < s_mvp_seen_n; i++) {
    if (s_mvp_seen[i].w == s_mvp_vp_w && s_mvp_seen[i].h == s_mvp_vp_h &&
        fabsf(s_mvp_seen[i].ratio - ratio_key) < 0.0001f) {
      return; // already logged this exact (viewport, ratio) combo
    }
  }
  if (s_mvp_seen_n < MVP_SEEN_N) {
    s_mvp_seen[s_mvp_seen_n].w = s_mvp_vp_w;
    s_mvp_seen[s_mvp_seen_n].h = s_mvp_vp_h;
    s_mvp_seen[s_mvp_seen_n].ratio = ratio_key;
    s_mvp_seen_n++;
  }

  s_mvp_count++;
  float pctX = (scaleX - roundf(scaleX));
  float pctY = (scaleY - roundf(scaleY));
  int nonIntX = fabsf(pctX) > 0.01f;
  int nonIntY = fabsf(pctY) > 0.01f;
  float pct_wide = (ratio - 1.0f) * 100.0f;

  mvp_log("mvp #%d: vp=%dx%d  scaleX=%.4f scaleY=%.4f  RATIO=%.5f (%+.2f%% wide)%s%s%s\n",
          s_mvp_count, s_mvp_vp_w, s_mvp_vp_h, scaleX, scaleY, ratio, pct_wide,
          (nonIntX || nonIntY) ? "  [NON-INT:" : "",
          nonIntX ? " X" : "", (nonIntX || nonIntY) ? (nonIntY ? " Y]" : "]") : "");
}
#endif

// ---------------------------------------------------------------------------
// DIAG_PHASE -- the one thing QUADSIZE (last round) could NOT see.
//
// QUADSIZE proved every quad's own WIDTH (x1-x0) is an exact integer during
// real gameplay. But that says nothing about POSITION: two quads can each be
// a perfectly clean 48.0px wide, while one sits at x0=320.000 and its
// neighbour sits at x0=368.333 -- same width, different sub-pixel PHASE.
// Under nearest-neighbour sampling, quads at different phases sample their
// source texels differently, which is exactly "some columns compressed,
// some expanded, in the same still frame" -- a phase mismatch, not a size
// mismatch, and QUADSIZE structurally could not detect it.
//
// This hooks the SAME buffer uploads and, within EACH SINGLE upload (which
// batches many tiles from one layer together in cocos2d), collects the
// distinct fractional x0 phases seen among tile/sprite-sized quads (integer
// width, 8-200px -- excludes huge UI panels and 1px degenerate quads).
// If more than one distinct phase shows up in the SAME upload, that's
// definitive, direct proof of a phase-split -- logged with concrete examples.
//
//   phase: calls=.. splitCalls=.. qualQuads=..
//     SPLIT call#N: phases_seen={0.000:12, 0.333:4} example x0=320.000 x0=368.333
// ---------------------------------------------------------------------------
#define DIAG_PHASE 0
#if DIAG_PHASE
static void ph_log(const char *fmt, ...) {
  FILE *f = fopen("gl_phase.log", "a");
  if (!f) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fclose(f);
}

static long s_ph_calls = 0;
static long s_ph_split_calls = 0;
static long s_ph_qual_quads = 0;
static int  s_ph_examples_logged = 0;

static void ph_scan(const void *data, long size) {
  if (!data || size < 24) return;
  long nverts = size / 24;
  if (nverts < 4) return;
  const unsigned char *base = (const unsigned char *)data;

  s_ph_calls++;

  // distinct phases seen in THIS call only
  #define PH_MAXPHASE 16
  float ph_val[PH_MAXPHASE];
  int   ph_cnt[PH_MAXPHASE];
  float ph_x0[PH_MAXPHASE]; // one example raw x0 per phase bucket
  int   ph_n = 0;
  int   qual_this_call = 0;

  long nquads = nverts / 4;
  if (nquads > 20000) nquads = 20000; // sanity cap, matches earlier sessions

  for (long q = 0; q < nquads; q++) {
    float xs[4];
    int ok = 1;
    for (int v = 0; v < 4; v++) {
      const unsigned char *vp = base + (q * 4 + v) * 24;
      float x, y;
      memcpy(&x, vp + 0, 4);
      memcpy(&y, vp + 4, 4);
      if (!(x == x) || !(y == y)) { ok = 0; break; } // NaN guard
      if (fabsf(x) > 8000.0f || fabsf(y) > 8000.0f) { ok = 0; break; }
      xs[v] = x;
    }
    if (!ok) continue;

    float x0 = xs[0], x1 = xs[0];
    for (int v = 1; v < 4; v++) { if (xs[v] < x0) x0 = xs[v]; if (xs[v] > x1) x1 = xs[v]; }
    float width = x1 - x0;
    if (width < 8.0f || width > 200.0f) continue;          // skip UI/degenerate
    if (fabsf(width - roundf(width)) > 0.01f) continue;    // only clean-width quads (matches QUADSIZE's finding)

    float frac = x0 - floorf(x0);
    float frac_key = roundf(frac * 1000.0f) / 1000.0f;
    if (frac_key >= 0.999f) frac_key = 0.0f; // 1.0 == 0.0

    qual_this_call++;

    int found = 0;
    for (int i = 0; i < ph_n; i++) {
      if (fabsf(ph_val[i] - frac_key) < 0.002f) { ph_cnt[i]++; found = 1; break; }
    }
    if (!found && ph_n < PH_MAXPHASE) {
      ph_val[ph_n] = frac_key; ph_cnt[ph_n] = 1; ph_x0[ph_n] = x0; ph_n++;
    }
  }

  s_ph_qual_quads += qual_this_call;

  if (ph_n > 1) {
    s_ph_split_calls++;
    if (s_ph_examples_logged < 40) {
      s_ph_examples_logged++;
      char buf[512]; int off = 0;
      off += snprintf(buf + off, sizeof(buf) - off, "  SPLIT call#%ld: phases_seen={", s_ph_calls);
      for (int i = 0; i < ph_n && i < 6; i++) {
        off += snprintf(buf + off, sizeof(buf) - off, "%s%.3f:%d", i ? "," : "", ph_val[i], ph_cnt[i]);
      }
      off += snprintf(buf + off, sizeof(buf) - off, "} examples");
      for (int i = 0; i < ph_n && i < 4; i++) {
        off += snprintf(buf + off, sizeof(buf) - off, " x0=%.3f", ph_x0[i]);
      }
      snprintf(buf + off, sizeof(buf) - off, "\n");
      ph_log("%s", buf);
    }
  }

  if (s_ph_calls % 60 == 0) {
    ph_log("phase: calls=%ld splitCalls=%ld qualQuads=%ld\n",
           s_ph_calls, s_ph_split_calls, s_ph_qual_quads);
  }
}

static void glBufferData_phase(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
  if (target == GL_ARRAY_BUFFER && data && size >= 24) ph_scan(data, (long)size);
  glBufferData(target, size, data, usage);
}

static void glBufferSubData_phase(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) {
  if (target == GL_ARRAY_BUFFER && data && size >= 24) ph_scan(data, (long)size);
  glBufferSubData(target, offset, size, data);
}
#endif

// ---------------------------------------------------------------------------
// DIAG_BIGQUAD (flag defined above the texture tracker) -- logs every DISTINCT
// large quad with its texture id, real texture pixel size, on-screen size, UV
// span in texels, and per-axis texel->point scale. Purpose: earlier captures
// showed big quads with anisotropic NON-integer scales (e.g. 1.40625 x 0.41667
// on a 1024x1024 texture = 4.22px x 1.25px per texel docked) -- geometry that
// CANNOT render uniform pixel squares. This capture identifies whether the
// surface the player actually sees the artifact on is one of those quads.
// Dedup'd hard: one line per distinct (tex, size) shape; tiny log, no lag.
// ---------------------------------------------------------------------------
#if DIAG_BIGQUAD
static void bq_log(const char *fmt, ...) {
  FILE *f = fopen("gl_bigquad.log", "a");
  if (!f) return;
  va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
  fclose(f);
}
typedef struct { GLuint tex; float w, h; } BqKey;
static BqKey    s_bq_seen[256];
static int      s_bq_nseen = 0;
static unsigned s_bq_calls = 0;
static int bq_already(GLuint tex, float w, float h) {
  for (int i = 0; i < s_bq_nseen; i++)
    if (s_bq_seen[i].tex == tex &&
        fabsf(s_bq_seen[i].w - w) < 0.51f && fabsf(s_bq_seen[i].h - h) < 0.51f)
      return 1;
  if (s_bq_nseen < 256) { s_bq_seen[s_bq_nseen].tex = tex;
    s_bq_seen[s_bq_nseen].w = w; s_bq_seen[s_bq_nseen].h = h; s_bq_nseen++; }
  return 0;
}
static void bq_scan(const void *data, long size) {
  const unsigned char *p = (const unsigned char *)data;
  long nquads = (size / 24) / 4;
  if (nquads > 2048) nquads = 2048;
  for (long q = 0; q < nquads; q++) {
    const unsigned char *v0 = p + (q * 4 + 0) * 24;
    const unsigned char *v3 = p + (q * 4 + 3) * 24;
    float x0, y0, u0, t0, x3, y3, u3, t3;
    memcpy(&x0, v0 + 0, 4);  memcpy(&y0, v0 + 4, 4);
    memcpy(&u0, v0 + 16, 4); memcpy(&t0, v0 + 20, 4);
    memcpy(&x3, v3 + 0, 4);  memcpy(&y3, v3 + 4, 4);
    memcpy(&u3, v3 + 16, 4); memcpy(&t3, v3 + 20, 4);
    float sw = fabsf(x3 - x0), sh = fabsf(y3 - y0);
    float uw = fabsf(u3 - u0), th = fabsf(t3 - t0);
    // Sprite-sized and larger textured quads (characters are ~8..64pt).
    if (sw < 8.0f || sh < 8.0f) continue;
    if (uw < 0.0005f || th < 0.0005f) continue;
    if (x0 < -2000.0f || x0 > 4000.0f || y0 < -2000.0f || y0 > 4000.0f) continue;
    GLsizei tw = 0, thh = 0;
    int haveTex = texTrack_get(s_cur_tex2d, &tw, &thh);
    if (bq_already(s_cur_tex2d, sw, sh)) continue;
    if (haveTex && tw > 0 && thh > 0) {
      float srcw = uw * (float)tw, srch = th * (float)thh;      // texels sampled
      float sclx = (srcw > 0.01f) ? sw / srcw : 0.0f;           // pts per texel
      float scly = (srch > 0.01f) ? sh / srch : 0.0f;
      float phx = x0 - floorf(x0), phy = y0 - floorf(y0);
      bq_log("BIGQUAD tex=%u(%dx%d) onscreen=%.3fx%.3f pos(%.1f,%.1f) phase=(%.3f,%.3f) "
             "srcTexels=%.2fx%.2f ptPerTexel=%.5fx%.5f%s%s\n",
             s_cur_tex2d, (int)tw, (int)thh, sw, sh, x0, y0, phx, phy,
             srcw, srch, sclx, scly,
             (fabsf(sclx - scly) > 0.002f) ? "  [ANISO]" : "",
             (fabsf(sclx * 2.0f - floorf(sclx * 2.0f + 0.5f)) > 0.01f ||
              fabsf(scly * 2.0f - floorf(scly * 2.0f + 0.5f)) > 0.01f)
               ? " [NONINT]" : "");
    } else {
      bq_log("BIGQUAD tex=%u(?x?) onscreen=%.3fx%.3f pos(%.1f,%.1f) uv=%.5fx%.5f\n",
             s_cur_tex2d, sw, sh, x0, y0, uw, th);
    }
  }
}
static void bq_tick(void) {
  s_bq_calls++;
  if (s_bq_calls % 900 == 0)
    bq_log("bigquad: uploads=%u distinct=%d\n", s_bq_calls, s_bq_nseen);
}
static void glBufferData_bigquad(GLenum target, GLsizeiptr size,
                                 const void *data, GLenum usage) {
  if (target == GL_ARRAY_BUFFER && data && size >= 96 && (size % 24) == 0)
    bq_scan(data, (long)size);
  bq_tick();
  glBufferData(target, size, data, usage);
}
static void glBufferSubData_bigquad(GLenum target, GLintptr offset,
                                    GLsizeiptr size, const void *data) {
  if (target == GL_ARRAY_BUFFER && data && size >= 96 && (size % 24) == 0)
    bq_scan(data, (long)size);
  bq_tick();
  glBufferSubData(target, offset, size, data);
}
#endif /* DIAG_BIGQUAD */

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------

DynLibFunction dynlib_functions[] = {
  // --- bionic runtime bits we must own (NOT the C++ ABI: that is in
  //     libc++_shared.so and resolves there) ---
  { "__sF", (uintptr_t)&fake_sF },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit_fake },
  { "__cxa_finalize", (uintptr_t)&__cxa_finalize_fake },
  { "__errno", (uintptr_t)&__errno },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail_fake },
  { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "syscall", (uintptr_t)&syscall_fake },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "dlsym", (uintptr_t)&dlsym_fake },
  { "dlopen", (uintptr_t)&dlopen_fake },
  { "dlclose", (uintptr_t)&dlclose_fake },
  { "dlerror", (uintptr_t)&dlerror_fake },
  { "openlog", (uintptr_t)&ret0 },
  { "closelog", (uintptr_t)&ret0 },
  { "syslog", (uintptr_t)&ret0 },
  { "abort", (uintptr_t)&abort },
  { "exit", (uintptr_t)&exit },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "__register_atfork", (uintptr_t)&__register_atfork_fake },
  { "__cxa_thread_atexit_impl", (uintptr_t)&__cxa_thread_atexit_impl_fake },

  // android logging
  { "__android_log_print", (uintptr_t)&__android_log_print_fake },
  { "__android_log_write", (uintptr_t)&__android_log_write_fake },
  { "__android_log_assert", (uintptr_t)&__android_log_assert_fake },

  // fortify (_chk) wrappers
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_fake },
  { "__memmove_chk", (uintptr_t)&__memmove_chk_fake },
  { "__memset_chk", (uintptr_t)&__memset_chk_fake },
  { "__strcat_chk", (uintptr_t)&__strcat_chk_fake },
  { "__strchr_chk", (uintptr_t)&__strchr_chk_fake },
  { "__strrchr_chk", (uintptr_t)&__strrchr_chk_fake },
  { "__strcpy_chk", (uintptr_t)&__strcpy_chk_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__strncat_chk", (uintptr_t)&__strncat_chk_fake },
  { "__strncpy_chk", (uintptr_t)&__strncpy_chk_fake },
  { "__strncpy_chk2", (uintptr_t)&__strncpy_chk2_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__vsprintf_chk", (uintptr_t)&__vsprintf_chk_fake },
  { "__snprintf_chk", (uintptr_t)&__snprintf_chk_fake },
  { "__sprintf_chk", (uintptr_t)&__sprintf_chk_fake },
  { "__read_chk", (uintptr_t)&__read_chk_fake },
  { "__open_2", (uintptr_t)&__open_2_fake },
  { "__FD_SET_chk", (uintptr_t)&__FD_SET_chk_fake },
  { "__FD_CLR_chk", (uintptr_t)&__FD_CLR_chk_fake },
  { "__FD_ISSET_chk", (uintptr_t)&__FD_ISSET_chk_fake },

  // AAsset NDK API (served from the loose assets, see asset.c)
  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava_fake },
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open_fake },
  { "AAssetManager_openDir", (uintptr_t)&AAssetManager_openDir_fake },
  { "AAsset_close", (uintptr_t)&AAsset_close_fake },
  { "AAsset_getLength", (uintptr_t)&AAsset_getLength_fake },
  { "AAsset_getLength64", (uintptr_t)&AAsset_getLength64_fake },
  { "AAsset_read", (uintptr_t)&AAsset_read_fake },
  { "AAsset_seek", (uintptr_t)&AAsset_seek_fake },
  { "AAsset_seek64", (uintptr_t)&AAsset_seek64_fake },
  { "AAsset_getRemainingLength", (uintptr_t)&AAsset_getRemainingLength_fake },
  { "AAsset_getRemainingLength64", (uintptr_t)&AAsset_getRemainingLength64_fake },
  { "AAsset_getBuffer", (uintptr_t)&AAsset_getBuffer_fake },
  { "AAsset_openFileDescriptor", (uintptr_t)&AAsset_openFileDescriptor_fake },
  { "AAsset_isAllocated", (uintptr_t)&AAsset_isAllocated_fake },
  { "AAssetDir_getNextFileName", (uintptr_t)&AAssetDir_getNextFileName_fake },
  { "AAssetDir_rewind", (uintptr_t)&AAssetDir_rewind_fake },
  { "AAssetDir_close", (uintptr_t)&AAssetDir_close_fake },

  // memory
  { "malloc", (uintptr_t)&malloc },
  { "calloc", (uintptr_t)&calloc },
  { "realloc", (uintptr_t)&realloc },
  { "free", (uintptr_t)&free },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },
  { "memalign", (uintptr_t)&memalign },
  { "malloc_usable_size", (uintptr_t)&malloc_usable_size },

  // mem/str
  { "memchr", (uintptr_t)&memchr },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "memrchr", (uintptr_t)&memrchr },
  { "strcat", (uintptr_t)&strcat },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcasecmp", (uintptr_t)&strcasecmp },
  { "strcpy", (uintptr_t)&strcpy },
  { "strdup", (uintptr_t)&strdup },
  { "strlen", (uintptr_t)&strlen },
  { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy },
  { "strncat", (uintptr_t)&strncat },
  { "strrchr", (uintptr_t)&strrchr },
  { "strstr", (uintptr_t)&strstr },
  { "strpbrk", (uintptr_t)&strpbrk },
  { "strspn", (uintptr_t)&strspn },
  { "strcspn", (uintptr_t)&strcspn },
  { "strtok", (uintptr_t)&strtok },
  { "strtok_r", (uintptr_t)&strtok_r },
  { "strerror", (uintptr_t)&strerror },
  { "strerror_r", (uintptr_t)&strerror_r_fake },
  { "strcoll", (uintptr_t)&strcoll },
  { "strxfrm", (uintptr_t)&strxfrm },
  { "stpcpy", (uintptr_t)&stpcpy },
  { "strtod", (uintptr_t)&strtod },
  { "strtof", (uintptr_t)&strtof },
  { "strtol", (uintptr_t)&strtol },
  { "strtold", (uintptr_t)&strtold },
  { "strtoll", (uintptr_t)&strtoll },
  { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull },
  { "atoi", (uintptr_t)&atoi },
  { "atol", (uintptr_t)&atol },
  { "atoll", (uintptr_t)&atoll },
  { "atof", (uintptr_t)&atof },
  { "toupper", (uintptr_t)&toupper },
  { "tolower", (uintptr_t)&tolower },
  { "bsearch", (uintptr_t)&bsearch },
  { "qsort", (uintptr_t)&qsort },
  { "rand", (uintptr_t)&rand },
  { "srand", (uintptr_t)&srand },
  { "getenv", (uintptr_t)&getenv },
  { "setenv", (uintptr_t)&setenv },
  { "system", (uintptr_t)&system_fake },

  // wide char + locale
  { "wcslen", (uintptr_t)&wcslen },
  { "wcscmp", (uintptr_t)&wcscmp },
  { "wcsncmp", (uintptr_t)&wcsncmp },
  { "wcscpy", (uintptr_t)&wcscpy },
  { "wcsncpy", (uintptr_t)&wcsncpy },
  { "wcsxfrm", (uintptr_t)&wcsxfrm },
  { "wcscoll", (uintptr_t)&wcscoll },
  { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof },
  { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold },
  { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul },
  { "wcstoull", (uintptr_t)&wcstoull },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },
  { "fputwc", (uintptr_t)&fputwc },
  { "getwc", (uintptr_t)&getwc },
  { "ungetwc", (uintptr_t)&ungetwc },
  { "fgetwc", (uintptr_t)&fgetwc },
  { "wcrtomb", (uintptr_t)&wcrtomb },
  { "wctob", (uintptr_t)&wctob },
  { "btowc", (uintptr_t)&btowc },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "mbrlen", (uintptr_t)&mbrlen },
  { "mbtowc", (uintptr_t)&mbtowc },
  { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs_fake },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs_fake },
  { "setlocale", (uintptr_t)&setlocale_fake },
  { "localeconv", (uintptr_t)&localeconv },
  { "newlocale", (uintptr_t)&newlocale_fake },
  { "freelocale", (uintptr_t)&freelocale_fake },
  { "uselocale", (uintptr_t)&uselocale_fake },
  { "iswalpha_l", (uintptr_t)&iswalpha_l_fake },
  { "iswblank_l", (uintptr_t)&iswblank_l_fake },
  { "iswcntrl_l", (uintptr_t)&iswcntrl_l_fake },
  { "iswdigit_l", (uintptr_t)&iswdigit_l_fake },
  { "iswlower_l", (uintptr_t)&iswlower_l_fake },
  { "iswprint_l", (uintptr_t)&iswprint_l_fake },
  { "iswpunct_l", (uintptr_t)&iswpunct_l_fake },
  { "iswspace_l", (uintptr_t)&iswspace_l_fake },
  { "iswupper_l", (uintptr_t)&iswupper_l_fake },
  { "iswxdigit_l", (uintptr_t)&iswxdigit_l_fake },
  { "towlower_l", (uintptr_t)&towlower_l_fake },
  { "towupper_l", (uintptr_t)&towupper_l_fake },
  { "strcoll_l", (uintptr_t)&strcoll_l_fake },
  { "strxfrm_l", (uintptr_t)&strxfrm_l_fake },
  { "strftime_l", (uintptr_t)&strftime_l_fake },
  { "strtod_l", (uintptr_t)&strtod_l_fake },
  { "strtof_l", (uintptr_t)&strtof_l_fake },
  { "strtold_l", (uintptr_t)&strtold_l_fake },
  { "strtol_l", (uintptr_t)&strtol_l_fake },
  { "strtoul_l", (uintptr_t)&strtoul_l_fake },
  { "strtoll_l", (uintptr_t)&strtoll_l_fake },
  { "strtoull_l", (uintptr_t)&strtoull_l_fake },
  { "wcscoll_l", (uintptr_t)&wcscoll_l_fake },
  { "wcsxfrm_l", (uintptr_t)&wcsxfrm_l_fake },

  // printf family
  { "printf", (uintptr_t)&debugPrintf },
  { "putchar", (uintptr_t)&putchar },
  { "puts", (uintptr_t)&puts },
  { "perror", (uintptr_t)&ret0 },
  { "snprintf", (uintptr_t)&snprintf },
  { "sprintf", (uintptr_t)&sprintf },
  { "swprintf", (uintptr_t)&swprintf },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf },
  { "vasprintf", (uintptr_t)&vasprintf },
  { "asprintf", (uintptr_t)&asprintf },
  { "sscanf", (uintptr_t)&sscanf },
  { "vsscanf", (uintptr_t)&vsscanf },

  // math
  { "acos", (uintptr_t)&acos },
  { "acosf", (uintptr_t)&acosf },
  { "asin", (uintptr_t)&asin },
  { "asinf", (uintptr_t)&asinf },
  { "atan", (uintptr_t)&atan },
  { "atanf", (uintptr_t)&atanf },
  { "atan2", (uintptr_t)&atan2 },
  { "atan2f", (uintptr_t)&atan2f },
  { "ceil", (uintptr_t)&ceil },
  { "ceilf", (uintptr_t)&ceilf },
  { "cos", (uintptr_t)&cos },
  { "cosf", (uintptr_t)&cosf },
  { "cosh", (uintptr_t)&cosh },
  { "exp", (uintptr_t)&exp },
  { "exp2", (uintptr_t)&exp2 },
  { "exp2f", (uintptr_t)&exp2f },
  { "expf", (uintptr_t)&expf },
  { "floor", (uintptr_t)&floor },
  { "floorf", (uintptr_t)&floorf },
  { "fmod", (uintptr_t)&fmod },
  { "fmodf", (uintptr_t)&fmodf },
  { "frexp", (uintptr_t)&frexp },
  { "frexpf", (uintptr_t)&frexpf },
  { "ldexp", (uintptr_t)&ldexp },
  { "log", (uintptr_t)&log },
  { "log10", (uintptr_t)&log10 },
  { "log10f", (uintptr_t)&log10f },
  { "logf", (uintptr_t)&logf },
  { "modf", (uintptr_t)&modf },
  { "modff", (uintptr_t)&modff },
  { "pow", (uintptr_t)&pow },
  { "powf", (uintptr_t)&powf },
  { "round", (uintptr_t)&round },
  { "roundf", (uintptr_t)&roundf },
  { "sin", (uintptr_t)&sin },
  { "sinf", (uintptr_t)&sinf },
  { "sinh", (uintptr_t)&sinh },
  { "sincos", (uintptr_t)&sincos_fake },
  { "sincosf", (uintptr_t)&sincosf_fake },
  { "sqrt", (uintptr_t)&sqrt },
  { "sqrtf", (uintptr_t)&sqrtf },
  { "tan", (uintptr_t)&tan },
  { "tanf", (uintptr_t)&tanf },
  { "tanh", (uintptr_t)&tanh },
  { "trunc", (uintptr_t)&trunc },

  // time
  { "gettimeofday", (uintptr_t)&gettimeofday_paced },
  { "clock_gettime", (uintptr_t)&clock_gettime_fake },
  { "clock", (uintptr_t)&clock },
  { "gmtime", (uintptr_t)&gmtime },
  { "gmtime_r", (uintptr_t)&gmtime_r },
  { "localtime", (uintptr_t)&localtime },
  { "localtime_r", (uintptr_t)&localtime_r },
  { "mktime", (uintptr_t)&mktime },
  { "strftime", (uintptr_t)&strftime },
  { "time", (uintptr_t)&time },
  { "nanosleep", (uintptr_t)&nanosleep },
  { "usleep", (uintptr_t)&usleep },
  { "sleep", (uintptr_t)&sleep },

  // stdio (over the fake bionic __sF and buffered fopen)
  { "fopen", (uintptr_t)&fopen_fake },
  { "fdopen", (uintptr_t)&fdopen },
  { "freopen", (uintptr_t)&freopen },
  { "fclose", (uintptr_t)&fclose_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "fgets", (uintptr_t)&fgets },
  { "fgetc", (uintptr_t)&fgetc },
  { "getc", (uintptr_t)&getc_fake },
  { "ungetc", (uintptr_t)&ungetc_fake },
  { "fseek", (uintptr_t)&fseek_fake },
  { "fseeko", (uintptr_t)&fseeko },
  { "ftell", (uintptr_t)&ftell },
  { "ftello", (uintptr_t)&ftello },
  { "rewind", (uintptr_t)&rewind },
  { "fflush", (uintptr_t)&fflush_fake },
  { "ferror", (uintptr_t)&ferror_fake },
  { "feof", (uintptr_t)&feof },
  { "fileno", (uintptr_t)&fileno_fake },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "fputc", (uintptr_t)&fputc_fake },
  { "fputs", (uintptr_t)&fputs_fake },
  { "putc", (uintptr_t)&fputc_fake },
  { "setbuf", (uintptr_t)&setbuf_fake },
  { "setvbuf", (uintptr_t)&setvbuf },
  { "vfprintf", (uintptr_t)&vfprintf_fake },
  { "remove", (uintptr_t)&remove },
  { "rename", (uintptr_t)&rename },
  { "tmpfile", (uintptr_t)&tmpfile },

  // posix file
  { "open", (uintptr_t)&open_fake },
  { "openat", (uintptr_t)&openat_fake },
  { "__openat", (uintptr_t)&openat_fake },
  { "close", (uintptr_t)&close },
  { "read", (uintptr_t)&read },
  { "write", (uintptr_t)&write },
  { "lseek", (uintptr_t)&lseek },
  { "lseek64", (uintptr_t)&lseek },
  { "ftruncate", (uintptr_t)&ftruncate },
  { "truncate", (uintptr_t)&truncate },
  { "unlink", (uintptr_t)&unlink },
  { "unlinkat", (uintptr_t)&unlinkat_fake },
  { "access", (uintptr_t)&access },
  { "mkdir", (uintptr_t)&mkdir },
  { "rmdir", (uintptr_t)&rmdir },
  { "chdir", (uintptr_t)&chdir },
  { "getcwd", (uintptr_t)&getcwd },
  { "isatty", (uintptr_t)&isatty },
  { "link", (uintptr_t)&link },
  { "symlink", (uintptr_t)&symlink },
  { "readlink", (uintptr_t)&readlink },
  { "realpath", (uintptr_t)&realpath_fake },
  { "opendir", (uintptr_t)&opendir },
  { "fdopendir", (uintptr_t)&fdopendir_fake },
  { "closedir", (uintptr_t)&closedir },
  { "readdir", (uintptr_t)&readdir_fake },
  { "rewinddir", (uintptr_t)&rewinddir },
  { "stat", (uintptr_t)&stat_fake },
  { "fstat", (uintptr_t)&fstat_fake },
  { "lstat", (uintptr_t)&lstat_fake },
  { "fstatat", (uintptr_t)&fstatat_fake },
  { "statvfs", (uintptr_t)&statvfs_fake },
  { "fchmod", (uintptr_t)&fchmod_fake },
  { "fchmodat", (uintptr_t)&fchmodat_fake },
  { "chmod", (uintptr_t)&ret0 },
  { "utimensat", (uintptr_t)&utimensat_fake },
  { "sendfile", (uintptr_t)&sendfile_fake },
  { "pathconf", (uintptr_t)&pathconf_fake },
  { "mmap", (uintptr_t)&mmap_fake },
  { "munmap", (uintptr_t)&munmap_fake },
  { "sysconf", (uintptr_t)&sysconf_fake },

  // pthread
  { "pthread_create", (uintptr_t)&pthread_create_fake },
  { "pthread_join", (uintptr_t)&pthread_join },
  { "pthread_detach", (uintptr_t)&pthread_detach_fake },
  { "pthread_self", (uintptr_t)&pthread_self },
  { "pthread_exit", (uintptr_t)&pthread_exit },
  { "pthread_equal", (uintptr_t)&pthread_equal },
  { "pthread_key_create", (uintptr_t)&pthread_key_create },
  { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific },
  { "pthread_once", (uintptr_t)&pthread_once_fake },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake },
  { "pthread_mutexattr_destroy", (uintptr_t)&ret0 },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake },
  { "pthread_condattr_init", (uintptr_t)&ret0 },
  { "pthread_condattr_destroy", (uintptr_t)&ret0 },
  { "pthread_condattr_setclock", (uintptr_t)&pthread_condattr_setclock_fake },
  { "pthread_rwlock_init", (uintptr_t)&pthread_rwlock_init_fake },
  { "pthread_rwlock_destroy", (uintptr_t)&pthread_rwlock_destroy_fake },
  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake },
  { "pthread_attr_init", (uintptr_t)&ret0 },
  { "pthread_attr_destroy", (uintptr_t)&ret0 },
  { "pthread_attr_setdetachstate", (uintptr_t)&ret0 },
  { "pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize_fake },
  { "pthread_attr_getstacksize", (uintptr_t)&pthread_attr_getstacksize_fake },
  { "pthread_attr_setschedparam", (uintptr_t)&pthread_attr_setschedparam_fake },
  { "pthread_attr_getschedparam", (uintptr_t)&pthread_attr_getschedparam_fake },
  { "pthread_setschedparam", (uintptr_t)&pthread_setschedparam_fake },
  { "pthread_setname_np", (uintptr_t)&pthread_setname_np_fake },

  // semaphores
  { "sem_init", (uintptr_t)&sem_init_fake },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_post", (uintptr_t)&sem_post_fake },
  { "sem_wait", (uintptr_t)&sem_wait_fake },
  { "sem_trywait", (uintptr_t)&sem_trywait_fake },
  { "sem_getvalue", (uintptr_t)&sem_getvalue_fake },

  // misc extras
  { "getpid", (uintptr_t)&getpid_fake },
  { "gettid", (uintptr_t)&gettid_fake },
  { "sched_yield", (uintptr_t)&sched_yield_fake },
  { "longjmp", (uintptr_t)&longjmp },
  { "setjmp", (uintptr_t)&setjmp },

  // networking (Cocos2dxDownloader) -- stubbed
  { "socket", (uintptr_t)&socket_fake },
  { "connect", (uintptr_t)&connect_fake },
  { "bind", (uintptr_t)&bind_fake },
  { "listen", (uintptr_t)&listen_fake },
  { "accept", (uintptr_t)&accept_fake },
  { "sendto", (uintptr_t)&sendto_fake },
  { "recvfrom", (uintptr_t)&recvfrom_fake },
  { "select", (uintptr_t)&select_fake },
  { "setsockopt", (uintptr_t)&setsockopt_fake },
  { "getaddrinfo", (uintptr_t)&getaddrinfo_fake },
  { "freeaddrinfo", (uintptr_t)&freeaddrinfo_fake },
  { "gai_strerror", (uintptr_t)&gai_strerror_fake },
  { "inet_ntop", (uintptr_t)&inet_ntop_fake },
  { "inet_pton", (uintptr_t)&inet_pton_fake },
  { "ioctl", (uintptr_t)&ioctl_fake },

  // EGL (only eglGetProcAddress is used by libchrono; resolve to real EGL)
  { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress },

  // GLES2 fixed entry points (mesa libGLESv2)
  { "glActiveTexture", (uintptr_t)&glActiveTexture },
  { "glAttachShader", (uintptr_t)&glAttachShader },
  { "glBindAttribLocation", (uintptr_t)&glBindAttribLocation },
  { "glBindBuffer", (uintptr_t)&glBindBuffer },
  { "glBindFramebuffer", (uintptr_t)&glBindFramebuffer },
  { "glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer },
#if DIAG_VERTS || DIAG_BIGQUAD
  { "glBindTexture", (uintptr_t)&glBindTexture_track },
#else
  { "glBindTexture", (uintptr_t)&glBindTexture },
#endif
  { "glBlendColor", (uintptr_t)&glBlendColor },
  { "glBlendEquation", (uintptr_t)&glBlendEquation },
  { "glBlendEquationSeparate", (uintptr_t)&glBlendEquationSeparate },
  { "glBlendFunc", (uintptr_t)&glBlendFunc },
  { "glBlendFuncSeparate", (uintptr_t)&glBlendFuncSeparate },
#if PIXEL_SNAP_TEST
  { "glBufferData", (uintptr_t)&glBufferData_snap },
  { "glBufferSubData", (uintptr_t)&glBufferSubData_snap },
#elif DIAG_VERTS
  { "glBufferData", (uintptr_t)&glBufferData_diag },
  { "glBufferSubData", (uintptr_t)&glBufferSubData_diag },
#elif DIAG_PHASE
  { "glBufferData", (uintptr_t)&glBufferData_phase },
  { "glBufferSubData", (uintptr_t)&glBufferSubData_phase },
#elif DIAG_BIGQUAD
  { "glBufferData", (uintptr_t)&glBufferData_bigquad },
  { "glBufferSubData", (uintptr_t)&glBufferSubData_bigquad },
#else
  { "glBufferData", (uintptr_t)&glBufferData },
  { "glBufferSubData", (uintptr_t)&glBufferSubData },
#endif
  { "glCheckFramebufferStatus", (uintptr_t)&glCheckFramebufferStatus },
  { "glClear", (uintptr_t)&glClear },
  { "glClearColor", (uintptr_t)&glClearColor },
  { "glClearDepthf", (uintptr_t)&glClearDepthf },
  { "glClearStencil", (uintptr_t)&glClearStencil },
  { "glColorMask", (uintptr_t)&glColorMask },
  { "glCompileShader", (uintptr_t)&glCompileShader },
  { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D },
  { "glCompressedTexSubImage2D", (uintptr_t)&glCompressedTexSubImage2D },
  { "glCreateProgram", (uintptr_t)&glCreateProgram },
  { "glCreateShader", (uintptr_t)&glCreateShader },
  { "glCullFace", (uintptr_t)&glCullFace },
  { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
  { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers },
  { "glDeleteProgram", (uintptr_t)&glDeleteProgram },
  { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers },
  { "glDeleteShader", (uintptr_t)&glDeleteShader },
  { "glDeleteTextures", (uintptr_t)&glDeleteTextures },
  { "glDepthFunc", (uintptr_t)&glDepthFunc },
  { "glDepthMask", (uintptr_t)&glDepthMask },
  { "glDepthRangef", (uintptr_t)&glDepthRangef },
  { "glDetachShader", (uintptr_t)&glDetachShader },
  { "glDisable", (uintptr_t)&glDisable },
  { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray },
  { "glDrawArrays", (uintptr_t)&glDrawArrays },
  { "glDrawElements", (uintptr_t)&glDrawElements },
  { "glEnable", (uintptr_t)&glEnable },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
  { "glFinish", (uintptr_t)&glFinish },
  { "glFlush", (uintptr_t)&glFlush },
  { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer },
  { "glFramebufferTexture2D", (uintptr_t)&glFramebufferTexture2D },
  { "glFrontFace", (uintptr_t)&glFrontFace },
  { "glGenBuffers", (uintptr_t)&glGenBuffers },
  { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers },
  { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers },
  { "glGenTextures", (uintptr_t)&glGenTextures },
  { "glGenerateMipmap", (uintptr_t)&glGenerateMipmap },
  { "glGetActiveAttrib", (uintptr_t)&glGetActiveAttrib },
  { "glGetActiveUniform", (uintptr_t)&glGetActiveUniform },
  { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
  { "glGetBooleanv", (uintptr_t)&glGetBooleanv },
  { "glGetError", (uintptr_t)&glGetError },
  { "glGetFloatv", (uintptr_t)&glGetFloatv },
  { "glGetIntegerv", (uintptr_t)&glGetIntegerv },
  { "glGetProgramInfoLog", (uintptr_t)&glGetProgramInfoLog },
  { "glGetProgramiv", (uintptr_t)&glGetProgramiv },
  { "glGetShaderInfoLog", (uintptr_t)&glGetShaderInfoLog },
  { "glGetShaderSource", (uintptr_t)&glGetShaderSource },
  { "glGetShaderiv", (uintptr_t)&glGetShaderiv },
  { "glGetString", (uintptr_t)&glGetString },
  { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
  { "glHint", (uintptr_t)&glHint },
  { "glIsBuffer", (uintptr_t)&glIsBuffer },
  { "glIsEnabled", (uintptr_t)&glIsEnabled },
  { "glIsRenderbuffer", (uintptr_t)&glIsRenderbuffer },
  { "glLineWidth", (uintptr_t)&glLineWidth },
  { "glLinkProgram", (uintptr_t)&glLinkProgram },
  { "glMapBufferOES", (uintptr_t)&gl_MapBufferOES },
  { "glUnmapBufferOES", (uintptr_t)&gl_UnmapBufferOES },
  { "glPixelStorei", (uintptr_t)&glPixelStorei },
  { "glPolygonOffset", (uintptr_t)&glPolygonOffset },
  { "glReadPixels", (uintptr_t)&glReadPixels },
  { "glRenderbufferStorage", (uintptr_t)&glRenderbufferStorage },
  { "glScissor", (uintptr_t)&glScissor },
  { "glShaderSource", (uintptr_t)&glShaderSource },
  { "glStencilFunc", (uintptr_t)&glStencilFunc },
  { "glStencilFuncSeparate", (uintptr_t)&glStencilFuncSeparate },
  { "glStencilMask", (uintptr_t)&glStencilMask },
  { "glStencilOp", (uintptr_t)&glStencilOp },
  { "glStencilOpSeparate", (uintptr_t)&glStencilOpSeparate },
#if DIAG_VERTS || DIAG_BIGQUAD
  { "glTexImage2D", (uintptr_t)&glTexImage2D_guard_tracked },
#else
  { "glTexImage2D", (uintptr_t)&glTexImage2D_guard },
#endif
  { "glTexParameterf", (uintptr_t)&glTexParameterf_nearest },
  { "glTexParameterfv", (uintptr_t)&glTexParameterfv },
  { "glTexParameteri", (uintptr_t)&glTexParameteri_nearest },
  { "glTexSubImage2D", (uintptr_t)&glTexSubImage2D },
  { "glUniform1f", (uintptr_t)&glUniform1f },
  { "glUniform1fv", (uintptr_t)&glUniform1fv },
  { "glUniform1i", (uintptr_t)&glUniform1i },
  { "glUniform1iv", (uintptr_t)&glUniform1iv },
  { "glUniform2f", (uintptr_t)&glUniform2f },
  { "glUniform2fv", (uintptr_t)&glUniform2fv },
  { "glUniform2i", (uintptr_t)&glUniform2i },
  { "glUniform2iv", (uintptr_t)&glUniform2iv },
  { "glUniform3f", (uintptr_t)&glUniform3f },
  { "glUniform3fv", (uintptr_t)&glUniform3fv },
  { "glUniform3i", (uintptr_t)&glUniform3i },
  { "glUniform3iv", (uintptr_t)&glUniform3iv },
  { "glUniform4f", (uintptr_t)&glUniform4f },
  { "glUniform4fv", (uintptr_t)&glUniform4fv },
  { "glUniform4i", (uintptr_t)&glUniform4i },
  { "glUniform4iv", (uintptr_t)&glUniform4iv },
  { "glUniformMatrix2fv", (uintptr_t)&glUniformMatrix2fv },
  { "glUniformMatrix3fv", (uintptr_t)&glUniformMatrix3fv },
#if DIAG_MVP
  { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv_mvp },
#else
  { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv },
#endif
  { "glUseProgram", (uintptr_t)&glUseProgram },
  { "glValidateProgram", (uintptr_t)&glValidateProgram },
  { "glVertexAttrib1f", (uintptr_t)&glVertexAttrib1f },
  { "glVertexAttrib2fv", (uintptr_t)&glVertexAttrib2fv },
  { "glVertexAttrib3fv", (uintptr_t)&glVertexAttrib3fv },
  { "glVertexAttrib4fv", (uintptr_t)&glVertexAttrib4fv },
  { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer },
#if DIAG_MVP
  { "glViewport", (uintptr_t)&glViewport_mvp },
#else
  { "glViewport", (uintptr_t)&glViewport },
#endif
  // VAO functions (glGenVertexArraysOES etc.) are fetched by the engine through
  // eglGetProcAddress, so they are not listed here.

  // OpenSL ES (SDL2-backed shim, see opensles.c)
  { "slCreateEngine", (uintptr_t)&slCreateEngine },
  { "SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE },
  { "SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY },
  { "SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE },
  { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE },
  { "SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME },
  { "SL_IID_OUTPUTMIX", (uintptr_t)&SL_IID_OUTPUTMIX },
  { "SL_IID_OBJECT", (uintptr_t)&SL_IID_OBJECT },
  { "SL_IID_NULL", (uintptr_t)&SL_IID_NULL },
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

void ct_resolve_imports(so_module *mod) {
  so_relocate(mod);
  so_resolve(mod, dynlib_functions, (int)dynlib_numfunctions, 1);
}