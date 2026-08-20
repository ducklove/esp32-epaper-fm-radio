# 1비트 PNG 를 Adafruit_GFX drawBitmap 용 C 배열 헤더로 바꾼다.
#
#   python tools/png_to_header.py f_tight.png src/boards/epaper/photo.h PHOTO
#
# PNG 는 0 이 검정인데 drawBitmap 은 비트 1 을 찍으므로 반전해서 담는다.
import sys

from PIL import Image

SRC, OUT, NAME = sys.argv[1], sys.argv[2], sys.argv[3]
im = Image.open(SRC).convert("1")
W, H = im.size
assert W % 8 == 0, "폭이 8의 배수여야 한다"

px = im.load()
rows = []
for y in range(H):
    row = []
    for xb in range(W // 8):
        b = 0
        for bit in range(8):
            if px[xb * 8 + bit, y] == 0:  # 검정
                b |= 0x80 >> bit
        row.append(b)
    rows.append(row)

with open(OUT, "w", encoding="utf-8") as f:
    f.write("// 자동 생성 — 편집하지 말 것 (tools/png_to_header.py)\n")
    f.write(f"// 원본 사진을 {W}x{H} 1비트로 변환한 것. 비트 1 = 검정.\n")
    f.write("#pragma once\n\n#include <Arduino.h>\n\n")
    f.write(f"constexpr int16_t {NAME}_W = {W};\n")
    f.write(f"constexpr int16_t {NAME}_H = {H};\n\n")
    f.write(f"const uint8_t {NAME}[] PROGMEM = {{\n")
    for row in rows:
        f.write("    " + ", ".join(f"0x{b:02X}" for b in row) + ",\n")
    f.write("};\n")

print(f"{OUT} 생성 — {W}x{H}, {W * H // 8} 바이트")
