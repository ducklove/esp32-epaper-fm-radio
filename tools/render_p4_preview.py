"""제품 UI가 기록한 도형과 글꼴 픽셀을 PNG로 변환한다. Pillow 필요."""
from pathlib import Path
from PIL import Image, ImageDraw

def color(c):
    return ((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)

for path in Path('.pio/preview').glob('*.draw'):
    image = Image.new('RGB', (480, 320))
    draw = ImageDraw.Draw(image)
    for line in path.read_text().splitlines():
        op, *args = line.split()
        *a, c = map(int, args)
        c = color(c)
        if op in ('rect', 'round', 'outline'):
            x, y, w, h, *radius = a
            box = (x, y, x + w - 1, y + h - 1)
            if op == 'rect': draw.rectangle(box, fill=c)
            elif op == 'round': draw.rounded_rectangle(box, radius=radius[0], fill=c)
            else: draw.rounded_rectangle(box, radius=radius[0], outline=c)
        elif op in ('circle', 'ring'):
            x, y, r = a
            draw.ellipse((x-r, y-r, x+r, y+r), **({'fill': c} if op == 'circle' else {'outline': c}))
        elif op == 'line': draw.line(a, fill=c)
        elif op == 'triangle': draw.polygon(a, fill=c)
    image.save(path.with_suffix('.png'))
    print(path.with_suffix('.png'))
