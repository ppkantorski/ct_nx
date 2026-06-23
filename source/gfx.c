/* gfx.c -- system-font text rasterisation (FreeType on the Switch shared font)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <switch.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "gfx.h"
#include "util.h"

// We load the full set of Switch shared fonts and fall back across them per
// glyph, so Latin, CJK and symbol coverage all work (the engine may ask us to
// draw Japanese with the system font).
#define MAX_FACES 6
static FT_Library g_ft;
static FT_Face g_faces[MAX_FACES];
static int g_face_count = 0;
static int g_ok = 0;

void gfx_init(void) {
  if (FT_Init_FreeType(&g_ft)) {
    debugPrintf("gfx: FT_Init_FreeType failed\n");
    return;
  }

  static const PlSharedFontType types[] = {
    PlSharedFontType_Standard,
    PlSharedFontType_NintendoExt,
    PlSharedFontType_ChineseSimplified,
    PlSharedFontType_ExtChineseSimplified,
    PlSharedFontType_ChineseTraditional,
    PlSharedFontType_KO,
  };
  for (unsigned i = 0; i < sizeof(types) / sizeof(*types) && g_face_count < MAX_FACES; i++) {
    PlFontData font;
    if (R_FAILED(plGetSharedFontByType(&font, types[i])))
      continue;
    if (FT_New_Memory_Face(g_ft, font.address, font.size, 0, &g_faces[g_face_count]) == 0)
      g_face_count++;
  }

  g_ok = g_face_count > 0;
  if (!g_ok)
    debugPrintf("gfx: no shared fonts available\n");
}

// minimal UTF-8 decoder: returns the codepoint and advances *p
static uint32_t utf8_next(const char **p) {
  const unsigned char *s = (const unsigned char *)*p;
  uint32_t c = *s++;
  if (c >= 0xF0 && s[0] && s[1] && s[2]) {
    c = ((c & 0x07) << 18) | ((s[0] & 0x3F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    s += 3;
  } else if (c >= 0xE0 && s[0] && s[1]) {
    c = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F);
    s += 2;
  } else if (c >= 0xC0 && s[0]) {
    c = ((c & 0x1F) << 6) | (s[0] & 0x3F);
    s += 1;
  }
  *p = (const char *)s;
  return c;
}

// pick the first face that has a glyph for cp and load+render it; returns the
// face used (with glyph rendered into its slot) or NULL.
static FT_Face load_glyph(uint32_t cp, int px) {
  for (int i = 0; i < g_face_count; i++) {
    if (FT_Get_Char_Index(g_faces[i], cp) == 0 && cp != ' ')
      continue;
    FT_Set_Pixel_Sizes(g_faces[i], 0, px);
    if (FT_Load_Char(g_faces[i], cp, FT_LOAD_RENDER) == 0)
      return g_faces[i];
  }
  // last resort: render with the primary face's notdef
  if (g_face_count > 0) {
    FT_Set_Pixel_Sizes(g_faces[0], 0, px);
    if (FT_Load_Char(g_faces[0], cp, FT_LOAD_RENDER) == 0)
      return g_faces[0];
  }
  return NULL;
}

// measure the pixel width of a single line of text at px size
static int measure_line(const char *s, const char *end, int px) {
  int w = 0;
  const char *p = s;
  while (p < end && *p) {
    uint32_t cp = utf8_next(&p);
    if (cp == '\n') break;
    FT_Face f = load_glyph(cp, px);
    if (f) w += (int)(f->glyph->advance.x >> 6);
  }
  return w;
}

unsigned char *gfx_render_text_rgba(const char *text, int font_size,
                                    int r, int g, int b, int a,
                                    int align_h, int max_w, int max_h, int wrap,
                                    int *out_w, int *out_h) {
  if (!g_ok || !text)
    return NULL;
  const int px = font_size > 0 ? font_size : 16;

  // The Switch shared font is visually larger per pixel than the Android font the
  // engine's sizes target, so render glyphs a touch smaller (rpx) and centre them
  // in the full px line cell: matches Android's size without clipping descenders.
  int rpx = px - (px / 8 + 1);
  if (rpx < 1) rpx = 1;

  // cell/line metrics from the primary face at the full engine size
  FT_Set_Pixel_Sizes(g_faces[0], 0, px);
  int ascender = (int)(g_faces[0]->size->metrics.ascender >> 6);
  int descender = (int)(-(g_faces[0]->size->metrics.descender >> 6));
  int line_h = (int)(g_faces[0]->size->metrics.height >> 6);
  if (line_h <= 0) line_h = px + px / 4;
  if (ascender <= 0) ascender = (px * 4) / 5;

  // reduced-size glyph metrics, for vertical centring within each px line cell
  FT_Set_Pixel_Sizes(g_faces[0], 0, rpx);
  int asc_r = (int)(g_faces[0]->size->metrics.ascender >> 6);
  int desc_r = (int)(-(g_faces[0]->size->metrics.descender >> 6));
  if (asc_r <= 0) asc_r = (rpx * 4) / 5;
  int content_h = asc_r + desc_r;
  if (content_h <= 0) content_h = rpx;
  int top_pad = (line_h - content_h) / 2;
  if (top_pad < 0) top_pad = 0;

  // split into lines on '\n'; optionally greedy-wrap to max_w
  // (we collect line start/end byte ranges)
  #define MAX_LINES 256
  const char *ls[MAX_LINES];
  const char *le[MAX_LINES];
  int nlines = 0;
  const char *p = text;
  const char *line_start = text;
  while (*p && nlines < MAX_LINES) {
    const char *cur = p;
    uint32_t cp = utf8_next(&p);
    if (cp == '\n') {
      ls[nlines] = line_start; le[nlines] = cur; nlines++;
      line_start = p;
      continue;
    }
    if (wrap && max_w > 0) {
      int w = measure_line(line_start, p, rpx);
      if (w > max_w && cur != line_start) {
        // break before the current glyph (prefer a previous space if any)
        const char *brk = cur;
        for (const char *q = cur; q > line_start; q--) {
          if (*q == ' ') { brk = q; break; }
        }
        ls[nlines] = line_start; le[nlines] = brk; nlines++;
        line_start = (*brk == ' ') ? brk + 1 : brk;
        p = line_start;
      }
    }
  }
  if (nlines < MAX_LINES) { ls[nlines] = line_start; le[nlines] = p; nlines++; }

  // measured width = widest line; height = lines * line_h
  int meas_w = 0;
  for (int i = 0; i < nlines; i++) {
    int w = measure_line(ls[i], le[i], rpx);
    if (w > meas_w) meas_w = w;
  }
  int meas_h = nlines * line_h;
  if (meas_h < ascender + descender) meas_h = ascender + descender;

  int W = max_w > 0 ? max_w : meas_w;
  int H = max_h > 0 ? max_h : meas_h;
  if (W <= 0) W = 1;
  if (H <= 0) H = 1;
  if (W > 4096) W = 4096;
  if (H > 4096) H = 4096;

  unsigned char *out = calloc((size_t)W * H * 4, 1);
  if (!out)
    return NULL;

  for (int li = 0; li < nlines; li++) {
    int lw = measure_line(ls[li], le[li], rpx);
    int pen_x = 0;
    if (align_h == GFX_ALIGN_CENTER) pen_x = (W - lw) / 2;
    else if (align_h == GFX_ALIGN_RIGHT) pen_x = W - lw;
    if (pen_x < 0) pen_x = 0;
    // baseline of the reduced-size glyphs, centred in this line's px cell
    const int baseline = li * line_h + top_pad + asc_r;

    const char *q = ls[li];
    while (q < le[li] && *q) {
      uint32_t cp = utf8_next(&q);
      if (cp == '\n') break;
      FT_Face f = load_glyph(cp, rpx);
      if (!f) continue;
      FT_GlyphSlot sl = f->glyph;
      const int gx = pen_x + sl->bitmap_left;
      const int gy = baseline - sl->bitmap_top;
      for (unsigned ry = 0; ry < sl->bitmap.rows; ry++) {
        const int dy = gy + (int)ry;
        if (dy < 0 || dy >= H) continue;
        const uint8_t *srow = sl->bitmap.buffer + (size_t)ry * sl->bitmap.pitch;
        for (unsigned rx = 0; rx < sl->bitmap.width; rx++) {
          const int dx = gx + (int)rx;
          if (dx < 0 || dx >= W) continue;
          const int cov = srow[rx];
          if (!cov) continue;
          // premultiplied RGBA8888 (byte order R,G,B,A)
          const int af = cov * a / 255;       // final alpha
          unsigned char *px4 = out + ((size_t)dy * W + dx) * 4;
          // simple "over" against transparent: write the strongest coverage
          if (af > px4[3]) {
            px4[0] = (unsigned char)(r * af / 255);
            px4[1] = (unsigned char)(g * af / 255);
            px4[2] = (unsigned char)(b * af / 255);
            px4[3] = (unsigned char)af;
          }
        }
      }
      pen_x += (int)(sl->advance.x >> 6);
    }
  }

  if (out_w) *out_w = W;
  if (out_h) *out_h = H;
  return out;
}
