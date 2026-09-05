"""DejaVu Sans에서 주파수 표시용 72px GFX 비트맵 폰트를 만든다."""
import argparse
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

parser = argparse.ArgumentParser()
parser.add_argument("font", type=Path)
parser.add_argument("output", type=Path)
args = parser.parse_args()
font = ImageFont.truetype(str(args.font), 72)
bitmap, glyphs = [], []
for code in range(46, 58):
    char = chr(code)
    x0, y0, x1, y1 = font.getbbox(char, anchor="ls")
    width, height = x1 - x0, y1 - y0
    im = Image.new("L", (width, height))
    ImageDraw.Draw(im).text((-x0, -y0), char, font=font, fill=255, anchor="ls")
    bits = [int(value >= 128) for value in im.getdata()]
    bits += [0] * (-len(bits) % 8)
    glyphs.append((len(bitmap), width, height, round(font.getlength(char)), x0, y0))
    bitmap.extend(sum(bits[i + j] << (7 - j) for j in range(8)) for i in range(0, len(bits), 8))
lines = ["// 생성: tools/make_dial_font.py. 출처와 이용허락: docs/fonts/DejaVu-LICENSE.txt",
         "#pragma once", "const uint8_t DialDigitsBitmaps[] PROGMEM = {"]
for i in range(0, len(bitmap), 16):
    lines.append("    " + ", ".join(f"0x{b:02x}" for b in bitmap[i:i + 16]) + ",")
lines.extend(["};", "const GFXglyph DialDigitsGlyphs[] PROGMEM = {"])
lines.extend("    {" + ", ".join(map(str, row)) + "}," for row in glyphs)
lines.extend(["};", "const GFXfont DialDigits PROGMEM = {",
              "    (uint8_t*)DialDigitsBitmaps, (GFXglyph*)DialDigitsGlyphs, 46, 57, 86};", ""])
args.output.write_text("\n".join(lines), encoding="utf-8")
