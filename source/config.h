/* config.h -- Chrono Trigger Switch wrapper configuration
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// Memory reserved for the two .so load images. The rest of the heap backs both
// engine malloc and mesa GPU textures (field maps need the bulk) -- see
// __libnx_initheap in main.c.
#define SO_HEAP_RESERVE_MB 96

// The engine (libchrono.so) and its C++ runtime (libc++_shared.so). The Java-side
// libRMS.so/libencrypt.so are not needed -- the wrapper drives the cocos2d-x JNI
// entry points directly.
#define SO_NAME    "libchrono.so"
#define SOCPP_NAME "libc++_shared.so"

// Loose game assets (the APK's assets/ folder), served via the fake AAssetManager.
#define ASSETS_DIR "assets"

#define CONFIG_NAME "config.ini"
// Pre-INI format (flat "key value" lines, no section). read_config() migrates
// from this once if config.ini doesn't exist yet, then renames it to
// "config.txt.bak" so it's never read again -- config.ini is authoritative
// from that point on.
#define CONFIG_LEGACY_NAME "config.txt"
#define LOG_NAME    "debug.log"

#define DEBUG_LOG 0

// Opt-in diagnostics (no effect on a normal build). Set to 1 to write:
//   textdbg.log   -- every text-bitmap request (text, size, align, box w/h),
//                    so we can see exactly how the name/input fields are laid out
//   frametime.log -- a frame-time summary every ~5s (how many frames overran the
//                    16.67ms vsync budget), to tell GPU/streaming hitches apart
//                    from delta-time jitter.
// Both files land next to config.ini in /switch/ct/.
#define DEBUG_INSTR 0

extern int screen_width;
extern int screen_height;

// Default UI language (see lang_index in main.c for the supported codes).
#define LANG_DEFAULT "en"

typedef struct {
  // Per-mode render resolution. -1/-1 (or anything out of range) auto-picks
  // 1280x720 handheld / 1920x1080 docked. An explicit value is honoured up to
  // 4K, so you can also render ABOVE native for supersampling (e.g. 2560x1440
  // docked) -- the compositor scales it to the real output either way. Keep a
  // 16:9 ratio to avoid stretch. The two pairs are independent: e.g. stay near
  // native docked (where GPU clock is higher) while scaling down further in
  // handheld. Applied live -- changing dock state mid-session resizes the
  // render target and tells the engine, no restart needed.
  int screen_width_handheld;
  int screen_height_handheld;
  int screen_width_docked;
  int screen_height_docked;
  char language[8];
  // Performance knobs (see read_config for defaults):
  //   gl_threaded -- 1 = run mesa's GL command submission on a second CPU core
  //                  (glthread). The engine is single-threaded and spends much
  //                  of each frame inside the nouveau driver; this offloads that
  //                  onto a spare core, shortening the critical path that
  //                  otherwise needs a CPU/EMC overclock to reach 60fps. Set 0
  //                  to disable if it causes trouble. No-op if mesa lacks it.
  //   gl_no_error -- 1 = skip mesa's internal validation of GL calls (bad enum/
  //                  state checks) before they reach the driver. cocos2d-x's
  //                  calls are already well-formed, so this just removes CPU
  //                  bookkeeping on a path that fires constantly. Set 0 if a
  //                  future mesa build ever mishandles it.
  //   game_font   -- 1 = render the engine's *system-font* labels with a TTF from
  //                  the game/SD instead of the Switch shared font, so they match
  //                  the in-game look. (The game's own TTF labels are unaffected;
  //                  they're already drawn by the engine.) Glyphs the game font
  //                  lacks fall back to the shared font. Set 0 for the old look.
  //   game_font_path -- path to the TTF to use for game_font. Relative to the
  //                  /switch/ct dir (e.g. "assets/font/xxxxx.ttf"). If empty, a
  //                  few common asset locations are probed; if none load, falls
  //                  back to the shared font.
  //   game_font_scale -- fine-tune multiplier on the game font's auto-fitted
  //                  size. 1.0 = auto (cap height matched to the shared font);
  //                  lower (e.g. 0.85) if it still looks too big, higher if small.
  //   game_font_xscale -- horizontal-only squeeze for the game font. 1.0 = none.
  //                  Use < 1 (e.g. 0.7) to condense a wide/monospaced font so the
  //                  game's pre-broken lines stay inside their window. A
  //                  proportional CT font usually needs no squeeze.
  //   text_shadow -- 1 = draw a crisp drop shadow behind the system-font labels,
  //                  in the classic CT/SNES style (offset down-right, hard-edged
  //                  so it suits a pixel font; no blur). 0 = no shadow.
  //   text_shadow_alpha -- shadow opacity, 0..255 (the shadow is black). It also
  //                  scales with each label's own alpha, so fades stay consistent.
  //                  Default 230 -- a strong, solid-looking shadow.
  //   text_shadow_scale -- multiplier on the auto offset (which itself scales with
  //                  the glyph size, ~1px per 12px of glyph). 1.0 = default; >1 for
  //                  a heavier shadow, <1 for a tighter one.
  int gl_threaded;
  int gl_no_error;
  int game_font;
  char game_font_path[256];
  float game_font_scale;
  float game_font_xscale;
  int text_shadow;
  int text_shadow_alpha;
  float text_shadow_scale;
  // Runtime binary patches applied to libchrono.so at boot (before so_finalize).
  // Each flag is independent; disable any that cause trouble with a future build.
  //   cursor_fix         -- keep selected-item text WHITE and use a dark
  //                         semi-transparent highlight instead of the cream
  //                         colour-inversion look.
  //   remove_bilinear_filter -- force GL_NEAREST (pixel-perfect point sampling)
  //                         everywhere instead of bilinear texture filtering.
  //   remove_mobile_ui   -- hide the on-screen touch-overlay buttons (field
  //                         menu button, world-map menu/map/warp buttons, and
  //                         all five right-side title-screen icon buttons).
  int cursor_fix;
  int remove_bilinear_filter;
  int remove_mobile_ui;
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif