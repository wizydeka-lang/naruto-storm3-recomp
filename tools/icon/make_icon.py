#!/usr/bin/env python3
"""Draws the app's own launcher icon (an original design, no game art):
a white four-point shuriken on an orange-to-red gradient tile.
Writes android/app/src/main/res/mipmap-*/ic_launcher.png and a 512 px copy
for the README/releases. Usage: python3 tools/icon/make_icon.py"""
import math
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
SIZE = 1024


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def draw():
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    # Gradient tile with rounded corners.
    grad = Image.new("RGB", (SIZE, SIZE))
    px = grad.load()
    top, bottom = (255, 150, 30), (205, 45, 20)
    for y in range(SIZE):
        for x in range(SIZE):
            t = min(1.0, max(0.0, (x * 0.35 + y * 0.65) / SIZE))
            px[x, y] = lerp(top, bottom, t)
    mask = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, SIZE - 1, SIZE - 1), radius=230, fill=255)
    img.paste(grad, (0, 0), mask)

    c = SIZE / 2
    # Four-point shuriken: long points, curved inner notches.
    outer, inner = 400, 105
    points = []
    for k in range(8):
        angle = math.radians(k * 45 - 90 + 15)
        r = outer if k % 2 == 0 else inner
        points.append((c + r * math.cos(angle), c + r * math.sin(angle)))
    shadow = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    ImageDraw.Draw(shadow).polygon([(x + 18, y + 24) for x, y in points], fill=(80, 10, 0, 140))
    img.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(22)))
    star = ImageDraw.Draw(img)
    star.polygon(points, fill=(250, 248, 242, 255))
    # Edge shading on one side of each blade for depth.
    for k in range(0, 8, 2):
        tip = points[k]
        side = points[(k + 1) % 8]
        star.polygon([(c, c), tip, side], fill=(222, 218, 210, 255))
    # Centre ring.
    r = 70
    star.ellipse((c - r, c - r, c + r, c + r), fill=(205, 45, 20, 255))
    r = 34
    star.ellipse((c - r, c - r, c + r, c + r), fill=(250, 248, 242, 255))
    return img


def main():
    icon = draw()
    sizes = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
    for name, size in sizes.items():
        out = ROOT / f"android/app/src/main/res/mipmap-{name}/ic_launcher.png"
        icon.resize((size, size), Image.LANCZOS).save(out)
    icon.resize((512, 512), Image.LANCZOS).save(ROOT / "tools/icon/icon-512.png")
    print("icons written")


if __name__ == "__main__":
    main()
