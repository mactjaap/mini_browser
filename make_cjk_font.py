import struct

FONT = "unifont_jp-17.0.02.hex"
OUT = "unifont_cjk.bin"

# Direct-indexed Unicode ranges.
RANGES = [
    (0x3000, 0x303F, "CJK punctuation"),
    (0x3040, 0x309F, "Hiragana"),
    (0x30A0, 0x30FF, "Katakana"),
    (0x4E00, 0x9FFF, "CJK Unified Ideographs"),
]

glyphs = {}

with open(FONT, "r", encoding="ascii") as f:
    for line in f:
        line = line.strip()

        if not line or ":" not in line:
            continue

        cp_hex, bitmap_hex = line.split(":", 1)
        cp = int(cp_hex, 16)

        wanted = any(start <= cp <= end for start, end, _ in RANGES)
        if not wanted:
            continue

        bitmap = bytes.fromhex(bitmap_hex)

        # We only use 16x16 / 32-byte glyphs.
        if len(bitmap) == 32:
            glyphs[cp] = bitmap

blank = bytes(32)

print("Generating direct-indexed CJK font")
print()

total_slots = 0
total_glyphs = 0

with open(OUT, "wb") as f:
    # Small header
    f.write(b"MBCJ")
    f.write(struct.pack("<I", 1))
    f.write(struct.pack("<I", len(RANGES)))

    # Calculate offsets.
    header_size = 12 + len(RANGES) * 12
    offset = header_size

    range_info = []

    for start, end, name in RANGES:
        count = end - start + 1

        range_info.append((start, end, offset, name))

        f.write(struct.pack("<III", start, end, offset))

        offset += count * 32
        total_slots += count

    # Glyph data.
    for start, end, _, name in range_info:
        present = 0

        for cp in range(start, end + 1):
            bitmap = glyphs.get(cp)

            if bitmap is None:
                f.write(blank)
            else:
                f.write(bitmap)
                present += 1
                total_glyphs += 1

        print(
            f"{name:25s} "
            f"U+{start:04X}-U+{end:04X}: "
            f"{present} glyphs"
        )

print()
print(f"Glyphs present : {total_glyphs}")
print(f"Glyph slots    : {total_slots}")
print(f"Output         : {OUT}")
print(f"File size      : {offset:,} bytes")
