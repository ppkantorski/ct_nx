#!/usr/bin/env python3
"""
remove_bilinear.py  --  Force GL_NEAREST (point sampling) in Chrono Trigger NX's
libchrono.so, removing all bilinear texture filtering.

The bilinear filtering is NOT in the .fsh/.vsh shader assets. It is GL texture
state set in native code (Cocos2d-x). This tool patches two functions:

  cocos2d::Texture2D::initWithMipmaps          (every texture passes through here)
  cocos2d::Texture2D::setAntiAliasTexParameters (fonts / labels / render targets)

In initWithMipmaps the engine loads GL_NEAREST and uses `cinc` to add 1 -> GL_LINEAR
whenever the per-texture antialias flag is set (the default). We turn those `cinc`
into plain `mov`, so the MIN/MAG filters are always nearest regardless of the flag.
In setAntiAliasTexParameters we flip the GL_LINEAR / GL_LINEAR_MIPMAP_NEAREST
constants to their GL_NEAREST equivalents.

Patches are matched by AArch64 instruction encoding *inside the located function
ranges only*, so they will not corrupt unrelated code. Same file size, valid ELF.

Usage:
    python3 remove_bilinear.py libchrono.so
    python3 remove_bilinear.py libchrono.so -o libchrono_nofilter.so
    python3 remove_bilinear.py libchrono.so --in-place        # edits file, keeps .bak
    python3 remove_bilinear.py libchrono.so --dry-run         # report only

After patching: drop the .so back into lib/arm64-v8a/ in the APK, then
zipalign + re-sign (apksigner) before installing.
"""

import argparse
import struct
import sys

# ----------------------------------------------------------------------------
# Patch tables. Keys are the function's mangled symbol name; values are lists of
# (old_word, new_word, human_description). old/new are little-endian uint32
# AArch64 instruction encodings.
# ----------------------------------------------------------------------------
PATCHES = {
    # cocos2d::Texture2D::initWithMipmaps(...)
    "_ZN7cocos2d9Texture2D15initWithMipmaps": [
        (0x1A980708, 0x2A1803E8, "MIN(mipmap) cinc w8,w24,ne -> mov w8,w24   (always NEAREST)"),
        (0x1A9C0789, 0x2A1C03E9, "MIN        cinc w9,w28,ne -> mov w9,w28   (always NEAREST)"),
        (0x1A9C0782, 0x2A1C03E2, "MAG        cinc w2,w28,ne -> mov w2,w28   (always NEAREST)"),
        (0x5284C02A, 0x5284C00A, "VolatileMgr restore  GL_LINEAR -> GL_NEAREST"),
        (0x5284E029, 0x5284E009, "VolatileMgr restore  GL_LINEAR_MIPMAP -> GL_NEAREST_MIPMAP"),
    ],
    # cocos2d::Texture2D::setAntiAliasTexParameters()
    "_ZN7cocos2d9Texture2D25setAntiAliasTexParameters": [
        (0x5284E035, 0x5284E015, "MIN  GL_LINEAR_MIPMAP_NEAREST -> GL_NEAREST_MIPMAP_NEAREST"),
        (0x5284C036, 0x5284C016, "MIN  GL_LINEAR -> GL_NEAREST"),
        (0x5284C022, 0x5284C002, "MAG  GL_LINEAR -> GL_NEAREST"),
    ],
}


def die(msg):
    print("ERROR: " + msg, file=sys.stderr)
    sys.exit(1)


def parse_elf(data):
    """Minimal ELF64-LE parser. Returns (text_addr, text_off, text_size, symbols)
    where symbols maps mangled_name_prefix-matchable -> (vaddr, size)."""
    if data[:4] != b"\x7fELF":
        die("not an ELF file")
    if data[4] != 2 or data[5] != 1:
        die("expected 64-bit little-endian ELF (AArch64 .so)")

    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x3A)[0]
    e_shnum = struct.unpack_from("<H", data, 0x3C)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x3E)[0]

    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size, sh_link, sh_info, \
            sh_addralign, sh_entsize = struct.unpack_from("<IIQQQQIIQQ", data, off)
        sections.append(dict(name=sh_name, type=sh_type, addr=sh_addr, offset=sh_offset,
                             size=sh_size, link=sh_link, entsize=sh_entsize))

    shstr = sections[e_shstrndx]

    def secname(s):
        start = shstr["offset"] + s["name"]
        end = data.index(b"\x00", start)
        return data[start:end].decode("latin-1")

    text = None
    dynsym = None
    for s in sections:
        nm = secname(s)
        if nm == ".text":
            text = s
        elif nm == ".dynsym":
            dynsym = s
    if text is None:
        die(".text section not found")
    if dynsym is None:
        die(".dynsym section not found (stripped of dynamic symbols?)")

    strtab = sections[dynsym["link"]]
    symbols = {}
    n = dynsym["size"] // 24  # Elf64_Sym is 24 bytes
    for i in range(n):
        off = dynsym["offset"] + i * 24
        st_name, st_info, st_other, st_shndx, st_value, st_size = \
            struct.unpack_from("<IBBHQQ", data, off)
        if st_value == 0 or st_size == 0:
            continue
        nstart = strtab["offset"] + st_name
        nend = data.index(b"\x00", nstart)
        name = data[nstart:nend].decode("latin-1")
        symbols[name] = (st_value, st_size)

    return text, symbols


def find_func(symbols, prefix):
    """Find a symbol whose name starts with prefix (handles trailing mangling)."""
    for name, (vaddr, size) in symbols.items():
        if name.startswith(prefix):
            return name, vaddr, size
    return None, None, None


def vaddr_to_off(vaddr, text):
    return vaddr - text["addr"] + text["offset"]


def main():
    ap = argparse.ArgumentParser(description="Remove bilinear filtering from libchrono.so")
    ap.add_argument("input", help="path to libchrono.so")
    ap.add_argument("-o", "--output", help="output path (default: <input>.nofilter.so)")
    ap.add_argument("--in-place", action="store_true",
                    help="patch the input file directly (writes a .bak backup first)")
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would change without writing")
    args = ap.parse_args()

    with open(args.input, "rb") as f:
        data = bytearray(f.read())

    text, symbols = parse_elf(data)
    text_lo = text["offset"]
    text_hi = text["offset"] + text["size"]

    total_applied = 0
    total_already = 0
    missing = []

    for prefix, patch_list in PATCHES.items():
        name, vaddr, size = find_func(symbols, prefix)
        if name is None:
            missing.append(prefix)
            print("!! function not found: %s*  (different build?)" % prefix)
            continue
        f_off = vaddr_to_off(vaddr, text)
        f_end = f_off + size
        if f_off < text_lo or f_end > text_hi:
            die("function %s lies outside .text" % name)
        print("\n[%s  @0x%x  size=%d]" % (name, vaddr, size))

        for old, new, desc in patch_list:
            old_b = struct.pack("<I", old)
            new_b = struct.pack("<I", new)
            region = data[f_off:f_end]
            occ_old = []
            occ_new = []
            i = 0
            while True:
                j = region.find(old_b, i)
                if j < 0:
                    break
                occ_old.append(f_off + j)
                i = j + 4
            i = 0
            while True:
                j = region.find(new_b, i)
                if j < 0:
                    break
                occ_new.append(f_off + j)
                i = j + 4

            if not occ_old and occ_new:
                print("   = already patched : %s" % desc)
                total_already += 1
                continue
            if not occ_old:
                print("   ?? NOT FOUND      : %s  (0x%08x)" % (desc, old))
                continue
            if len(occ_old) > 1:
                print("   !! %d matches for 0x%08x in this function; patching all: %s"
                      % (len(occ_old), old, desc))
            for off in occ_old:
                if not args.dry_run:
                    data[off:off + 4] = new_b
                print("   + 0x%06x  %08x -> %08x  %s" % (off, old, new, desc))
                total_applied += 1

    print("\nSummary: %d patched, %d already-patched, %d functions missing"
          % (total_applied, total_already, len(missing)))

    if args.dry_run:
        print("(dry-run: no file written)")
        return
    if total_applied == 0:
        print("Nothing to write (already patched or nothing matched).")
        return

    if args.in_place:
        bak = args.input + ".bak"
        with open(bak, "wb") as f:
            f.write(open(args.input, "rb").read())
        out = args.input
        print("backup written: %s" % bak)
    else:
        out = args.output or (args.input.rsplit(".", 1)[0] + ".nofilter.so"
                              if "." in args.input else args.input + ".nofilter")

    with open(out, "wb") as f:
        f.write(data)
    print("written: %s" % out)
    print("\nNext: place this .so in lib/arm64-v8a/ inside the APK, then "
          "zipalign + re-sign (apksigner) before installing.")


if __name__ == "__main__":
    main()
