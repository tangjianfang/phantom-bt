#!/usr/bin/env python3
"""Generate res/app.ico for the phantom-bt GUI.

Visual concept (matches the instrument-panel GUI theme):
- rounded graphite tile (SURFACE #1F2328 -> BG #17191D vertical gradient)
- phosphor-teal (#3FD0B6) Bluetooth rune with a soft glow (oscilloscope feel)
- an offset ghost echo of the rune conveys "phantom" (virtual device)

Run:  python tools/make_icon.py   (writes res/app.ico)
"""
from PIL import Image, ImageDraw, ImageFilter

TEAL = (63, 208, 182)       # ACCENT phosphor teal
GHOST = (63, 208, 182)      # echo, low alpha applied per-size
SURFACE = (35, 39, 44)      # gradient top
BG = (23, 25, 29)           # gradient bottom
STROKE = (46, 52, 59)       # tile border


def rune_points(cx, cy, r):
    """Bluetooth rune geometry: vertical stem + full diamond outline.

    The canonical Bluetooth symbol is a diamond (top, left, right, bottom
    vertices) with a stem running from the top vertex through to the bottom
    vertex. Returns point-pairs (x0,y0,x1,y1) to stroke as lines.
    """
    k = r * 0.58  # horizontal reach of the diamond
    return [
        (cx, cy - r, cx, cy + r),   # stem: top vertex to bottom vertex
        (cx, cy - r, cx - k, cy),   # top to left
        (cx - k, cy, cx, cy + r),   # left to bottom
        (cx, cy - r, cx + k, cy),   # top to right
        (cx + k, cy, cx, cy + r),   # right to bottom
    ]


def draw_rune(dr, pts, width, fill):
    """Stroke the rune with round caps and joints."""
    width = max(1, int(round(width)))
    rad = width / 2.0
    for x0, y0, x1, y1 in pts:  # round caps
        for (px, py) in ((x0, y0), (x1, y1)):
            dr.ellipse((px - rad, py - rad, px + rad, py + rad), fill=fill)
    for x0, y0, x1, y1 in pts:
        dr.line((x0, y0, x1, y1), fill=fill, width=width)


def tile(size, radius_ratio, gradient=True, border=True):
    """Rounded graphite tile with vertical gradient."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    dr = ImageDraw.Draw(img)
    rad = int(size * radius_ratio)
    if gradient:
        for y in range(size):
            t = y / max(1, size - 1)
            c = tuple(int(SURFACE[i] + (BG[i] - SURFACE[i]) * t) for i in range(3)) + (255,)
            dr.line((0, y, size, y), fill=c)
        mask = Image.new("L", (size, size), 0)
        ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), rad, fill=255)
        img.putalpha(mask)
    else:
        dr.rounded_rectangle((0, 0, size - 1, size - 1), rad, fill=BG + (255,))
    if border:
        dr.rounded_rectangle((0, 0, size - 1, size - 1), rad, outline=STROKE + (255,), width=max(1, size // 128))
    return img


def render(size):
    """Render the app icon at a given pixel size."""
    s = 8  # supersample factor for crisp diagonals
    big = size * s
    img = tile(big, radius_ratio=0.225, gradient=(size >= 32))
    dr = ImageDraw.Draw(img)

    cx, cy = big / 2, big / 2
    if size >= 48:
        # full detail: ghost echo (up-right, thinner) + glow + solid rune
        r = big * 0.30
        w = big * 0.062
        off = big * 0.055
        echo = rune_points(cx + off, cy - off * 0.6, r * 0.97)
        overlay = Image.new("RGBA", (big, big), (0, 0, 0, 0))
        draw_rune(ImageDraw.Draw(overlay), echo, w * 0.8, GHOST + (70,))
        img = Image.alpha_composite(img, overlay)
        # glow: blurred teal rune underneath the solid one
        glow = Image.new("RGBA", (big, big), (0, 0, 0, 0))
        draw_rune(ImageDraw.Draw(glow), rune_points(cx, cy, r), w * 2.2, TEAL + (110,))
        glow = glow.filter(ImageFilter.GaussianBlur(big * 0.045))
        img = Image.alpha_composite(img, glow)
        draw_rune(ImageDraw.Draw(img), rune_points(cx, cy, r), w, TEAL + (255,))
    elif size == 32:
        r = big * 0.32
        w = big * 0.085
        off = big * 0.05
        echo = rune_points(cx + off, cy - off * 0.6, r * 0.97)
        draw_rune(dr, echo, w * 0.85, GHOST + (80,))
        draw_rune(dr, rune_points(cx, cy, r), w, TEAL + (255,))
    else:  # 16px: max simplicity, no echo/glow/gradient
        r = big * 0.34
        w = big * 0.11
        draw_rune(dr, rune_points(cx, cy, r), w, TEAL + (255,))

    return img.resize((size, size), Image.LANCZOS)


def main():
    sizes = [256, 64, 48, 32, 16]
    base = render(256)
    base.save("res/app.ico", format="ICO",
              sizes=[(s, s) for s in sizes], append_images=[render(s) for s in sizes[1:]])
    base.save("res/app-256.png")
    print("wrote res/app.ico + res/app-256.png")


if __name__ == "__main__":
    main()
