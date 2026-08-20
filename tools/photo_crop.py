# 사진 한 장을 ePaper 딥슬립 화면용 200x200 1비트 이미지로 만든다.
#
# 자르기를 이미지 중심이 아니라 "머리 중심" 기준으로 한다. 처음에는 가운데를
# 잘랐는데 인물이 눈에 띄게 오른쪽으로 밀려 보였다 — 원본에서 머리 중심이
# 이미지 중심보다 20px 오른쪽에 있었기 때문이다. 상자를 머리에 맞추면 해결된다.
#
# 기본값은 원래 쓴 사진(435x515)에서 잰 값이다. 다른 사진을 넣을 때는
# --head-cx / --head-top 을 그 사진에 맞게 재서 넘긴다.
#
#   python tools/photo_crop.py 원본.jpg 출력디렉토리 --head-cx 237.6 --head-top 165
#
# 세 가지 크기를 한꺼번에 뽑고 비교용 시트도 남긴다. 고른 것을 png_to_header.py
# 에 넘기면 photo.h 가 된다.
import argparse

from PIL import Image, ImageFilter

W = H = 200

# 흑백 2치화 전에 대비를 세운다. 전자종이는 중간 톤이 없어서, 원본의 밝기
# 범위를 그대로 두면 디더링이 지저분해진다. 40 이하는 검정, 150 이상은 흰색.
LEVEL_BLACK = 40
LEVEL_WHITE = 150

# 축소 전에 아주 약하게 흐린다. 디더링 노이즈가 줄어든다.
BLUR_RADIUS = 0.5

# (한 변, 머리 위 여백) — 크게 자를수록 인물이 작게 들어간다.
VARIANTS = {
    "tight": (300, 30),  # 얼굴 위주
    "mid":   (330, 40),  # 얼굴 + 어깨 조금
    "wide":  (370, 55),  # 상반신
}


def levels(img, black, white):
    scale = 255.0 / max(1, (white - black))
    return img.point(
        lambda p: 0 if p <= black else (255 if p >= white else int((p - black) * scale))
    )


def make(src, side, headroom, head_cx, head_top):
    """머리 중심에 가로를 맞추고, 머리 위로 headroom 만큼 여백을 둔다."""
    w, h = src.size
    left = int(round(head_cx - side / 2))
    left = max(0, min(left, w - side))  # 이미지 밖으로 나가지 않게
    top = max(0, min(head_top - headroom, h - side))
    crop = src.crop((left, top, left + side, top + side)).resize((W, H), Image.LANCZOS)
    img = levels(crop.filter(ImageFilter.GaussianBlur(BLUR_RADIUS)), LEVEL_BLACK, LEVEL_WHITE)
    return img.convert("1", dither=Image.FLOYDSTEINBERG), (left, top, side)


def main():
    ap = argparse.ArgumentParser(description="사진 -> 200x200 1비트 (ePaper 딥슬립 화면)")
    ap.add_argument("src", help="원본 이미지")
    ap.add_argument("out", help="결과를 넣을 디렉토리")
    ap.add_argument("--head-cx", type=float, default=237.6, help="머리 가로 중심 x")
    ap.add_argument("--head-top", type=int, default=165, help="머리 상단 y")
    args = ap.parse_args()

    src = Image.open(args.src).convert("L")
    made = {
        name: make(src, side, headroom, args.head_cx, args.head_top)
        for name, (side, headroom) in VARIANTS.items()
    }

    for name, (img, box) in made.items():
        img.save(f"{args.out}/f_{name}.png")
        print(f"{name}: crop={box}  (상자 중심 x={box[0] + box[2] / 2:.1f})")

    # 눈으로 고르라고 3배 확대해 나란히 붙인 시트를 남긴다.
    S = 3
    sheet = Image.new("1", (W * 3 * S + 40, H * S), 1)
    for i, name in enumerate(made):
        sheet.paste(made[name][0].resize((W * S, H * S), Image.NEAREST), (i * (W * S + 20), 0))
    sheet.save(f"{args.out}/compare.png")
    print(f"{args.out}/compare.png — 셋을 나란히 붙인 비교 시트")


if __name__ == "__main__":
    main()
