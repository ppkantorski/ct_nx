#!/usr/bin/env python3
"""
generate_text_ctp.py -- builds switch_text_patches.ctp from YOUR OWN resources.bin

WHAT THIS DOES
--------------
ct_nx's Switch port loads Chrono Trigger's field-message text verbatim from
the original Android build's resources.bin, in every language. Those files
were written for a touchscreen (e.g. "Touch blue, yellow, then red to
activate the switch"), which doesn't make sense with a Joy-Con in your
hands -- and this isn't English-only: the same touch/tap/color-button
wording exists in all 9 shipped localizations (de, en, es, fr, it, ja, ko,
zh-Hans, zh-Hant).

This script reads your own, legally-extracted resources.bin, finds every
language's copy of the affected lines, and rewrites them to reference the
actual action a Switch player presses -- using the same <BTN_CONF>/
<BTN_DASH>/<BTN_MENU>/<BTN_WARP>/<BTN_L>/<BTN_R> tags the game's own
renderer already understands (see ct_nx's controller_glyphs patch, which
makes those tags draw as A/B/Y/X/L/R). It then packages every changed file,
across every language, into a single switch_text_patches.ctp, in the exact
format ct_nx's mod loader already expects (drop it in your mods/ folder).

WHY THIS SCRIPT EXISTS INSTEAD OF A SHARED .ctp FILE
-----------------------------------------------------
A pre-built .ctp containing this text would bundle Square Enix's localized
strings, in 9 languages, which isn't something we can redistribute. Running
this script against your own resources.bin means SE's text never leaves
your machine and never passes through us -- only the (small) substitution
logic here does, and that's ours to share.

Two kinds of edits, per language:
  - CAT_A: lines copied byte-for-byte from the `_pad` twin file that ALREADY
    ships inside your resources.bin for that language (Square Enix's own
    official DS/controller wording -- present in the game's assets today,
    just never loaded by the engine). This is entirely mechanical: verified
    to exist and differ at the same 32 message IDs in all 9 languages. None
    of this text is embedded in this script; it's extracted live from your
    own file at runtime.
  - CAT_B: lines with no `_pad` counterpart in ANY language, so the
    replacement is original wording (not Square Enix's), written per
    language using the terminology each language's own official `_pad`
    text already establishes elsewhere (press/hold/directional-button
    phrasing, etc.) for consistency. This is the only text actually stored
    in this script.

LOCALIZATION NOTES (read before wide release)
-----------------------------------------------
A few of the CAT_B lines don't have real localization in every language to
begin with -- some shipped files silently fall back to raw Japanese or
English source text rather than a real translation (a pre-existing gap in
Square Enix's own build, unrelated to this fix). Where that's the case, the
replacement text here provides a real translation in that language rather
than perpetuating the fallback. de/es/fr/it translations are native-
fluency-level. ja/ko/zh-Hans/zh-Hant are accurate and natural, but as with
any translation work, a native speaker's pass before wide release is good
practice.

Two deliberate exclusions:
  - ko/msg/tutorial.txt: every single line in this file (all 13 entries,
    not just the ones touch/color affects) is the literal stub "16" in the
    shipped game -- not missing translation, a deliberate non-content stub.
    Left untouched.
  - de/msg/menu.txt's name-entry prompt: already reads "use the keyboard to
    enter a name" and never mentions touch. Already correct, left untouched.

Every line is located either by its message ID (a short technical key, not
expressive text) or, for the one file with no IDs (menu.txt), by a CRC32
fingerprint of the original line -- never by embedding Square Enix's
original sentence as a search target. If a line's content doesn't match
the expected fingerprint (e.g. a future SE patch changed the wording),
that single edit is skipped with a warning rather than guessed at.

USAGE
-----
    python3 generate_text_ctp.py /path/to/resources.bin
    python3 generate_text_ctp.py /path/to/resources.bin -o switch_text_patches.ctp
    python3 generate_text_ctp.py /path/to/resources.bin --lang en      # English only
    python3 generate_text_ctp.py /path/to/resources.bin --lang en,ja   # subset

Then drop the resulting .ctp into your ct_nx mods/ folder (see config.ini's
mods_dir).

RESOURCES.BIN FORMAT
---------------------
The ARC1 reader below is a clean-room Python port of the documented format
(XOR-keystreamed header/index/entries + DEFLATE-compressed entries), cross-
referenced against ChronoMod (github.com/jimzrt/ChronoMod, MIT licensed)
and against ct_nx's own modpack.c. It is READ-ONLY: this script never
writes to resources.bin itself. The output .ctp is a plain zip of the
corrected files at their resources.bin-relative paths -- ct_nx's modpack
loader does the actual re-encoding into resources.bin at runtime.
"""

import argparse
import binascii
import struct
import sys
import zipfile
import zlib

# ---------------------------------------------------------------------------
# ARC1 (resources.bin) read-only reader
# ---------------------------------------------------------------------------

def _xor_keystream(offset, data):
    """Position-dependent LCG XOR keystream used throughout resources.bin."""
    tmp = (0x19000000 + offset) & 0xFFFFFFFF
    out = bytearray(len(data))
    for i, b in enumerate(data):
        tmp = (tmp * 0x41C64E6D + 0x3039) & 0xFFFFFFFF
        out[i] = b ^ ((tmp >> 24) & 0xFF)
    return bytes(out)


def _inflate(payload, expected_size):
    """Decompress a DEFLATE stream. resources.bin always uses gzip framing
    (ChronoMod's gzip_uncompress() calls inflateInit2(.., 16+MAX_WBITS) for
    both the index table and every entry) -- gzip is tried first, with
    zlib/raw as harmless fallbacks in case of a future format variant."""
    last_err = None
    for wbits in (zlib.MAX_WBITS | 16, zlib.MAX_WBITS, -zlib.MAX_WBITS):
        try:
            return zlib.decompress(payload, wbits)
        except zlib.error as e:
            last_err = e
            continue
    raise ValueError(f"could not inflate compressed block: {last_err}")


class ArcEntry:
    __slots__ = ("path", "offset", "length")

    def __init__(self, path, offset, length):
        self.path = path
        self.offset = offset
        self.length = length


class ResourcesBin:
    """Read-only accessor for a resources.bin (ARC1) file."""

    def __init__(self, path):
        with open(path, "rb") as f:
            self._data = f.read()
        self._parse_header()
        self._parse_index()

    def _parse_header(self):
        raw = self._data[0:16]
        if len(raw) < 16:
            raise ValueError("file too short to be a resources.bin (ARC1) archive")
        hdr = _xor_keystream(0, raw)
        magic = hdr[0:4]
        if magic != b"ARC1":
            raise ValueError(
                f"bad magic {magic!r} (expected b'ARC1') -- this doesn't look "
                f"like a Chrono Trigger resources.bin"
            )
        self.index_offset = struct.unpack_from("<I", hdr, 8)[0]
        self.index_length = struct.unpack_from("<I", hdr, 12)[0]

    def _parse_index(self):
        raw = self._data[self.index_offset: self.index_offset + self.index_length]
        if len(raw) != self.index_length:
            raise ValueError("index table truncated -- unexpected file layout")
        dec = _xor_keystream(self.index_offset, raw)
        inflated_size = struct.unpack_from(">I", dec, 0)[0]
        idx = _inflate(dec[4:], inflated_size)

        entry_count = struct.unpack_from("<I", idx, 0)[0]

        # path_name_offset is an ABSOLUTE index into the inflated index
        # buffer (matching ChronoMod's resourcebin.cpp: the C++ reads it as
        # &uncompressed_buffer[path_name_offset] directly, with no separate
        # base added) -- NOT relative to a separately-computed names region.
        self._by_path = {}
        for i in range(entry_count):
            off = 4 + i * 12
            name_off, entry_off, entry_len = struct.unpack_from("<III", idx, off)
            start = name_off
            end = idx.index(b"\x00", start)
            path = idx[start:end].decode("utf-8", "replace")
            self._by_path[path] = ArcEntry(path, entry_off, entry_len)

    def has(self, path):
        return path in self._by_path

    def extract(self, path):
        e = self._by_path.get(path)
        if e is None:
            raise KeyError(path)
        raw = self._data[e.offset: e.offset + e.length]
        if len(raw) != e.length:
            raise ValueError(f"truncated entry blob for {path}")
        dec = _xor_keystream(e.offset, raw)
        inflated_size = struct.unpack_from(">I", dec, 0)[0]
        return _inflate(dec[4:], inflated_size)


# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# Edit catalogue
# ---------------------------------------------------------------------------

# ============================================================
# CAT_A: per-language, ID-based. Pulled live from each language's
# own _pad twin file at runtime -- no text embedded here, just
# CRC32 fingerprints of the CURRENT (unmodified) content, used to
# locate + verify each line before replacing it.
# ============================================================
CAT_A = {
    "de": {
        "cmes0.txt": {"pad_file": "cmes0_pad.txt", "ids": {
            "FLD_CMES0_094": 0x647c0f68,
            "FLD_CMES0_115": 0xf767204d,
            "FLD_CMES0_217": 0xfc9c1ddb,
            "FLD_CMES0_220": 0xaa3158e3,
            "FLD_CMES0_221": 0xa811a92a,
            "FLD_CMES0_222": 0x4a9ae3e7,
            "FLD_CMES0_224": 0xa1a1b403,
            "FLD_CMES0_225": 0x731635e,
        }},
        "cmes1.txt": {"pad_file": "cmes1_pad.txt", "ids": {
            "FLD_CMES1_014": 0xd49b8f5b,
            "FLD_CMES1_108": 0x32efb339,
            "FLD_CMES1_109": 0xf69b79b7,
        }},
        "cmes2.txt": {"pad_file": "cmes2_pad.txt", "ids": {
            "FLD_CMES2_094": 0x31a7a900,
            "FLD_CMES2_166": 0x5f698b68,
        }},
        "mesi0.txt": {"pad_file": "mesi0_pad.txt", "ids": {
            "FLD_MESI0_003": 0xae7e27a1,
            "FLD_MESI0_004": 0x1a9d2ad,
            "FLD_MESI0_036": 0xaa976576,
            "FLD_MESI0_041": 0x762e5b15,
        }},
        "mesk0.txt": {"pad_file": "mesk0_pad.txt", "ids": {
            "FLD_MESK0_025": 0xc91ed52,
            "FLD_MESK0_031": 0xf1a3152,
        }},
        "mesk1.txt": {"pad_file": "mesk1_pad.txt", "ids": {
            "FLD_MESK1_076": 0xab7ec6f,
        }},
        "mesk3.txt": {"pad_file": "mesk3_pad.txt", "ids": {
            "FLD_MESK3_062": 0x5f602e29,
            "FLD_MESK3_063": 0xeb5a630c,
            "FLD_MESK3_064": 0x3ac60382,
        }},
        "mess0.txt": {"pad_file": "mess0_pad.txt", "ids": {
            "FLD_MESS0_063": 0xe95348c,
        }},
        "mest3.txt": {"pad_file": "mest3_pad.txt", "ids": {
            "FLD_MEST3_155": 0x8a44525c,
            "FLD_MEST3_156": 0xbd63d4d3,
            "FLD_MEST3_157": 0x7445733,
            "FLD_MEST3_158": 0xd1013497,
            "FLD_MEST3_159": 0x3a6bfc96,
            "FLD_MEST3_160": 0x33dccc2,
            "FLD_MEST3_161": 0x74cad304,
            "FLD_MEST3_162": 0xd5faa6ca,
        }},
    },
    "en": {
        "cmes0.txt": {"pad_file": "cmes0_pad.txt", "ids": {
            "FLD_CMES0_094": 0xc9c98140,
            "FLD_CMES0_115": 0x1b1242a3,
            "FLD_CMES0_217": 0xba3f42b3,
            "FLD_CMES0_220": 0xab26d49,
            "FLD_CMES0_221": 0xe3f0c37e,
            "FLD_CMES0_222": 0x71da84b,
            "FLD_CMES0_224": 0xfe7d8a35,
            "FLD_CMES0_225": 0x3f7a678c,
        }},
        "cmes1.txt": {"pad_file": "cmes1_pad.txt", "ids": {
            "FLD_CMES1_014": 0x7234e21a,
            "FLD_CMES1_108": 0xb3b5bab,
            "FLD_CMES1_109": 0x605e5a25,
        }},
        "cmes2.txt": {"pad_file": "cmes2_pad.txt", "ids": {
            "FLD_CMES2_094": 0x40d7d078,
            "FLD_CMES2_166": 0x5c761e64,
        }},
        "mesi0.txt": {"pad_file": "mesi0_pad.txt", "ids": {
            "FLD_MESI0_003": 0xf84cc6e,
            "FLD_MESI0_004": 0x18fe301c,
            "FLD_MESI0_036": 0x7e837944,
            "FLD_MESI0_041": 0x7f65a76a,
        }},
        "mesk0.txt": {"pad_file": "mesk0_pad.txt", "ids": {
            "FLD_MESK0_025": 0x3b42394d,
            "FLD_MESK0_031": 0x6e04b10a,
        }},
        "mesk1.txt": {"pad_file": "mesk1_pad.txt", "ids": {
            "FLD_MESK1_076": 0x619cce55,
        }},
        "mesk3.txt": {"pad_file": "mesk3_pad.txt", "ids": {
            "FLD_MESK3_062": 0xeb257392,
            "FLD_MESK3_063": 0x945df7b0,
            "FLD_MESK3_064": 0xcca7005f,
        }},
        "mess0.txt": {"pad_file": "mess0_pad.txt", "ids": {
            "FLD_MESS0_063": 0x802943ad,
        }},
        "mest3.txt": {"pad_file": "mest3_pad.txt", "ids": {
            "FLD_MEST3_155": 0x67aecf85,
            "FLD_MEST3_156": 0x23997db,
            "FLD_MEST3_157": 0xfa19e748,
            "FLD_MEST3_158": 0xf04033e,
            "FLD_MEST3_159": 0xaaef23fb,
            "FLD_MEST3_160": 0x91661898,
            "FLD_MEST3_161": 0x57e6fa86,
            "FLD_MEST3_162": 0x135e20b9,
        }},
    },
    "es": {
        "cmes0.txt": {"pad_file": "cmes0_pad.txt", "ids": {
            "FLD_CMES0_094": 0x483ff83d,
            "FLD_CMES0_115": 0xa0fcb27b,
            "FLD_CMES0_217": 0xd9e4bb78,
            "FLD_CMES0_220": 0xe5d6b543,
            "FLD_CMES0_221": 0x261d5191,
            "FLD_CMES0_222": 0x79ae69ee,
            "FLD_CMES0_224": 0xf4a58b9c,
            "FLD_CMES0_225": 0x4b99b349,
        }},
        "cmes1.txt": {"pad_file": "cmes1_pad.txt", "ids": {
            "FLD_CMES1_014": 0x35fa19e4,
            "FLD_CMES1_108": 0xc356cc4f,
            "FLD_CMES1_109": 0x283d8fe4,
        }},
        "cmes2.txt": {"pad_file": "cmes2_pad.txt", "ids": {
            "FLD_CMES2_094": 0xf0145bf9,
            "FLD_CMES2_166": 0x5be0a4ba,
        }},
        "mesi0.txt": {"pad_file": "mesi0_pad.txt", "ids": {
            "FLD_MESI0_003": 0xb5883355,
            "FLD_MESI0_004": 0x833efc92,
            "FLD_MESI0_036": 0x6d01a349,
            "FLD_MESI0_041": 0x22488378,
        }},
        "mesk0.txt": {"pad_file": "mesk0_pad.txt", "ids": {
            "FLD_MESK0_025": 0xee4e7aad,
            "FLD_MESK0_031": 0x96e936ca,
        }},
        "mesk1.txt": {"pad_file": "mesk1_pad.txt", "ids": {
            "FLD_MESK1_076": 0xc4f14729,
        }},
        "mesk3.txt": {"pad_file": "mesk3_pad.txt", "ids": {
            "FLD_MESK3_062": 0xc128ef74,
            "FLD_MESK3_063": 0x8cd4451e,
            "FLD_MESK3_064": 0xd3983d7,
        }},
        "mess0.txt": {"pad_file": "mess0_pad.txt", "ids": {
            "FLD_MESS0_063": 0x42505b2c,
        }},
        "mest3.txt": {"pad_file": "mest3_pad.txt", "ids": {
            "FLD_MEST3_155": 0x620d280c,
            "FLD_MEST3_156": 0x6c3ffe68,
            "FLD_MEST3_157": 0xc760bff8,
            "FLD_MEST3_158": 0x31bdfe4f,
            "FLD_MEST3_159": 0x305a0502,
            "FLD_MEST3_160": 0x270f50de,
            "FLD_MEST3_161": 0xaeff02f1,
            "FLD_MEST3_162": 0x211b3c11,
        }},
    },
    "fr": {
        "cmes0.txt": {"pad_file": "cmes0_pad.txt", "ids": {
            "FLD_CMES0_094": 0x232d3f8d,
            "FLD_CMES0_115": 0xcd0546f9,
            "FLD_CMES0_217": 0x94348f8,
            "FLD_CMES0_220": 0x5da953be,
            "FLD_CMES0_221": 0xed517093,
            "FLD_CMES0_222": 0xc44440b3,
            "FLD_CMES0_224": 0xfcdf6baa,
            "FLD_CMES0_225": 0xf2fad874,
        }},
        "cmes1.txt": {"pad_file": "cmes1_pad.txt", "ids": {
            "FLD_CMES1_014": 0x50d56ffe,
            "FLD_CMES1_108": 0x53128b7c,
            "FLD_CMES1_109": 0x6eb5ae37,
        }},
        "cmes2.txt": {"pad_file": "cmes2_pad.txt", "ids": {
            "FLD_CMES2_094": 0x777eef52,
            "FLD_CMES2_166": 0x57e6fd6b,
        }},
        "mesi0.txt": {"pad_file": "mesi0_pad.txt", "ids": {
            "FLD_MESI0_003": 0x4a9675d2,
            "FLD_MESI0_004": 0x412db107,
            "FLD_MESI0_036": 0xf27cc2c9,
            "FLD_MESI0_041": 0x1e5215f8,
        }},
        "mesk0.txt": {"pad_file": "mesk0_pad.txt", "ids": {
            "FLD_MESK0_025": 0x741e4b43,
            "FLD_MESK0_031": 0x351c08ce,
        }},
        "mesk1.txt": {"pad_file": "mesk1_pad.txt", "ids": {
            "FLD_MESK1_076": 0xf744d5c6,
        }},
        "mesk3.txt": {"pad_file": "mesk3_pad.txt", "ids": {
            "FLD_MESK3_062": 0xa19067c6,
            "FLD_MESK3_063": 0x42429226,
            "FLD_MESK3_064": 0xede6e7d0,
        }},
        "mess0.txt": {"pad_file": "mess0_pad.txt", "ids": {
            "FLD_MESS0_063": 0xe2bbd02d,
        }},
        "mest3.txt": {"pad_file": "mest3_pad.txt", "ids": {
            "FLD_MEST3_155": 0xc54c783f,
            "FLD_MEST3_156": 0xe46472cd,
            "FLD_MEST3_157": 0xc7ee47e3,
            "FLD_MEST3_158": 0xac56c51b,
            "FLD_MEST3_159": 0xfc1a905c,
            "FLD_MEST3_160": 0xa682ebd9,
            "FLD_MEST3_161": 0xf6bcbf25,
            "FLD_MEST3_162": 0x8e5b2b2,
        }},
    },
    "it": {
        "cmes0.txt": {"pad_file": "cmes0_pad.txt", "ids": {
            "FLD_CMES0_094": 0x851e58bf,
            "FLD_CMES0_115": 0x6e9c4abf,
            "FLD_CMES0_217": 0xab50a533,
            "FLD_CMES0_220": 0xa1f03241,
            "FLD_CMES0_221": 0x951e004b,
            "FLD_CMES0_222": 0xfa3c18c9,
            "FLD_CMES0_224": 0x95dd893e,
            "FLD_CMES0_225": 0xac315717,
        }},
        "cmes1.txt": {"pad_file": "cmes1_pad.txt", "ids": {
            "FLD_CMES1_014": 0x21148f16,
            "FLD_CMES1_108": 0x8aa34c7f,
            "FLD_CMES1_109": 0x59b74945,
        }},
        "cmes2.txt": {"pad_file": "cmes2_pad.txt", "ids": {
            "FLD_CMES2_094": 0xd3c8d5ca,
            "FLD_CMES2_166": 0x56779bbe,
        }},
        "mesi0.txt": {"pad_file": "mesi0_pad.txt", "ids": {
            "FLD_MESI0_003": 0x2531301b,
            "FLD_MESI0_004": 0x4241531b,
            "FLD_MESI0_036": 0xd8b8af83,
            "FLD_MESI0_041": 0x241b0d2,
        }},
        "mesk0.txt": {"pad_file": "mesk0_pad.txt", "ids": {
            "FLD_MESK0_025": 0xa39d4555,
            "FLD_MESK0_031": 0x625c5188,
        }},
        "mesk1.txt": {"pad_file": "mesk1_pad.txt", "ids": {
            "FLD_MESK1_076": 0xe6d9bffc,
        }},
        "mesk3.txt": {"pad_file": "mesk3_pad.txt", "ids": {
            "FLD_MESK3_062": 0x12eefd2d,
            "FLD_MESK3_063": 0xcd123ffa,
            "FLD_MESK3_064": 0x47df6272,
        }},
        "mess0.txt": {"pad_file": "mess0_pad.txt", "ids": {
            "FLD_MESS0_063": 0x376d628e,
        }},
        "mest3.txt": {"pad_file": "mest3_pad.txt", "ids": {
            "FLD_MEST3_155": 0x7872a1b1,
            "FLD_MEST3_156": 0xb44ba0f5,
            "FLD_MEST3_157": 0x3a3769da,
            "FLD_MEST3_158": 0x30d695ea,
            "FLD_MEST3_159": 0x86f3ced6,
            "FLD_MEST3_160": 0x1d5c3264,
            "FLD_MEST3_161": 0x8170a180,
            "FLD_MEST3_162": 0x3553e104,
        }},
    },
    "ja": {
        "cmes0.txt": {"pad_file": "cmes0_pad.txt", "ids": {
            "FLD_CMES0_094": 0x15cf10c6,
            "FLD_CMES0_115": 0xa8d34278,
            "FLD_CMES0_217": 0xc518556e,
            "FLD_CMES0_220": 0x5c0f6cd4,
            "FLD_CMES0_221": 0x4e173d3a,
            "FLD_CMES0_222": 0xcbeaa144,
            "FLD_CMES0_224": 0x247d6d4,
            "FLD_CMES0_225": 0x195f11df,
        }},
        "cmes1.txt": {"pad_file": "cmes1_pad.txt", "ids": {
            "FLD_CMES1_014": 0xa62485d5,
            "FLD_CMES1_108": 0xe77b6e28,
            "FLD_CMES1_109": 0xb9ad626f,
        }},
        "cmes2.txt": {"pad_file": "cmes2_pad.txt", "ids": {
            "FLD_CMES2_094": 0xb1839ef1,
            "FLD_CMES2_166": 0xa774958a,
        }},
        "mesi0.txt": {"pad_file": "mesi0_pad.txt", "ids": {
            "FLD_MESI0_003": 0xead3060a,
            "FLD_MESI0_004": 0x66d01a93,
            "FLD_MESI0_036": 0x2bc98f3b,
            "FLD_MESI0_041": 0x8805ab,
        }},
        "mesk0.txt": {"pad_file": "mesk0_pad.txt", "ids": {
            "FLD_MESK0_025": 0xf2b94b6b,
            "FLD_MESK0_031": 0x8aacd2ce,
        }},
        "mesk1.txt": {"pad_file": "mesk1_pad.txt", "ids": {
            "FLD_MESK1_076": 0x190ffdf7,
        }},
        "mesk3.txt": {"pad_file": "mesk3_pad.txt", "ids": {
            "FLD_MESK3_062": 0xf62863d9,
            "FLD_MESK3_063": 0x9292b76f,
            "FLD_MESK3_064": 0xa9713196,
        }},
        "mess0.txt": {"pad_file": "mess0_pad.txt", "ids": {
            "FLD_MESS0_063": 0xca90b519,
        }},
        "mest3.txt": {"pad_file": "mest3_pad.txt", "ids": {
            "FLD_MEST3_155": 0xb7934df6,
            "FLD_MEST3_156": 0x788661ca,
            "FLD_MEST3_157": 0x6ddbb838,
            "FLD_MEST3_158": 0x8eb27f77,
            "FLD_MEST3_159": 0x32082591,
            "FLD_MEST3_160": 0x42c4b6ac,
            "FLD_MEST3_161": 0xbe79093b,
            "FLD_MEST3_162": 0xe00a1ee5,
        }},
    },
    "ko": {
        "cmes0.txt": {"pad_file": "cmes0_pad.txt", "ids": {
            "FLD_CMES0_094": 0xb5767e5b,
            "FLD_CMES0_115": 0xfb71c391,
            "FLD_CMES0_217": 0x32136433,
            "FLD_CMES0_220": 0x137bd391,
            "FLD_CMES0_221": 0x54aed8da,
            "FLD_CMES0_222": 0x29bfface,
            "FLD_CMES0_224": 0x237769fb,
            "FLD_CMES0_225": 0x4c189079,
        }},
        "cmes1.txt": {"pad_file": "cmes1_pad.txt", "ids": {
            "FLD_CMES1_014": 0x1509588c,
            "FLD_CMES1_108": 0xa96a40ff,
            "FLD_CMES1_109": 0xde92883b,
        }},
        "cmes2.txt": {"pad_file": "cmes2_pad.txt", "ids": {
            "FLD_CMES2_094": 0x38467a67,
            "FLD_CMES2_166": 0x1364e6e5,
        }},
        "mesi0.txt": {"pad_file": "mesi0_pad.txt", "ids": {
            "FLD_MESI0_003": 0x2a30745e,
            "FLD_MESI0_004": 0x3a35dfa2,
            "FLD_MESI0_036": 0x357a6cc1,
            "FLD_MESI0_041": 0x536ce523,
        }},
        "mesk0.txt": {"pad_file": "mesk0_pad.txt", "ids": {
            "FLD_MESK0_025": 0xde34661e,
            "FLD_MESK0_031": 0xdf437ea5,
        }},
        "mesk1.txt": {"pad_file": "mesk1_pad.txt", "ids": {
            "FLD_MESK1_076": 0xfb4a1583,
        }},
        "mesk3.txt": {"pad_file": "mesk3_pad.txt", "ids": {
            "FLD_MESK3_062": 0x7ad8418d,
            "FLD_MESK3_063": 0x329f4f99,
            "FLD_MESK3_064": 0xf71e3381,
        }},
        "mess0.txt": {"pad_file": "mess0_pad.txt", "ids": {
            "FLD_MESS0_063": 0xcec0a8c5,
        }},
        "mest3.txt": {"pad_file": "mest3_pad.txt", "ids": {
            "FLD_MEST3_155": 0x3d019ff5,
            "FLD_MEST3_156": 0x8371f6d,
            "FLD_MEST3_157": 0x34044351,
            "FLD_MEST3_158": 0xc0bd9476,
            "FLD_MEST3_159": 0x198b3b5e,
            "FLD_MEST3_160": 0x35f00296,
            "FLD_MEST3_161": 0x468a5e06,
            "FLD_MEST3_162": 0x6230c3b5,
        }},
    },
    "zh-Hans": {
        "cmes0.txt": {"pad_file": "cmes0_pad.txt", "ids": {
            "FLD_CMES0_094": 0x8e6db9af,
            "FLD_CMES0_115": 0xb7cceef9,
            "FLD_CMES0_217": 0xc9bc1ac0,
            "FLD_CMES0_220": 0xa980d0b6,
            "FLD_CMES0_221": 0xbe11b864,
            "FLD_CMES0_222": 0x3fd6719,
            "FLD_CMES0_224": 0x7e94fb6e,
            "FLD_CMES0_225": 0x9b1d4a18,
        }},
        "cmes1.txt": {"pad_file": "cmes1_pad.txt", "ids": {
            "FLD_CMES1_014": 0x16f18282,
            "FLD_CMES1_108": 0x565cf48c,
            "FLD_CMES1_109": 0xbfd28b82,
        }},
        "cmes2.txt": {"pad_file": "cmes2_pad.txt", "ids": {
            "FLD_CMES2_094": 0x310ad722,
            "FLD_CMES2_166": 0x7995776f,
        }},
        "mesi0.txt": {"pad_file": "mesi0_pad.txt", "ids": {
            "FLD_MESI0_003": 0x27dd4738,
            "FLD_MESI0_004": 0x30e303dd,
            "FLD_MESI0_036": 0xe4aae815,
            "FLD_MESI0_041": 0xe6e9dfc6,
        }},
        "mesk0.txt": {"pad_file": "mesk0_pad.txt", "ids": {
            "FLD_MESK0_025": 0x82a3cec4,
            "FLD_MESK0_031": 0x570bbde9,
        }},
        "mesk1.txt": {"pad_file": "mesk1_pad.txt", "ids": {
            "FLD_MESK1_076": 0x34f37da2,
        }},
        "mesk3.txt": {"pad_file": "mesk3_pad.txt", "ids": {
            "FLD_MESK3_062": 0xf5028de9,
            "FLD_MESK3_063": 0xb816a91d,
            "FLD_MESK3_064": 0x7d55a9cf,
        }},
        "mess0.txt": {"pad_file": "mess0_pad.txt", "ids": {
            "FLD_MESS0_063": 0x128a76b5,
        }},
        "mest3.txt": {"pad_file": "mest3_pad.txt", "ids": {
            "FLD_MEST3_155": 0x992dc49f,
            "FLD_MEST3_156": 0x17fedadf,
            "FLD_MEST3_157": 0x20b7dd6a,
            "FLD_MEST3_158": 0x6d8af7f1,
            "FLD_MEST3_159": 0x8076c073,
            "FLD_MEST3_160": 0x44922589,
            "FLD_MEST3_161": 0x48d3b03,
            "FLD_MEST3_162": 0x13f4924d,
        }},
    },
    "zh-Hant": {
        "cmes0.txt": {"pad_file": "cmes0_pad.txt", "ids": {
            "FLD_CMES0_094": 0x2893ccb4,
            "FLD_CMES0_115": 0x8ac78bcc,
            "FLD_CMES0_217": 0x52452e3a,
            "FLD_CMES0_220": 0xe29c327d,
            "FLD_CMES0_221": 0xa752b4f7,
            "FLD_CMES0_222": 0x9c28ae2a,
            "FLD_CMES0_224": 0x581a6bd3,
            "FLD_CMES0_225": 0x8518f86c,
        }},
        "cmes1.txt": {"pad_file": "cmes1_pad.txt", "ids": {
            "FLD_CMES1_014": 0x839828ce,
            "FLD_CMES1_108": 0xd8acc30,
            "FLD_CMES1_109": 0x49289667,
        }},
        "cmes2.txt": {"pad_file": "cmes2_pad.txt", "ids": {
            "FLD_CMES2_094": 0xf1737af0,
            "FLD_CMES2_166": 0xff2880bf,
        }},
        "mesi0.txt": {"pad_file": "mesi0_pad.txt", "ids": {
            "FLD_MESI0_003": 0x2e829e4a,
            "FLD_MESI0_004": 0xda135a80,
            "FLD_MESI0_036": 0x49c355c6,
            "FLD_MESI0_041": 0x40ff1a8e,
        }},
        "mesk0.txt": {"pad_file": "mesk0_pad.txt", "ids": {
            "FLD_MESK0_025": 0xc29d993f,
            "FLD_MESK0_031": 0x33f1e0bc,
        }},
        "mesk1.txt": {"pad_file": "mesk1_pad.txt", "ids": {
            "FLD_MESK1_076": 0x4a6dfa1,
        }},
        "mesk3.txt": {"pad_file": "mesk3_pad.txt", "ids": {
            "FLD_MESK3_062": 0x2770b659,
            "FLD_MESK3_063": 0x5773a229,
            "FLD_MESK3_064": 0xb48d2f39,
        }},
        "mess0.txt": {"pad_file": "mess0_pad.txt", "ids": {
            "FLD_MESS0_063": 0x7031c583,
        }},
        "mest3.txt": {"pad_file": "mest3_pad.txt", "ids": {
            "FLD_MEST3_155": 0xbe57d0a0,
            "FLD_MEST3_156": 0x23cfa46c,
            "FLD_MEST3_157": 0xa56b75a7,
            "FLD_MEST3_158": 0x4af0e3ce,
            "FLD_MEST3_159": 0x17013219,
            "FLD_MEST3_160": 0x3fbec0da,
            "FLD_MEST3_161": 0xd6bdfe19,
            "FLD_MEST3_162": 0x4ad47f0,
        }},
    },
}
# ============================================================
# CAT_B: per-language, original-authored replacement text (not
# Square Enix's -- these lines have no official _pad source in
# ANY language). orig_crc anchors to the CURRENT shipped content
# in that language so the mismatch guard still protects against
# unexpected builds.
#
# Exclusions (deliberate, documented):
#  - ko/tutorial.txt: entire file is a content stub ("16" on
#    every line) in the shipped game -- not edited.
#  - de/menu.txt line 128: already says 'use the keyboard',
#    never mentions touch -- not edited.
# ============================================================
CAT_B = {
    "de": {
        "start.txt": [
            {"kind": "id", "id": "MSG_NEWGAME_10", "orig_crc": 0x41f894e5, "new": 'MSG_NEWGAME_10,Bitte drücken'},
        ],
        "cmes0.txt": [
            {"kind": "id", "id": "FLD_CMES0_218", "orig_crc": 0xe4b7922e, "new": 'FLD_CMES0_218,Wusstest du schon, dass du dich mit den Richtungstasten\\bewegen kannst?<PAGE>\\Drücke eine Richtung, um in diese Richtung zu rennen.\\Halte <BTN_DASH> gedrückt, während du eine Richtung drückst,\\um stattdessen zu gehen.<PAGE>\\Du kannst auch <BTN_CONF> drücken, um mit jemandem\\zu sprechen oder ein Objekt zu untersuchen.'},
        ],
        "tutorial.txt": [
            {"kind": "id", "id": "MSG_TUTO_HELP_02", "orig_crc": 0x34a85631, "new": 'MSG_TUTO_HELP_02,Du kannst die Knöpfe benutzen, um\\durch das Menü zu navigieren.'},
            {"kind": "id", "id": "MSG_TUTO_HELP_07", "orig_crc": 0x4fa5a7fc, "new": 'MSG_TUTO_HELP_07,Du kannst ein Symbol auch verschieben, indem du es\\auswählst und die <BTN_CONF>-Taste drückst.'},
            {"kind": "id", "id": "MSG_TUTO_HELP_08", "orig_crc": 0xe890d519, "new": 'MSG_TUTO_HELP_08,Um ein Symbol auszublenden, wähle es aus und drücke\\die <BTN_WARP>-Taste, oder wähle das Feld\\mit der Aufschrift „Ein/Aus“.'},
            {"kind": "id", "id": "MSG_TUTO_HELP_09", "orig_crc": 0xa27e24f6, "new": 'MSG_TUTO_HELP_09,Um alle Symbole auf ihre Standardpositionen\\zurückzusetzen, drücke SELECT oder wähle das Feld\\mit der Aufschrift „Standard“.'},
            {"kind": "id", "id": "MSG_TUTO_HELP_11", "orig_crc": 0x4a1ad4fc, "new": 'MSG_TUTO_HELP_11,Wenn du mit der Anpassung fertig bist, drücke\\START oder wähle „Einstellung abschließen“.'},
            {"kind": "id", "id": "MSG_TUTO_HELP_13", "orig_crc": 0xc827383b, "new": 'MSG_TUTO_HELP_13,Press A Button'},
        ],
    },
    "en": {
        "start.txt": [
            {"kind": "id", "id": "MSG_NEWGAME_10", "orig_crc": 0x7cfb7fe9, "new": 'MSG_NEWGAME_10,Press to Start'},
        ],
        "cmes0.txt": [
            {"kind": "id", "id": "FLD_CMES0_218", "orig_crc": 0xc57198d0, "new": 'FLD_CMES0_218,Did you know you can move around with the\\directional buttons?<PAGE>\\Press a direction to run that way. Hold <BTN_DASH>\\while pressing a direction to walk instead.<PAGE>\\You can also press <BTN_CONF> to talk to\\someone or examine an object.'},
        ],
        "tutorial.txt": [
            {"kind": "id", "id": "MSG_TUTO_HELP_02", "orig_crc": 0x5106a3dd, "new": 'MSG_TUTO_HELP_02,You can use the buttons to navigate the\\menu as desired.'},
            {"kind": "id", "id": "MSG_TUTO_HELP_07", "orig_crc": 0x31c2ae46, "new": 'MSG_TUTO_HELP_07,You can also pick up an icon by selecting it\\and pressing the <BTN_CONF> Button.'},
            {"kind": "id", "id": "MSG_TUTO_HELP_08", "orig_crc": 0xe606eb01, "new": 'MSG_TUTO_HELP_08,To disable an icon, select it and press the\\<BTN_WARP> Button, or choose the panel\\labeled "Enable/Disable."'},
            {"kind": "id", "id": "MSG_TUTO_HELP_09", "orig_crc": 0x222a37ae, "new": 'MSG_TUTO_HELP_09,To restore all icons to their default positions,\\press SELECT or choose the panel labeled\\"Defaults."'},
            {"kind": "id", "id": "MSG_TUTO_HELP_11", "orig_crc": 0x8b0bff21, "new": 'MSG_TUTO_HELP_11,To save your customized layout scheme, press\\START or choose the panel labeled "Save & Apply."'},
            {"kind": "id", "id": "MSG_TUTO_HELP_13", "orig_crc": 0x80db9632, "new": 'MSG_TUTO_HELP_13,Press the <BTN_CONF> Button to continue.'},
        ],
        "menu.txt": [
            {"kind": "line", "orig_crc": 0xcb3a919c, "new": 'Select the input area\\and enter a name.'},
        ],
    },
    "es": {
        "start.txt": [
            {"kind": "id", "id": "MSG_NEWGAME_10", "orig_crc": 0xe0470047, "new": 'MSG_NEWGAME_10,Pulsa para comenzar.'},
        ],
        "cmes0.txt": [
            {"kind": "id", "id": "FLD_CMES0_218", "orig_crc": 0x4e2fb296, "new": 'FLD_CMES0_218,¿Sabías que puedes moverte con las\\flechas de dirección?<PAGE>\\Pulsa una dirección para correr hacia allí. Mantén pulsado\\<BTN_DASH> mientras pulsas una dirección para caminar en su lugar.<PAGE>\\También puedes pulsar <BTN_CONF> para hablar\\con un personaje o examinar un objeto.'},
        ],
        "tutorial.txt": [
            {"kind": "id", "id": "MSG_TUTO_HELP_02", "orig_crc": 0x5106a3dd, "new": 'MSG_TUTO_HELP_02,Puedes usar los botones para navegar\\por el menú.'},
            {"kind": "id", "id": "MSG_TUTO_HELP_07", "orig_crc": 0x31c2ae46, "new": 'MSG_TUTO_HELP_07,También puedes mover un icono seleccionándolo\\y pulsando el botón <BTN_CONF>.'},
            {"kind": "id", "id": "MSG_TUTO_HELP_08", "orig_crc": 0xe606eb01, "new": 'MSG_TUTO_HELP_08,Para desactivar un icono, selecciónalo y pulsa\\el botón <BTN_WARP>, o elige el panel\\llamado "Activar/Desactivar".'},
            {"kind": "id", "id": "MSG_TUTO_HELP_09", "orig_crc": 0x222a37ae, "new": 'MSG_TUTO_HELP_09,Para restaurar todos los iconos a sus posiciones\\predeterminadas, pulsa SELECT o elige el panel\\llamado "Predeterminado".'},
            {"kind": "id", "id": "MSG_TUTO_HELP_11", "orig_crc": 0x8b0bff21, "new": 'MSG_TUTO_HELP_11,Para guardar tu diseño personalizado, pulsa\\START o elige el panel llamado "Guardar y aplicar".'},
            {"kind": "id", "id": "MSG_TUTO_HELP_13", "orig_crc": 0x80db9632, "new": 'MSG_TUTO_HELP_13,Pulsa el botón <BTN_CONF> para continuar.'},
        ],
        "menu.txt": [
            {"kind": "line", "orig_crc": 0x803b2ab4, "new": 'Selecciona el campo de texto e introduce un nombre.'},
        ],
    },
    "fr": {
        "start.txt": [
            {"kind": "id", "id": "MSG_NEWGAME_10", "orig_crc": 0x1686945e, "new": 'MSG_NEWGAME_10,Appuyez pour commencer.'},
        ],
        "cmes0.txt": [
            {"kind": "id", "id": "FLD_CMES0_218", "orig_crc": 0xd1adee8c, "new": 'FLD_CMES0_218,Dites, vous savez que vous pouvez vous déplacer avec\\les touches directionnelles ?<PAGE>\\Appuyez sur une direction pour courir dans cette direction.\\Maintenez <BTN_DASH> enfoncé en appuyant sur une direction\\pour marcher à la place.<PAGE>\\Vous pouvez aussi appuyer sur <BTN_CONF> pour parler aux gens\\ou examiner un objet.'},
        ],
        "tutorial.txt": [
            {"kind": "id", "id": "MSG_TUTO_HELP_02", "orig_crc": 0xefd06854, "new": 'MSG_TUTO_HELP_02,Pour parcourir le menu, vous pouvez utiliser\\les boutons.'},
            {"kind": "id", "id": "MSG_TUTO_HELP_07", "orig_crc": 0x2135f895, "new": 'MSG_TUTO_HELP_07,Vous pouvez aussi déplacer une icône en la\\sélectionnant et en appuyant sur <BTN_CONF>.'},
            {"kind": "id", "id": "MSG_TUTO_HELP_08", "orig_crc": 0x7fbd1534, "new": 'MSG_TUTO_HELP_08,Pour désactiver une icône, sélectionnez-la et appuyez\\sur <BTN_WARP>, ou choisissez le panneau\\intitulé « Activer/Désactiver ».'},
            {"kind": "id", "id": "MSG_TUTO_HELP_09", "orig_crc": 0x6c9aebe8, "new": "MSG_TUTO_HELP_09,Pour retourner à l'arrangement par défaut,\\appuyez sur SELECT ou choisissez « Rétablir »."},
            {"kind": "id", "id": "MSG_TUTO_HELP_11", "orig_crc": 0x9b9ee479, "new": 'MSG_TUTO_HELP_11,Lorsque vous avez terminé vos modifications,\\appuyez sur START ou choisissez « Terminer ».'},
            {"kind": "id", "id": "MSG_TUTO_HELP_13", "orig_crc": 0x4bb92556, "new": 'MSG_TUTO_HELP_13,Appuyez sur le bouton <BTN_CONF> pour continuer.'},
        ],
        "menu.txt": [
            {"kind": "line", "orig_crc": 0x2c6a6ce3, "new": 'Sélectionnez la case\\et saisissez un nom.'},
        ],
    },
    "it": {
        "start.txt": [
            {"kind": "id", "id": "MSG_NEWGAME_10", "orig_crc": 0x64791ca6, "new": 'MSG_NEWGAME_10,Premi per iniziare.'},
        ],
        "cmes0.txt": [
            {"kind": "id", "id": "FLD_CMES0_218", "orig_crc": 0xc57198d0, "new": 'FLD_CMES0_218,Lo sapevi che puoi muoverti con i\\pulsanti direzionali?<PAGE>\\Premi una direzione per correre in quella direzione. Tieni premuto\\<BTN_DASH> mentre premi una direzione per camminare invece.<PAGE>\\Puoi anche premere <BTN_CONF> per parlare\\con qualcuno o esaminare un oggetto.'},
        ],
        "tutorial.txt": [
            {"kind": "id", "id": "MSG_TUTO_HELP_02", "orig_crc": 0x34a85631, "new": 'MSG_TUTO_HELP_02,Puoi usare i pulsanti per navigare\\nel menu.'},
            {"kind": "id", "id": "MSG_TUTO_HELP_07", "orig_crc": 0x4fa5a7fc, "new": "MSG_TUTO_HELP_07,Puoi anche spostare un'icona selezionandola\\e premendo il pulsante <BTN_CONF>."},
            {"kind": "id", "id": "MSG_TUTO_HELP_08", "orig_crc": 0xe890d519, "new": 'MSG_TUTO_HELP_08,Per disattivare un\'icona, selezionala e premi\\il pulsante <BTN_WARP>, oppure scegli il pannello\\etichettato "Attiva/Disattiva".'},
            {"kind": "id", "id": "MSG_TUTO_HELP_09", "orig_crc": 0xa27e24f6, "new": 'MSG_TUTO_HELP_09,Per ripristinare tutte le icone alle posizioni\\predefinite, premi SELECT oppure scegli il pannello\\etichettato "Predefinito".'},
            {"kind": "id", "id": "MSG_TUTO_HELP_11", "orig_crc": 0x4a1ad4fc, "new": 'MSG_TUTO_HELP_11,Per salvare la disposizione personalizzata, premi\\START oppure scegli il pannello "Salva e applica".'},
            {"kind": "id", "id": "MSG_TUTO_HELP_13", "orig_crc": 0xc827383b, "new": 'MSG_TUTO_HELP_13,Premi il pulsante <BTN_CONF> per continuare.'},
        ],
        "menu.txt": [
            {"kind": "line", "orig_crc": 0xf87e0a51, "new": 'Seleziona il campo\\e inserisci un nome.'},
        ],
    },
    "ja": {
        "start.txt": [
            {"kind": "id", "id": "MSG_NEWGAME_10", "orig_crc": 0x96227918, "new": 'MSG_NEWGAME_10,Press to Start'},
        ],
        "cmes0.txt": [
            {"kind": "id", "id": "FLD_CMES0_218", "orig_crc": 0xe4b7922e, "new": 'FLD_CMES0_218,ねぇねぇ。\\方向キーで移動できるって\\しってた？<PAGE>\\方向キーを押すとその方向に走るよ。\\<BTN_DASH>を押しながら方向キーを押すと\\歩けるんだ。<PAGE>\\<BTN_CONF>を押せば\\話したり調べたりもできるのよ。'},
        ],
        "tutorial.txt": [
            {"kind": "id", "id": "MSG_TUTO_HELP_02", "orig_crc": 0x34a85631, "new": 'MSG_TUTO_HELP_02,ボタン操作で\\メニューを操作できるよ。'},
            {"kind": "id", "id": "MSG_TUTO_HELP_07", "orig_crc": 0x4fa5a7fc, "new": 'MSG_TUTO_HELP_07,アイコンを選んで\\<BTN_CONF>ボタンを押すと\\アイコンを動かせるよ。'},
            {"kind": "id", "id": "MSG_TUTO_HELP_08", "orig_crc": 0xe890d519, "new": 'MSG_TUTO_HELP_08,アイコンの表示を消したい時は、\\アイコンを選んで<BTN_WARP>を押すよ。\\「未使用」と書かれた場所を\\選んでもＯＫです。'},
            {"kind": "id", "id": "MSG_TUTO_HELP_09", "orig_crc": 0xa27e24f6, "new": 'MSG_TUTO_HELP_09,初期設定に戻したいときは\\セレクトボタンを押すか\\「初期設定」と書かれた場所を\\選ぶと、元に戻ります。'},
            {"kind": "id", "id": "MSG_TUTO_HELP_11", "orig_crc": 0x4a1ad4fc, "new": 'MSG_TUTO_HELP_11,最後にカスタマイズが完了したら\\スタートボタンを押すか\\「設定完了」を選んでください。'},
            {"kind": "id", "id": "MSG_TUTO_HELP_13", "orig_crc": 0xc827383b, "new": 'MSG_TUTO_HELP_13,<BTN_CONF>を押してね'},
        ],
        "menu.txt": [
            {"kind": "line", "orig_crc": 0x76a7ad90, "new": '名前入力欄を選んで\\名前を入力してください'},
        ],
    },
    "ko": {
        "start.txt": [
            {"kind": "id", "id": "MSG_NEWGAME_10", "orig_crc": 0xaf1be9f2, "new": 'MSG_NEWGAME_10,버튼을 눌러 게임 시작'},
        ],
        "cmes0.txt": [
            {"kind": "id", "id": "FLD_CMES0_218", "orig_crc": 0xe4b7922e, "new": 'FLD_CMES0_218,저기 있잖아.\\방향 키로 이동할 수 있다는 거\\알고 있었어？<PAGE>\\방향 키를 누르면 그 방향으로 달려.\\<BTN_DASH>를 누른 채로 방향 키를 누르면\\대신 걸을 수 있어.<PAGE>\\<BTN_CONF>를 누르면\\이야기하거나 조사할 수도 있어.'},
        ],
        "menu.txt": [
            {"kind": "line", "orig_crc": 0x689e23d5, "new": '이름 입력란을 선택하여\\이름을 입력하십시오.'},
        ],
    },
    "zh-Hans": {
        "start.txt": [
            {"kind": "id", "id": "MSG_NEWGAME_10", "orig_crc": 0x96227918, "new": 'MSG_NEWGAME_10,Press to Start'},
        ],
        "cmes0.txt": [
            {"kind": "id", "id": "FLD_CMES0_218", "orig_crc": 0x1ac0a439, "new": 'FLD_CMES0_218,我说我说。\\用方向键可以移动\\你知道吗？<PAGE>\\按下方向键就会朝那个方向奔跑。\\按住<BTN_DASH>的同时按方向键，\\就会变成步行。<PAGE>\\按下<BTN_CONF>\\也能对话或者调查呢。'},
        ],
        "tutorial.txt": [
            {"kind": "id", "id": "MSG_TUTO_HELP_02", "orig_crc": 0x34a85631, "new": 'MSG_TUTO_HELP_02,你可以使用按钮来\\浏览菜单。'},
            {"kind": "id", "id": "MSG_TUTO_HELP_07", "orig_crc": 0x4fa5a7fc, "new": 'MSG_TUTO_HELP_07,选中图标后按下\\<BTN_CONF>按钮也可以移动图标。'},
            {"kind": "id", "id": "MSG_TUTO_HELP_08", "orig_crc": 0xe890d519, "new": 'MSG_TUTO_HELP_08,想要关闭图标显示时，\\选中图标后按下<BTN_WARP>。\\选择写有“未使用”的位置\\也可以。'},
            {"kind": "id", "id": "MSG_TUTO_HELP_09", "orig_crc": 0xa27e24f6, "new": 'MSG_TUTO_HELP_09,想要恢复初期设定时，\\按下SELECT按钮，或选择写有\\“初期设定”的位置，\\就能恢复原状。'},
            {"kind": "id", "id": "MSG_TUTO_HELP_11", "orig_crc": 0x4a1ad4fc, "new": 'MSG_TUTO_HELP_11,最后完成自定义设置后，\\按下START按钮，或选择\\“设定完成”。'},
            {"kind": "id", "id": "MSG_TUTO_HELP_13", "orig_crc": 0xc827383b, "new": 'MSG_TUTO_HELP_13,按下<BTN_CONF>按钮继续'},
        ],
        "menu.txt": [
            {"kind": "line", "orig_crc": 0xf355f5ea, "new": '请选择名字输入框输入名字'},
        ],
    },
    "zh-Hant": {
        "start.txt": [
            {"kind": "id", "id": "MSG_NEWGAME_10", "orig_crc": 0x96227918, "new": 'MSG_NEWGAME_10,Press to Start'},
        ],
        "cmes0.txt": [
            {"kind": "id", "id": "FLD_CMES0_218", "orig_crc": 0xf0cf237f, "new": 'FLD_CMES0_218,我說我說。\\用方向鍵可以移動\\你知道嗎？<PAGE>\\按下方向鍵就會朝那個方向奔跑。\\按住<BTN_DASH>的同時按方向鍵，\\就會變成步行。<PAGE>\\按下<BTN_CONF>\\也能對話或者調查呢。'},
        ],
        "tutorial.txt": [
            {"kind": "id", "id": "MSG_TUTO_HELP_02", "orig_crc": 0x34a85631, "new": 'MSG_TUTO_HELP_02,你可以使用按鈕來\\瀏覽選單。'},
            {"kind": "id", "id": "MSG_TUTO_HELP_07", "orig_crc": 0x4fa5a7fc, "new": 'MSG_TUTO_HELP_07,選取圖示後按下\\<BTN_CONF>按鈕也可以移動圖示。'},
            {"kind": "id", "id": "MSG_TUTO_HELP_08", "orig_crc": 0xe890d519, "new": 'MSG_TUTO_HELP_08,想要關閉圖示顯示時，\\選取圖示後按下<BTN_WARP>。\\選擇寫有「未使用」的位置\\也可以。'},
            {"kind": "id", "id": "MSG_TUTO_HELP_09", "orig_crc": 0xa27e24f6, "new": 'MSG_TUTO_HELP_09,想要恢復初期設定時，\\按下SELECT按鈕，或選擇寫有\\「初期設定」的位置，\\就能恢復原狀。'},
            {"kind": "id", "id": "MSG_TUTO_HELP_11", "orig_crc": 0x4a1ad4fc, "new": 'MSG_TUTO_HELP_11,最後完成自訂設定後，\\按下START按鈕，或選擇\\「設定完成」。'},
            {"kind": "id", "id": "MSG_TUTO_HELP_13", "orig_crc": 0xc827383b, "new": 'MSG_TUTO_HELP_13,按下<BTN_CONF>按鈕繼續'},
        ],
        "menu.txt": [
            {"kind": "line", "orig_crc": 0xa8953f17, "new": '請選擇名字輸入框輸入名字'},
        ],
    },
}

# ---------------------------------------------------------------------------
# Patch application
# ---------------------------------------------------------------------------

def crc(b):
    return binascii.crc32(b) & 0xFFFFFFFF


def apply_id_edit(data, mid, expected_crc, new_text, log, file_label):
    lines = data.split(b"\n")
    target_prefix = mid.encode() + b","
    found_idx = None
    for i, ln in enumerate(lines):
        if ln.startswith(target_prefix):
            found_idx = i
            break
    if found_idx is None:
        log.append(f"  [SKIP] {file_label} {mid}: id not found in this resources.bin")
        return data, False
    ln = lines[found_idx]
    had_cr = ln.endswith(b"\r")
    content = ln.rstrip(b"\r")
    actual_crc = crc(content)
    if actual_crc != expected_crc:
        log.append(
            f"  [SKIP] {file_label} {mid}: content doesn't match expected build "
            f"(crc {actual_crc:#x} != {expected_crc:#x}) -- left unchanged"
        )
        return data, False
    new_bytes = new_text.encode("utf-8") + (b"\r" if had_cr else b"")
    lines[found_idx] = new_bytes
    log.append(f"  [OK]   {file_label} {mid}: patched")
    return b"\n".join(lines), True


def apply_line_edit(data, expected_crc, new_text, log, file_label):
    """Locate a line with no message ID (e.g. menu.txt) purely by CRC32
    fingerprint of its current content -- robust to the line moving if a
    future SE patch inserts/removes lines earlier in the file, unlike a
    hardcoded line-number lookup."""
    lines = data.split(b"\n")
    found_idx = None
    for i, ln in enumerate(lines):
        content = ln.rstrip(b"\r")
        if crc(content) == expected_crc:
            found_idx = i
            break
    if found_idx is None:
        log.append(
            f"  [SKIP] {file_label}: no line matched the expected "
            f"fingerprint -- left unchanged"
        )
        return data, False
    had_cr = lines[found_idx].endswith(b"\r")
    new_bytes = new_text.encode("utf-8") + (b"\r" if had_cr else b"")
    lines[found_idx] = new_bytes
    log.append(f"  [OK]   {file_label}: patched")
    return b"\n".join(lines), True


def process_language(arc, lang, log):
    """Apply every CAT_A and CAT_B edit for one language. Returns a dict of
    {resources.bin-relative path: patched bytes} for files that changed,
    plus (changed_count, skipped_count)."""
    msg_dir = f"Localize/{lang}/msg"
    changed_count = 0
    skipped_count = 0
    output_files = {}

    cat_a = CAT_A.get(lang, {})
    cat_b = CAT_B.get(lang, {})
    all_files = sorted(set(cat_a.keys()) | set(cat_b.keys()))

    for fn in all_files:
        res_path = f"{msg_dir}/{fn}"
        file_label = f"{lang}/{fn}"
        if not arc.has(res_path):
            log.append(f"  [SKIP] {file_label}: not found in resources.bin at {res_path}")
            continue
        data = arc.extract(res_path)
        file_changed = False

        # Category A edits (pull replacement text live from the _pad twin)
        if fn in cat_a:
            pad_fn = cat_a[fn]["pad_file"]
            pad_path = f"{msg_dir}/{pad_fn}"
            pad_data = None
            if arc.has(pad_path):
                pad_data = arc.extract(pad_path)
            else:
                log.append(f"  [SKIP] {file_label}: pad twin {pad_fn} not found, skipping its edits")

            for mid, expected_crc in cat_a[fn]["ids"].items():
                if pad_data is None:
                    skipped_count += 1
                    continue
                pad_line = None
                prefix = mid.encode() + b","
                for ln in pad_data.split(b"\n"):
                    if ln.startswith(prefix):
                        pad_line = ln.rstrip(b"\r").decode("utf-8", "replace")
                        break
                if pad_line is None:
                    log.append(f"  [SKIP] {file_label} {mid}: id not found in {pad_fn}")
                    skipped_count += 1
                    continue
                data, ok = apply_id_edit(data, mid, expected_crc, pad_line, log, file_label)
                file_changed |= ok
                changed_count += 1 if ok else 0
                skipped_count += 0 if ok else 1

        # Category B edits (original wording, embedded in this script)
        if fn in cat_b:
            for edit in cat_b[fn]:
                if edit["kind"] == "id":
                    data, ok = apply_id_edit(data, edit["id"], edit["orig_crc"], edit["new"], log, file_label)
                else:  # "line" -- no message ID in this file (e.g. menu.txt)
                    data, ok = apply_line_edit(data, edit["orig_crc"], edit["new"], log, file_label)
                file_changed |= ok
                changed_count += 1 if ok else 0
                skipped_count += 0 if ok else 1

        if file_changed:
            output_files[res_path] = data

    return output_files, changed_count, skipped_count


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("resources_bin", help="path to your own resources.bin")
    ap.add_argument("-o", "--output", default="switch_text_patches.ctp",
                     help="output .ctp path (default: switch_text_patches.ctp)")
    ap.add_argument("--lang", default=None,
                     help="comma-separated language subset, e.g. 'en' or 'en,ja' "
                          "(default: all 9 languages: " + ",".join(sorted(CAT_A.keys())) + ")")
    args = ap.parse_args()

    langs = sorted(CAT_A.keys())
    if args.lang:
        requested = [l.strip() for l in args.lang.split(",") if l.strip()]
        unknown = [l for l in requested if l not in CAT_A]
        if unknown:
            print(f"ERROR: unknown language(s) {unknown}; available: {langs}", file=sys.stderr)
            sys.exit(1)
        langs = requested

    print(f"Reading {args.resources_bin} ...")
    try:
        arc = ResourcesBin(args.resources_bin)
    except Exception as e:
        print(f"ERROR: could not read resources.bin: {e}", file=sys.stderr)
        sys.exit(1)
    print("  OK, parsed ARC1 archive successfully.")
    print()

    all_output_files = {}
    total_changed = 0
    total_skipped = 0
    per_lang_summary = []

    for lang in langs:
        log = []
        output_files, changed_count, skipped_count = process_language(arc, lang, log)
        print(f"=== {lang} ===")
        print("\n".join(log))
        print(f"  -> {changed_count} line(s) patched across {len(output_files)} file(s); "
              f"{skipped_count} skipped.")
        print()
        all_output_files.update(output_files)
        total_changed += changed_count
        total_skipped += skipped_count
        per_lang_summary.append(
            f"  {lang:10} {changed_count:3} patched, {len(output_files):2} files, {skipped_count:2} skipped"
        )

    print("=== summary ===")
    print("\n".join(per_lang_summary))
    print(f"\nTOTAL: {total_changed} line(s) patched across {len(all_output_files)} file(s) "
          f"in {len(langs)} language(s); {total_skipped} skipped (see warnings above).")

    if not all_output_files:
        print("Nothing to write -- no edits applied. Check the warnings above.", file=sys.stderr)
        sys.exit(1)

    with zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED) as zf:
        for path, content in sorted(all_output_files.items()):
            zf.writestr(path, content)

    print()
    print(f"Wrote {args.output} ({len(all_output_files)} files). Drop it into your ct_nx mods/ folder.")


if __name__ == "__main__":
    main()