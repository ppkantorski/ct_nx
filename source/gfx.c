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
#include "config.h"

// We load the full set of Switch shared fonts and fall back across them per
// glyph, so Latin, CJK and symbol coverage all work (the engine may ask us to
// draw Japanese with the system font).
#define MAX_FACES 6
static FT_Library g_ft;
static FT_Face g_faces[MAX_FACES];
static int g_face_count = 0;
static int g_ok = 0;

// Optional in-game font (config.game_font): a TTF loaded from the SD/assets and
// used as the PRIMARY face for the engine's system-font labels, so they match
// the game's look. The shared fonts above stay loaded as per-glyph fallback for
// anything the game font lacks (CJK, symbols).
static FT_Face g_game_face = NULL;
static int g_game_ok = 0;

// horizontal squeeze applied to the game font only (config.game_font_xscale): a
// proportional font wants 1.0, but a wide/monospaced font can be condensed to
// keep the engine's pre-broken lines inside their window. Set per render.
static double g_game_xscale = 1.0;

// Try config.game_font_path first, then a few common asset locations. Loading is
// lazy-tolerant: any failure just leaves g_game_ok = 0 and we use the shared font.
static void load_game_font(void) {
  if (!config.game_font)
    return;

  const char *paths[8];
  int n = 0;
  if (config.game_font_path[0])
    paths[n++] = config.game_font_path;
  // best-effort probes for a bundled TTF (harmless if absent)
  paths[n++] = "assets/font/font.ttf";
  paths[n++] = "assets/fonts/font.ttf";
  paths[n++] = "assets/Font/Font.ttf";
  paths[n++] = "assets/res/font.ttf";
  paths[n++] = "font.ttf";

  for (int i = 0; i < n; i++) {
    if (FT_New_Face(g_ft, paths[i], 0, &g_game_face) == 0) {
      g_game_ok = 1;
      debugPrintf("gfx: using game font \"%s\"\n", paths[i]);
      return;
    }
  }
  debugPrintf("gfx: no game font found; using the Switch shared font\n");
}

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

  load_game_font();

  g_ok = (g_face_count > 0) || g_game_ok;
  if (!g_ok)
    debugPrintf("gfx: no fonts available\n");
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

// ---------------------------------------------------------------------------
// glyph cache
//
// The engine rebuilds a dialogue label's whole bitmap every time its string
// changes -- and Chrono Trigger's typewriter effect changes it once per
// revealed character, so a single line of dialogue can call all the way down
// into gfx_render_text_rgba() dozens of times in a couple of seconds. On top
// of that, a single call used to measure each line's width up to three times
// over (once per character while checking word-wrap, once to find the
// widest line, once again per-line during the draw pass), and "measuring"
// meant calling load_glyph(), which always did a full FT_LOAD_RENDER --
// anti-aliased rasterisation, the single most expensive thing FreeType does
// here, just to read an advance width.
//
// None of that needs to happen more than once per (face, codepoint, pixel
// size). This cache makes every repeat of it a hash lookup instead of a
// re-rasterisation, both within one call (wrap-check vs. width vs. draw) and
// across calls (the same glyphs reappearing every typewriter frame). It does
// not change what gets rendered -- same face-selection order, same pixel
// sizes, same bitmaps -- it only avoids recomputing them.
// ---------------------------------------------------------------------------
typedef struct {
  int used;
  FT_Face face;
  uint32_t cp;
  int px;
  int advance_x;            // 26.6 fixed point, same units as FT glyph->advance.x
  int bitmap_left, bitmap_top;
  unsigned int width, rows;
  int pitch;
  unsigned char *buffer;    // owned copy of the glyph bitmap; NULL if blank (e.g. space)
} CachedGlyph;

#define GLYPH_CACHE_BITS 10
#define GLYPH_CACHE_SIZE (1 << GLYPH_CACHE_BITS)
#define GLYPH_CACHE_MASK (GLYPH_CACHE_SIZE - 1)
static CachedGlyph g_glyph_cache[GLYPH_CACHE_SIZE];

static unsigned glyph_hash(FT_Face face, uint32_t cp, int px) {
  uintptr_t h = (uintptr_t)face;
  h = h * 2654435761u ^ cp;
  h = h * 2654435761u ^ (unsigned)px;
  return (unsigned)(h ^ (h >> 16)) & GLYPH_CACHE_MASK;
}

// Cache lookup/fill for one specific face (no fallback selection here -- the
// caller has already decided which face to use). hpx is the FT_Set_Pixel_Sizes
// horizontal size argument (0 = square/auto, matching the original call sites).
// Direct-mapped: a collision just evicts the old entry, never a correctness
// issue since the (face,cp,px) tag is checked before treating it as a hit.
static CachedGlyph *get_glyph_for_face(FT_Face face, uint32_t cp, int px, FT_UInt hpx) {
  if (!face) return NULL;
  unsigned slot = glyph_hash(face, cp, px);
  CachedGlyph *c = &g_glyph_cache[slot];
  if (c->used && c->face == face && c->cp == cp && c->px == px)
    return c; // hit: no FreeType call at all

  FT_Set_Pixel_Sizes(face, hpx, px);
  if (FT_Load_Char(face, cp, FT_LOAD_RENDER) != 0)
    return NULL;

  FT_GlyphSlot sl = face->glyph;
  if (c->buffer) { free(c->buffer); c->buffer = NULL; }
  c->used = 1;
  c->face = face;
  c->cp = cp;
  c->px = px;
  c->advance_x = (int)sl->advance.x;
  c->bitmap_left = sl->bitmap_left;
  c->bitmap_top = sl->bitmap_top;
  c->width = sl->bitmap.width;
  c->rows = sl->bitmap.rows;
  c->pitch = sl->bitmap.pitch;
  if (c->rows > 0 && c->pitch != 0) {
    // FreeType's buffer pointer is always the lowest address in memory and
    // spans rows * |pitch| bytes, regardless of pitch's sign; copying that
    // verbatim and keeping the original (possibly negative) pitch means
    // indexing the copy later behaves exactly like indexing FreeType's own
    // buffer would have.
    size_t n = (size_t)c->rows * (size_t)(c->pitch < 0 ? -c->pitch : c->pitch);
    c->buffer = malloc(n);
    if (c->buffer) memcpy(c->buffer, sl->bitmap.buffer, n);
    else c->rows = 0; // OOM: degrade to a blank glyph rather than risk a bad read
  }
  return c;
}

// pick the first face that has a glyph for cp, same fallback order as before
// (game font, then each shared font, then a last-resort notdef face), and
// return its cached/rendered glyph.
static CachedGlyph *get_glyph(uint32_t cp, int px) {
  FT_Face face = NULL;
  FT_UInt hpx = 0;

  // game font first, so system-font labels adopt the in-game look; the shared
  // fonts below cover any glyph the game font is missing.
  if (g_game_ok && (FT_Get_Char_Index(g_game_face, cp) != 0 || cp == ' ')) {
    face = g_game_face;
    // non-square size condenses the game font horizontally when g_game_xscale < 1
    hpx = (FT_UInt)(px * g_game_xscale + 0.5);
    if (hpx < 1) hpx = 1;
  } else {
    for (int i = 0; i < g_face_count; i++) {
      if (FT_Get_Char_Index(g_faces[i], cp) == 0 && cp != ' ')
        continue;
      face = g_faces[i];
      break;
    }
  }
  if (!face) // last resort: notdef from whichever primary face we have
    face = g_game_ok ? g_game_face : (g_face_count > 0 ? g_faces[0] : NULL);

  return get_glyph_for_face(face, cp, px, hpx);
}

// rendered pixel height of one glyph at a given size; used to scale a game font
// so its visible glyphs match what the shared font produced (CT pixel fonts fill
// the em box and otherwise render far too large).
static int glyph_px_height(FT_Face f, uint32_t cp, int px) {
  if (!f || px < 1) return 0;
  CachedGlyph *g = get_glyph_for_face(f, cp, px, 0);
  return g ? (int)g->rows : 0;
}

// measure the pixel width of a single line of text at px size
static int measure_line(const char *s, const char *end, int px) {
  int w = 0;
  const char *p = s;
  while (p < end && *p) {
    uint32_t cp = utf8_next(&p);
    if (cp == '\n') break;
    CachedGlyph *g = get_glyph(cp, px);
    if (g) w += (g->advance_x >> 6);
  }
  return w;
}

// actual rendered ink extents of a line: tallest pixels above the baseline
// (*above) and deepest below it (*below). Unlike font ascender/descender these
// reflect the glyphs really present, so we can centre the visible text rather
// than the font's metric box (which reserves space for accents/descenders the
// text may not use -- that reserved space is what pushed labels too high).
static void line_ink_extents(const char *s, const char *end, int px,
                             int *above, int *below) {
  int a = 0, b = 0;
  const char *p = s;
  while (p < end && *p) {
    uint32_t cp = utf8_next(&p);
    if (cp == '\n') break;
    CachedGlyph *g = get_glyph(cp, px);
    if (!g || g->rows == 0) continue; // skip spaces/empties
    int top = g->bitmap_top;                                   // above baseline
    int bot = (int)g->rows - top;                              // below baseline
    if (top > a) a = top;
    if (bot > b) b = bot;
  }
  *above = a; *below = b;
}

// Composite one cached glyph into the premultiplied RGBA8888 buffer at (gx,gy).
// cr/cg/cb are the 0-255 colour; ca scales the glyph coverage to a final alpha.
// `under` selects the Porter-Duff operator:
//   under == 0  source-OVER-dest -- the text: painted on top of whatever's there
//               (the shadow, or the clear background -- on clear it reduces to a
//               plain write, identical to the old path).
//   under != 0  dest-OVER-source -- the shadow: slid *beneath* existing ink, so a
//               glyph's shadow can never darken text that's already been drawn,
//               whatever order the glyphs arrive in. That order-independence is
//               what lets the shadow and the text be laid down in a single pass.
// Both buffer and output stay premultiplied, matching nativeInitBitmapDC's format.
static void blit_glyph(unsigned char *out, int W, int H,
                       int gx, int gy, const CachedGlyph *g,
                       int cr, int cg, int cb, int ca, int under) {
  if (!g || !g->buffer || ca <= 0) return;
  for (unsigned ry = 0; ry < g->rows; ry++) {
    const int dy = gy + (int)ry;
    if (dy < 0 || dy >= H) continue;
    const uint8_t *srow = g->buffer + (size_t)ry * g->pitch;
    for (unsigned rx = 0; rx < g->width; rx++) {
      const int dx = gx + (int)rx;
      if (dx < 0 || dx >= W) continue;
      const int cov = srow[rx];
      if (!cov) continue;
      const int sa = cov * ca / 255;            // this fragment's alpha
      if (!sa) continue;
      unsigned char *px4 = out + ((size_t)dy * W + dx) * 4;
      if (under) {
        // dest-over-source: out = dst + src*(1 - dst_a). For a black shadow the
        // colour terms are zero, so this only fills alpha where the cell is still
        // (partly) clear -- never over the text.
        const int id = 255 - px4[3];
        px4[0] = (unsigned char)(px4[0] + cr * sa / 255 * id / 255);
        px4[1] = (unsigned char)(px4[1] + cg * sa / 255 * id / 255);
        px4[2] = (unsigned char)(px4[2] + cb * sa / 255 * id / 255);
        px4[3] = (unsigned char)(px4[3] + sa * id / 255);
      } else {
        // source-over-dest: out = src + dst*(1 - src_a).
        const int is = 255 - sa;
        px4[0] = (unsigned char)(cr * sa / 255 + px4[0] * is / 255);
        px4[1] = (unsigned char)(cg * sa / 255 + px4[1] * is / 255);
        px4[2] = (unsigned char)(cb * sa / 255 + px4[2] * is / 255);
        px4[3] = (unsigned char)(sa + px4[3] * is / 255);
      }
    }
  }
}

unsigned char *gfx_render_text_rgba(const char *text, int font_size,
                                    int r, int g, int b, int a,
                                    int align_h, int align_v,
                                    int max_w, int max_h, int wrap,
                                    int *out_w, int *out_h) {
  if (!g_ok || !text)
    return NULL;
  const int px = font_size > 0 ? font_size : 16;

  // horizontal squeeze for the game font (1.0 = none). Affects glyph rendering
  // and width measurement via load_glyph, so wrapping/sizing stay consistent.
  g_game_xscale = (g_game_ok && config.game_font_xscale > 0.0f)
                    ? (double)config.game_font_xscale : 1.0;

  // ALL vertical layout -- line spacing, baseline and centring -- comes from the
  // shared font, which already sits correctly in the engine's boxes. We never
  // derive the baseline from the game font's own metrics (those differ per font
  // and were what pushed some labels too high). The game font only supplies the
  // glyph bitmaps, scaled so its cap height matches the shared font's so the two
  // share a baseline. (Falls back to the game face if no shared font loaded.)
  FT_Face cell_face = (g_face_count > 0) ? g_faces[0] : g_game_face;
  FT_Set_Pixel_Sizes(cell_face, 0, px);
  int ascender = (int)(cell_face->size->metrics.ascender >> 6);
  int descender = (int)(-(cell_face->size->metrics.descender >> 6));
  int line_h = (int)(cell_face->size->metrics.height >> 6);
  if (line_h <= 0) line_h = px + px / 4;
  if (ascender <= 0) ascender = (px * 4) / 5;

  // reference reduced size: what the shared font renders glyphs at. Baseline and
  // cell centring are computed from the shared font at THIS size, always.
  const int cell_rpx = (px - (px / 8 + 1) < 1) ? 1 : (px - (px / 8 + 1));
  FT_Set_Pixel_Sizes(cell_face, 0, cell_rpx);
  int asc_r = (int)(cell_face->size->metrics.ascender >> 6);
  int desc_r = (int)(-(cell_face->size->metrics.descender >> 6));
  if (asc_r <= 0) asc_r = (cell_rpx * 4) / 5;
  int content_h = asc_r + desc_r;
  if (content_h <= 0) content_h = cell_rpx;
  int top_pad = (line_h - content_h) / 2;
  if (top_pad < 0) top_pad = 0;

  // Glyph RENDER size (rpx). Shared font: cell_rpx. Game font: scaled so a
  // capital 'A' matches the shared font's 'A' height at cell_rpx, so both fonts
  // share the baseline above; then game_font_scale fine-tunes.
  FT_Face mf = g_game_ok ? g_game_face : g_faces[0];
  int rpx;
  if (g_game_ok) {
    int ref_cap  = glyph_px_height(cell_face, 'A', cell_rpx);
    int game_cap = glyph_px_height(mf, 'A', px);
    double s = (game_cap > 0 && ref_cap > 0) ? (double)ref_cap / game_cap : 1.0;
    if (config.game_font_scale > 0.0f) s *= (double)config.game_font_scale;
    rpx = (int)(px * s + 0.5);
  } else {
    rpx = cell_rpx;
  }
  if (rpx < 1) rpx = 1;

  // Drop shadow (config.text_shadow): a crisp, hard-edged shadow offset down and
  // to the right, in the classic CT/SNES style. The offset scales with the glyph
  // size (so it stays the same *proportion* at every label size) -- ~1px per 12px
  // of glyph -- rather than a blur, which would smear a pixel font. sh is the
  // offset in pixels for both axes; shadow_a folds in the label's own alpha so a
  // faded label fades its shadow with it.
  int sh = 0, shadow_a = 0;
  if (config.text_shadow && config.text_shadow_alpha > 0 && a > 0) {
    double k = (config.text_shadow_scale > 0.0f) ? (double)config.text_shadow_scale : 1.0;
    sh = (int)(rpx * k / 12.0 + 0.5);
    if (sh < 1) sh = 1;
    if (sh > 8) sh = 8;                       // keep it sane at huge sizes
    int sa = config.text_shadow_alpha;
    if (sa > 255) sa = 255;
    shadow_a = a * sa / 255;
    if (shadow_a <= 0) sh = 0;
  }

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

  // measured width = widest line; height = lines * line_h. Stash each line's
  // width here too -- the draw pass below needs the same number per line for
  // alignment, and with glyphs now cached the redundant FreeType work is gone,
  // but there's no reason to even repeat the hash lookups for a number we
  // already have.
  int meas_w = 0;
  int lwidths[MAX_LINES];
  for (int i = 0; i < nlines; i++) {
    int w = measure_line(ls[i], le[i], rpx);
    lwidths[i] = w;
    if (w > meas_w) meas_w = w;
  }
  int meas_h = nlines * line_h;
  if (meas_h < ascender + descender) meas_h = ascender + descender;

  // Width: size the bitmap to the actual text (content width), like the Android
  // Cocos2dxBitmap does. max_w is only a wrap/limit hint (used above), NOT the
  // output width -- the engine's Label centres/aligns this content-width texture
  // within the field node itself. Forcing W to max_w (e.g. an EditBox's 200px
  // box) bakes left padding into the texture, which the engine then anchors from
  // the left, pushing the text right of the field's centre.
  // Height stays padded to max_h: the engine does NOT re-centre vertically, so
  // we centre the text in the box height ourselves (see y_off below).
  // sh extends the bitmap by the shadow offset on the right and bottom only, so
  // the shadow always has room and is never clipped (the glyphs themselves stay
  // aligned within the meas_w/meas_h content box -- see pen_x below).
  int W = meas_w + sh;
  int H = max_h > 0 ? max_h : (meas_h + sh);
  if (W <= 0) W = 1;
  if (H <= 0) H = 1;

  // position the whole text block vertically within the (possibly taller) box.
  // EditBox/label fields hand us a box taller than the text and expect it
  // vertically centred; without this the glyphs pin to the top and sit high in
  // the field. block_h is the laid-out text height (line cells). Computed before
  // the buffer is sized so we can guarantee the last line's shadow row(s) fit.
  const int block_h = nlines * line_h;
  int y_off = 0;
  if (align_v == GFX_VALIGN_CENTER)      y_off = (H - block_h) / 2;
  else if (align_v == GFX_VALIGN_BOTTOM) y_off = H - block_h;
  if (y_off < 0) y_off = 0;
  if (H < y_off + block_h + sh) H = y_off + block_h + sh; // keep the shadow inside

  if (W > 4096) W = 4096;
  if (H > 4096) H = 4096;

  unsigned char *out = calloc((size_t)W * H * 4, 1);
  if (!out)
    return NULL;

  // Stable vertical reference for the game-font path. Every line is centred on
  // ONE fixed ink box -- the cap height (plus any descender depth) of a constant
  // set of plain reference glyphs, measured once here -- NOT on each line's own
  // glyphs. Centring on per-line ink made the baseline drift whenever the glyphs
  // changed: an accent (a, e, i, o, u with a tilde/acute, or n-tilde) reaches a
  // pixel higher than a bare capital, which nudged that line down, so Spanish
  // text appeared to jump as accented characters appeared (and flickered through
  // the typewriter reveal). A fixed reference keeps the baseline identical for
  // "Si" and "Si-acute"; accents and descenders simply extend into the cell's
  // padding instead of moving the line. Reference is ASCII-only on purpose.
  int ref_above = 0, ref_below = 0;
  if (g_game_ok) {
    static const char REF[] = "AHKMWXgjpqyў♪"; // caps -> cap height; g/j/p/q/y -> descender
    line_ink_extents(REF, REF + sizeof(REF) - 1, rpx, &ref_above, &ref_below);
    if (ref_above <= 0) ref_above = (rpx * 7) / 10; // fallback if refs somehow blank
  }

  for (int li = 0; li < nlines; li++) {
    int lw = lwidths[li];
    // Align each line within the content width (meas_w), not the padded W: the
    // extra sh pixels are reserved on the right for the shadow, so right/centre
    // alignment must not consume them or the shadow would spill off the edge.
    int pen_x = 0;
    if (align_h == GFX_ALIGN_CENTER) pen_x = (meas_w - lw) / 2;
    else if (align_h == GFX_ALIGN_RIGHT) pen_x = meas_w - lw;
    if (pen_x < 0) pen_x = 0;

    // Baseline within this line's cell. For the game font we centre a FIXED ink
    // box (ref_above/ref_below, computed once above) in the cell, so every line
    // shares one baseline regardless of which glyphs it contains -- short labels
    // still sit visually centred, but accents/descenders no longer shift the line
    // (see the ref_above note above). The shared font keeps its tuned metric
    // baseline (it already fits).
    int baseline;
    if (g_game_ok) {
      int cell_top = (line_h - (ref_above + ref_below)) / 2; // centre the ref box
      if (cell_top < 0) cell_top = 0;
      baseline = y_off + li * line_h + cell_top + ref_above;
    } else {
      baseline = y_off + li * line_h + top_pad + asc_r;
    }

    const char *q = ls[li];
    while (q < le[li] && *q) {
      uint32_t cp = utf8_next(&q);
      if (cp == '\n') break;
      CachedGlyph *gph = get_glyph(cp, rpx);
      if (!gph) continue;
      const int gx = pen_x + gph->bitmap_left;
      const int gy = baseline - gph->bitmap_top;
      // Shadow first (slid underneath, so it never touches text already drawn),
      // then the glyph itself on top. Both go through one composite per pixel.
      if (sh)
        blit_glyph(out, W, H, gx + sh, gy + sh, gph, 0, 0, 0, shadow_a, 1);
      blit_glyph(out, W, H, gx, gy, gph, r, g, b, a, 0);
      pen_x += (gph->advance_x >> 6);
    }
  }

  if (out_w) *out_w = W;
  if (out_h) *out_h = H;
  return out;
}