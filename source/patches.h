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
  // <BTN_*> glyphs: bracketed letters -> plain "(A)"-style ASCII glyphs
  //
  // The builder at 0x5fce6c assembles each glyph string inline in registers
  // (movz/movk) then stores it into every swap-variant slot of TWO parallel
  // tables: a KEY table at 0xbf3708 (the <BTN_*> search strings) and a VALUE
  // table at 0xbf3798 (the glyph each key is replaced with). The dialogue
  // text builder runs std::string::replace(key -> value) for all six rows.
  //
  // Originally each VALUE glyph was the 7-byte UTF-8 string
  //   U+3010 <letter> U+3011   ( e.g. e3 80 90 41 e3 80 91 = bracketed 'A' )
  // built as two overlapping 4-byte halves in a _lo/_hi register pair. We
  // replace each with the plain 3-byte ASCII string "(<letter>)" (e.g.
  // "(A)" = 28 41 29), so the button prompt renders as ordinary text in
  // whichever font is already drawing the surrounding dialogue -- no
  // Private-Use codepoints and no separate shared-font lookup required.
  //
  // CRITICAL: the per-glyph SSO size byte must be set WITHOUT shrinking the
  // search keys. The builder originally sourced BOTH the glyph size and the
  // key size from w20, so shrinking w20 also truncated <BTN_R>/<BTN_L> to
  // <BT, leaving 'N_R>'/'N_L>' after the icon. We therefore leave w20 at its
  // original 0x0e (keys stay length 7) and instead route every glyph-VALUE
  // size byte through w16 (set to 0x06 = length 3), which the L/R glyphs
  // already used. Per glyph: set _lo = '28 <letter> 29 00', zero _hi (NUL
  // pad) -- the ASCII form is the same 3-byte length as the old PUA glyph,
  // so the size-byte plumbing below is unchanged.
  // ---------------------------------------------------------------------
  // <BTN_A> glyph -> "(A)" (28 41 29)
  P_RAW(0x5fce84, 0x52901c68, 0x52882508,
    "<BTN_A> glyph lo: movz w8,#0x4128 (ASCII 28 41 = \"(A\")"),
  P_RAW(0x5fceac, 0x72a83208, 0x72a00528,
    "<BTN_A> glyph lo: movk w8,#0x0029,lsl16 (ASCII 29 00 = \")\\0\")"),
  P_RAW(0x5fce88, 0x529c682e, 0x5280000e,
    "<BTN_A> glyph hi: movz w14,#0 (NUL pad)"),
  P_RAW(0x5fceb0, 0x72b2300e, 0x72a0000e,
    "<BTN_A> glyph hi: movk w14,#0,lsl16"),
  // <BTN_B> glyph -> "(B)" (28 42 29)
  P_RAW(0x5fce8c, 0x52901c75, 0x52884515,
    "<BTN_B> glyph lo: movz w21,#0x4228 (ASCII 28 42 = \"(B\")"),
  P_RAW(0x5fceb4, 0x72a85215, 0x72a00535,
    "<BTN_B> glyph lo: movk w21,#0x0029,lsl16 (ASCII 29 00 = \")\\0\")"),
  P_RAW(0x5fce90, 0x529c6856, 0x52800016,
    "<BTN_B> glyph hi: movz w22,#0 (NUL pad)"),
  P_RAW(0x5fceb8, 0x72b23016, 0x72a00016,
    "<BTN_B> glyph hi: movk w22,#0,lsl16"),
  // <BTN_X> glyph -> "(X)" (28 58 29)
  P_RAW(0x5fce94, 0x52901c6a, 0x528b050a,
    "<BTN_X> glyph lo: movz w10,#0x5828 (ASCII 28 58 = \"(X\")"),
  P_RAW(0x5fcebc, 0x72ab120a, 0x72a0052a,
    "<BTN_X> glyph lo: movk w10,#0x0029,lsl16 (ASCII 29 00 = \")\\0\")"),
  P_RAW(0x5fce98, 0x529c6b0f, 0x5280000f,
    "<BTN_X> glyph hi: movz w15,#0 (NUL pad)"),
  P_RAW(0x5fcec0, 0x72b2300f, 0x72a0000f,
    "<BTN_X> glyph hi: movk w15,#0,lsl16"),
  // <BTN_Y> glyph -> "(Y)" (28 59 29)
  P_RAW(0x5fce9c, 0x52901c6b, 0x528b250b,
    "<BTN_Y> glyph lo: movz w11,#0x5928 (ASCII 28 59 = \"(Y\")"),
  P_RAW(0x5fcec4, 0x72ab320b, 0x72a0052b,
    "<BTN_Y> glyph lo: movk w11,#0x0029,lsl16 (ASCII 29 00 = \")\\0\")"),
  P_RAW(0x5fcea0, 0x529c6b31, 0x52800011,
    "<BTN_Y> glyph hi: movz w17,#0 (NUL pad)"),
  P_RAW(0x5fcec8, 0x72b23011, 0x72a00011,
    "<BTN_Y> glyph hi: movk w17,#0,lsl16"),
  // <BTN_R> glyph -> "(R)" (28 52 29)   |   <BTN_L> glyph -> "(L)" (28 4c 29)
  P_RAW(0x5fce6c, 0xd2901c6c, 0xd28a450c,
    "<BTN_R> glyph: movz x12,#0x5228 (ASCII 28 52 = \"(R\")"),
  P_RAW(0x5fce7c, 0xf2aa520c, 0xf2a0052c,
    "<BTN_R> glyph: movk x12,#0x0029,lsl16 (ASCII 29 00 = \")\\0\")"),
  P_RAW(0x5fcea4, 0xf2dc684c, 0xf2c0000c,
    "<BTN_R> glyph: movk x12,#0,lsl32 (NUL pad)"),
  P_RAW(0x5fced0, 0xf2f2300c, 0xf2e0000c,
    "<BTN_R> glyph: movk x12,#0,lsl48 (NUL pad)"),
  P_RAW(0x5fce70, 0xd2901c6d, 0xd289850d,
    "<BTN_L> glyph: movz x13,#0x4c28 (ASCII 28 4c = \"(L\")"),
  P_RAW(0x5fce80, 0xf2a9920d, 0xf2a0052d,
    "<BTN_L> glyph: movk x13,#0x0029,lsl16 (ASCII 29 00 = \")\\0\")"),
  P_RAW(0x5fcea8, 0xf2dc684d, 0xf2c0000d,
    "<BTN_L> glyph: movk x13,#0,lsl32 (NUL pad)"),
  P_RAW(0x5fced4, 0xf2f2300d, 0xf2e0000d,
    "<BTN_L> glyph: movk x13,#0,lsl48 (NUL pad)"),

  // --- key/glyph size-byte decoupling (the fix for the 'N_R>' leak) ---
  // w20 is left UNTOUCHED at 0x0e so all six <BTN_*> search keys keep their
  // full length. w16 becomes 0x06 and is used as the size byte for every
  // glyph VALUE row (A/B/X/Y newly routed to it; L/R already used it).
  P_RAW(0x5fcecc, 0x52800210, 0x528000d0,
    "glyph size reg: mov w16,#0x10 -> #0x06 (SSO len 3, shared by all 6 glyph values)"),
  // Route the 15 A/B/X/Y glyph-value size stores from w20 -> w16 so they
  // get length 3 while the keys (still w20) keep length 7.
  P_RAW(0x5fcedc, 0x39000134, 0x39000130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x0] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcef4, 0x39006134, 0x39006130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x18] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf04, 0x3900c134, 0x3900c130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x30] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf14, 0x39012134, 0x39012130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x48] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf48, 0x3902a134, 0x3902a130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0xa8] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf58, 0x39030134, 0x39030130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0xc0] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf68, 0x39036134, 0x39036130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0xd8] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf8c, 0x39048134, 0x39048130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x120] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcf98, 0x3904e134, 0x3904e130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x138] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcfa8, 0x39054134, 0x39054130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x150] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcfb8, 0x3905a134, 0x3905a130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x168] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcfd8, 0x3906c134, 0x3906c130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x1b0] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcfe4, 0x39072134, 0x39072130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x1c8] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fcff4, 0x39078134, 0x39078130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x1e0] (use size-3 reg, not key-size reg)"),
  P_RAW(0x5fd000, 0x3907e134, 0x3907e130,
    "A/B/X/Y glyph size store: strb w20 -> w16 [x9,#0x1f8] (use size-3 reg, not key-size reg)"),
};

// ---------------------------------------------------------------------------
// 5.  diagonal_movement
//
//     Same bug class CTExt fixes on PC (FieldImpl::UserScrollDiagonal hook,
//     ctext.hooks.field_impl.ixx: "Fixes the diagonal movement stutter bug by
//     reverting to the original behaviour"). FieldImpl::UserScroll (0x5a1380)
//     is one shared switch over the 8-direction input mask (w8 = case index):
//
//       cardinal cases (w8=0,1,3,7 = up/down/left/right): raw per-axis delta
//         (+-0x10/0x20 walk/run) is stored to [x8,#0x90/#0x94] AND committed
//         straight into the X/Y target accumulators at [x8,#0x98]/[x8,#0xa4]
//         every frame -- smooth, constant per-frame step.
//
//       diagonal cases (w8=4,5,8,9 = the 4 diagonals): the SAME raw delta is
//         stored to [x8,#0x90]/[x8,#0x94] (so cardinal-style consumers see
//         the right magnitude), but the X/Y TARGET accumulators instead go
//         through an extra float path: divide the raw delta by 0x3fb1eb85
//         (1.39, a rough "diagonal speed compensation" constant -- notably
//         NOT sqrt(2)=1.41421...), truncate to int (fcvtzs = round toward
//         zero) and add. Truncating every single frame instead of carrying
//         the fractional remainder means the target consistently advances
//         slower (~23/frame instead of 32/frame, etc.) than the raw/cardinal
//         rate -- a per-frame divergence between what's stored as "raw" and
//         what the camera/position actually commits to. That's the stutter.
//
//     Fix: make diagonal commit the raw delta directly, exactly like
//     cardinal already does (target += raw, no normalize/truncate), instead
//     of porting the float math byte-for-byte. Each of the 4 diagonal blocks
//     ends with a "cbnz w12, <go down the float path>" immediately after
//     storing the raw delta to [x8,#0x90]/[x8,#0x94] -- redirect each of
//     those 4 branches (unconditionally) into a small shared cave that does
//     the same raw-int accumulate the cardinal blocks use, then jumps to the
//     function's existing epilogue. Verified: redirecting these 4 sites
//     touches only diagonal-block code; the cardinal blocks' own commit site
//     (0x5a16bc) and the function epilogue (0x5a16c8) are untouched and are
//     also where this cave's last instruction lands.
//
//     Net gameplay effect: diagonal movement speed equals raw axis speed
//     (matches cardinal exactly) instead of running ~28% slower with visible
//     truncation-stutter -- the same direction CTExt's PC fix takes (drop
//     the lossy per-frame normalize/truncate, accumulate raw deltas).
// ---------------------------------------------------------------------------
static const PatchEntry g_diagonal_patches[] = {
  // Redirect each diagonal block's post-store branch into the shared cave below.
  // Original instruction at each site is the first half of a float divide-by-1.39
  // "diagonal speed normalization" (cbnz into the float path); replacing it with an
  // unconditional branch skips that path entirely for all 4 diagonal blocks.
  P_RAW(0x5a14e4, 0x35000a0c, 0x1400003a,
    "UserScroll diagonal block w8=4 (down-right/up-right family): skip float normalize, use raw-int accumulate"),
  P_RAW(0x5a151c, 0x35000a0c, 0x1400002c,
    "UserScroll diagonal block w8=9: skip float normalize, use raw-int accumulate"),
  P_RAW(0x5a1570, 0x350003ac, 0x14000017,
    "UserScroll diagonal block w8=5: skip float normalize, use raw-int accumulate"),
  P_RAW(0x5a15c8, 0x3500018c, 0x14000001,
    "UserScroll diagonal block w8=8: skip float normalize, use raw-int accumulate"),

  // Cave @ 0x5a15cc (40 bytes of now-dead code after the 4 redirects above;
  // verified empty/unreachable -- nothing else in the function branches here).
  P_CAVE(0x5a15cc, 0x29522d0a, "cave: ldp w10,w11,[x8,#0x90]   ; w10,w11 = raw per-axis step just stored (same magnitude cardinal uses)"),
  P_CAVE(0x5a15d0, 0xb940990c, "cave: ldr w12,[x8,#0x98]       ; w12 = current X target"),
  P_CAVE(0x5a15d4, 0xb0a018c, "cave: add w12,w12,w10          ; X target += raw X step (no normalization, no truncation loss)"),
  P_CAVE(0x5a15d8, 0xb900990c, "cave: str w12,[x8,#0x98]"),
  P_CAVE(0x5a15dc, 0xb900a10c, "cave: str w12,[x8,#0xa0]       ; commit X (matches cardinal's commit site)"),
  P_CAVE(0x5a15e0, 0xb940a509, "cave: ldr w9,[x8,#0xa4]        ; w9 = current Y target"),
  P_CAVE(0x5a15e4, 0xb0b0129, "cave: add w9,w9,w11            ; Y target += raw Y step"),
  P_CAVE(0x5a15e8, 0xb900a509, "cave: str w9,[x8,#0xa4]"),
  P_CAVE(0x5a15ec, 0xb900ad09, "cave: str w9,[x8,#0xac]        ; commit Y (matches cardinal's commit site)"),
  P_CAVE(0x5a15f0, 0x14000036, "cave: b 0x5a16c8 (UserScroll epilogue, restores x19/x29/x30 and returns)"),
};

// ---------------------------------------------------------------------------
// 6.  fixed_timestep
//
//     Root cause of the "everything shimmers/scoots while moving, clean when
//     stationary" artifact -- confirmed by frametime.log: a locked ~60fps
//     (avg 16.72ms, ~0 dropped frames) but with heavy per-frame jitter (a large
//     share of frames land 17-20ms while others land short). cocos2d advances
//     ALL motion by Director::_deltaTime (Director+0xe8), which Director::
//     drawScene recomputes every frame as the raw wall-clock gap since the
//     previous frame:
//
//         _deltaTime = max( (now - _lastUpdate) seconds , 0 )   // no smoothing
//
//     Vsync pins presentation to a clean 60Hz, but that measured gap still
//     wobbles, so constant-speed motion advances by an uneven number of pixels
//     each displayed frame. The eye integrates the unevenness as a directional
//     smear -- worse the faster you move, a faint "flag in the wind" wave when
//     slow, gone at rest -- and it hits everything on that clock: field scroll,
//     free sprites, and sliding menu text alike. Notably it is NOT a scaling or
//     sampling artifact (identical with bilinear on or off) and NOT dropped
//     frames (over34ms ~= 0), which is what ruled every earlier theory out.
//
//     CT is frame-based at heart (the SNES ran a rock-steady 60Hz); its motion
//     logic wants a constant step. Fix: force _deltaTime to a fixed 1/60s every
//     frame instead of the jittery measured value, so every frame advances by
//     exactly the same amount. Patched at BOTH writers of _deltaTime:
//     Director::drawScene (the live per-frame path; dt is inlined there) and the
//     sibling Director::calculateDeltaTime. At each site the dead measured-dt
//     computation block is overwritten IN PLACE with "w8 = 1/60 bits; s0 =
//     (float)w8; branch to the existing _deltaTime store". The _nextDeltaTimeZero
//     reset path (dt forced to 0 for one frame after a pause/scene load) and the
//     _lastUpdate write are both left untouched, so only the normal-frame delta
//     changes. 1/60 = 0x3c888889; it is too small to encode as fmov #imm, hence
//     the movz/movk + fmov s0,w8.
//
//     Trade-off: if the game ever SUSTAINS below 60fps (rare here), a fixed step
//     makes motion run real-time-slow for those frames rather than skipping --
//     for a single-player RPG that is the right call and far less visible than
//     the judder. Disable this flag to revert to the original measured dt.
// ---------------------------------------------------------------------------
static const PatchEntry g_fixed_timestep_patches[] = {
  // Director::drawScene @0x8d85a0 -- live per-frame dt store. Overwrite the
  // else-branch (measured-dt math) with: w8=0x3c888889; s0=(float)w8; b <store>.
  P_RAW(0x8d85dc, 0xf940ba68, 0x52911128, "drawScene: movz w8,#0x8889 (lo half of 1/60)"),
  P_RAW(0x8d85e0, 0xcb080008, 0x72a79108, "drawScene: movk w8,#0x3c88,lsl#16 -> w8 = 1/60 bits"),
  P_RAW(0x8d85e4, 0x9b557d08, 0x1e270100, "drawScene: fmov s0,w8 (dt = 1/60)"),
  P_RAW(0x8d85e8, 0x9347fd09, 0x14000009, "drawScene: b 0x8d860c (fall into the _deltaTime store)"),

  // Director::calculateDeltaTime @0x8d8888 -- sibling copy, patched identically.
  P_RAW(0x8d88b0, 0xd29ef9e9, 0x52911128, "calcDeltaTime: movz w8,#0x8889"),
  P_RAW(0x8d88b4, 0xf940ba68, 0x72a79108, "calcDeltaTime: movk w8,#0x3c88,lsl#16"),
  P_RAW(0x8d88b8, 0xf2bc6a69, 0x1e270100, "calcDeltaTime: fmov s0,w8"),
  P_RAW(0x8d88bc, 0xf2d374a9, 0x1400000d, "calcDeltaTime: b 0x8d88f0 (fall into the _deltaTime store)"),
};

// ---------------------------------------------------------------------------
// 7.  design_resolution_fix
//
//     ROOT CAUSE of the motion shimmer / "scooting" / thinning-eyes artifact
//     (confirmed via scale_diag.log, not guessed): AppDelegate::
//     applicationDidFinishLaunching hardcodes the cocos2d-x design resolution
//     to 568x320 -- leftover iPhone-5 boilerplate (aspect 568/320 = 1.775),
//     completely unrelated to the Switch's panel. cocos2d::GLView::
//     updateDesignResolutionSize computes mScaleX = frameWidth/designWidth
//     and mScaleY = frameHeight/designHeight; every Switch panel is exact
//     16:9, so this always lands on 1280/568 = 720/320 = 2.253521... --
//     never an integer, on every resolution, handheld or docked. That
//     fractional scale is what every layer (field render-targets, sprites,
//     and UI/menu text alike) gets stretched by, and GL_NEAREST sampling of
//     a non-integer, per-frame-shifting scroll position is exactly what
//     reads as scoot/shimmer in motion and clean at rest.
//
//     This supersedes the earlier integer_scale_fix attempt (rounding
//     mScaleX/mScaleY after the fact, at 0x8b76b4) -- that only changed the
//     zoom level, because the *input* to the division was still the wrong
//     design resolution. Fixing the actual constant is the correct place:
//     640x360 is exact 16:9, so it divides every Switch resolution cleanly
//     -- 1280x720 -> 2.0x, 1920x1080 -> 3.0x, 2560x1440 -> 4.0x -- with no
//     rounding, no cave, no downstream patch needed at all.
//
//     Patches the two float-literal immediates in the small unnamed static
//     initializer that populates the Size table AppDelegate reads from
//     (function at 0x641bf4, no exported symbol): 568.0 -> 640.0 and
//     320.0 -> 360.0. Both original and replacement values happen to encode
//     with zero low-16-bits, so each is a single "mov w_,#imm" instruction
//     (movz, no movk needed) -- a pure immediate swap, no branches added.
//
//     Trade-off: at the new design resolution the game shows ~12% more of
//     the world per screen edge-to-edge (568/320 -> 640/360 is a small
//     zoom-out), since more world now maps to the same screen. Sprite/UI
//     pixel density is unaffected -- this is a scale-alignment fix, not a
//     resize -- only the visible margin changes slightly.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Design resolution fix, table-wide + config-driven.
//
// The game does NOT have one design resolution: a static initializer at
// 0x641c00.. builds an aspect-bucketed TABLE of candidate design Sizes --
// (568x320) 16:9, (480x360) 4:3, (568x340) 16:10, (1680x720) ultrawide --
// and a runtime picker selects from it on every surface event (boot, dock,
// undock). Patching only the 16:9 entry (all earlier builds) left the picker
// free to apply other entries after resize events, producing the observed
// boot-vs-redock inconsistency and, in one state, width from one entry with
// height from another (480x360 -> scaleX=4 / scaleY=3 at 1080p: uniform
// widths, broken heights -- the "rectangles").
//
// Fix: stamp EVERY entry in the table with the same (w,h), so the picker's
// choice is irrelevant. Values are config-driven per mode (see config.h) so
// the scale/framing trade-off can be A/B'd from config.ini without rebuilds.
// The (568x340) entry's width register reuses the 16:9 entry's (s8), so only
// its height instruction exists to patch -- width follows automatically.
// ---------------------------------------------------------------------------

// Encode: movz w<rd>, #(top 16 bits of float v), lsl #16
static uint32_t movz_topf(int rd, float v) {
  union { float f; uint32_t u; } x; x.f = v;
  return 0x52A00000u | ((x.u >> 16) << 5) | (uint32_t)rd;
}

static void apply_design_resolution(so_module *mod, float w, float h) {
  static const struct { uint32_t va; int rd; float oldv; int is_h; } sites[] = {
    { 0x641c34, 8,  568.0f, 0 },  // 16:9 entry, width
    { 0x641c38, 9,  320.0f, 1 },  // 16:9 entry, height
    { 0x641c4c, 8,  480.0f, 0 },  // 4:3 entry, width
    { 0x641c50, 9,  360.0f, 1 },  // 4:3 entry, height
    { 0x641c68, 8,  340.0f, 1 },  // 16:10 entry, height (width = 16:9 width via s8)
    { 0x641c80, 8, 1680.0f, 0 },  // ultrawide entry, width
    { 0x641c84, 9,  720.0f, 1 },  // ultrawide entry, height
  };
  PatchEntry e[7];
  for (int i = 0; i < 7; i++) {
    e[i].sym_name  = NULL;
    e[i].func_off  = 0;
    e[i].raw_vaddr = sites[i].va;
    e[i].old_word  = movz_topf(sites[i].rd, sites[i].oldv);
    e[i].new_word  = movz_topf(sites[i].rd, sites[i].is_h ? h : w);
    e[i].desc      = "design resolution table entry (unified per-mode value)";
  }
  apply_patches(mod, e, 7);
}

// ---------------------------------------------------------------------------
// 8.  game_area_width_fix
//
//     ROOT CAUSE of the non-square art pixels (squares wider than tall by
//     ~1.127 = 640/568), measured three times from panel crops and invariant
//     across handheld/docked and across every screen_width/height combination
//     -- which is why hand-tuning the surface size could approach but never
//     reach square: the error is upstream of both scaling stages.
//
//     AppDelegate::applicationDidFinishLaunching ends by constructing the
//     global cocos2d::Rect  ctr::gameArea  (exported symbol _ZN3ctr8gameAreaE)
//     at 0x641a54..0x641ac0. Its HEIGHT is adaptive (visibleSize.height
//     based -- the same adaptive style as the field code's visH-320 and
//     visH*192/320 math), but its WIDTH is the hardcoded float 568.0:
//
//         gameArea.width  = 568 - 2*visibleOrigin.x        (568 literal)
//         gameArea.height = visibleSize.height - y_offset  (adaptive)
//
//     568 is the iPhone-5 design width the port was authored against. With
//     the stock 568x320 design this rect exactly equals the visible area, so
//     the mismatch is invisible on Android. With design_resolution_fix
//     widening the design to 640x360, the game keeps laying the world out
//     568 wide and fills the 640-wide screen with it: a 640/568 = 1.1268
//     horizontal-only stretch. Vertical stays clean because the height side
//     was adaptive all along -- matching the measurement exactly (vertical
//     uniform, horizontal 6,6,5 / 4,4,4,5 patterns, ratio 1.123-1.133).
//
//     Fix: make the width adaptive the same way the height already is.
//     visibleRect is on the stack at [sp+0x10..0x1f] at this point (sret of
//     the getVisibleRect vtable call at 0x641a64; width at [sp+0x18] --
//     the code itself loads it from there at 0x641a6c):
//
//       0x641a94  mov  w8,#0x440e0000 (568.0f) -> ldr s6,[sp,#0x18] (visW)
//       0x641a98  fmov s6,w8                   -> nop
//       0x641ab0  fadd s0,s3,s0 (x=2*ox+m-44)  -> fmov s0,s3 (x=origin.x)
//
//     giving gameArea = (origin.x, y, visW - 2*origin.x, h). Provably a
//     NO-OP at the stock 568-wide design (origin.x=0: x 0->0, w 568->568),
//     so it only takes effect exactly where the bug exists. w8 is dead
//     between 0x641a98 and its next write at 0x641ac4, and s6 is unread
//     between the new load and its consumption at 0x641aa8 (verified in
//     disassembly). Encodings verified with aarch64-linux-gnu-as.
//
//     STATUS (on-device testing): patch applies and boots clean, but the
//     field-map stretch was not visibly cured by it -- gameArea's 20 reader
//     sites are UI/menu/world-map code, and the field pipeline appears to
//     carry its OWN copy of the 568-era horizontal constants (hardcoded
//     44/480 margin math in FieldMap::setScrollLimit at 0x570cec, vs its
//     adaptive visH-based vertical math). Panel-crop measurement pending to
//     confirm; if confirmed, the follow-up patch targets those field
//     constants the same way. Keep this fix on regardless: it is the
//     correct generalization for every gameArea consumer.
// ---------------------------------------------------------------------------
static const PatchEntry g_gamearea_patches[] = {
  P_RAW(0x641a94, 0x52a881c8, 0xbd401be6,
        "gameArea: width literal 568.0 -> ldr s6,[sp,#0x18] (visible width)"),
  P_RAW(0x641a98, 0x1e270106, 0xd503201f,
        "gameArea: fmov s6,w8 -> nop (s6 now loaded directly)"),
  P_RAW(0x641ab0, 0x1e202860, 0x1e204060,
        "gameArea: x = origin.x (fadd s0,s3,s0 -> fmov s0,s3)"),
};

// ---------------------------------------------------------------------------
// 9.  field_pixel_perfect
//
//     Full-frame measurement (1280x720 screenshot, autocorrelation over the
//     whole panel) pinned the field's art-pixel size to exactly 3.75 x
//     3.3333 panel px -- ratio 9/8 -- with a visible world of 341.33 x 216
//     art px. Those decode to the game's view-sizing formulas verbatim:
//     216 = visibleHeight*192/320 (FieldMap::init -> [this+0x34c]) and
//     341.33 = visibleWidth*256/480 (setScrollLimit). The port sizes its
//     field view at 256 art px per 480 design units horizontally but 192
//     per 320 vertically: densities 8/15 vs 3/5, unequal by exactly 9/8.
//     The same formulas produce the same 9/8 on the original iPhone-5
//     layout -- i.e. this is Square Enix's deliberate approximation of the
//     SNES/CRT pixel aspect (true CRT: 8/7), not an accident. It is also
//     why no design/surface/GL configuration could ever remove it.
//
//     This patch sets both densities to exactly 1/2 (numerators only:
//     192 -> 160 = 320/2, 256 -> 240 = 480/2; the 320/480/-320/44 authored
//     constants are left alone so every margin/limit expression stays
//     dimensionally consistent). Result with the 640x360 design: the view
//     becomes 320x180 art px -- 16:9 in art terms -- so art pixels are
//     4x4 panel px in handheld and 6x6 docked: square AND integer in both
//     modes, eliminating both the aspect stretch and the uneven
//     tight/loose pixel-width pattern (the motion shimmer). The field
//     camera is ~7%% tighter horizontally and ~17%% vertically than the
//     CRT-style view; menus/UI do not use these formulas and are
//     unaffected. All three vertical-density sites (init, setScrollLimit,
//     the camera-follow at 0x608bf0) and the single horizontal site are
//     patched together so view size, scroll limits and camera centering
//     move coherently. Encodings verified with aarch64-linux-gnu-as.
//
//     CORRECTION (superseding the previous handoff note below): the rodata
//     pair at 0x360b08 is NOT the blit-scale source. Symbol resolution
//     (.dynsym) shows its two readers are FieldMap::onTouchMoved and
//     FieldMap::onTouchEnded -- touch-drag delta / tap-to-tile hit-testing,
//     mobile-only input code, never on the render path. That is exactly why
//     patching it produced no display effect. It's left patched below
//     anyway (harmless) purely so touch-drag math stays dimensionally
//     consistent with the density patches above, on the off chance the
//     touch screen is used in handheld mode.
//
//     The actual blit-scale source: FieldMap::makeField (0x57520c-0x576668)
//     builds a child Node -- internally named "fieldmap" via an inlined
//     8-byte string constant a few instructions earlier -- adds it as a
//     child, stashes the pointer at field+0x3b8, and immediately calls
//     Node::setScale(x,y) (vtable slot 0x90) on it with two HARD-CODED
//     immediates: s0 = 1.875f via a single `fmov` (exactly representable in
//     the 8-bit float-immediate encoding), s1 = 1.66667f assembled into w9
//     via `mov`+`movk` (0x3FD55555, NOT exactly representable as an fmov
//     immediate) then moved with `fmov s1, w9`. This is a third, previously
//     unknown occurrence of the (1.875, 1.66667) ratio -- distinct from
//     both the density formulas and the rodata pair above -- and it's
//     never loaded from memory, which is why full-frame measurement showed
//     H locked at 3.75 px/art through every prior patch: 1.875 * 2.0 =
//     3.75 and 1.66667 * 2.0 = 3.33334 match the measured art-pixel size
//     exactly, where the outer 2.0/3.0 is the handheld/docked content-scale
//     factor applied downstream of this node. Setting both to 2.0 here
//     composes to the same 4x4 handheld / 6x6 docked target as the density
//     patches above, on the actual render path this time.
//
//     Patch replaces `mov w9,#0x5555` / `movk w9,#0x3fd5,lsl#16` with NOPs
//     (the fmov-immediate form doesn't need the general register at all)
//     and both `fmov` sites' encoded value with 2.0f's. Instruction count
//     and addresses are unchanged, so nothing downstream shifts. Encodings
//     verified with keystone (cross-checked against the original fmov
//     #1.875 bytes) and re-disassembled after patching to confirm a clean
//     `fmov s0,#2.0` / `fmov s1,#2.0` pair with no corruption of the
//     neighboring `ldr`/`mov`/`blr` instructions.
//
//     NOT YET MEASURED ON HARDWARE -- apply and re-run the autocorrelation
//     measurement to confirm 4.0000x4.0000 before trusting this comment.
// ---------------------------------------------------------------------------
static const PatchEntry g_field_pixel_perfect_patches[] = {
  P_RAW(0x56d874, 0x52a86808, 0x52a86408, "init: view-height density 192/320 -> 160/320"),
  P_RAW(0x570d0c, 0x52a86809, 0x52a86409, "setScrollLimit: vertical density 192 -> 160"),
  P_RAW(0x608bf0, 0x52a86808, 0x52a86408, "camera-follow: vertical density 192 -> 160"),
  P_RAW(0x570cfc, 0x52a87008, 0x52a86e08, "setScrollLimit: horizontal density 256 -> 240"),
  // Touch-drag / hit-test converters only -- confirmed no display effect.
  // Kept for touch-drag consistency; safe no-op for controller-only play.
  P_RAW(0x360b08, 0x3ff00000, 0x40000000, "onTouchMoved/Ended: rodata view scale X: 1.875f -> 2.0f"),
  P_RAW(0x360b0c, 0x3fd55555, 0x40000000, "onTouchMoved/Ended: rodata view scale Y: 1.66667f -> 2.0f"),
  // The real fix: FieldMap::makeField's "fieldmap" node, setScale(1.875, 1.66667) -> setScale(2.0, 2.0).
  P_RAW(0x5761fc, 0x528aaaa9, 0xd503201f, "makeField: mov w9,#0x5555 (dead, was s1 lo half) -> nop"),
  P_RAW(0x576200, 0x1e2fd000, 0x1e201000, "makeField: fieldmap node setScale X: fmov s0,#1.875 -> #2.0"),
  P_RAW(0x576208, 0x72a7faa9, 0xd503201f, "makeField: movk w9,#0x3fd5,lsl#16 (dead, was s1 hi half) -> nop"),
  P_RAW(0x576210, 0x1e270121, 0x1e201001, "makeField: fieldmap node setScale Y: fmov s1,w9(1.66667) -> fmov s1,#2.0"),
};

// ---------------------------------------------------------------------------
// The identical hard-coded (1.875, 1.66667) setScale(x,y) pattern also
// appears, unpatched, at these WorldMap sites -- the overworld map screen
// almost certainly has the exact same non-square art-pixel issue as the
// field screens, via the same idiom (immediate-encoded fmov pair feeding
// vtable slot 0x90). NOT enabled by default -- field_pixel_perfect was
// scoped to the field only. Fold into g_field_pixel_perfect_patches (same
// nop+fmov replacement shape as above, at these addresses) if/when the
// overworld map is measured and confirmed to have the same 9/8 stretch:
//   WorldMap::Init2          0x607c98 / 0x607c9c   (first  fieldmap-alike node)
//   WorldMap::Init2          0x607db4 / 0x607db8   (second fieldmap-alike node)
//   WorldMap::initWeatherMap 0x60840c / 0x608424
//   WorldMap::exitMiniMap    0x609c28 / 0x609c2c
// Deliberately NOT touching the other three occurrences found by the same
// scan (BattleMenu::Proc 0x5b143c/0x5b15ec, AgeSelectScene::init 0x744264/
// 0x744268, SpecialEventScene::init 0x777ed0/0x777ed4) -- no evidence yet
// they're the same "low-res canvas stretched to fill the screen" idiom
// rather than an unrelated coincidental reuse of the same ratio; patching
// those blind risks warping unrelated UI/background art.
// ---------------------------------------------------------------------------

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

  if (config.fix_diagonal_movement) {
    debugPrintf("patches: applying diagonal_movement fix\n");
    apply_patches(mod, g_diagonal_patches, PATCH_COUNT(g_diagonal_patches));
  }

  if (config.fixed_timestep) {
    debugPrintf("patches: applying fixed_timestep\n");
    apply_patches(mod, g_fixed_timestep_patches, PATCH_COUNT(g_fixed_timestep_patches));
  }

  if (config.field_pixel_perfect) {
    debugPrintf("patches: applying field_pixel_perfect\n");
    apply_patches(mod, g_field_pixel_perfect_patches, PATCH_COUNT(g_field_pixel_perfect_patches));
  }

  if (config.game_area_width_fix) {
    debugPrintf("patches: applying game_area_width_fix\n");
    apply_patches(mod, g_gamearea_patches, PATCH_COUNT(g_gamearea_patches));
  }

  if (config.design_resolution_fix) {
    // 640x360 stamped across the WHOLE aspect table, both modes: 2x at 720p
    // handheld, 3x at 1080p docked -- the familiar framing. Table-wide
    // stamping keeps it stable across boot/dock/undock (the game's runtime
    // aspect picker can no longer swap entries).
    debugPrintf("patches: design resolution 640x360 (all aspect-table entries)\n");
    apply_design_resolution(mod, 640.0f, 360.0f);
  }
}

#endif /* __PATCHES_H__ */