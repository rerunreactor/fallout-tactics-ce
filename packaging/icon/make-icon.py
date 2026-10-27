#!/usr/bin/env python3
"""Draws the Fallout Tactics CE icon and writes the platform files.

    python3 packaging/icon/make-icon.py

Three amber chevrons on a dark olive tile: simple shapes, nothing from the game.
Writes packaging/macos/AppIcon.icns (with macOS's own iconutil),
packaging/windows/fotgame.ico and packaging/icon/icon.png, a 1024-pixel preview.
Needs Pillow.
"""

import os
import shutil
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
PACKAGING = os.path.dirname(HERE)

SS = 4            # drawn at 4x, then scaled down, for smooth edges
SIZE = 1024
TOP = (74, 74, 50)        # tile gradient, top
BOTTOM = (31, 32, 22)     # tile gradient, bottom
RIM = (112, 110, 74)      # thin inner rim
AMBER = (240, 168, 48)


def tile(margin, radius):
    """The icon at SIZE x SIZE: a rounded tile inset by `margin`, with chevrons."""
    n = SIZE * SS
    m = margin * SS
    side = n - 2 * m

    # Vertical gradient, cut to the rounded tile.
    grad = Image.new("RGB", (n, n))
    px = ImageDraw.Draw(grad)
    for y in range(n):
        t = min(max((y - m) / max(side, 1), 0.0), 1.0)
        c = tuple(round(TOP[i] + (BOTTOM[i] - TOP[i]) * t) for i in range(3))
        px.line([(0, y), (n, y)], fill=c)
    mask = Image.new("L", (n, n), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [m, m, n - m - 1, n - m - 1], radius=radius * SS, fill=255)
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    img.paste(grad, (0, 0), mask)

    draw = ImageDraw.Draw(img)
    rim = 6 * SS
    draw.rounded_rectangle(
        [m + rim, m + rim, n - m - 1 - rim, n - m - 1 - rim],
        radius=(radius - 6) * SS, outline=RIM + (255,), width=3 * SS)

    # Three chevrons, nested and centred.
    w = side * 0.56         # width
    h = side * 0.20         # rise from the arms to the apex
    t = side * 0.085        # band thickness, measured vertically
    g = side * 0.055        # gap between bands
    pitch = t + g
    total = h + t + 2 * pitch
    cx = n / 2
    y0 = n / 2 - total / 2
    for k in range(3):
        top = y0 + k * pitch
        draw.polygon([
            (cx - w / 2, top + h), (cx, top), (cx + w / 2, top + h),
            (cx + w / 2, top + h + t), (cx, top + t), (cx - w / 2, top + h + t),
        ], fill=AMBER + (255,))

    return img.resize((SIZE, SIZE), Image.LANCZOS)


def main():
    # macOS draws icons on its own grid: an 824-pixel tile in a 1024 canvas.
    mac = tile(margin=100, radius=185)
    # Windows shows the image as it is, so the tile fills more of it.
    win = tile(margin=24, radius=170)

    mac.save(os.path.join(HERE, "icon.png"))

    iconutil = shutil.which("iconutil")
    if iconutil is None:
        sys.exit("iconutil not found: AppIcon.icns has to be made on a Mac")
    with tempfile.TemporaryDirectory() as tmp:
        iconset = os.path.join(tmp, "AppIcon.iconset")
        os.mkdir(iconset)
        for pts in (16, 32, 128, 256, 512):
            mac.resize((pts, pts), Image.LANCZOS).save(
                os.path.join(iconset, f"icon_{pts}x{pts}.png"))
            mac.resize((pts * 2, pts * 2), Image.LANCZOS).save(
                os.path.join(iconset, f"icon_{pts}x{pts}@2x.png"))
        subprocess.run([iconutil, "-c", "icns", iconset, "-o",
                        os.path.join(PACKAGING, "macos", "AppIcon.icns")], check=True)

    win.save(os.path.join(PACKAGING, "windows", "fotgame.ico"), format="ICO",
             sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])


if __name__ == "__main__":
    main()
