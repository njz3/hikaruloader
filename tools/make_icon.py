#!/usr/bin/env python3
#
# Hikaruloader
# Copyright (C) 2026, Benjamin Maurin (njz3)
#
# Hikaruloader is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Hikaruloader is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Hikaruloader.  If not, see <http://www.gnu.org/licenses/>.

"""The program's icon, "HL" on a night-blue rounded square: each size drawn
for itself (64 with its ring, glow and spark, 32 lighter, 16 the letters
alone),
eight times larger, then scaled down. Writes data/hikaruloader.ico (16, 32,
48, 64, 256) and data/hikaruloader.png (256), which src/hl/hikaruloader.rc
puts in the program. Needs Pillow and a bold italic font (Segoe UI Black
Italic on Windows).

Usage: tools/make_icon.py [OUT_DIR]"""

import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONTS = ["seguibli.ttf", "arialbi.ttf", "DejaVuSans-BoldOblique.ttf"]
SCALE = 8


def font(size):
    for name in FONTS:
        for folder in ("", os.path.join(os.environ.get("WINDIR", "C:\\Windows"), "Fonts")):
            try:
                return ImageFont.truetype(os.path.join(folder, name), size)
            except OSError:
                pass
    sys.exit("make_icon.py: no bold italic font found")


def gradient(size, top, bottom):
    """A vertical gradient, top to bottom"""
    img = Image.new("RGBA", (1, size))
    for y in range(size):
        t = y / max(1, size - 1)
        img.putpixel((0, y), tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))
    return img.resize((size, size))


def diagonal(size, a, b):
    """A diagonal gradient, a at the bottom left, b at the top right"""
    img = Image.new("RGBA", (size, size))
    px = img.load()
    for y in range(size):
        for x in range(size):
            t = min(1.0, max(0.0, (x + (size - 1 - y)) / (2.0 * (size - 1)) * 1.6 - 0.3))
            px[x, y] = tuple(int(c + (d - c) * t) for c, d in zip(a, b)) + (255,)
    return img


def draw(size):
    s = size * SCALE
    detail = 2 if size >= 48 else 1 if size >= 32 else 0
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))

    # The rounded square: night blue, lighter at the top, a blue border
    margin = round(s * (0.03 if detail else 0.0))
    radius = round(s * (0.22 if detail else 0.18))
    box = (margin, margin, s - 1 - margin, s - 1 - margin)
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle(box, radius, fill=255)
    img.paste(gradient(s, (34, 58, 112), (8, 16, 42)), (0, 0), mask)
    glow = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse((s * 0.15, s * 0.05, s * 0.85, s * 0.75), fill=(60, 120, 220, 70))
    glow = glow.filter(ImageFilter.GaussianBlur(s * 0.08))
    img = Image.alpha_composite(img, Image.composite(glow, Image.new("RGBA", (s, s)), mask))
    border = max(SCALE, round(s * 0.035))
    ImageDraw.Draw(img).rounded_rectangle(box, radius, outline=(78, 132, 214, 255), width=border)
    if detail:
        gloss = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        ImageDraw.Draw(gloss).rounded_rectangle((box[0] + border, box[1] + border, box[2] - border,
                                                 s * 0.42), radius * 0.8, fill=(255, 255, 255, 22))
        img = Image.alpha_composite(img, gloss)

    # The ring, with its dots
    if detail:
        c, r = (s * 0.5, s * 0.5), s * 0.33
        ring = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        d = ImageDraw.Draw(ring)
        d.ellipse((c[0] - r, c[1] - r, c[0] + r, c[1] + r), outline=(96, 170, 255, 150),
                  width=max(SCALE, round(s * 0.018)))
        if detail >= 2:
            for ax, ay in ((-0.71, -0.71), (0.71, 0.71), (-1, 0), (1, 0), (0.71, -0.71)):
                x, y, w = c[0] + ax * r, c[1] + ay * r, s * 0.022
                d.ellipse((x - w, y - w, x + w, y + w), fill=(150, 210, 255, 220))
        img = Image.alpha_composite(img, ring.filter(ImageFilter.GaussianBlur(SCALE * 0.4)))

    # "HL", italic, outlined, glowing: the H blue, the L gold, each lighter
    # at the top, a gold spark on the H's right stem
    f = font(round(s * (0.50 if detail else 0.74)))
    stroke = round(s * (0.03 if detail else 0.045))
    probe = ImageDraw.Draw(img)
    left, top, right, bottom = probe.textbbox((0, 0), "HL", font=f, stroke_width=stroke)
    x = (s - (right - left)) / 2 - left
    y = (s - (bottom - top)) / 2 - top + s * (0.0 if detail else 0.02)
    x_l = x + probe.textlength("H", font=f) - s * 0.01
    letters = {}
    outline = Image.new("L", (s, s), 0)
    for ch, at in (("H", x), ("L", x_l)):
        m = Image.new("L", (s, s), 0)
        ImageDraw.Draw(m).text((at, y), ch, font=f, fill=255)
        letters[ch] = m
        ImageDraw.Draw(outline).text((at, y), ch, font=f, fill=255, stroke_width=stroke, stroke_fill=255)
    if detail:
        halo = Image.new("RGBA", (s, s), (110, 190, 255, 0))
        halo.putalpha(outline.filter(ImageFilter.GaussianBlur(s * 0.05)).point(lambda v: v * 0.8))
        img = Image.alpha_composite(img, halo)
    img.paste(Image.new("RGBA", (s, s), (6, 12, 34, 255)), (0, 0), outline)
    glyph_top, glyph_bottom = y + top + stroke, y + bottom - stroke
    span = max(1, round(glyph_bottom - glyph_top))
    blue = gradient(span, (120, 205, 255), (20, 84, 230)).resize((s, span))
    gold = gradient(span, (255, 240, 130), (255, 150, 20)).resize((s, span))
    for ch, fill in (("H", blue), ("L", gold)):
        layer = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        layer.paste(fill, (0, round(glyph_top)))
        img.paste(layer, (0, 0), letters["H" if ch == "H" else "L"])
    if detail >= 2:
        # The spark: a gold zigzag across the H's upper right (a speck
        # below 48)
        w, h = (right - left) * 0.18, (bottom - top) * 0.42
        bx, by = x + probe.textlength("H", font=f) * 0.72, y + top + (bottom - top) * 0.06
        bolt = [(bx + w * 0.55, by), (bx + w * 0.05, by + h * 0.55), (bx + w * 0.42, by + h * 0.50),
                (bx + w * 0.10, by + h), (bx + w * 0.95, by + h * 0.38), (bx + w * 0.55, by + h * 0.42),
                (bx + w * 0.90, by)]
        spark = Image.new("RGBA", (s, s), (0, 0, 0, 0))
        ImageDraw.Draw(spark).polygon(bolt, fill=(255, 220, 60, 255), outline=(6, 12, 34, 255),
                                      width=max(SCALE // 2, round(s * 0.012)))
        img = Image.alpha_composite(img, spark)

    return img.resize((size, size), Image.LANCZOS)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data")
    sizes = [16, 32, 48, 64, 256]
    images = [draw(n) for n in sizes]
    images[-1].save(os.path.join(out, "hikaruloader.ico"), sizes=[(n, n) for n in sizes],
                    append_images=images[:-1])
    images[-1].save(os.path.join(out, "hikaruloader.png"))
    print("wrote", os.path.join(out, "hikaruloader.ico"))


if __name__ == "__main__":
    main()
