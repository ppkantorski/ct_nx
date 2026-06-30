/* cache_progress.c -- "Caching mods..." splash drawn directly over the live
 * EGL surface while modpack.c rebuilds the mod cache on a cache miss.
 *
 * Mirrors the raw-GLES2 quad pattern in movie_player.c: this runs before the
 * engine has rendered a single frame of its own (still inside the first
 * e_nativeInit() call), on the same thread that owns the EGL context set up
 * by egl_init() in main.c, so we can draw directly with no engine
 * cooperation needed -- and tear it back down via cocos2d's own
 * invalidateStateCache before handing control back, exactly like
 * movie_player.c does after a cutscene.
 *
 * The text comes from gfx_render_text_rgba(), the same system-font
 * rasteriser the engine's own labels use, so the splash matches whatever
 * font (shared Switch font or a configured game font) is already in use
 * elsewhere -- no separate font, nothing engine-specific.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "cache_progress.h"
#include "gfx.h"
#include "config.h"
#include "util.h"

static void (*s_gl_invalidate)(void);

void cache_progress_set_gl_invalidate(void (*fn)(void)) {
  s_gl_invalidate = fn;
}

/* --- shaders ------------------------------------------------------------
 * Two tiny programs: a flat-colour one for the bar border/fill rectangles,
 * and a textured one (straight alpha-blended) for the text label. Both take
 * vertex positions already in clip space -- see rect_verts() below. */

static const char *VSH_FLAT =
  "attribute vec2 aPos; void main(){ gl_Position = vec4(aPos, 0.0, 1.0); }";
static const char *FSH_FLAT =
  "precision mediump float; uniform vec4 uColor;"
  "void main(){ gl_FragColor = uColor; }";

static const char *VSH_TEX =
  "attribute vec2 aPos; attribute vec2 aTex; varying vec2 vTex;"
  "void main(){ vTex = aTex; gl_Position = vec4(aPos, 0.0, 1.0); }";
static const char *FSH_TEX =
  "precision mediump float; varying vec2 vTex; uniform sampler2D uTex;"
  "void main(){ gl_FragColor = texture2D(uTex, vTex); }";

static GLuint compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { char log[256]; glGetShaderInfoLog(s, sizeof(log), NULL, log); debugPrintf("cache_progress: shader err: %s\n", log); }
  return s;
}

static GLuint link_prog(GLuint vs, GLuint fs) {
  GLuint p = glCreateProgram();
  glAttachShader(p, vs);
  glAttachShader(p, fs);
  glLinkProgram(p);
  GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) debugPrintf("cache_progress: program link failed\n");
  return p;
}

typedef struct {
  int    ready;
  GLuint flat_prog, flat_vs, flat_fs;
  GLint  flat_aPos, flat_uColor;
  GLuint tex_prog, tex_vs, tex_fs;
  GLint  tex_aPos, tex_aTex, tex_uTex;
  GLuint text_tex;
  int    text_w, text_h;
  int    last_percent;  // -1 = none rendered yet; avoids re-rasterising the
                         // label every call when the displayed percent hasn't
                         // changed since the last one.
  int    sw, sh;
} GLState;

static GLState g;

/* Pixel-space (origin top-left, y-down, matching screen_width/screen_height)
 * -> a triangle-strip quad in clip space. Vertex order TL, BL, TR, BR, the
 * same convention movie_player.c uses for its letterboxed quad. */
static void rect_verts(float x, float y, float w, float h, int sw, int sh, GLfloat out[8]) {
  float x0 = (x       / (float)sw) * 2.0f - 1.0f;
  float x1 = ((x + w) / (float)sw) * 2.0f - 1.0f;
  float y0 = 1.0f - (y       / (float)sh) * 2.0f;
  float y1 = 1.0f - ((y + h) / (float)sh) * 2.0f;
  out[0] = x0; out[1] = y0; /* TL */
  out[2] = x0; out[3] = y1; /* BL */
  out[4] = x1; out[5] = y0; /* TR */
  out[6] = x1; out[7] = y1; /* BR */
}

static void draw_flat_rect(float x, float y, float w, float h, float r, float gg, float b, float a) {
  if (w <= 0.0f || h <= 0.0f) return;
  GLfloat quad[8];
  rect_verts(x, y, w, h, g.sw, g.sh, quad);
  glUseProgram(g.flat_prog);
  glUniform4f(g.flat_uColor, r, gg, b, a);
  glEnableVertexAttribArray(g.flat_aPos);
  glVertexAttribPointer(g.flat_aPos, 2, GL_FLOAT, GL_FALSE, 0, quad);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(g.flat_aPos);
}

static void draw_text(float x, float y) {
  if (!g.text_tex || g.text_w <= 0 || g.text_h <= 0) return;
  GLfloat pos[8];
  rect_verts(x, y, (float)g.text_w, (float)g.text_h, g.sw, g.sh, pos);
  static const GLfloat uv[8] = { 0,0,  0,1,  1,0,  1,1 };
  glUseProgram(g.tex_prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g.text_tex);
  glUniform1i(g.tex_uTex, 0);
  glEnableVertexAttribArray(g.tex_aPos);
  glEnableVertexAttribArray(g.tex_aTex);
  glVertexAttribPointer(g.tex_aPos, 2, GL_FLOAT, GL_FALSE, 0, pos);
  glVertexAttribPointer(g.tex_aTex, 2, GL_FLOAT, GL_FALSE, 0, uv);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(g.tex_aPos);
  glDisableVertexAttribArray(g.tex_aTex);
}

static float ui_scale(void) {
  float scale = (float)g.sh / 720.0f;
  if (scale < 0.5f) scale = 0.5f;
  if (scale > 2.5f) scale = 2.5f;
  return scale;
}

static int ensure_ready(void) {
  if (g.ready) return 1;
  memset(&g, 0, sizeof(g));
  g.last_percent = -1;

  GLint vp[4] = { 0, 0, 0, 0 };
  glGetIntegerv(GL_VIEWPORT, vp); // the engine's current viewport == screen
  g.sw = vp[2] > 0 ? vp[2] : screen_width;
  g.sh = vp[3] > 0 ? vp[3] : screen_height;
  if (g.sw <= 0) g.sw = 1280;
  if (g.sh <= 0) g.sh = 720;

  g.flat_vs   = compile(GL_VERTEX_SHADER, VSH_FLAT);
  g.flat_fs   = compile(GL_FRAGMENT_SHADER, FSH_FLAT);
  g.flat_prog = link_prog(g.flat_vs, g.flat_fs);
  g.flat_aPos   = glGetAttribLocation(g.flat_prog, "aPos");
  g.flat_uColor = glGetUniformLocation(g.flat_prog, "uColor");

  g.tex_vs   = compile(GL_VERTEX_SHADER, VSH_TEX);
  g.tex_fs   = compile(GL_FRAGMENT_SHADER, FSH_TEX);
  g.tex_prog = link_prog(g.tex_vs, g.tex_fs);
  g.tex_aPos = glGetAttribLocation(g.tex_prog, "aPos");
  g.tex_aTex = glGetAttribLocation(g.tex_prog, "aTex");
  g.tex_uTex = glGetUniformLocation(g.tex_prog, "uTex");

  glGenTextures(1, &g.text_tex);
  glBindTexture(GL_TEXTURE_2D, g.text_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  g.ready = (g.flat_prog && g.tex_prog && g.text_tex);
  return g.ready;
}

/* "Caching mods... NN%" translated into whatever language the engine itself
 * is about to come up in -- same nine codes/aliases main.c's lang_index()
 * recognises for DeviceInfo::mCurrentLanguage, so the splash always matches
 * resources.bin's Localize/<code>/ set rather than always reading English.
 * %d stays a plain printf conversion in every string; only the wording
 * around it changes. */
static const char *caching_label_fmt(void) {
  const char *l = config.language;
  if (!strcmp(l, "ja")) return "MODをキャッシュ中... %d%%";
  if (!strcmp(l, "de")) return "Mods werden zwischengespeichert... %d%%";
  if (!strcmp(l, "it")) return "Memorizzazione delle mod in cache... %d%%";
  if (!strcmp(l, "es")) return "Almacenando mods en caché... %d%%";
  if (!strcmp(l, "fr")) return "Mise en cache des mods... %d%%";
  if (!strcmp(l, "zh") || !strcmp(l, "zh-Hans") || !strcmp(l, "zh_CN"))
    return "正在缓存模组... %d%%";
  if (!strcmp(l, "zh-Hant") || !strcmp(l, "zh_TW"))
    return "正在快取模組... %d%%";
  if (!strcmp(l, "ko")) return "모드 캐싱 중... %d%%";
  return "Caching mods... %d%%"; // "en" and unrecognised codes
}

/* (Re-)rasterises the caching label into g.text_tex via the same system-font
 * path the engine's own labels use, only when the displayed percentage has
 * actually changed. */
static void update_label(int percent) {
  if (percent == g.last_percent) return;
  g.last_percent = percent;

  char buf[64];
  snprintf(buf, sizeof(buf), caching_label_fmt(), percent);

  int font_px = (int)(30.0f * ui_scale() + 0.5f);
  if (font_px < 14) font_px = 14;

  int tw = 0, th = 0;
  unsigned char *rgba = gfx_render_text_rgba(buf, font_px,
                                              255, 255, 255, 255,
                                              GFX_ALIGN_LEFT, GFX_VALIGN_TOP,
                                              0, 0, 0, &tw, &th);
  if (!rgba || tw <= 0 || th <= 0) {
    if (rgba) free(rgba);
    debugPrintf("cache_progress: text render failed\n");
    return;
  }

  glBindTexture(GL_TEXTURE_2D, g.text_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
  free(rgba);
  g.text_w = tw;
  g.text_h = th;
}

void cache_progress_update(float frac) {
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  if (!ensure_ready()) return;

  int percent = (int)(frac * 100.0f + 0.5f);
  if (percent > 100) percent = 100;
  update_label(percent);

  float scale = ui_scale();

  const float bar_w_ref  = 420.0f; // outer border box, design (1280x720) px
  const float bar_h_ref  = 26.0f;
  const float border_ref = 2.0f;
  const float gap_ref    = 4.0f;   // inset kept between border and fill, all sides
  const float vgap_ref   = 18.0f;  // gap between the text block and bar block

  float bar_w  = bar_w_ref  * scale;
  float bar_h  = bar_h_ref  * scale;
  float border = border_ref * scale; if (border < 1.0f) border = 1.0f;
  float igap   = gap_ref    * scale; if (igap < 1.0f) igap = 1.0f;
  float vgap   = vgap_ref   * scale;

  float cx = (float)g.sw * 0.5f;
  float cy = (float)g.sh * 0.5f;

  /* Text and bar sit symmetrically above/below screen centre, so the
   * midpoint between the text's centre and the bar's centre is the screen
   * centre, with a fixed visual gap between the two blocks. */
  float k = (vgap + bar_h * 0.5f + (float)g.text_h * 0.5f) * 0.5f;
  float text_cy = cy - k;
  float bar_cy  = cy + k;

  float text_x = cx - (float)g.text_w * 0.5f;
  float text_y = text_cy - (float)g.text_h * 0.5f;

  float bar_x = cx - bar_w * 0.5f;
  float bar_y = bar_cy - bar_h * 0.5f;

  glViewport(0, 0, g.sw, g.sh);
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // premultiplied alpha (gfx.c's format)
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  draw_text(text_x, text_y);

  /* Border: 4 thin strips forming an unfilled outline rectangle. */
  draw_flat_rect(bar_x,                  bar_y,                  bar_w,  border, 1,1,1,1);
  draw_flat_rect(bar_x,                  bar_y + bar_h - border, bar_w,  border, 1,1,1,1);
  draw_flat_rect(bar_x,                  bar_y,                  border, bar_h,  1,1,1,1);
  draw_flat_rect(bar_x + bar_w - border, bar_y,                  border, bar_h,  1,1,1,1);

  /* Inner fill: inset from the border by a small constant gap on every side
   * (left/right/top/bottom), growing rightward from that inset left edge. */
  float inner_x = bar_x + border + igap;
  float inner_y = bar_y + border + igap;
  float inner_w = bar_w - 2.0f * (border + igap);
  float inner_h = bar_h - 2.0f * (border + igap);
  if (inner_w < 0.0f) inner_w = 0.0f;
  if (inner_h < 0.0f) inner_h = 0.0f;
  draw_flat_rect(inner_x, inner_y, inner_w * frac, inner_h, 1,1,1,1);

  EGLDisplay dpy = eglGetCurrentDisplay();
  EGLSurface sfc = eglGetCurrentSurface(EGL_DRAW);
  if (dpy != EGL_NO_DISPLAY && sfc != EGL_NO_SURFACE)
    eglSwapBuffers(dpy, sfc);
}

void cache_progress_finish(void) {
  if (g.text_tex)  glDeleteTextures(1, &g.text_tex);
  if (g.flat_prog) glDeleteProgram(g.flat_prog);
  if (g.flat_vs)   glDeleteShader(g.flat_vs);
  if (g.flat_fs)   glDeleteShader(g.flat_fs);
  if (g.tex_prog)  glDeleteProgram(g.tex_prog);
  if (g.tex_vs)    glDeleteShader(g.tex_vs);
  if (g.tex_fs)    glDeleteShader(g.tex_fs);
  glUseProgram(0);
  memset(&g, 0, sizeof(g));

  if (s_gl_invalidate) s_gl_invalidate();
}