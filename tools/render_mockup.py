#!/usr/bin/env python3
"""Render a 240x240 mockup of the Clawd screen exactly matching ClawdManager::render().

Mirrors the layout in firmware/src/clawd/ClawdManager.cpp render() function.
Uses Arduino_GFX default font (6x8 px per char, scales with textSize).
"""

import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("pip install Pillow", file=sys.stderr)
    sys.exit(1)


W, H = 240, 240

BLACK = (0, 0, 0)
WHITE = (255, 255, 255)
RED = (248, 0, 0)
ORANGE = (255, 165, 0)
YELLOW = (255, 255, 0)
GREEN = (0, 252, 0)
GRAY_DARK = (60, 60, 60)
GRAY_LIGHT = (180, 180, 180)
BAR_BG = (50, 50, 60)


def color_for_pct(pct: int) -> tuple[int, int, int]:
    if pct >= 90:
        return RED
    if pct >= 70:
        return ORANGE
    if pct >= 40:
        return YELLOW
    return GREEN


def format_reset(minutes: int) -> str:
    if minutes <= 0:
        return "—"
    if minutes < 60:
        return f"{minutes}m"
    h = minutes // 60
    rem_m = minutes % 60
    if h < 24:
        return f"{h}h" if rem_m == 0 else f"{h}h {rem_m}m"
    d = h // 24
    rem_h = h % 24
    return f"{d}d" if rem_h == 0 else f"{d}d {rem_h}h"


# Arduino_GFX builtin font is 5x7 pixels with 1px spacing -> 6x8 cell.
# Use a monospace bitmap-ish font; PIL doesn't have it, so we approximate
# by loading a pixel-style font if available, otherwise the default font
# scaled up. The geometry below uses *exactly* the same char dims as the
# device (6*size, 8*size).
def get_font(size_mult: int) -> ImageFont.ImageFont:
    # We render text by drawing rectangles for each char width = 6*size,
    # height = 8*size. Using a real TTF would misalign, so we draw via
    # bitmap blits below for exact match.
    return ImageFont.load_default()


# Hand-rolled tiny 5x7 bitmap font for ASCII chars used in this screen.
# Each glyph: 5 columns x 7 rows, bit-packed top-to-bottom, MSB=leftmost.
# Only the chars we actually need.
GLYPHS = {
    " ": [0, 0, 0, 0, 0, 0, 0],
    "%": [
        0b11000_1,
        0b11001_0,
        0b00010_0,
        0b00100_0,
        0b01000_0,
        0b10011_0,
        0b00011_0,
    ],
    "—": [0, 0, 0, 0b11111_0, 0, 0, 0],
    ":": [0, 0b01100_0, 0b01100_0, 0, 0b01100_0, 0b01100_0, 0],
    "•": [0, 0, 0b01100_0, 0b01100_0, 0, 0, 0],
}


# Easier: encode 5-wide glyphs as 5-bit rows in a list of 7 ints (0..31).
def G(rows):
    return rows


FONT5x7 = {
    "0": G([0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110]),
    "1": G([0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110]),
    "2": G([0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111]),
    "3": G([0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110]),
    "4": G([0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010]),
    "5": G([0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110]),
    "6": G([0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110]),
    "7": G([0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000]),
    "8": G([0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110]),
    "9": G([0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100]),
    "%": G([0b11001, 0b11010, 0b00010, 0b00100, 0b01000, 0b01011, 0b10011]),
    "A": G([0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001]),
    "B": G([0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110]),
    "C": G([0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110]),
    "D": G([0b11100, 0b10010, 0b10001, 0b10001, 0b10001, 0b10010, 0b11100]),
    "E": G([0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111]),
    "G": G([0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01110]),
    "H": G([0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001]),
    "I": G([0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110]),
    "K": G([0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001]),
    "L": G([0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111]),
    "M": G([0b10001, 0b11011, 0b10101, 0b10001, 0b10001, 0b10001, 0b10001]),
    "N": G([0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001]),
    "O": G([0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110]),
    "P": G([0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000]),
    "R": G([0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001]),
    "S": G([0b01110, 0b10001, 0b10000, 0b01110, 0b00001, 0b10001, 0b01110]),
    "T": G([0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100]),
    "U": G([0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110]),
    "W": G([0b10001, 0b10001, 0b10001, 0b10001, 0b10101, 0b11011, 0b10001]),
    "Y": G([0b10001, 0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100]),
    "a": G([0b00000, 0b00000, 0b01110, 0b00001, 0b01111, 0b10001, 0b01111]),
    "b": G([0b10000, 0b10000, 0b11110, 0b10001, 0b10001, 0b10001, 0b11110]),
    "c": G([0b00000, 0b00000, 0b01110, 0b10000, 0b10000, 0b10001, 0b01110]),
    "d": G([0b00001, 0b00001, 0b01111, 0b10001, 0b10001, 0b10001, 0b01111]),
    "e": G([0b00000, 0b00000, 0b01110, 0b10001, 0b11111, 0b10000, 0b01110]),
    "f": G([0b00110, 0b01001, 0b01000, 0b11100, 0b01000, 0b01000, 0b01000]),
    "g": G([0b00000, 0b00000, 0b01111, 0b10001, 0b01111, 0b00001, 0b01110]),
    "h": G([0b10000, 0b10000, 0b10110, 0b11001, 0b10001, 0b10001, 0b10001]),
    "i": G([0b00100, 0b00000, 0b01100, 0b00100, 0b00100, 0b00100, 0b01110]),
    "k": G([0b10000, 0b10000, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010]),
    "l": G([0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110]),
    "m": G([0b00000, 0b00000, 0b11010, 0b10101, 0b10101, 0b10101, 0b10101]),
    "n": G([0b00000, 0b00000, 0b10110, 0b11001, 0b10001, 0b10001, 0b10001]),
    "o": G([0b00000, 0b00000, 0b01110, 0b10001, 0b10001, 0b10001, 0b01110]),
    "p": G([0b00000, 0b00000, 0b11110, 0b10001, 0b11110, 0b10000, 0b10000]),
    "r": G([0b00000, 0b00000, 0b10110, 0b11001, 0b10000, 0b10000, 0b10000]),
    "s": G([0b00000, 0b00000, 0b01111, 0b10000, 0b01110, 0b00001, 0b11110]),
    "t": G([0b01000, 0b01000, 0b11110, 0b01000, 0b01000, 0b01001, 0b00110]),
    "u": G([0b00000, 0b00000, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110]),
    "v": G([0b00000, 0b00000, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100]),
    "w": G([0b00000, 0b00000, 0b10001, 0b10001, 0b10101, 0b10101, 0b01010]),
    "y": G([0b00000, 0b00000, 0b10001, 0b10001, 0b01111, 0b00001, 0b01110]),
    " ": G([0, 0, 0, 0, 0, 0, 0]),
    "—": G([0, 0, 0, 0b11111, 0, 0, 0]),
    "-": G([0, 0, 0, 0b11111, 0, 0, 0]),
    ".": G([0, 0, 0, 0, 0, 0, 0b00100]),
    "/": G([0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0, 0]),
    "_": G([0, 0, 0, 0, 0, 0, 0b11111]),
    ":": G([0, 0b00100, 0b00100, 0, 0b00100, 0b00100, 0]),
}


def draw_char(img: Image.Image, x: int, y: int, ch: str, size: int, color: tuple) -> int:
    rows = FONT5x7.get(ch)
    if rows is None:
        # Fallback: tiny rectangle
        ImageDraw.Draw(img).rectangle([x, y, x + 5 * size - 1, y + 7 * size - 1], outline=color)
        return 6 * size
    px = img.load()
    for ry, row in enumerate(rows):
        for rx in range(5):
            if (row >> (4 - rx)) & 1:
                # plot a size*size block
                for dy in range(size):
                    for dx in range(size):
                        xx = x + rx * size + dx
                        yy = y + ry * size + dy
                        if 0 <= xx < W and 0 <= yy < H:
                            px[xx, yy] = color
    return 6 * size  # 5 + 1 spacing, all * size


def draw_text(img: Image.Image, x: int, y: int, text: str, size: int, color: tuple) -> int:
    cur_x = x
    for ch in text:
        cur_x += draw_char(img, cur_x, y, ch, size, color)
    return cur_x


def draw_text_centered(img: Image.Image, y: int, text: str, size: int, color: tuple) -> None:
    text_w = len(text) * 6 * size
    x = (W - text_w) // 2
    if x < 0:
        x = 0
    draw_text(img, x, y, text, size, color)


def draw_bar(draw: ImageDraw.ImageDraw, x: int, y: int, w: int, h: int, pct: int, fg: tuple) -> None:
    draw.rectangle([x, y, x + w - 1, y + h - 1], fill=BAR_BG)
    fill = (w * pct) // 100
    if fill < 0:
        fill = 0
    if fill > w:
        fill = w
    if fill > 0:
        draw.rectangle([x, y, x + fill - 1, y + h - 1], fill=fg)
    draw.rectangle([x, y, x + w - 1, y + h - 1], outline=WHITE)


def render(session_pct: int, session_reset_min: int, weekly_pct: int, weekly_reset_min: int,
           status: str, output_path: Path, clock: str = "14:46:58", ago: str = "3s ago") -> None:
    img = Image.new("RGB", (W, H), BLACK)
    draw = ImageDraw.Draw(img)

    # Header: "Clawdmeter" left + clock right
    draw_text(img, 10, 6, "Clawdmeter", 2, WHITE)
    draw_text(img, W - 48 - 4, 12, clock, 1, GRAY_LIGHT)
    draw.line([(10, 28), (W - 11, 28)], fill=GRAY_DARK)

    # Session block label
    draw_text(img, 10, 36, "5h SESSION", 1, GRAY_LIGHT)

    # Session big number — center
    s_pct_str = f"{session_pct}%"
    draw_text_centered(img, 50, s_pct_str, 5, color_for_pct(session_pct))

    # Session bar
    draw_bar(draw, 10, 92, W - 20, 10, session_pct, color_for_pct(session_pct))

    # Session reset
    label = "resets in " + format_reset(session_reset_min)
    draw_text(img, 10, 108, label, 1, GRAY_LIGHT)

    # Weekly block label
    draw_text(img, 10, 128, "7d WEEKLY", 1, GRAY_LIGHT)
    draw_text_centered(img, 138, f"{weekly_pct}%", 3, color_for_pct(weekly_pct))
    draw_bar(draw, 10, 168, W - 20, 8, weekly_pct, color_for_pct(weekly_pct))
    draw_text(img, 10, 180, "resets in " + format_reset(weekly_reset_min), 1, GRAY_LIGHT)

    # Footer
    draw.line([(10, 212), (W - 11, 212)], fill=GRAY_DARK)
    status_color = GREEN
    if status == "allowed_warning":
        status_color = ORANGE
    elif status not in ("allowed", "ok"):
        status_color = RED
    draw.ellipse([10, 220, 18, 228], fill=status_color)
    footer_text = f"{status} - {ago}"
    draw_text(img, 22, 220, footer_text, 1, GRAY_LIGHT)

    # Upscale 3x for viewing clarity
    upscaled = img.resize((W * 3, H * 3), Image.NEAREST)
    upscaled.save(output_path)
    print(f"Wrote {output_path} ({W * 3}x{H * 3}, native {W}x{H})")


def render_error(message: str, hint: str, output_path: Path) -> None:
    img = Image.new("RGB", (W, H), BLACK)
    draw_text_centered(img, 80, "CLAUDE CODE", 2, WHITE)
    draw_text_centered(img, 120, message, 2, RED)
    draw_text_centered(img, 150, hint, 1, GRAY_LIGHT)
    upscaled = img.resize((W * 3, H * 3), Image.NEAREST)
    upscaled.save(output_path)
    print(f"Wrote {output_path}")


if __name__ == "__main__":
    out_dir = Path(__file__).parent.parent / "screenshots"
    out_dir.mkdir(exist_ok=True)

    # Sample 1: typical mid-day usage
    render(
        session_pct=45,
        session_reset_min=135,  # 2h 15m
        weekly_pct=28,
        weekly_reset_min=7200,  # 5d
        status="allowed",
        output_path=out_dir / "mockup_normal.png",
    )

    # Sample 2: high usage warning
    render(
        session_pct=87,
        session_reset_min=43,
        weekly_pct=65,
        weekly_reset_min=4320,  # 3d
        status="allowed_warning",
        output_path=out_dir / "mockup_warning.png",
    )

    # Sample 3: maxed out
    render(
        session_pct=98,
        session_reset_min=12,
        weekly_pct=92,
        weekly_reset_min=2880,  # 2d
        status="rate_limited",
        output_path=out_dir / "mockup_limited.png",
    )

    # Sample 4: no token
    render_error(
        "no token",
        "open /clawd.html",
        out_dir / "mockup_no_token.png",
    )
