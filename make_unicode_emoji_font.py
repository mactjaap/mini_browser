#!/usr/bin/env python3
import struct
import sys
from pathlib import Path

FONT = sys.argv[1] if len(sys.argv) > 1 else "unifont_jp-17.0.02.hex"
OUT = sys.argv[2] if len(sys.argv) > 2 else "unifont_cjk.bin"

RANGES = [
    (0x00A0, 0x00FF, "Latin-1 Supplement"),
    (0x0100, 0x017F, "Latin Extended-A"),
    (0x0180, 0x024F, "Latin Extended-B"),
    (0x0250, 0x02AF, "IPA Extensions"),
    (0x02B0, 0x02FF, "Spacing Modifier Letters"),
    (0x0300, 0x036F, "Combining Diacritical Marks"),
    (0x0370, 0x03FF, "Greek and Coptic"),
    (0x0400, 0x04FF, "Cyrillic"),
    (0x0500, 0x052F, "Cyrillic Supplement"),
    (0x0530, 0x058F, "Armenian"),
    (0x10A0, 0x10FF, "Georgian"),
    (0x1E00, 0x1EFF, "Latin Extended Additional"),
    (0x1F00, 0x1FFF, "Greek Extended"),
    (0x2000, 0x206F, "General Punctuation"),
    (0x2070, 0x209F, "Superscripts and Subscripts"),
    (0x20A0, 0x20CF, "Currency Symbols"),
    (0x2100, 0x214F, "Letterlike Symbols"),
    (0x2150, 0x218F, "Number Forms"),
    (0x2190, 0x21FF, "Arrows"),
    (0x2200, 0x22FF, "Mathematical Operators"),
    (0x2300, 0x23FF, "Miscellaneous Technical"),
    (0x2500, 0x257F, "Box Drawing"),
    (0x2580, 0x259F, "Block Elements"),
    (0x25A0, 0x25FF, "Geometric Shapes"),
    (0x2600, 0x26FF, "Miscellaneous Symbols"),
    (0x2700, 0x27BF, "Dingbats"),
    (0x3000, 0x303F, "CJK Symbols and Punctuation"),
    (0x3040, 0x309F, "Hiragana"),
    (0x30A0, 0x30FF, "Katakana"),
    (0x4E00, 0x9FFF, "CJK Unified Ideographs"),
    (0xFF00, 0xFFEF, "Halfwidth and Fullwidth Forms"),
    (0x1F000, 0x1F02F, "Mahjong Tiles"),
    (0x1F0A0, 0x1F0FF, "Playing Cards"),
    (0x1F100, 0x1F1FF, "Enclosed Alphanumeric Supplement"),
    (0x1F200, 0x1F2FF, "Enclosed Ideographic Supplement"),
    (0x1F300, 0x1F5FF, "Misc Symbols and Pictographs"),
    (0x1F600, 0x1F64F, "Emoticons"),
    (0x1F680, 0x1F6FF, "Transport and Map Symbols"),
    (0x1F700, 0x1F77F, "Alchemical Symbols"),
    (0x1F780, 0x1F7FF, "Geometric Shapes Extended"),
    (0x1F800, 0x1F8FF, "Supplemental Arrows-C"),
    (0x1F900, 0x1F9FF, "Supplemental Symbols and Pictographs"),
    (0x1FA00, 0x1FA6F, "Chess Symbols"),
    (0x1FA70, 0x1FAFF, "Symbols and Pictographs Extended-A"),
]

GLYPH_BYTES = 32

def wanted(cp):
    return any(start <= cp <= end for start, end, _ in RANGES)

def normalize_bitmap(bitmap):
    """
    Mini Browser always stores 16x16 / 32-byte glyph slots.

    GNU Unifont .hex uses:
      16 bytes -> 8x16 glyph
      32 bytes -> 16x16 glyph

    Expand 8x16 glyphs into centered 16x16 slots so Latin, Greek,
    Cyrillic, punctuation, etc. work with the same badge renderer.
    """
    if len(bitmap) == 32:
        return bitmap

    if len(bitmap) == 16:
        out = bytearray()
        for row in bitmap:
            # Center an 8-pixel row inside 16 pixels: 4 blank pixels
            # on each side. The badge renderer reads MSB first.
            bits = row << 4
            out.append((bits >> 8) & 0xFF)
            out.append(bits & 0xFF)
        return bytes(out)

    return None

glyphs = {}

with open(FONT, "r", encoding="ascii") as f:
    for line in f:
        line = line.strip()
        if not line or ":" not in line:
            continue

        cp_hex, bitmap_hex = line.split(":", 1)

        try:
            cp = int(cp_hex, 16)
            bitmap = bytes.fromhex(bitmap_hex)
        except ValueError:
            continue

        if not wanted(cp):
            continue

        normalized = normalize_bitmap(bitmap)
        if normalized is not None:
            glyphs[cp] = normalized

range_count = len(RANGES)
header_size = 12 + range_count * 12

records = []
offset = header_size

for start, end, name in RANGES:
    records.append((start, end, offset, name))
    offset += (end - start + 1) * GLYPH_BYTES

blank = bytes(GLYPH_BYTES)

with open(OUT, "wb") as f:
    f.write(b"MBCJ")
    f.write(struct.pack("<I", 1))
    f.write(struct.pack("<I", range_count))

    for start, end, glyph_offset, _ in records:
        f.write(struct.pack("<III", start, end, glyph_offset))

    present = 0
    missing = 0

    for start, end, _, _ in records:
        for cp in range(start, end + 1):
            bitmap = glyphs.get(cp)
            if bitmap is None:
                f.write(blank)
                missing += 1
            else:
                f.write(bitmap)
                present += 1

expected_size = offset
actual_size = Path(OUT).stat().st_size

print("Mini Browser extended Unicode font")
print()
for start, end, glyph_offset, name in records:
    slots = end - start + 1
    available = sum(1 for cp in range(start, end + 1) if cp in glyphs)
    print(
        f"{name:32s} U+{start:04X}-U+{end:04X}  "
        f"offset={glyph_offset:6d}  present={available:5d}/{slots:5d}"
    )

print()
print(f"Glyphs present : {present}")
print(f"Blank slots    : {missing}")
print(f"Range count    : {range_count}")
print(f"Output         : {OUT}")
print(f"File size      : {actual_size:,} bytes")

if actual_size != expected_size:
    raise SystemExit(
        f"ERROR: expected {expected_size:,} bytes, got {actual_size:,}"
    )

print("Size check     : OK")
