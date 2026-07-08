/* gfx.c -- system-font text rasterisation (FreeType on the Switch shared font)
 *
 * Copyright (C) 2026 NaGaa95 <https://github.com/NaGaa95>
 * Copyright (C) 2026 ppkantorski <https://github.com/ppkantorski>
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
#include <arm_neon.h>

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
  int abs_pitch;            // abs(pitch): row stride in bytes, cached to avoid a branch per row
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
  c->abs_pitch = c->pitch < 0 ? -c->pitch : c->pitch;
  if (c->rows > 0 && c->pitch != 0) {
    // FreeType's buffer pointer is always the lowest address in memory and
    // spans rows * |pitch| bytes, regardless of pitch's sign; copying that
    // verbatim and keeping the original (possibly negative) pitch means
    // indexing the copy later behaves exactly like indexing FreeType's own
    // buffer would have.
    size_t n = (size_t)c->rows * (size_t)c->abs_pitch;
    c->buffer = malloc(n);
    if (c->buffer) memcpy(c->buffer, sl->bitmap.buffer, n);
    else c->rows = 0; // OOM: degrade to a blank glyph rather than risk a bad read
  }
  return c;
}

// ---------------------------------------------------------------------------
// pick the first face that has a glyph for cp (game font, then each shared
// font, then a last-resort notdef face), and return its cached/rendered glyph.
//
// Note: in-message <BTN_*> tags (e.g. "[A]") are now plain ASCII produced by
// the libchrono glyph patch, so they render through this normal path in
// whatever face is already drawing the surrounding text -- no Private-Use
// codepoints and no separate shared-font icon lookup is needed here anymore.
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
//
// *below is allowed to come out NEGATIVE. Normally a glyph's ink reaches at
// least down to the font's own baseline (bot = rows - bitmap_top >= 0), but
// some fonts (e.g. ChronoType.ttf, an SNES-style pixel font) bake in a small
// gap so that even plain capitals stop short of the baseline -- their bottom
// row sits a few pixels *above* it. That gap is proportional to the render
// size (measured ~0.19*px for ChronoType at every size tested), not a fixed
// pixel count, so it has to be measured per call rather than guessed as a
// constant. If we clamped *below* at 0 here we'd silently overstate how tall
// the reference ink box is for such fonts by exactly that gap, which is what
// previously made the centred text sit visibly too high -- and by an amount
// that grew or shrank with font_size, so no single fixed offset could ever
// correct it across every text box in the game. Letting *below go negative
// lets the caller's box shrink to the font's *real* measured ink span, so
// centring is correct at every size with no per-font fudge factor needed.
static void line_ink_extents(const char *s, const char *end, int px,
                             int *above, int *below) {
  int a = 0, b = 0, have = 0;
  const char *p = s;
  while (p < end && *p) {
    uint32_t cp = utf8_next(&p);
    if (cp == '\n') break;
    CachedGlyph *g = get_glyph(cp, px);
    if (!g || g->rows == 0) continue; // skip spaces/empties
    int top = g->bitmap_top;                                   // above baseline
    int bot = (int)g->rows - top;                              // below baseline (may be < 0)
    if (!have || top > a) a = top;
    if (!have || bot > b) b = bot;
    have = 1;
  }
  *above = a; *below = b;
}

// ---------------------------------------------------------------------------
// NEON-accelerated glyph compositing helpers
//
// The buffer is plain linear RGBA8888 (R at byte 0, A at byte 3), premultiplied,
// matching nativeInitBitmapDC's expected format.  Because the layout is linear
// we can use vld4_u8/vst4_u8 to deinterleave/reinterleave all four channels in
// one instruction -- much cleaner than tesla's RGBA4444 packed-shift approach.
//
// Division by 255 is approximated as (x * 257) >> 16 in the scalar path, and
// as >>8 (i.e. /256) in the NEON path.  The NEON approximation is off by at
// most 1 count per channel, imperceptible in text rendering.  The scalar path
// uses the exact >>16 trick to stay faithful to the original /255 semantics.
//
// Two Porter-Duff operators are implemented:
//   source-over  (under == 0): text painted on top of whatever is there.
//   dest-over    (under != 0): shadow slid *beneath* existing ink.  For a
//                black shadow (cr=cg=cb=0) the colour terms vanish, leaving
//                only the alpha accumulation -- the shadow fast path exploits
//                this to skip three of the four multiply-add lanes.
// ---------------------------------------------------------------------------

// Fast integer approximation of (x * y) / 255 using the identity
//   x*y/255 == (x*y*257) >> 16   for x,y in [0,255]
// The result is exact for all inputs; no /255 division is emitted.
#define MUL255(x, y) (((unsigned)(x) * (unsigned)(y) * 257u) >> 16)

// Process 8 consecutive pixels with source-over compositing using NEON.
// cov_ptr: pointer to 8 FreeType coverage bytes (need not be aligned).
// dst_ptr: pointer to 8 RGBA8888 pixels = 32 bytes (need not be aligned).
// vCa/vCr/vCg/vCb: caller alpha and colour channels, broadcast to uint16x8_t.
// Returns via dst_ptr (in-place).
static __attribute__((always_inline)) inline void
blit8_srcover_neon(const uint8_t *cov_ptr, unsigned char *dst_ptr,
                   uint16x8_t vCa,
                   uint16x8_t vCr, uint16x8_t vCg, uint16x8_t vCb)
{
  // sa = (cov * ca) >> 8   -- /256 approximation, max error 1 LSB
  uint16x8_t cov16 = vmovl_u8(vld1_u8(cov_ptr));
  uint16x8_t sa    = vshrq_n_u16(vmulq_u16(cov16, vCa), 8);
  uint16x8_t is_   = vsubq_u16(vdupq_n_u16(255), sa);

  // Deinterleave 8 dst RGBA pixels into separate channel lanes.
  uint8x8x4_t dst = vld4_u8(dst_ptr);
  uint16x8_t dR = vmovl_u8(dst.val[0]);
  uint16x8_t dG = vmovl_u8(dst.val[1]);
  uint16x8_t dB = vmovl_u8(dst.val[2]);
  uint16x8_t dA = vmovl_u8(dst.val[3]);

  // out = (src * sa + dst * is) >> 8
  uint8x8x4_t res;
  res.val[0] = vmovn_u16(vshrq_n_u16(vaddq_u16(vmulq_u16(vCr, sa), vmulq_u16(dR, is_)), 8));
  res.val[1] = vmovn_u16(vshrq_n_u16(vaddq_u16(vmulq_u16(vCg, sa), vmulq_u16(dG, is_)), 8));
  res.val[2] = vmovn_u16(vshrq_n_u16(vaddq_u16(vmulq_u16(vCb, sa), vmulq_u16(dB, is_)), 8));
  // Alpha: sa + dst_a * is / 256
  res.val[3] = vmovn_u16(vshrq_n_u16(vaddq_u16(
    vshlq_n_u16(sa, 8),          // sa * 256 (becomes sa after >>8)
    vmulq_u16(dA, is_)), 8));

  vst4_u8(dst_ptr, res);
}

// Process 8 consecutive pixels with dest-over compositing, black shadow fast path.
// When cr==cg==cb==0 (always true for the shadow) only alpha needs computing:
//   out_a = dst_a + sa * (255 - dst_a) / 256
// RGB channels are untouched.  sa is derived the same way as in blit8_srcover.
static __attribute__((always_inline)) inline void
blit8_dstover_black_neon(const uint8_t *cov_ptr, unsigned char *dst_ptr,
                         uint16x8_t vCa)
{
  uint16x8_t cov16 = vmovl_u8(vld1_u8(cov_ptr));
  uint16x8_t sa    = vshrq_n_u16(vmulq_u16(cov16, vCa), 8);

  uint8x8x4_t dst = vld4_u8(dst_ptr);
  uint16x8_t dA   = vmovl_u8(dst.val[3]);
  uint16x8_t id   = vsubq_u16(vdupq_n_u16(255), dA);

  // out_a = dst_a + (sa * id) >> 8
  dst.val[3] = vmovn_u16(vaddq_u16(dA,
    vshrq_n_u16(vmulq_u16(sa, id), 8)));

  // RGB unchanged -- vst4_u8 writes all four lanes so we must re-store them.
  vst4_u8(dst_ptr, dst);
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
//
// Implementation notes:
//  - The row loop clips to [0,H) exactly as before; only the inner pixel loop
//    changes.
//  - For each row we compute the clipped x range [x0, x1) once, then run the
//    NEON path over the aligned interior and scalar tails.
//  - The scalar fallback uses MUL255 (multiply-then-shift) so no integer
//    divisions are emitted; the code-gen matches what a good compiler would
//    produce for the original /255 path.
//  - The NEON paths process 8 pixels unconditionally (zero-coverage pixels
//    produce sa=0 which makes out==dst -- a harmless identity write). This
//    removes the per-pixel branch that was causing mispredictions on glyph edges.
//  - A pre-scan using vmaxv_u8 skips fully-empty 8-pixel chunks with one
//    instruction rather than eight branch checks.
static void blit_glyph(unsigned char *out, int W, int H,
                       int gx, int gy, const CachedGlyph *g,
                       int cr, int cg, int cb, int ca, int under) {
  if (!g || !g->buffer || ca <= 0) return;

  // Broadcast scalar constants once outside the row loop.
  const uint16x8_t vCa = vdupq_n_u16((uint16_t)ca);
  const uint16x8_t vCr = vdupq_n_u16((uint16_t)cr);
  const uint16x8_t vCg = vdupq_n_u16((uint16_t)cg);
  const uint16x8_t vCb = vdupq_n_u16((uint16_t)cb);

  // Is the shadow a plain black source (cr=cg=cb=0)?  If so we use the
  // cheaper dest-over-black NEON path that skips three multiply-add lanes.
  const int shadow_is_black = under && (cr | cg | cb) == 0;

  // Clamp the glyph rectangle to the output buffer horizontally once.
  // Vertical clipping is done row by row (dy check), but the x window
  // [x0_rel, x1_rel) within the glyph row is constant for all rows.
  const int x0_abs = gx < 0 ? 0 : gx;
  const int x1_abs = (gx + (int)g->width) > W ? W : (gx + (int)g->width);
  if (x0_abs >= x1_abs) return; // entirely off-screen horizontally

  // Offsets within the glyph row for the clipped region.
  const unsigned rx0 = (unsigned)(x0_abs - gx); // first coverage byte to use
  const int span    = x1_abs - x0_abs;           // pixel count to composite

  for (unsigned ry = 0; ry < g->rows; ry++) {
    const int dy = gy + (int)ry;
    if (dy < 0 || dy >= H) continue;

    const uint8_t *cov_row = g->buffer + (size_t)ry * (size_t)g->abs_pitch + rx0;
    unsigned char *dst_row = out + ((size_t)dy * (size_t)W + (size_t)x0_abs) * 4;

    int rx = 0; // index into the clipped span

    if (!under) {
      // ── source-over (text) ───────────────────────────────────────────────
      // NEON body: 8 pixels per iteration, unconditional (zero-cov == no-op).
      for (; rx + 8 <= span; rx += 8) {
        // Skip entirely-zero 8-byte chunks: vmaxv finds the lane maximum in
        // one instruction.  All-zero => sa=0 => out==dst; skip the RMW cycle.
        uint8x8_t cov8 = vld1_u8(cov_row + rx);
        if (vmaxv_u8(cov8) == 0) continue;
        blit8_srcover_neon(cov_row + rx, dst_row + rx * 4, vCa, vCr, vCg, vCb);
      }
      // Scalar tail (< 8 remaining pixels).
      for (; rx < span; rx++) {
        const unsigned cov = cov_row[rx];
        if (!cov) continue;
        const unsigned sa  = MUL255(cov, (unsigned)ca);
        if (!sa) continue;
        unsigned char *p   = dst_row + rx * 4;
        const unsigned is_ = 255 - sa;
        p[0] = (unsigned char)(MUL255((unsigned)cr, sa) + MUL255(p[0], is_));
        p[1] = (unsigned char)(MUL255((unsigned)cg, sa) + MUL255(p[1], is_));
        p[2] = (unsigned char)(MUL255((unsigned)cb, sa) + MUL255(p[2], is_));
        p[3] = (unsigned char)(sa                       + MUL255(p[3], is_));
      }
    } else if (shadow_is_black) {
      // ── dest-over, black source (shadow fast path) ───────────────────────
      // Only alpha is modified; RGB is untouched.
      for (; rx + 8 <= span; rx += 8) {
        uint8x8_t cov8 = vld1_u8(cov_row + rx);
        if (vmaxv_u8(cov8) == 0) continue;
        blit8_dstover_black_neon(cov_row + rx, dst_row + rx * 4, vCa);
      }
      for (; rx < span; rx++) {
        const unsigned cov = cov_row[rx];
        if (!cov) continue;
        const unsigned sa  = MUL255(cov, (unsigned)ca);
        if (!sa) continue;
        unsigned char *p   = dst_row + rx * 4;
        const unsigned id  = 255 - p[3];
        p[3] = (unsigned char)(p[3] + MUL255(sa, id));
        // p[0..2] unchanged: black source contributes 0 to RGB.
      }
    } else {
      // ── dest-over, coloured source (general path, rare) ──────────────────
      // The shadow is always black in practice (cr=cg=cb=0), so this branch
      // is a correctness fallback only.  It mirrors the original scalar maths
      // using MUL255 to avoid integer division.
      for (; rx < span; rx++) {
        const unsigned cov = cov_row[rx];
        if (!cov) continue;
        const unsigned sa  = MUL255(cov, (unsigned)ca);
        if (!sa) continue;
        unsigned char *p   = dst_row + rx * 4;
        const unsigned id  = 255 - p[3];
        p[0] = (unsigned char)(p[0] + MUL255(MUL255((unsigned)cr, sa), id));
        p[1] = (unsigned char)(p[1] + MUL255(MUL255((unsigned)cg, sa), id));
        p[2] = (unsigned char)(p[2] + MUL255(MUL255((unsigned)cb, sa), id));
        p[3] = (unsigned char)(p[3] + MUL255(sa, id));
      }
    }
  }
}

// Grow-only scratch for the returned bitmap. The one caller
// (createTextBitmapShadowStroke) hands this straight to the engine, which
// copies it out synchronously (GetByteArrayRegion) before control returns --
// so a single persistent buffer replaces a calloc/free on every label update,
// and the typewriter effect's dozens-of-renders-per-line stop touching the
// global malloc lock entirely after warmup. Relies on the same single-threaded
// text-render assumption the (unlocked, shared) glyph cache above already makes.
// NOTE: callers must NOT free the pointer returned by gfx_render_text_rgba().
static unsigned char *g_rgba_scratch = NULL;
static size_t g_rgba_scratch_cap = 0;

static unsigned char *rgba_scratch(size_t need) {
  if (need > g_rgba_scratch_cap) {
    size_t cap = g_rgba_scratch_cap ? g_rgba_scratch_cap : 4096;
    while (cap < need) cap <<= 1;
    unsigned char *n = realloc(g_rgba_scratch, cap);
    if (!n) return NULL;              // keep the old buffer on failure
    g_rgba_scratch = n;
    g_rgba_scratch_cap = cap;
  }
  return g_rgba_scratch;
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

  const size_t out_bytes = (size_t)W * H * 4;
  unsigned char *out = rgba_scratch(out_bytes);
  if (!out)
    return NULL;
  memset(out, 0, out_bytes); // reused buffer: zero it ourselves (calloc used to)

  // Stable vertical reference for the game-font path. Every line is centred on
  // ONE fixed ink box -- the cap height (and however far these specific glyphs'
  // ink actually reaches relative to the font's own baseline -- see
  // line_ink_extents) of a constant set of plain reference glyphs, measured
  // once here -- NOT on each line's own glyphs. Centring on per-line ink made
  // the baseline drift whenever the glyphs changed: an accent (a, e, i, o, u
  // with a tilde/acute, or n-tilde) reaches a pixel higher than a bare
  // capital, which nudged that line down, so Spanish text appeared to jump as
  // accented characters appeared (and flickered through the typewriter
  // reveal). A fixed reference keeps the baseline identical for "Si" and
  // "Si-acute"; accents and descenders simply extend into the cell's padding
  // instead of moving the line. Reference is ASCII caps on purpose: most text
  // in any given line has no true descender, so this represents the common
  // case and rare descenders (g/j/p/q/y) just dip into the cell's padding
  // rather than shifting every line's baseline for the sake of an infrequent
  // glyph.
  int ref_above = 0, ref_below = 0;
  if (g_game_ok) {
    static const char REF[] = "ABHTMW";
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