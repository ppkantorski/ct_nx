#!/usr/bin/env python3
"""
remove_menu_buttons.py (v1) -- Chrono Trigger Android libchrono.so

Companion to remove_mobile_ui.py. That script hides the field/world-map/
title-screen touch overlay buttons. This one targets the menu SYSTEM screens
(Equip, Item, Tech, Config, SaveLoad, Formation, Suspend, and the top-level
Main Menu bar) which all share one reusable header class, nsMenu::StatusBar.

--------------------------------------------------------------------------
BACKGROUND
--------------------------------------------------------------------------
Every one of those screens builds its header by calling one of two
overloads of nsMenu::StatusBar::init(...):

  * 4-arg overload  (title, bool, backCallback, closeCallback)
      -- used by MenuNodeEquip/Item/Tech/Config/SaveLoad/Formation
  * 5-arg overload  (..., isTopBar)
      -- same body, plus an isTopBar-style branch; also present in the
         binary and patched defensively even though the verified callers
         above all resolve to the 4-arg overload

Confirmed via static analysis (capstone disassembly + relocation-table
symbol resolution) that BOTH overloads share this structure:

    ldr  x8, [x20, #0x320]      ; backCallback's std::function target ptr
    cbz  x8, <no_back_button>   ; null  -> skip creating the back button
    ...                         ; non-null -> createBackButton(...), then
                                ;   subtract the button's width from the
                                ;   bar's usable width
    <no_back_button>:
    mov  w8, #0x43f00000        ; float 480.0 -- literal full design width
    fmov s0, w8                 ; used as "no button" bar width instead

    ldr  x8, [x20, #0x350]      ; closeCallback's std::function target ptr
    cbz  x8, <no_close_button>  ; null  -> skip creating the close button
    ...                         ; non-null -> createCloseButton(...), same
                                ;   width-subtraction pattern
    <no_close_button>:
    ...                          ; falls through, reusing whatever width
                                  ; was computed above

In other words the "no button, full-width bar" code path already exists
and is exercised by the engine itself (e.g. wherever callers pass an empty
std::function for either callback) -- it is not something we are adding.
We are simply making the ALWAYS-CREATE gate at 0x320/0x350 always take the
"skip" branch, for every StatusBar, regardless of what the caller passed.

This is deliberately NOT a "hide the button after creating it" patch (that
would leave a dead gap where the button used to be). Forcing the existing
skip-branch means the bar's width math is computed exactly as it already is
for the no-button case, so the Main Menu / location / time / money bars
resize to fill the space properly instead of leaving a hole.

Net effect once patched: no menu screen ever creates a back or close
button node. B-button / controller "cancel" input is untouched -- these
button NODES are purely visual touch targets wired to their own
std::function callbacks; nothing else in the input pipeline depends on
their existence (StatusBar::setInteractive and the destructor both
null-check the pointers before touching them, and the constructor
zero-initializes both fields), so the game keeps working when they're
never allocated.

--------------------------------------------------------------------------
FIX -- nsMenu::StatusBar::init (both overloads), force the skip branches
--------------------------------------------------------------------------
4-arg overload (_ZN6nsMenu9StatusBar4init...SH_):
    +0x160  CBZ X8, +0x40   ->  B +0x40   (back button: always skip)
    +0x258  CBZ X8, +0xF0   ->  B +0xF0   (close button: always skip)

5-arg overload (_ZN6nsMenu9StatusBar4init...SH_b):
    +0x17C  CBZ X8, +0x40   ->  B +0x40   (back button: always skip)
    +0x2D8  CBZ X8, +0x154  ->  B +0x154  (close button: always skip)

Usage:
    python3 remove_menu_buttons.py libchrono.so
    python3 remove_menu_buttons.py libchrono.so -o libchrono_noui.so
    python3 remove_menu_buttons.py libchrono.so --in-place
    python3 remove_menu_buttons.py libchrono.so --dry-run

Typical combined workflow with the mobile-UI patch:
    python3 remove_mobile_ui.py   libchrono.so -o libchrono_step1.so
    python3 remove_menu_buttons.py libchrono_step1.so -o libchrono_noui.so
"""

import argparse
import struct
import sys

# ---------------------------------------------------------------------------
# Shared instruction helpers
# ---------------------------------------------------------------------------

def make_b(cur_vaddr: int, target_vaddr: int) -> int:
    """Encode an unconditional B from cur_vaddr to target_vaddr."""
    delta = target_vaddr - cur_vaddr
    assert delta % 4 == 0, "branch target must be 4-byte aligned"
    assert -(1 << 27) <= delta < (1 << 27), "branch out of +-128MB range"
    imm26 = (delta // 4) & 0x3FFFFFF
    return 0x14000000 | imm26


# ---------------------------------------------------------------------------
# Fix: symbol+offset patches. Each tuple is
#   (symbol, func_offset, old_word, branch_target_offset, desc)
# branch_target_offset is relative to the SAME function's start, matching
# how the original CBZ's target sat inside the same function.
# ---------------------------------------------------------------------------
INIT4_SYM = (
    "_ZN6nsMenu9StatusBar4initERKNSt6__ndk112basic_stringIcNS1_11char_"
    "traitsIcEENS1_9allocatorIcEEEEbRKNS1_8functionIFvPN7cocos2d3RefEEEESH_"
)
INIT5_SYM = INIT4_SYM + "b"

SYMBOL_PATCHES = [
    (
        INIT4_SYM, 0x160,
        0xB4000208,   # CBZ X8, +0x40 (skip back-button creation if null)
        0x1A0,        # absolute func-offset of the target (0x160 + 0x40)
        "StatusBar::init(4-arg) -- back button: force the existing "
        "'no button' skip branch (was conditional on backCallback == null)",
    ),
    (
        INIT4_SYM, 0x258,
        0xB4000788,   # CBZ X8, +0xF0
        0x348,        # 0x258 + 0xF0
        "StatusBar::init(4-arg) -- close button: force the existing "
        "'no button' skip branch (was conditional on closeCallback == null)",
    ),
    (
        INIT5_SYM, 0x17C,
        0xB4000208,   # CBZ X8, +0x40
        0x1BC,        # 0x17C + 0x40
        "StatusBar::init(5-arg) -- back button: force the existing "
        "'no button' skip branch",
    ),
    (
        INIT5_SYM, 0x2D8,
        0xB4000AA8,   # CBZ X8, +0x154
        0x42C,        # 0x2D8 + 0x154
        "StatusBar::init(5-arg) -- close button: force the existing "
        "'no button' skip branch",
    ),
]

# Reverts: nothing to revert yet (v1), kept for symmetry / future-proofing.
REVERTS = []


def die(msg: str) -> None:
    print("ERROR: " + msg, file=sys.stderr)
    sys.exit(1)


def parse_elf(data: bytes):
    if data[:4] != b"\x7fELF":
        die("not an ELF file")
    if data[4] != 2 or data[5] != 1:
        die("expected 64-bit little-endian ELF (AArch64 .so)")

    e_shoff     = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x3A)[0]
    e_shnum     = struct.unpack_from("<H", data, 0x3C)[0]
    e_shstrndx  = struct.unpack_from("<H", data, 0x3E)[0]

    sections = []
    for i in range(e_shnum):
        base = e_shoff + i * e_shentsize
        (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size,
         sh_link, sh_info, sh_addralign, sh_entsize) = struct.unpack_from(
            "<IIQQQQIIQQ", data, base)
        sections.append(dict(
            name=sh_name, type=sh_type, addr=sh_addr,
            offset=sh_offset, size=sh_size, link=sh_link,
        ))

    shstr = sections[e_shstrndx]

    def secname(s):
        start = shstr["offset"] + s["name"]
        end   = data.index(b"\x00", start)
        return data[start:end].decode("latin-1")

    text   = None
    dynsym = None
    for s in sections:
        nm = secname(s)
        if nm == ".text":     text   = s
        elif nm == ".dynsym": dynsym = s

    if text   is None: die(".text section not found")
    if dynsym is None: die(".dynsym section not found (stripped of dynamic symbols?)")

    strtab  = sections[dynsym["link"]]
    symbols = {}
    count   = dynsym["size"] // 24
    for i in range(count):
        base = dynsym["offset"] + i * 24
        (st_name, st_info, st_other, st_shndx,
         st_value, st_size) = struct.unpack_from("<IBBHQQ", data, base)
        if st_value == 0 or st_size == 0:
            continue
        nstart = strtab["offset"] + st_name
        nend   = data.index(b"\x00", nstart)
        name   = data[nstart:nend].decode("latin-1")
        symbols[name] = (st_value, st_size)

    return text, symbols


def vaddr_to_fileoff(vaddr: int, text: dict) -> int:
    return vaddr - text["addr"] + text["offset"]


def run_symbol_table(data, text, symbols, table, label):
    applied, already, skipped = 0, 0, []
    for (sym_name, func_offset, old_word, target_func_offset, desc) in table:
        if sym_name not in symbols:
            print(f"!! symbol not found: {sym_name}  (different build?)")
            skipped.append(sym_name)
            continue

        vaddr, func_size = symbols[sym_name]
        func_file_off    = vaddr_to_fileoff(vaddr, text)
        func_end         = func_file_off + func_size
        patch_off        = func_file_off + func_offset

        if patch_off + 4 > func_end:
            die(f"patch offset +{func_offset:#x} is beyond end of {sym_name}")
        if target_func_offset > func_size:
            die(f"branch target +{target_func_offset:#x} is beyond end of {sym_name}")

        target_vaddr = vaddr + target_func_offset
        patch_vaddr  = vaddr + func_offset
        new_word     = make_b(patch_vaddr, target_vaddr)

        current = struct.unpack_from("<I", data, patch_off)[0]

        if current == new_word:
            print(f"   = already {label} : {desc}")
            already += 1
            continue
        if current != old_word:
            print(
                f"   !! MISMATCH at 0x{patch_off:08X} (vaddr 0x{patch_vaddr:08X}): "
                f"expected {old_word:08X}, found {current:08X} -- skipping\n"
                f"      ({desc})"
            )
            skipped.append(sym_name)
            continue

        struct.pack_into("<I", data, patch_off, new_word)
        print(f"   + 0x{patch_off:08X}  {old_word:08X} -> {new_word:08X}  {desc}")
        applied += 1

    return applied, already, skipped


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Remove back/close buttons from menu status bars in "
                    "libchrono.so, letting the bar reuse its own full-width "
                    "no-button layout (v1)")
    ap.add_argument("input", help="path to libchrono.so")
    ap.add_argument("-o", "--output", help="output path (default: <input>.noui.so)")
    ap.add_argument("--in-place", action="store_true",
                     help="patch input file directly (writes a .bak first)")
    ap.add_argument("--dry-run", action="store_true",
                     help="report what would change without writing")
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        data = bytearray(f.read())

    text, symbols = parse_elf(bytes(data))

    total_applied, total_already, total_skipped = 0, 0, []

    print("-- fix: nsMenu::StatusBar::init -- always skip back/close button "
          "creation, reuse existing full-width layout path --")
    a, al, sk = run_symbol_table(data, text, symbols, SYMBOL_PATCHES, "patched")
    total_applied += a; total_already += al; total_skipped += sk

    print(f"\nSummary: {total_applied} changed, {total_already} unchanged, "
          f"{len(total_skipped)} skipped/missing")

    if args.dry_run:
        print("(dry-run: no file written)")
        return
    if total_applied == 0 and not total_already:
        print("Nothing to write.")
        return
    if total_applied == 0:
        print("Nothing new to write (already patched).")
        return

    if args.in_place:
        bak = args.input + ".bak"
        with open(bak, "wb") as f_bak, open(args.input, "rb") as f_src:
            f_bak.write(f_src.read())
        out = args.input
        print(f"backup -> {bak}")
    else:
        if args.output:
            out = args.output
        elif "." in args.input:
            out = args.input.rsplit(".", 1)[0] + ".noui.so"
        else:
            out = args.input + ".noui"

    with open(out, "wb") as f_out:
        f_out.write(data)
    print(f"written: {out}")
    print("\nNext: drop this .so into lib/arm64-v8a/ in the APK, then "
          "zipalign + re-sign (apksigner) before installing. If you're also "
          "using remove_mobile_ui.py, run that one first (or combine both "
          "patch tables) -- both only touch disjoint code, so order doesn't "
          "matter functionally, but chain the -o files so you don't clobber "
          "either patch.")


if __name__ == "__main__":
    main()
