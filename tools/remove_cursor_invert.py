#!/usr/bin/env python3
"""
remove_cursor_invert.py  (v9)  --  Chrono Trigger NX  libchrono.so  (ARM64)

Copyright (C) 2026 ppkantorski <https://github.com/ppkantorski>
This software may be modified and distributed under the terms of the MIT
license. See the LICENSE file for details.

WHAT THIS FIXES
===============
When the cursor moves onto a menu item or dialogue choice:
  - The selected row's text turned BLACK  →  now stays WHITE
  - The selection highlight was a cream/white rectangle  →  now dark semi-transparent

Covers: battle tech/item list menus, Attack/Combo/Item command menu,
        dialogue choice windows (MsgWindow), field menu, world-map menu buttons.

WHAT CHANGES  (10 logical fixes / 36 word-writes)
=================================

1. BattleTechMenu::setListButtonFontColors(int)
     0x6e40cc  ldr x10,[x10,#0xcb0](BLACK) -> ldr x10,[x10,#0xd20](WHITE)

2. BattleItemMenu::onSelectChanged(int)  [CRASH FIX]
     0x6e52dc  strh w9,[sp,#0x14]  ->  strh w10,[sp,#0x14]  (WHITE r,g via w10)
     0x6e52e0  strb w8,[sp,#0x16]  ->  strb w10,[sp,#0x16]  (WHITE b via w10)

3. MsgWindow::createMenu — dialogue choice windows
     0x5fd998  mov x0,x23(WHITE) -> mov x0,x21(BLACK)  [ItemSpriteA selectedImg]
     0x5fd9e8  mov x0,x23(WHITE) -> mov x0,x21(BLACK)  [ItemSpriteB normalImg — THE FIX]
     0x5fda18  mov x1,x21(BLACK) -> mov x1,x23(WHITE)  [text label tint stays WHITE]

4. FieldMenu::init()
     0x755538  ldr x8,[x8,#0xcb0](BLACK) -> ldr x8,[x8,#0xd20](WHITE)

5. WorldMenu button helper @ 0x782c7c  (MENU / MAP / WARP — 3 buttons)
     0x782ca4  ldr x9,[x9,#0xcb0](BLACK) -> ldr x9,[x9,#0xd20](WHITE)

6. BattleMenu::drawCommandWindow(int,bool,...)
     0x5b56f0  ldr x10,[x10,#0xcb0](BLACK) -> ldr x10,[x10,#0xd20](WHITE)

7. BattleMenu::drawCommandWindow()
     0x5b2210  ldr x2,[x2,#0xcb0](BLACK) -> ldr x2,[x2,#0xd20](WHITE)

8. BattleListMenu highlight-colour init
   Color4B(0xfe,0xff,0xdd,0xcc) cream  ->  Color4B(0,0,0,160) dark semi-transparent
     0x6e6994  mov w1,#0xfe -> #0x00
     0x6e6998  mov w2,#0xff -> #0x00
     0x6e699c  mov w3,#0xdd -> #0x00
     0x6e69a0  mov w4,#0xcc -> #0xa0

9. BattleMenu::init()  command-window SELECTED-FILL colour
   The cream selection FILL is a Color4B(254,255,221,204) built in init at two
   sites and passed (with a gray Color4B(123,123,123,128) "deselect" colour) to
   the highlight-setup helper 0x5ae924 -- the same select/deselect pattern as the
   BattleListMenu highlight in fix 8. Recolour the cream "select" Color4B to
   (0,0,0,160) dark semi-transparent at both sites:
     0x5ad8b0..0x5ad8bc  mov w1..w4 #0xfe,#0xff,#0xdd,#0xcc -> #0,#0,#0,#0xa0
     0x5ada68..0x5ada74  mov w1..w4 #0xfe,#0xff,#0xdd,#0xcc -> #0,#0,#0,#0xa0

Note: the command-window FRAME/BORDER is intentionally left at its original
colour so the frame glyph stays visible on top of the dark fill from fix 9.

10. MsgWindow::createMenu  dialogue selector NON-selected transparency
   Each choice background is a textureless container node from makeColorNode
   (0x5fdc24) whose visible part is 3 "menu_win" child sprites; the transparency
   is the menu_win texture's own alpha (children render at full opacity, opacity
   does NOT cascade by default -- only colour does, which is why fix 3 worked but
   a plain setOpacity did not). The selected/cursor background is therefore at the
   texture's alpha ceiling and cannot be made MORE opaque. To make the cursor
   stand out, the NON-selected (normal) background is made more transparent: its
   makeColorNode call (0x5fd950, the `normal` arg of the cursor MenuItemSprite) is
   routed through a cave wrapper that enables opacity cascade on the whole node
   (nsSpriteUtils::setCascadeOpacityEnabledRecursive, 0x7430ec) then setOpacity
   (0xa0)=160, so the menu_win children dim. The selected/cursor background keeps
   full alpha, so the cursor reads as the more-solid black.

   Helper @ 0xaa56a0:
       stp x19, x30, [sp, #-0x10]!
       bl  #0x5fdc24              ; makeColorNode(colour) -> node
       mov x19, x0
       mov w1, #1
       bl  #0x7430ec             ; setCascadeOpacityEnabledRecursive(node, true)
       mov x0, x19
       mov w1, #0xa0             ; opacity 160 (dim the non-selected bg)
       ldr x8, [x19]
       ldr x8, [x8, #0x490]      ; Node::setOpacity(GLubyte)
       blr x8
       mov x0, x19
       ldp x19, x30, [sp], #0x10
       ret
   Redirect: 0x5fd950  bl makeColorNode (normal bg) -> bl 0xaa56a0

Usage
-----
    python3 remove_cursor_invert.py libchrono.so
    python3 remove_cursor_invert.py libchrono.so -o libchrono_noInvert.so
    python3 remove_cursor_invert.py libchrono.so --in-place   # keeps .bak
    python3 remove_cursor_invert.py libchrono.so --dry-run    # report only

Verified against libchrono.so BuildID 227bf5c808d16ee3aa584ec0e6fad1a69bb44bcf.
"""

import argparse
import struct
import sys

# ---------------------------------------------------------------------------
# Patch table  (file_offset, old_word_LE, new_word_LE, description)
# ---------------------------------------------------------------------------
PATCHES = [

    # 1. BattleTechMenu::setListButtonFontColors(int)
    (0x6e40cc, 0xf946594a, 0xf946914a,
     "BattleTechMenu::setListButtonFontColors  selected text BLACK->WHITE"),

    # 2. BattleItemMenu::onSelectChanged(int)  CRASH FIX
    (0x6e52dc, 0x79002be9, 0x79002bea,
     "BattleItemMenu::onSelectChanged  new-select r,g: BLACK->WHITE  (strh w9->w10)"),
    (0x6e52e0, 0x39005be8, 0x39005bea,
     "BattleItemMenu::onSelectChanged  new-select b:   BLACK->WHITE  (strb w8->w10)"),

    # 3. MsgWindow::createMenu — dialogue choice windows
    (0x5fd998, 0xaa1703e0, 0xaa1503e0,
     "MsgWindow::createMenu  ItemSpriteA selectedImg bg: WHITE->BLACK"),
    (0x5fd9e8, 0xaa1703e0, 0xaa1503e0,
     "MsgWindow::createMenu  ItemSpriteB normalImg bg: WHITE->BLACK  [THE FIX]"),
    (0x5fda18, 0xaa1503e1, 0xaa1703e1,
     "MsgWindow::createMenu  selected-choice text tint: BLACK->WHITE"),

    # 4. FieldMenu::init()
    (0x755538, 0xf9465908, 0xf9469108,
     "FieldMenu::init  selected-state sprite colour BLACK->WHITE"),

    # 5. WorldMenu button helper (MENU / MAP / WARP)
    (0x782ca4, 0xf9465929, 0xf9469129,
     "WorldMenu button helper  selected-state sprite colour BLACK->WHITE"),

    # 6. BattleMenu::drawCommandWindow(int,bool,...)
    (0x5b56f0, 0xf946594a, 0xf946914a,
     "BattleMenu::drawCommandWindow(int,bool)  selected text BLACK->WHITE"),

    # 7. BattleMenu::drawCommandWindow()
    (0x5b2210, 0xf9465842, 0xf9469042,
     "BattleMenu::drawCommandWindow()  selected cmd text BLACK->WHITE"),

    # 8. BattleListMenu highlight-colour init
    (0x6e6994, 0x52801fc1, 0x52800001,
     "BattleListMenu highlight init  selected bg r: 0xfe->0x00"),
    (0x6e6998, 0x52801fe2, 0x52800002,
     "BattleListMenu highlight init  selected bg g: 0xff->0x00"),
    (0x6e699c, 0x52801ba3, 0x52800003,
     "BattleListMenu highlight init  selected bg b: 0xdd->0x00"),
    (0x6e69a0, 0x52801984, 0x52801404,
     "BattleListMenu highlight init  selected bg a: 0xcc->0xa0 (~63 % opaque)"),

    # 9. BattleMenu::init() command-window SELECTED-FILL colour
    #    cream Color4B(254,255,221,204) -> (0,0,0,160) dark transparent, 2 sites
    (0x5ad8b0, 0x52801fc1, 0x52800001,
     "cmd-window fill Color4B @0x5ad8b0 r 0xfe->0x00"),
    (0x5ad8b4, 0x52801fe2, 0x52800002,
     "cmd-window fill Color4B @0x5ad8b0 g 0xff->0x00"),
    (0x5ad8b8, 0x52801ba3, 0x52800003,
     "cmd-window fill Color4B @0x5ad8b0 b 0xdd->0x00"),
    (0x5ad8bc, 0x52801984, 0x52801404,
     "cmd-window fill Color4B @0x5ad8b0 a 0xcc->0xa0"),
    (0x5ada68, 0x52801fc1, 0x52800001,
     "cmd-window fill Color4B @0x5ada68 r 0xfe->0x00"),
    (0x5ada6c, 0x52801fe2, 0x52800002,
     "cmd-window fill Color4B @0x5ada68 g 0xff->0x00"),
    (0x5ada70, 0x52801ba3, 0x52800003,
     "cmd-window fill Color4B @0x5ada68 b 0xdd->0x00"),
    (0x5ada74, 0x52801984, 0x52801404,
     "cmd-window fill Color4B @0x5ada68 a 0xcc->0xa0"),

    # 10. MsgWindow::createMenu dialogue selector: dim NON-selected bg so cursor
    #     stands out. Route normal-bg makeColorNode (0x5fd950) through cave that
    #     enables opacity cascade then setOpacity(160).
    (0x5fd950, 0x940000b5, 0x94129f54,
     "dialogue normal bg makeColorNode -> cave (cascade + dim 160)"),
    (0xaa56a0, 0x00000000, 0xa9bf7bf3,
     "cave @0xaa56a0: stp x19,x30,[sp,#-0x10]!"),
    (0xaa56a4, 0x00000000, 0x97ed6160,
     "cave @0xaa56a4: bl #0x5fdc24 (makeColorNode)"),
    (0xaa56a8, 0x00000000, 0xaa0003f3,
     "cave @0xaa56a8: mov x19,x0"),
    (0xaa56ac, 0x00000000, 0x52800021,
     "cave @0xaa56ac: mov w1,#1"),
    (0xaa56b0, 0x00000000, 0x97f2768f,
     "cave @0xaa56b0: bl #0x7430ec (setCascadeOpacityEnabledRecursive)"),
    (0xaa56b4, 0x00000000, 0xaa1303e0,
     "cave @0xaa56b4: mov x0,x19"),
    (0xaa56b8, 0x00000000, 0x52801401,
     "cave @0xaa56b8: mov w1,#0xa0 (opacity 160)"),
    (0xaa56bc, 0x00000000, 0xf9400268,
     "cave @0xaa56bc: ldr x8,[x19]"),
    (0xaa56c0, 0x00000000, 0xf9424908,
     "cave @0xaa56c0: ldr x8,[x8,#0x490] (setOpacity)"),
    (0xaa56c4, 0x00000000, 0xd63f0100,
     "cave @0xaa56c4: blr x8"),
    (0xaa56c8, 0x00000000, 0xaa1303e0,
     "cave @0xaa56c8: mov x0,x19"),
    (0xaa56cc, 0x00000000, 0xa8c17bf3,
     "cave @0xaa56cc: ldp x19,x30,[sp],#0x10"),
    (0xaa56d0, 0x00000000, 0xd65f03c0,
     "cave @0xaa56d0: ret"),
]


def die(msg: str) -> None:
    print("ERROR: " + msg, file=sys.stderr)
    sys.exit(1)


def verify_elf(data: bytearray) -> None:
    if data[:4] != b"\x7fELF":
        die("not an ELF file")
    if data[4] != 2 or data[5] != 1:
        die("expected 64-bit little-endian ELF")
    if struct.unpack_from("<H", data, 0x12)[0] != 0xb7:
        die("ELF machine type is not AArch64 (0xb7)")


def apply_patches(data: bytearray, dry_run: bool) -> tuple:
    applied = already = errors = 0

    for offset, old, new, desc in PATCHES:
        if offset + 4 > len(data):
            print(f"   !! offset {hex(offset)} beyond end of file — skipping")
            errors += 1
            continue

        actual = struct.unpack_from("<I", data, offset)[0]

        if actual == new:
            print(f"   = {hex(offset)}  already patched")
            print(f"        {desc}")
            already += 1
            continue

        if actual != old:
            print(f"   !! {hex(offset)}  unexpected bytes: "
                  f"got {actual:#010x}, expected {old:#010x}")
            print(f"        {desc}")
            print(f"        (wrong build, already partially patched, or corrupted)")
            errors += 1
            continue

        if not dry_run:
            struct.pack_into("<I", data, offset, new)

        verb = "(would patch)" if dry_run else "patched"
        print(f"   + {hex(offset)}  {old:#010x} -> {new:#010x}  [{verb}]")
        print(f"        {desc}")
        applied += 1

    return applied, already, errors


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Remove cursor colour inversion from Chrono Trigger NX libchrono.so (v9)"
    )
    ap.add_argument("input", help="path to libchrono.so")
    ap.add_argument("-o", "--output", help="output path (default: <stem>.noInvert.so)")
    ap.add_argument("--in-place", action="store_true",
                    help="patch the file in place (writes a .bak backup first)")
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would change without writing anything")
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        data = bytearray(f.read())

    verify_elf(data)
    print(f"File: {args.input}  ({len(data):,} bytes)")
    print(f"Patches to apply: {len(PATCHES)}\n")

    applied, already, errors = apply_patches(data, args.dry_run)

    print(f"\nSummary: {applied} patched, {already} already-patched, {errors} errors")

    if args.dry_run:
        print("(dry-run: no file written)")
        return

    if errors:
        print("Errors encountered — file not written to avoid corruption.")
        sys.exit(1)

    if applied == 0:
        print("Nothing new to write.")
        return

    if args.in_place:
        bak = args.input + ".bak"
        with open(bak, "wb") as f:
            with open(args.input, "rb") as src:
                f.write(src.read())
        out_path = args.input
        print(f"Backup written: {bak}")
    else:
        if args.output:
            out_path = args.output
        elif "." in args.input:
            base, ext = args.input.rsplit(".", 1)
            out_path = f"{base}.noInvert.{ext}"
        else:
            out_path = args.input + ".noInvert"

    with open(out_path, "wb") as f:
        f.write(data)
    print(f"Written: {out_path}")
    print(
        "\nNext steps:\n"
        "  1. Place this .so in lib/arm64-v8a/ inside the APK.\n"
        "  2. zipalign the APK.\n"
        "  3. Re-sign with apksigner.\n"
        "  4. Install."
    )


if __name__ == "__main__":
    main()
