#!/usr/bin/env python3
"""
Generate Mini Browser 2.4 unifont_cjk.bin (MBCJ v1).

Sources:
  GNU Unifont 17.0.02
  unifont_jp-17.0.02.hex(.gz)       Plane 0 / Japanese variants
  unifont_upper-17.0.02.hex(.gz)    supplementary planes

The output keeps the existing Mini Browser MBCJ v1 format:
  12-byte header
  N x 12-byte range records
  fixed 32-byte 16x16 glyph slots

8x16 Unifont glyphs are centered into a 16x16 cell by shifting each
8-bit row left by four pixels. 16x16 glyphs are copied unchanged.
Other source widths are left as empty slots because Mini Browser's
renderer is deliberately fixed at 16x16 for external Unicode glyphs.
"""

import argparse
import gzip
import io
import os
import struct
import sys
import urllib.request
from pathlib import Path

UNIFONT_VERSION = "17.0.02"

JP_URL = (
    "https://unifoundry.com/pub/unifont/unifont-17.0.02/"
    "font-builds/unifont_jp-17.0.02.hex.gz"
)
UPPER_URL = (
    "https://unifoundry.com/pub/unifont/unifont-17.0.02/"
    "font-builds/unifont_upper-17.0.02.hex.gz"
)

RANGES = [
    (0xA0, 0x58F, 'Latin, IPA, combining marks, Greek, Cyrillic, Armenian'),
    (0x590, 0x5FF, 'Hebrew'),
    (0x600, 0x6FF, 'Arabic'),
    (0x700, 0x74F, 'Syriac'),
    (0x750, 0x77F, 'Arabic Supplement'),
    (0x780, 0x7BF, 'Thaana'),
    (0x7C0, 0x7FF, 'NKo'),
    (0x800, 0x83F, 'Samaritan'),
    (0x840, 0x85F, 'Mandaic'),
    (0x860, 0x86F, 'Syriac Supplement'),
    (0x870, 0x89F, 'Arabic Extended-B'),
    (0x8A0, 0x8FF, 'Arabic Extended-A'),
    (0x900, 0x97F, 'Devanagari'),
    (0x980, 0x9FF, 'Bengali'),
    (0xA00, 0xA7F, 'Gurmukhi'),
    (0xA80, 0xAFF, 'Gujarati'),
    (0xB00, 0xB7F, 'Odia'),
    (0xB80, 0xBFF, 'Tamil'),
    (0xC00, 0xC7F, 'Telugu'),
    (0xC80, 0xCFF, 'Kannada'),
    (0xD00, 0xD7F, 'Malayalam'),
    (0xD80, 0xDFF, 'Sinhala'),
    (0xE00, 0xE7F, 'Thai'),
    (0xE80, 0xEFF, 'Lao'),
    (0xF00, 0xFFF, 'Tibetan'),
    (0x1000, 0x109F, 'Myanmar'),
    (0x10A0, 0x10FF, 'Georgian'),
    (0x1100, 0x11FF, 'Hangul Jamo'),
    (0x1200, 0x137F, 'Ethiopic'),
    (0x13A0, 0x13FF, 'Cherokee'),
    (0x1400, 0x167F, 'Unified Canadian Aboriginal Syllabics'),
    (0x1680, 0x169F, 'Ogham'),
    (0x16A0, 0x16FF, 'Runic'),
    (0x1700, 0x177F, 'Philippine scripts'),
    (0x1780, 0x17FF, 'Khmer'),
    (0x1800, 0x18AF, 'Mongolian'),
    (0x1AB0, 0x1AFF, 'Combining Diacritical Marks Extended'),
    (0x1B00, 0x1BBF, 'Balinese and Sundanese'),
    (0x1C00, 0x1C7F, 'Lepcha and Ol Chiki'),
    (0x1CD0, 0x1CFF, 'Vedic Extensions'),
    (0x1D00, 0x1DBF, 'Phonetic Extensions'),
    (0x1DC0, 0x1DFF, 'Combining Diacritical Marks Supplement'),
    (0x1E00, 0x1FFF, 'Latin Extended Additional and Greek Extended'),
    (0x2000, 0x23FF, 'Punctuation, currency, arrows, math and technical symbols'),
    (0x2500, 0x27BF, 'Box drawing, blocks, shapes, symbols and dingbats'),
    (0x2C00, 0x2C5F, 'Glagolitic'),
    (0x2C60, 0x2C7F, 'Latin Extended-C'),
    (0x2C80, 0x2CFF, 'Coptic'),
    (0x2D00, 0x2D2F, 'Georgian Supplement'),
    (0x2D30, 0x2D7F, 'Tifinagh'),
    (0x2D80, 0x2DDF, 'Ethiopic Extended'),
    (0x2DE0, 0x2DFF, 'Cyrillic Extended-A'),
    (0x2E00, 0x2E7F, 'Supplemental Punctuation'),
    (0x3000, 0x318F, 'CJK punctuation, Kana, Bopomofo, Hangul compatibility'),
    (0x31A0, 0x31FF, 'Bopomofo Extended, CJK strokes, Katakana extensions'),
    (0x3400, 0x4DBF, 'CJK Unified Ideographs Extension A'),
    (0x4E00, 0x9FFF, 'CJK Unified Ideographs'),
    (0xA000, 0xA4CF, 'Yi syllables/radicals'),
    (0xA4D0, 0xA4FF, 'Lisu'),
    (0xA500, 0xA63F, 'Vai'),
    (0xA640, 0xA69F, 'Cyrillic Extended-B'),
    (0xA6A0, 0xA6FF, 'Bamum'),
    (0xA720, 0xA7FF, 'Latin Extended-D'),
    (0xA800, 0xA82F, 'Syloti Nagri'),
    (0xA840, 0xA87F, 'Phags-pa'),
    (0xA880, 0xA8DF, 'Saurashtra'),
    (0xA8E0, 0xA8FF, 'Devanagari Extended'),
    (0xA900, 0xA92F, 'Kayah Li'),
    (0xA930, 0xA95F, 'Rejang'),
    (0xA960, 0xA97F, 'Hangul Jamo Extended-A'),
    (0xA980, 0xA9DF, 'Javanese'),
    (0xAA00, 0xAA5F, 'Cham'),
    (0xAA60, 0xAA7F, 'Myanmar Extended-A'),
    (0xAA80, 0xAADF, 'Tai Viet'),
    (0xAB00, 0xAB2F, 'Ethiopic Extended-A'),
    (0xAB30, 0xAB6F, 'Latin Extended-E'),
    (0xABC0, 0xABFF, 'Meetei Mayek'),
    (0xAC00, 0xD7AF, 'Hangul Syllables'),
    (0xD7B0, 0xD7FF, 'Hangul Jamo Extended-B'),
    (0xF900, 0xFAFF, 'CJK Compatibility Ideographs'),
    (0xFB00, 0xFB4F, 'Alphabetic Presentation Forms'),
    (0xFB50, 0xFDFF, 'Arabic Presentation Forms-A'),
    (0xFE00, 0xFE0F, 'Variation Selectors'),
    (0xFE20, 0xFE2F, 'Combining Half Marks'),
    (0xFE30, 0xFE4F, 'CJK Compatibility Forms'),
    (0xFE50, 0xFE6F, 'Small Form Variants'),
    (0xFE70, 0xFEFF, 'Arabic Presentation Forms-B'),
    (0xFF00, 0xFFEF, 'Halfwidth and Fullwidth Forms'),
    (0x1F000, 0x1F02F, 'Mahjong Tiles'),
    (0x1F0A0, 0x1F0FF, 'Playing Cards'),
    (0x1F100, 0x1F1FF, 'Enclosed Alphanumeric Supplement'),
    (0x1F200, 0x1F2FF, 'Enclosed Ideographic Supplement'),
    (0x1F300, 0x1F5FF, 'Miscellaneous Symbols and Pictographs'),
    (0x1F600, 0x1F64F, 'Emoticons'),
    (0x1F680, 0x1F6FF, 'Transport and Map Symbols'),
    (0x1F700, 0x1F77F, 'Alchemical Symbols'),
    (0x1F780, 0x1F7FF, 'Geometric Shapes Extended'),
    (0x1F800, 0x1F8FF, 'Supplemental Arrows-C'),
    (0x1F900, 0x1F9FF, 'Supplemental Symbols and Pictographs'),
    (0x1FA00, 0x1FA6F, 'Chess Symbols'),
    (0x1FA70, 0x1FAFF, 'Symbols and Pictographs Extended-A'),
]

ZERO_GLYPH = bytes(32)


def read_source(path):
    path = Path(path)
    data = path.read_bytes()

    if path.suffix == ".gz" or data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)

    return data.decode("ascii")


def download(url, destination):
    destination = Path(destination)
    print(f"Downloading {url}", file=sys.stderr)

    request = urllib.request.Request(
        url,
        headers={"User-Agent": "MiniBrowser-Unifont-Generator/2.4"},
    )

    with urllib.request.urlopen(request, timeout=60) as response:
        destination.write_bytes(response.read())

    return destination


def normalize_glyph(hex_bitmap):
    raw = bytes.fromhex(hex_bitmap)

    # GNU Unifont 8x16 glyph: 16 rows x 8 bits.
    # Center it in Mini Browser's fixed 16x16 cell.
    if len(raw) == 16:
        out = bytearray()

        for row in raw:
            word = row << 4
            out.append((word >> 8) & 0xFF)
            out.append(word & 0xFF)

        return bytes(out)

    # GNU Unifont 16x16 glyph: 16 rows x 16 bits.
    if len(raw) == 32:
        return raw

    # 24/32-pixel-wide Unifont glyphs do not fit the renderer.
    return None


def parse_hex(text):
    glyphs = {}
    unsupported_width = 0

    for lineno, line in enumerate(text.splitlines(), 1):
        line = line.strip()

        if not line or line.startswith("#"):
            continue

        try:
            cp_text, bitmap_text = line.split(":", 1)
            cp = int(cp_text, 16)
            bitmap = normalize_glyph(bitmap_text.strip())
        except Exception as exc:
            raise ValueError(f"Invalid Unifont HEX line {lineno}: {line!r}") from exc

        if bitmap is None:
            unsupported_width += 1
            continue

        glyphs[cp] = bitmap

    return glyphs, unsupported_width


def write_mb_cj(output, glyphs):
    output = Path(output)
    range_count = len(RANGES)
    table_end = 12 + range_count * 12

    records = []
    offset = table_end

    for start, end, name in RANGES:
        records.append((start, end, offset, name))
        offset += (end - start + 1) * 32

    present = 0
    missing = 0

    with output.open("wb") as f:
        f.write(b"MBCJ")
        f.write(struct.pack("<I", 1))
        f.write(struct.pack("<I", range_count))

        for start, end, data_offset, _name in records:
            f.write(struct.pack("<III", start, end, data_offset))

        for start, end, _data_offset, _name in records:
            for cp in range(start, end + 1):
                bitmap = glyphs.get(cp)

                if bitmap is None:
                    f.write(ZERO_GLYPH)
                    missing += 1
                else:
                    f.write(bitmap)
                    present += 1

    return records, present, missing, output.stat().st_size


def main():
    parser = argparse.ArgumentParser(
        description="Generate the expanded Mini Browser 2.4 Unicode font"
    )
    parser.add_argument(
        "--jp",
        help="unifont_jp-17.0.02.hex or .hex.gz",
    )
    parser.add_argument(
        "--upper",
        help="unifont_upper-17.0.02.hex or .hex.gz",
    )
    parser.add_argument(
        "--download",
        action="store_true",
        help="download the GNU Unifont 17.0.02 source files automatically",
    )
    parser.add_argument(
        "-o",
        "--output",
        default="unifont_cjk.bin",
        help="output MBCJ file (default: unifont_cjk.bin)",
    )
    parser.add_argument(
        "--workdir",
        default=".",
        help="directory used by --download",
    )
    args = parser.parse_args()

    workdir = Path(args.workdir)
    workdir.mkdir(parents=True, exist_ok=True)

    jp = Path(args.jp) if args.jp else None
    upper = Path(args.upper) if args.upper else None

    if args.download:
        if jp is None:
            jp = workdir / f"unifont_jp-{UNIFONT_VERSION}.hex.gz"
        if upper is None:
            upper = workdir / f"unifont_upper-{UNIFONT_VERSION}.hex.gz"

        if not jp.exists():
            download(JP_URL, jp)

        if not upper.exists():
            download(UPPER_URL, upper)

    if jp is None or upper is None:
        parser.error("use --download or supply both --jp and --upper")

    print(f"Reading {jp}", file=sys.stderr)
    plane0, unsupported0 = parse_hex(read_source(jp))

    print(f"Reading {upper}", file=sys.stderr)
    upper_glyphs, unsupported_upper = parse_hex(read_source(upper))

    # Plane-0 Japanese source is authoritative for BMP code points.
    # Supplementary source supplies code points above U+FFFF.
    glyphs = dict(plane0)

    for cp, bitmap in upper_glyphs.items():
        if cp > 0xFFFF:
            glyphs[cp] = bitmap

    records, present, missing, size = write_mb_cj(args.output, glyphs)

    slots = present + missing

    print()
    print("Mini Browser 2.4 Unicode font generated")
    print(f"Output:             {args.output}")
    print(f"MBCJ ranges:        {len(records)}")
    print(f"Code-point slots:   {slots}")
    print(f"Usable glyphs:      {present}")
    print(f"Empty slots:        {missing}")
    print(f"Output bytes:       {size}")
    print(f"Output MiB:         {size / 1024 / 1024:.2f}")
    print(f"Unsupported widths: {unsupported0 + unsupported_upper}")
    print()
    print("The Mini Browser 2.4 loader reads these ranges from the file header;")
    print("no hard-coded C offset table needs to be regenerated.")


if __name__ == "__main__":
    raise SystemExit(main())
