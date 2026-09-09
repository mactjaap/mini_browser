import struct

FONT = "unifont_jp-17.0.02.hex"
OUT = "unifont_cjk.bin"

wanted_text = "无名博客一个聊的人罢了朝の紅葉"
wanted = {ord(ch) for ch in wanted_text}

glyphs = {}

with open(FONT, "r", encoding="ascii") as f:
    for line in f:
        line = line.strip()
        if not line or ":" not in line:
            continue

        cp_hex, bitmap_hex = line.split(":", 1)
        cp = int(cp_hex, 16)

        if cp not in wanted:
            continue

        bitmap = bytes.fromhex(bitmap_hex)

        # For this first test we require 16x16 glyphs = 32 bytes.
        if len(bitmap) == 32:
            glyphs[cp] = bitmap
        else:
            print(
                f"Skipping U+{cp:04X} {chr(cp)!r}: "
                f"{len(bitmap)} bytes, expected 32"
            )

missing = wanted - glyphs.keys()

with open(OUT, "wb") as f:
    for cp in sorted(glyphs):
        f.write(struct.pack("<I", cp))
        f.write(glyphs[cp])

print()
print(f"Wrote {len(glyphs)} glyphs to {OUT}")
print(f"File size: {len(glyphs) * 36} bytes")

for cp in sorted(glyphs):
    print(f"  U+{cp:04X} {chr(cp)}")

if missing:
    print()
    print("Missing:")
    for cp in sorted(missing):
        print(f"  U+{cp:04X} {chr(cp)}")
