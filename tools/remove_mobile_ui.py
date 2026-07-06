#!/usr/bin/env python3
"""
remove_mobile_ui.py (v8) -- Chrono Trigger Android libchrono.so

Copyright (C) 2026 ppkantorski <https://github.com/ppkantorski>
This software may be modified and distributed under the terms of the MIT
license. See the LICENSE file for details.

Hides the on-screen touch-overlay buttons (field menu button, world-map
menu/map/warp buttons, AND all five right-side title-screen icon buttons)
without breaking controller input. Replaces v1-v4.

--------------------------------------------------------------------------
FIX 1 -- FieldMenu::setMenuAvailable(bool)  [field screen menu button]
--------------------------------------------------------------------------
This function does three things with its bool argument:
  +20  MOV  W19,W1            save the bool (only used for this fn's return value)
  +24  STRB WZR,[X0,#0x330]   clear an internal "available" flag on the FieldMenu
  +28..+40                    button_node->setVisible(W1)   <- shows/hides button
  +44..+56  VirtualController::getInstance(); ...->setActive(...)
                                                  <- THIS arms the controller Start key

The fix changes ONLY the instruction at +24 to zero W1 before setVisible:

    +24  STRB WZR,[X0,#0x330]   ->   MOV W1,WZR

Effect: setVisible(false) always, VirtualController::setActive() untouched,
Start key still works. (See v3 notes for why RET-stubbing the whole function
broke Start.)

--------------------------------------------------------------------------
FIX 2 -- world-map MENU / MAP / WARP buttons
--------------------------------------------------------------------------
Three buttons built by a shared helper at 0x782C7C (no exported symbol),
called exactly 3 times from WorldMenu::init. The helper ends by calling
button->setPosition(x,y) via vtable[0xC8]. There is no setVisible anywhere
in this path -- they are created visible.

4 patches inside the helper repurpose the setPosition call into
setVisible(false) (vtable[0x170]), identical approach to Fix 1:

    0x782DC0  LDR X8,[X8,#0xC8]   ->  LDR X8,[X8,#0x170]  (path A vtable slot)
    0x782DC8  FADD S1,S1,S3       ->  MOV W1, WZR          (path A bool = false)
    0x782DF8  FMOV S1,#16.0       ->  MOV W1, WZR          (path B bool = false)
    0x782DFC  LDR X8,[X8,#0xC8]   ->  LDR X8,[X8,#0x170]  (path B vtable slot)

--------------------------------------------------------------------------
FIX 3 -- right-side title icons (cloud/upload, login, achievements, privacy,
          license): HIDE ALL + make NON-INTERACTIVE
--------------------------------------------------------------------------
setupMenuNodes builds the right-side icon buttons in a loop; each iteration
makes a CustomButton + nsMenu::FocusableState, inserts the state into the input
map keyed by an integer tag, and at the loop tail runs a range-check hide block
(0x77FE88) that hides only tags 7,8. v5 widened it to 6..10 but the cloud/upload
button's tag was outside that window, so it stayed visible. The bottom menu is a
SEPARATE earlier loop that never reaches 0x77FE88, so we make this block fire for
EVERY icon -- hiding the cloud button whatever its tag, and only icons.

3a. Make the hide unconditional:   0x77FE94  B.HI <loop>  ->  NOP

3b. setVisible(false) alone leaves the FocusableState navigable/clickable, so we
    repurpose the hide sequence into a call to a register-safe code-cave helper
    that does node->setVisible(false) AND State->setSkip(true)
    (nsStateMachine::State::setSkip removes it from cursor navigation):

        0x77FE98  LDR X8,[X26]       ->  MOV X0,X26
        0x77FE9C  LDR X8,[X8,#0x10]  ->  BL  0xAA6270
        0x77FEA0  MOV X0,X26         ->  B   #0x77FAC8

    Cave @ 0xAA6270 (zero padding; distinct from cursor-invert cave 0xAA56A0):
        stp x19,x30,[sp,#-0x10]! ; mov x19,x0 ; ldr x8,[x0] ; ldr x8,[x8,#0x10]
        blr x8 ; ldr x8,[x0] ; ldr x8,[x8,#0x170] ; mov w1,wzr ; blr x8
        mov x0,x19 ; mov w1,#1 ; bl 0xB4B200 ; ldp x19,x30,[sp],#0x10 ; ret

3c. RET-stub UpdateIconVisible (0x780CC8) so it can never re-show/re-enable
    icons 7/8. We do NOT rewrite its internals (that crashed v6/v7 on a stale
    x21); RET-stubbing is the safe half of v5's fix and we keep it.

        0x780CC8  STP X29,X30,[SP,#-0x30]!  ->  RET

--------------------------------------------------------------------------
WHAT CHANGED FROM v4
--------------------------------------------------------------------------
  - Fix 3 now hides EVERY right-side icon (incl. the cloud/upload button that
    v5 missed) and makes them non-navigable/non-clickable via State::setSkip,
    through a register-safe code cave. UpdateIconVisible stays RET-stubbed and
    its internals are NOT rewritten (that is what crashed v6/v7).

Usage:
    python3 remove_mobile_ui.py libchrono.so
    python3 remove_mobile_ui.py libchrono.so -o libchrono_noui.so
    python3 remove_mobile_ui.py libchrono.so --in-place      # edits file, keeps .bak
    python3 remove_mobile_ui.py libchrono.so --dry-run       # report only
    python3 remove_mobile_ui.py libchrono.so --no-revert     # skip the revert pass
"""

import argparse
import struct
import sys

# ---------------------------------------------------------------------------
# Shared instruction constants
# ---------------------------------------------------------------------------
RET = 0xD65F03C0          # RET (return via X30)
MOV_W1_WZR = 0x2A1F03E1    # MOV W1, WZR  (sets the bool arg to `false`)


def ldr_set_imm12(word: int, new_byte_offset: int) -> int:
    """Return `word` (an LDR Xt,[Xn,#imm], 64-bit unsigned-offset encoding)
    with its imm12 field rewritten so the offset becomes new_byte_offset.
    Only the imm12 bits [21:10] change; Rn/Rt/opcode bits are untouched."""
    assert new_byte_offset % 8 == 0, "LDR 64-bit offsets must be 8-byte aligned"
    new_imm12 = new_byte_offset // 8
    assert 0 <= new_imm12 <= 0xFFF
    return (word & ~(0xFFF << 10)) | (new_imm12 << 10)


# vtable[0xC8] (setPosition) -> vtable[0x170] (setVisible), same encoding shape
# at both world-helper sites (Rn=Rt=x8 in both), so one computed word covers both.
_LDR_OLD = 0xF9406508          # LDR X8,[X8,#0xC8]
_LDR_NEW = ldr_set_imm12(_LDR_OLD, 0x170)   # LDR X8,[X8,#0x170] -> 0xF940B908


# ---------------------------------------------------------------------------
# Fix 1: symbol+offset patch (FieldMenu::setMenuAvailable)
# ---------------------------------------------------------------------------
SYMBOL_PATCHES = [
    (
        "_ZN9FieldMenu16setMenuAvailableEb",
        24,
        0x390CC01F,     # STRB WZR,[X0,#0x330]  (clear "available" flag)
        MOV_W1_WZR,     # MOV  W1,WZR            (force setVisible(false))
        "FieldMenu::setMenuAvailable +24 -- field button stays hidden, "
        "VirtualStick::setActive (Start) and the return value are untouched",
    ),
]

# ---------------------------------------------------------------------------
# Fix 2: raw-address patches (shared world-button-builder helper @ 0x782C7C,
# no exported symbol). Each tuple is (vaddr, old_word, new_word, desc).
# ---------------------------------------------------------------------------
RAW_VA_PATCHES = [
    (
        0x782DC0,
        _LDR_OLD,
        _LDR_NEW,
        "world-button helper +0x144 -- vtable slot setPosition(0xC8) -> "
        "setVisible(0x170)  [path A: variants 0 & 1]",
    ),
    (
        0x782DC8,
        0x1E232821,     # FADD S1,S1,S3  (dead position math once we're hiding)
        MOV_W1_WZR,     # MOV  W1,WZR    (bool arg = false)
        "world-button helper +0x14C -- bool arg = false  [path A: variants 0 & 1]",
    ),
    (
        0x782DF8,
        0x1E261001,     # FMOV S1,#16.0  (dead position math once we're hiding)
        MOV_W1_WZR,     # MOV  W1,WZR    (bool arg = false)
        "world-button helper +0x17C -- bool arg = false  [path B: variants 2 & 3]",
    ),
    (
        0x782DFC,
        _LDR_OLD,
        _LDR_NEW,
        "world-button helper +0x180 -- vtable slot setPosition(0xC8) -> "
        "setVisible(0x170)  [path B: variants 2 & 3]",
    ),
]

# A few fixed landmarks used only as an extra build-match sanity check before
# trusting the raw-address patches above (does NOT get modified).
RAW_VA_GUARDS = [
    (0x782C7C, 0xD10183FF, "world-button helper prologue (sub sp,sp,#0x60)"),
]

# ---------------------------------------------------------------------------
# Fix 3: right-side title-screen icon buttons (tags 6..10 in setupMenuNodes).
# All five are raw-address patches; none have exported symbols.
#
# 3a. Widen the at-creation setVisible(false) range in TitleMenuMode::setupMenuNodes
#     from {tag7, tag8} to {tag6, tag7, tag8, tag9, tag10}.
#
# 3b. RET-stub TitleMenuMode::UpdateIconVisible so it can never un-hide them.
#     (It is only called via TitleMenuMode::update which is a direct tail-call
#     to UpdateIconVisible -- no other callers exist.)
# ---------------------------------------------------------------------------
ICON_VA_PATCHES = [
    # 3a. hide EVERY right-side icon (range-check hide block -> unconditional)
    (0x77FE94, 0x54FFE1A8, 0xD503201F,
     "setupMenuNodes +0x143C -- B.HI <loop> -> NOP (hide every icon, not just tags 7,8)"),
    # 3b. repurpose hide sequence -> cave (setVisible(false)+setSkip(true))
    (0x77FE98, 0xF9400348, 0xAA1A03E0,
     "setupMenuNodes +0x1440 -- LDR X8,[X26] -> MOV X0,X26 (State* arg)"),
    (0x77FE9C, 0xF9400908, 0x940C98F5,
     "setupMenuNodes +0x1444 -- LDR X8,[X8,#0x10] -> BL 0xAA6270 (setVisible+setSkip cave)"),
    (0x77FEA0, 0xAA1A03E0, 0x17FFFF0A,
     "setupMenuNodes +0x1448 -- MOV X0,X26 -> B #0x77FAC8 (loop continue)"),
    # 3b (cont). cave helper @ 0xAA6270 (96B zero padding; != cursor cave 0xAA56A0)
    (0xAA6270, 0x00000000, 0xA9BF7BF3,
     "cave @0xAA6270: stp x19,x30,[sp,#-0x10]!"),
    (0xAA6274, 0x00000000, 0xAA0003F3,
     "cave @0xAA6274: mov x19,x0"),
    (0xAA6278, 0x00000000, 0xF9400008,
     "cave @0xAA6278: ldr x8,[x0]"),
    (0xAA627C, 0x00000000, 0xF9400908,
     "cave @0xAA627C: ldr x8,[x8,#0x10] getNode"),
    (0xAA6280, 0x00000000, 0xD63F0100,
     "cave @0xAA6280: blr x8"),
    (0xAA6284, 0x00000000, 0xF9400008,
     "cave @0xAA6284: ldr x8,[x0]"),
    (0xAA6288, 0x00000000, 0xF940B908,
     "cave @0xAA6288: ldr x8,[x8,#0x170] setVisible"),
    (0xAA628C, 0x00000000, 0x2A1F03E1,
     "cave @0xAA628C: mov w1,wzr"),
    (0xAA6290, 0x00000000, 0xD63F0100,
     "cave @0xAA6290: blr x8"),
    (0xAA6294, 0x00000000, 0xAA1303E0,
     "cave @0xAA6294: mov x0,x19"),
    (0xAA6298, 0x00000000, 0x52800021,
     "cave @0xAA6298: mov w1,#1"),
    (0xAA629C, 0x00000000, 0x940293D9,
     "cave @0xAA629C: bl 0xB4B200 setSkip(true)"),
    (0xAA62A0, 0x00000000, 0xA8C17BF3,
     "cave @0xAA62A0: ldp x19,x30,[sp],#0x10"),
    (0xAA62A4, 0x00000000, 0xD65F03C0,
     "cave @0xAA62A4: ret"),
    # 3c. RET-stub UpdateIconVisible (no internal rewrite -> no crash)
    (0x780CC8, 0xA9BD7BFD, 0xD65F03C0,
     "UpdateIconVisible +0x0 -- RET-stub (icons stay hidden + skipped)"),
]
# Guard bytes confirming this is the correct build before applying Fix 3 patches.
# Chosen immediately adjacent to (but not overlapping) the patched words.
ICON_VA_GUARDS = [
    (0x77FE88, 0x51001E88, "setupMenuNodes: SUB W8,W20,#7 (build id; not patched in v8)"),
    (0x77FE8C, 0xF90027F3, "setupMenuNodes: STR X19,[SP,#0x48] (load-bearing; not patched)"),
    (0x77FEA4, 0xD63F0100, "setupMenuNodes: BLR X8 (orig setVisible call, now dead; not patched)"),
    (0x780CD0, 0xA9024FF4, "UpdateIconVisible +0x8: STP X20,X19 (function identity)"),
]
# ---------------------------------------------------------------------------
# Patches to actively revert if present, so this script produces a clean,
# correct result no matter which earlier version was last applied.
# ---------------------------------------------------------------------------
REVERTS = [
    (
        "_ZN9FieldMenu16setMenuAvailableEb",
        0,
        RET,
        0xA9BE7BFD,
        "FieldMenu::setMenuAvailable -- reverting old RET-stub (broke Start)",
    ),
    (
        "_ZN9WorldMenu4initEv",
        232,
        0x2A1F03E1,
        0x52800021,
        "WorldMenu::init -- reverting old incorrect W1 change (shifted button, didn't hide it)",
    ),
    (
        "_ZN9WorldMenu20setVisibleWarpButtonEb",
        0,
        RET,
        0xF9419000,
        "WorldMenu::setVisibleWarpButton -- reverting v3 RET-stub (never hid anything)",
    ),
    (
        "_ZN10WorldScene9mapButtonEv",
        0,
        RET,
        0xF9419008,
        "WorldScene::mapButton -- reverting v3 RET-stub (never hid anything)",
    ),
]



# Raw-address reverts: undo v5's Fix-3a range widen if present, so v8 applies
# cleanly whether the input is pristine or already has v5's icon patch.
RAW_VA_REVERTS = [
    (0x77FE88, 0x51001A88, 0x51001E88,
     "revert v5 Fix3a: SUB W8,W20,#6 -> #7 (original)"),
    (0x77FE90, 0x7100111F, 0x7100051F,
     "revert v5 Fix3a: CMP W8,#4 -> #1 (original)"),
]


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
    for (sym_name, func_offset, old_word, new_word, desc) in table:
        if sym_name not in symbols:
            print(f"!! symbol not found: {sym_name}  (different build?)")
            skipped.append(sym_name)
            continue

        vaddr, func_size = symbols[sym_name]
        func_file_off    = vaddr_to_fileoff(vaddr, text)
        func_end         = func_file_off + func_size
        patch_off        = func_file_off + func_offset

        if patch_off + 4 > func_end:
            die(f"patch offset +{func_offset} is beyond end of {sym_name}")

        current = struct.unpack_from("<I", data, patch_off)[0]

        if current == new_word:
            print(f"   = already {label} : {desc}")
            already += 1
            continue
        if current != old_word:
            print(
                f"   !! MISMATCH at 0x{patch_off:08X}: "
                f"expected {old_word:08X}, found {current:08X} -- skipping\n"
                f"      ({desc})"
            )
            skipped.append(sym_name)
            continue

        struct.pack_into("<I", data, patch_off, new_word)
        print(f"   + 0x{patch_off:08X}  {old_word:08X} -> {new_word:08X}  {desc}")
        applied += 1

    return applied, already, skipped


def run_raw_va_table(data, text, table, label):
    applied, already, skipped = 0, 0, []
    for (vaddr, old_word, new_word, desc) in table:
        off = vaddr_to_fileoff(vaddr, text)
        current = struct.unpack_from("<I", data, off)[0]

        if current == new_word:
            print(f"   = already {label} : {desc}")
            already += 1
            continue
        if current != old_word:
            print(
                f"   !! MISMATCH at 0x{vaddr:08X} (file 0x{off:08X}): "
                f"expected {old_word:08X}, found {current:08X} -- skipping\n"
                f"      ({desc})"
            )
            skipped.append(vaddr)
            continue

        struct.pack_into("<I", data, off, new_word)
        print(f"   + 0x{vaddr:08X}  {old_word:08X} -> {new_word:08X}  {desc}")
        applied += 1

    return applied, already, skipped


def check_guards(data, text, guards):
    ok = True
    for vaddr, expect, desc in guards:
        off = vaddr_to_fileoff(vaddr, text)
        cur = struct.unpack_from("<I", data, off)[0]
        if cur != expect:
            print(f"   !! build-match guard failed at 0x{vaddr:08X}: "
                  f"expected {expect:08X}, found {cur:08X}  ({desc})")
            print("      This binary doesn't match the build the world-button "
                  "patches were derived from -- skipping those 4 patches.")
            ok = False
    return ok


def main() -> None:
    ap = argparse.ArgumentParser(
        description="Hide mobile UI overlay buttons in libchrono.so (v8)")
    ap.add_argument("input", help="path to libchrono.so")
    ap.add_argument("-o", "--output", help="output path (default: <input>.noui.so)")
    ap.add_argument("--in-place", action="store_true",
                     help="patch input file directly (writes a .bak first)")
    ap.add_argument("--dry-run", action="store_true",
                     help="report what would change without writing")
    ap.add_argument("--no-revert", action="store_true",
                     help="skip reverting any earlier (v1-v3) patches first")
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        data = bytearray(f.read())

    text, symbols = parse_elf(bytes(data))

    total_applied, total_already, total_skipped = 0, 0, []

    if not args.no_revert:
        print("-- reverting any earlier (v1-v3) symbol patches, if present --")
        a, al, sk = run_symbol_table(data, text, symbols, REVERTS, "reverted")
        total_applied += a; total_already += al; total_skipped += sk
        print()
        print("-- reverting v5 Fix-3a range widen, if present --")
        a, al, sk = run_raw_va_table(data, text, RAW_VA_REVERTS, "reverted")
        total_applied += a; total_already += al; total_skipped += sk
        print()

    print("-- fix 1: FieldMenu::setMenuAvailable (field menu button) --")
    a, al, sk = run_symbol_table(data, text, symbols, SYMBOL_PATCHES, "patched")
    total_applied += a; total_already += al; total_skipped += sk

    print("\n-- fix 2: world-map menu/map/warp buttons --")
    if check_guards(data, text, RAW_VA_GUARDS):
        a, al, sk = run_raw_va_table(data, text, RAW_VA_PATCHES, "patched")
        total_applied += a; total_already += al; total_skipped += sk
    else:
        total_skipped.append("world-button-helper")

    print("\n-- fix 3: right-side title-screen icon buttons (cloud/login/achievements/privacy/license) --")
    if check_guards(data, text, ICON_VA_GUARDS):
        a, al, sk = run_raw_va_table(data, text, ICON_VA_PATCHES, "patched")
        total_applied += a; total_already += al; total_skipped += sk
    else:
        total_skipped.append("icon-buttons")

    print(f"\nSummary: {total_applied} changed, {total_already} unchanged, "
          f"{len(total_skipped)} skipped/missing")

    if args.dry_run:
        print("(dry-run: no file written)")
        return
    if total_applied == 0:
        print("Nothing to write.")
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
          "zipalign + re-sign (apksigner) before installing.")


if __name__ == "__main__":
    main()
