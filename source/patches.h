/* patches.h -- runtime ARM64 instruction patches for libchrono.so
 *
 * Applied at boot to load_base (writable RW mapping) before so_finalize()
 * locks it to RX. Identical in effect to the offline Python patcher scripts,
 * but driven by config.ini so individual fixes can be toggled without
 * re-patching the .so on disk.
 *
 * Three feature groups, each gated by a config flag:
 *
 *   cursor_fix         -- keep selected-item text WHITE and use a dark
 *                         semi-transparent highlight instead of the cream
 *                         colour-inversion look (remove_cursor_invert.py v9)
 *
 *   remove_bilinear    -- force GL_NEAREST (pixel-perfect point sampling)
 *                         everywhere instead of bilinear filtering
 *                         (remove_bilinear.py)
 *
 *   remove_mobile_ui   -- hide the on-screen touch-overlay buttons: field
 *                         menu button, world-map menu/map/warp buttons, and
 *                         all five right-side title-screen icon buttons
 *                         (remove_mobile_ui.py v8)
 *
 * Every patch entry records the old word so we can verify the .so matches
 * the expected build before writing anything. A mismatch prints a warning
 * and skips that entry -- it never silently corrupts.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __PATCHES_H__
#define __PATCHES_H__

#include <stdint.h>
#include "so_util.h"
#include "util.h"

// ---------------------------------------------------------------------------
// Patch descriptor
//
// sym_name: exported symbol whose load_base address anchors the patch.
//           NULL means use raw_vaddr directly (for functions with no exported
//           symbol -- the world-button helper, cave sites, etc.).
// func_off:  byte offset from the symbol's load_base address to the word.
//            Ignored when sym_name is NULL.
// raw_vaddr: link-time virtual address used when sym_name is NULL.
// old_word:  expected current instruction (LE uint32). 0 = "don't check"
//            (used for cave slots that are verified-zero in the .so).
// new_word:  replacement instruction.
// desc:      human-readable label for debug output.
// ---------------------------------------------------------------------------
typedef struct {
  const char   *sym_name;
  uint32_t      func_off;
  uint32_t      raw_vaddr;
  uint32_t      old_word;
  uint32_t      new_word;
  const char   *desc;
} PatchEntry;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Compute the writable load_base address for a patch entry.
// Returns 0 if the symbol is required but missing (caller skips).
static uintptr_t patch_addr(so_module *mod, const PatchEntry *p) {
  if (p->sym_name) {
    // Symbol-relative: load_base + symbol_value + func_off
    for (int i = 0; i < mod->num_syms; i++) {
      const char *name = mod->dynstrtab + mod->syms[i].st_name;
      if (__builtin_strcmp(name, p->sym_name) == 0)
        return (uintptr_t)mod->load_base + mod->syms[i].st_value + p->func_off;
    }
    debugPrintf("patches: symbol not found: %s\n", p->sym_name);
    return 0;
  } else {
    // Raw vaddr: subtract load_virtbase, add load_base (same as so_find_addr logic)
    return (uintptr_t)mod->load_base + p->raw_vaddr;
  }
}

// Apply one patch. Prints a diagnostic on mismatch but never aborts.
static void apply_patch(so_module *mod, const PatchEntry *p) {
  uintptr_t addr = patch_addr(mod, p);
  if (!addr) return;

  uint32_t cur;
  __builtin_memcpy(&cur, (void *)addr, 4);

  if (cur == p->new_word) {
    // Already at the desired value -- idempotent, no write needed.
    return;
  }
  if (p->old_word && cur != p->old_word) {
    debugPrintf("patches: MISMATCH @ %s+0x%x / vaddr 0x%x: "
                "expected %08x got %08x -- skipping (%s)\n",
                p->sym_name ? p->sym_name : "(raw)",
                p->sym_name ? p->func_off : p->raw_vaddr,
                p->raw_vaddr, p->old_word, cur, p->desc);
    return;
  }

  __builtin_memcpy((void *)addr, &p->new_word, 4);
  debugPrintf("patches: %08x -> %08x  %s\n", cur, p->new_word, p->desc);
}

// Apply an array of patches.
static void apply_patches(so_module *mod, const PatchEntry *table, int count) {
  for (int i = 0; i < count; i++)
    apply_patch(mod, &table[i]);
}

#define PATCH_COUNT(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

// ---------------------------------------------------------------------------
// Macro helpers for the patch tables
// ---------------------------------------------------------------------------

// Symbol-relative entry: sym at func_off, expected old -> new
#define P_SYM(sym, off, old, new, desc) \
  { (sym), (uint32_t)(off), 0, (uint32_t)(old), (uint32_t)(new), (desc) }

// Raw-vaddr entry: no symbol, raw virtual address, old -> new
// old=0 means "don't verify" (used for cave zero-slots).
#define P_RAW(vaddr, old, new, desc) \
  { NULL, 0, (uint32_t)(vaddr), (uint32_t)(old), (uint32_t)(new), (desc) }

// Cave slot: zero-initialised padding, no old-word check needed.
#define P_CAVE(vaddr, new, desc) \
  { NULL, 0, (uint32_t)(vaddr), 0, (uint32_t)(new), (desc) }

// ---------------------------------------------------------------------------
// 1.  cursor_fix  (remove_cursor_invert.py v9)
//
//     Selected-item text: BLACK -> WHITE on all menus
//     Selection highlight: cream (0xfe,0xff,0xdd,0xcc) -> dark semi-transparent (0,0,0,0xa0)
//     Dialogue choice NON-selected bg: normal opacity -> dimmed (cave at 0xaa56a0)
// ---------------------------------------------------------------------------
static const PatchEntry g_cursor_patches[] = {

  // 1. BattleTechMenu::setListButtonFontColors(int)  selected text BLACK->WHITE
  P_RAW(0x6e40cc, 0xf946594a, 0xf946914a,
    "BattleTechMenu::setListButtonFontColors selected text BLACK->WHITE"),

  // 2. BattleItemMenu::onSelectChanged(int)  crash fix + WHITE
  P_RAW(0x6e52dc, 0x79002be9, 0x79002bea,
    "BattleItemMenu::onSelectChanged r,g BLACK->WHITE (strh w9->w10)"),
  P_RAW(0x6e52e0, 0x39005be8, 0x39005bea,
    "BattleItemMenu::onSelectChanged b BLACK->WHITE (strb w8->w10)"),

  // 3. MsgWindow::createMenu  dialogue choice windows
  P_RAW(0x5fd998, 0xaa1703e0, 0xaa1503e0,
    "MsgWindow::createMenu ItemSpriteA selectedImg bg WHITE->BLACK"),
  P_RAW(0x5fd9e8, 0xaa1703e0, 0xaa1503e0,
    "MsgWindow::createMenu ItemSpriteB normalImg bg WHITE->BLACK"),
  P_RAW(0x5fda18, 0xaa1503e1, 0xaa1703e1,
    "MsgWindow::createMenu selected-choice text tint BLACK->WHITE"),

  // 4. FieldMenu::init()  selected-state sprite colour BLACK->WHITE
  P_RAW(0x755538, 0xf9465908, 0xf9469108,
    "FieldMenu::init selected-state sprite colour BLACK->WHITE"),

  // 5. WorldMenu button helper (MENU/MAP/WARP)  selected-state BLACK->WHITE
  P_RAW(0x782ca4, 0xf9465929, 0xf9469129,
    "WorldMenu button helper selected-state sprite colour BLACK->WHITE"),

  // 6. BattleMenu::drawCommandWindow(int,bool,...)  selected text BLACK->WHITE
  P_RAW(0x5b56f0, 0xf946594a, 0xf946914a,
    "BattleMenu::drawCommandWindow(int,bool) selected text BLACK->WHITE"),

  // 7. BattleMenu::drawCommandWindow()  selected cmd text BLACK->WHITE
  P_RAW(0x5b2210, 0xf9465842, 0xf9469042,
    "BattleMenu::drawCommandWindow() selected cmd text BLACK->WHITE"),

  // 8. BattleListMenu highlight-colour: cream -> dark semi-transparent
  P_RAW(0x6e6994, 0x52801fc1, 0x52800001,
    "BattleListMenu highlight r 0xfe->0x00"),
  P_RAW(0x6e6998, 0x52801fe2, 0x52800002,
    "BattleListMenu highlight g 0xff->0x00"),
  P_RAW(0x6e699c, 0x52801ba3, 0x52800003,
    "BattleListMenu highlight b 0xdd->0x00"),
  P_RAW(0x6e69a0, 0x52801984, 0x52801404,
    "BattleListMenu highlight a 0xcc->0xa0"),

  // 9. BattleMenu::init() cmd-window SELECTED-FILL colour (2 sites)
  P_RAW(0x5ad8b0, 0x52801fc1, 0x52800001,
    "cmd-window fill #0x5ad8b0 r 0xfe->0x00"),
  P_RAW(0x5ad8b4, 0x52801fe2, 0x52800002,
    "cmd-window fill #0x5ad8b0 g 0xff->0x00"),
  P_RAW(0x5ad8b8, 0x52801ba3, 0x52800003,
    "cmd-window fill #0x5ad8b0 b 0xdd->0x00"),
  P_RAW(0x5ad8bc, 0x52801984, 0x52801404,
    "cmd-window fill #0x5ad8b0 a 0xcc->0xa0"),
  P_RAW(0x5ada68, 0x52801fc1, 0x52800001,
    "cmd-window fill #0x5ada68 r 0xfe->0x00"),
  P_RAW(0x5ada6c, 0x52801fe2, 0x52800002,
    "cmd-window fill #0x5ada68 g 0xff->0x00"),
  P_RAW(0x5ada70, 0x52801ba3, 0x52800003,
    "cmd-window fill #0x5ada68 b 0xdd->0x00"),
  P_RAW(0x5ada74, 0x52801984, 0x52801404,
    "cmd-window fill #0x5ada68 a 0xcc->0xa0"),

  // 10. MsgWindow::createMenu  dialogue NON-selected bg: dim via cave
  //     Route normal-bg makeColorNode call through cave at 0xaa56a0
  //     that enables opacity cascade then setOpacity(160).
  P_RAW(0x5fd950, 0x940000b5, 0x94129f54,
    "dialogue normal bg makeColorNode -> cascade+dim cave"),

  // Cave @ 0xaa56a0 (zero-padding region; BL offset from 0x5fd950 verified above)
  P_CAVE(0xaa56a0, 0xa9bf7bf3, "cave: stp x19,x30,[sp,#-0x10]!"),
  P_CAVE(0xaa56a4, 0x97ed6160, "cave: bl #0x5fdc24 (makeColorNode)"),
  P_CAVE(0xaa56a8, 0xaa0003f3, "cave: mov x19,x0"),
  P_CAVE(0xaa56ac, 0x52800021, "cave: mov w1,#1"),
  P_CAVE(0xaa56b0, 0x97f2768f, "cave: bl #0x7430ec (setCascadeOpacityEnabledRecursive)"),
  P_CAVE(0xaa56b4, 0xaa1303e0, "cave: mov x0,x19"),
  P_CAVE(0xaa56b8, 0x52801401, "cave: mov w1,#0xa0 (opacity 160)"),
  P_CAVE(0xaa56bc, 0xf9400268, "cave: ldr x8,[x19]"),
  P_CAVE(0xaa56c0, 0xf9424908, "cave: ldr x8,[x8,#0x490] (setOpacity vtable slot)"),
  P_CAVE(0xaa56c4, 0xd63f0100, "cave: blr x8"),
  P_CAVE(0xaa56c8, 0xaa1303e0, "cave: mov x0,x19"),
  P_CAVE(0xaa56cc, 0xa8c17bf3, "cave: ldp x19,x30,[sp],#0x10"),
  P_CAVE(0xaa56d0, 0xd65f03c0, "cave: ret"),
};

// ---------------------------------------------------------------------------
// 2.  remove_bilinear  (remove_bilinear.py)
//
//     Force GL_NEAREST in initWithMipmaps and setAntiAliasTexParameters.
//     Uses symbol-relative addressing for correctness -- the two functions
//     have exported symbols in dynsym, so we look them up rather than
//     hardcoding load-time vaddrs that could shift if the .so is ever rebuilt.
// ---------------------------------------------------------------------------
static const PatchEntry g_bilinear_patches[] = {

  // cocos2d::Texture2D::initWithMipmaps(...)
  P_SYM("_ZN7cocos2d9Texture2D15initWithMipmaps",
        0, /* func_off is unused; we scan within the function below */
        0, 0, "(sentinel -- see note)"),
  // Note: remove_bilinear.py searches for instruction patterns within the
  // function body rather than at fixed offsets, because the patterns appear
  // at different offsets depending on optimisation. We replicate that scan
  // here with the same known old->new word pairs. Raw vaddrs work equally
  // well because this is a fixed build; symbol-relative are used for the
  // first entry only as a build-match guard (see apply_bilinear_patches).
  P_RAW(0, 0x1A980708, 0x2A1803E8,
    "initWithMipmaps MIN(mipmap) cinc->mov w8,w24 (always NEAREST)"),
  P_RAW(0, 0x1A9C0789, 0x2A1C03E9,
    "initWithMipmaps MIN cinc->mov w9,w28 (always NEAREST)"),
  P_RAW(0, 0x1A9C0782, 0x2A1C03E2,
    "initWithMipmaps MAG cinc->mov w2,w28 (always NEAREST)"),
  P_RAW(0, 0x5284C02A, 0x5284C00A,
    "initWithMipmaps VolatileMgr restore GL_LINEAR->GL_NEAREST"),
  P_RAW(0, 0x5284E029, 0x5284E009,
    "initWithMipmaps VolatileMgr restore GL_LINEAR_MIPMAP->GL_NEAREST_MIPMAP"),

  // cocos2d::Texture2D::setAntiAliasTexParameters()
  P_RAW(0, 0x5284E035, 0x5284E015,
    "setAntiAliasTexParameters MIN GL_LINEAR_MIPMAP_NEAREST->GL_NEAREST_MIPMAP_NEAREST"),
  P_RAW(0, 0x5284C036, 0x5284C016,
    "setAntiAliasTexParameters MIN GL_LINEAR->GL_NEAREST"),
  P_RAW(0, 0x5284C022, 0x5284C002,
    "setAntiAliasTexParameters MAG GL_LINEAR->GL_NEAREST"),
};

// ---------------------------------------------------------------------------
// 3.  remove_mobile_ui  (remove_mobile_ui.py v8)
//
//     Fix 1: FieldMenu::setMenuAvailable -- field button always hidden,
//             VirtualController::setActive (Start key) still works
//     Fix 2: world-map MENU/MAP/WARP buttons -- setPosition->setVisible(false)
//     Fix 3: right-side title icon buttons -- hide+setSkip via cave at 0xaa6270
// ---------------------------------------------------------------------------
static const PatchEntry g_mobile_ui_patches[] = {

  // Fix 1: FieldMenu::setMenuAvailable(bool)
  //   +24: STRB WZR,[X0,#0x330] -> MOV W1,WZR  (force setVisible(false))
  P_SYM("_ZN9FieldMenu16setMenuAvailableEb", 24,
        0x390CC01F, 0x2A1F03E1,
        "FieldMenu::setMenuAvailable +24 field button always hidden, Start untouched"),

  // Fix 2: world-button-builder helper @ 0x782C7C (no exported symbol)
  //   setPosition(0xC8) -> setVisible(0x170), bool arg = false, two paths
  P_RAW(0x782DC0, 0xF9406508, 0xF940B908,
    "world-button helper +0x144 vtable setPosition->setVisible [path A]"),
  P_RAW(0x782DC8, 0x1E232821, 0x2A1F03E1,
    "world-button helper +0x14C bool arg=false [path A]"),
  P_RAW(0x782DF8, 0x1E261001, 0x2A1F03E1,
    "world-button helper +0x17C bool arg=false [path B]"),
  P_RAW(0x782DFC, 0xF9406508, 0xF940B908,
    "world-button helper +0x180 vtable setPosition->setVisible [path B]"),

  // Fix 3a: setupMenuNodes hide range-check -> NOP (hide every right-side icon)
  P_RAW(0x77FE94, 0x54FFE1A8, 0xD503201F,
    "setupMenuNodes B.HI->NOP: hide all right-side icons unconditionally"),

  // Fix 3b: repurpose hide sequence -> cave call (setVisible+setSkip)
  P_RAW(0x77FE98, 0xF9400348, 0xAA1A03E0,
    "setupMenuNodes LDR X8,[X26]->MOV X0,X26 (State* arg for cave)"),
  P_RAW(0x77FE9C, 0xF9400908, 0x940C98F5,
    "setupMenuNodes LDR X8,[X8,#0x10]->BL 0xAA6270 (setVisible+setSkip cave)"),
  P_RAW(0x77FEA0, 0xAA1A03E0, 0x17FFFF0A,
    "setupMenuNodes MOV X0,X26->B #0x77FAC8 (loop continue)"),

  // Fix 3b cave @ 0xAA6270  (96B zero-padding; distinct from cursor cave 0xAA56A0)
  P_CAVE(0xAA6270, 0xA9BF7BF3, "cave: stp x19,x30,[sp,#-0x10]!"),
  P_CAVE(0xAA6274, 0xAA0003F3, "cave: mov x19,x0"),
  P_CAVE(0xAA6278, 0xF9400008, "cave: ldr x8,[x0]"),
  P_CAVE(0xAA627C, 0xF9400908, "cave: ldr x8,[x8,#0x10] getNode"),
  P_CAVE(0xAA6280, 0xD63F0100, "cave: blr x8"),
  P_CAVE(0xAA6284, 0xF9400008, "cave: ldr x8,[x0]"),
  P_CAVE(0xAA6288, 0xF940B908, "cave: ldr x8,[x8,#0x170] setVisible"),
  P_CAVE(0xAA628C, 0x2A1F03E1, "cave: mov w1,wzr"),
  P_CAVE(0xAA6290, 0xD63F0100, "cave: blr x8"),
  P_CAVE(0xAA6294, 0xAA1303E0, "cave: mov x0,x19"),
  P_CAVE(0xAA6298, 0x52800021, "cave: mov w1,#1"),
  P_CAVE(0xAA629C, 0x940293D9, "cave: bl 0xB4B200 setSkip(true)"),
  P_CAVE(0xAA62A0, 0xA8C17BF3, "cave: ldp x19,x30,[sp],#0x10"),
  P_CAVE(0xAA62A4, 0xD65F03C0, "cave: ret"),

  // Fix 3c: RET-stub UpdateIconVisible so it can never re-show icons
  P_RAW(0x780CC8, 0xA9BD7BFD, 0xD65F03C0,
    "UpdateIconVisible RET-stub: icons stay hidden"),
};

// ---------------------------------------------------------------------------
// 4.  controller_glyphs
//
//     In-message <BTN_*> tags (CONF/DASH/MENU/WARP/L/R) are substituted at
//     render time from one of two .bss glyph-string tables, chosen by the
//     message-text builder at 0x5fea40:
//
//       and  x8, w20, #3                  ; w20 = ChronoCanvas[0x10b2c] (A/B-swap cfg)
//       umaddl x8, w8, #0x90, 0xbf3798    ; x8 = CONTROLLER table + (swap&3)*0x90
//       add  x9, .., #0x9d8               ; x9 = KEYBOARD table 0xbf39d8
//       tst  w0, #1                       ; w0 = "last input was a key" (pad? bit)
//       csel x20, x8, x9, ne              ; pick keyboard when a key was last seen
//
//     With native_controller on this already resolves to the controller table,
//     but forcing the select guarantees Switch-button glyphs even on the very
//     first frame or in keyboard-compat mode. csel -> mov x20,x8 (controller).
//     The (swap&3) index is preserved, so the A/B-swap setting is still honoured.
// ---------------------------------------------------------------------------
static const PatchEntry g_glyph_patches[] = {
  P_RAW(0x5fea50, 0x9a891114, 0xaa0803f4,
    "MsgText <BTN_*> tags: force controller glyph table (csel x20,x8,x9,ne -> mov x20,x8)"),

  // ---------------------------------------------------------------------
  // <BTN_L>/<BTN_R> glyph text: "[LB]"/"[RB]" -> "[L]"/"[R]"
  //
  // The controller-glyph builder at 0x5fce6c constructs two inline UTF-8
  // strings via mov/movk into x12/x13, then stores them (stur x12/x13) into
  // EVERY swap-variant slot of the L/R entries -- one computation, many
  // copies, confirmed by tracing the stores across all 4 variants. The
  // strings are Xbox/generic-pad bumper labels, not Switch shoulder labels:
  //
  //   x12 = e3 80 90 52 42 e3 80 91  = U+3010 'R' 'B' U+3011  ([RB])  <BTN_R>
  //   x13 = e3 80 90 4c 42 e3 80 91  = U+3010 'L' 'B' U+3011  ([LB])  <BTN_L>
  //
  // R and L are directionally correct (R shows R-content, L shows
  // L-content) -- this is a labelling fix, not an input-swap fix. Drop the
  // 'B' byte from each (8 bytes -> 7) and shift the closing bracket down;
  // the libc++ short-string size byte (w16, shared by both) goes from
  // 0x10 (size 8) to 0x0e (size 7) to match:
  //
  //   x12 -> e3 80 90 52 e3 80 91        = U+3010 'R' U+3011      ([R])
  //   x13 -> e3 80 90 4c e3 80 91        = U+3010 'L' U+3011      ([L])
  //
  // Only the byte4..7 movk's change (byte0..3 are identical between the
  // "RB"/"R" and "LB"/"L" forms, so those instructions are untouched).
  // ---------------------------------------------------------------------
  P_RAW(0x5fcea4, 0xf2dc684c, 0xf2d01c6c,
    "<BTN_R> glyph: movk x12,#0xe342,lsl32 -> #0x80e3,lsl32 (drop 'B')"),
  P_RAW(0x5fced0, 0xf2f2300c, 0xf2e0122c,
    "<BTN_R> glyph: movk x12,#0x9180,lsl48 -> #0x0091,lsl48 (shift ])"),
  P_RAW(0x5fcea8, 0xf2dc684d, 0xf2d01c6d,
    "<BTN_L> glyph: movk x13,#0xe342,lsl32 -> #0x80e3,lsl32 (drop 'B')"),
  P_RAW(0x5fced4, 0xf2f2300d, 0xf2e0122d,
    "<BTN_L> glyph: movk x13,#0x9180,lsl48 -> #0x0091,lsl48 (shift ])"),
  P_RAW(0x5fcecc, 0x52800210, 0x528001d0,
    "<BTN_L>/<BTN_R> glyph: mov w16,#0x10 -> #0x0e (shared size byte 8->7)"),
};

// ---------------------------------------------------------------------------
// The bilinear patches use pattern-search within the function body (as the
// Python script does) rather than hard-coded relative offsets, because the
// compiler may arrange the instructions differently. We provide a dedicated
// helper that scans the known function range.
// ---------------------------------------------------------------------------
static void apply_bilinear_patches(so_module *mod) {
  // Locate the two target functions in the symbol table.
  uintptr_t iwm_base = 0, iwm_size = 0;  // initWithMipmaps
  uintptr_t aap_base = 0, aap_size = 0;  // setAntiAliasTexParameters

  for (int i = 0; i < mod->num_syms; i++) {
    const char *nm = mod->dynstrtab + mod->syms[i].st_name;
    if (__builtin_strncmp(nm, "_ZN7cocos2d9Texture2D15initWithMipmaps",
                          sizeof("_ZN7cocos2d9Texture2D15initWithMipmaps") - 1) == 0) {
      iwm_base = (uintptr_t)mod->load_base + mod->syms[i].st_value;
      iwm_size = mod->syms[i].st_size;
    } else if (__builtin_strncmp(nm, "_ZN7cocos2d9Texture2D25setAntiAliasTexParameters",
                                 sizeof("_ZN7cocos2d9Texture2D25setAntiAliasTexParameters") - 1) == 0) {
      aap_base = (uintptr_t)mod->load_base + mod->syms[i].st_value;
      aap_size = mod->syms[i].st_size;
    }
  }

  if (!iwm_base || !aap_base) {
    debugPrintf("patches: bilinear -- Texture2D symbols not found (different build?)\n");
    return;
  }

  // Pattern-search and patch table: (old, new, desc, base, size)
  // Each entry is searched within the relevant function's byte range.
  typedef struct { uint32_t old, new; const char *desc; uintptr_t base; uintptr_t size; } BPatch;
  const BPatch bp[] = {
    { 0x1A980708, 0x2A1803E8, "MIN(mipmap) cinc->mov w8,w24 (NEAREST)",  iwm_base, iwm_size },
    { 0x1A9C0789, 0x2A1C03E9, "MIN cinc->mov w9,w28 (NEAREST)",          iwm_base, iwm_size },
    { 0x1A9C0782, 0x2A1C03E2, "MAG cinc->mov w2,w28 (NEAREST)",          iwm_base, iwm_size },
    { 0x5284C02A, 0x5284C00A, "VolatileMgr GL_LINEAR->GL_NEAREST",       iwm_base, iwm_size },
    { 0x5284E029, 0x5284E009, "VolatileMgr GL_LINEAR_MIPMAP->GL_NEAREST_MIPMAP", iwm_base, iwm_size },
    { 0x5284E035, 0x5284E015, "setAntiAlias MIN GL_LINEAR_MIPMAP->GL_NEAREST_MIPMAP", aap_base, aap_size },
    { 0x5284C036, 0x5284C016, "setAntiAlias MIN GL_LINEAR->GL_NEAREST",  aap_base, aap_size },
    { 0x5284C022, 0x5284C002, "setAntiAlias MAG GL_LINEAR->GL_NEAREST",  aap_base, aap_size },
  };

  const int n = (int)(sizeof(bp) / sizeof(bp[0]));
  for (int i = 0; i < n; i++) {
    uint32_t old_le = bp[i].old;
    uint32_t new_le = bp[i].new;
    // Scan the function body for the old word.
    int found = 0;
    for (uintptr_t a = bp[i].base; a + 4 <= bp[i].base + bp[i].size; a += 4) {
      uint32_t cur;
      __builtin_memcpy(&cur, (void *)a, 4);
      if (cur == new_le) { found = 1; break; } // already patched
      if (cur == old_le) {
        __builtin_memcpy((void *)a, &new_le, 4);
        debugPrintf("patches: bilinear %08x->%08x @ 0x%lx  %s\n",
                    old_le, new_le, (unsigned long)(a - (uintptr_t)mod->load_base),
                    bp[i].desc);
        found = 1;
        break;
      }
    }
    if (!found)
      debugPrintf("patches: bilinear -- pattern %08x not found in function (%s)\n",
                  old_le, bp[i].desc);
  }
}

// ---------------------------------------------------------------------------
// Public entry point: apply all enabled patches.
// Call AFTER ct_resolve_imports() and resolve_entry_points(), but BEFORE
// so_finalize() -- load_base is writable at this point.
// ---------------------------------------------------------------------------
static inline void apply_game_patches(so_module *mod) {
  // These are included from config.h via the caller.
  extern Config config;

  if (config.cursor_fix) {
    debugPrintf("patches: applying cursor_fix\n");
    apply_patches(mod, g_cursor_patches, PATCH_COUNT(g_cursor_patches));
  }

  if (config.remove_bilinear_filter) {
    debugPrintf("patches: applying remove_bilinear\n");
    apply_bilinear_patches(mod);
  }

  if (config.remove_mobile_ui) {
    debugPrintf("patches: applying remove_mobile_ui\n");
    apply_patches(mod, g_mobile_ui_patches, PATCH_COUNT(g_mobile_ui_patches));
  }

  if (config.controller_glyphs) {
    debugPrintf("patches: applying controller_glyphs\n");
    apply_patches(mod, g_glyph_patches, PATCH_COUNT(g_glyph_patches));
  }
}

#endif /* __PATCHES_H__ */