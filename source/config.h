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

#define CONFIG_NAME "config.txt"
#define LOG_NAME    "debug.log"

#define DEBUG_LOG 0

// Opt-in diagnostics (no effect on a normal build). Set to 1 to write:
//   textdbg.log   -- every text-bitmap request (text, size, align, box w/h),
//                    so we can see exactly how the name/input fields are laid out
//   frametime.log -- a frame-time summary every ~5s (how many frames overran the
//                    16.67ms vsync budget), to tell GPU/streaming hitches apart
//                    from delta-time jitter.
// Both files land next to config.txt in /switch/ct/.
#define DEBUG_INSTR 0

extern int screen_width;
extern int screen_height;

// Default UI language (see lang_index in main.c for the supported codes).
#define LANG_DEFAULT "en"

typedef struct {
  int screen_width;
  int screen_height;
  char language[8];
  // Performance knobs (see read_config for defaults):
  //   gl_threaded -- 1 = run mesa's GL command submission on a second CPU core
  //                  (glthread). The engine is single-threaded and spends much
  //                  of each frame inside the nouveau driver; this offloads that
  //                  onto a spare core, shortening the critical path that
  //                  otherwise needs a CPU/EMC overclock to reach 60fps. Set 0
  //                  to disable if it causes trouble. No-op if mesa lacks it.
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
  int game_font;
  char game_font_path[256];
  float game_font_scale;
  float game_font_xscale;
  int text_shadow;
  int text_shadow_alpha;
  float text_shadow_scale;
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif