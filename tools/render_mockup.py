#!/usr/bin/env python3
"""Render 240x240 mockups matching the firmware exactly.

Mirrors:
  - firmware/src/clawd/ClawdManager.cpp::render() / renderHeader() / renderFooter()
  - firmware/src/display/DisplayManager.cpp::drawStartup()

Arduino_GFX builtin font cell is 6x8 px per char (5x7 glyph + 1 px spacing),
scaled by textSize. We replicate that exact geometry with a hand-rolled 5x7
bitmap font so the mockups line up to within a pixel of the device.
"""

import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw
except ImportError:
    print("pip install Pillow", file=sys.stderr)
    sys.exit(1)


W, H = 240, 240
UPSCALE = 3

# RGB565 palette from the firmware, expanded to RGB888.
BLACK       = (0, 0, 0)                # 0x0000
WHITE       = (255, 255, 255)          # 0xFFFF
RED         = (248, 0, 0)              # 0xF800
ORANGE      = (255, 164, 0)            # 0xFD20
YELLOW      = (255, 255, 0)            # 0xFFE0
GREEN       = (0, 252, 0)              # 0x07E0
BLUE        = (0, 0, 248)              # 0x001F
GRAY_LIGHT  = (189, 190, 189)          # 0xBDF7
GRAY_MID    = (132, 130, 132)          # 0x8410
BAR_BG      = (57, 60, 57)             # 0x39E7


def color_for_pct(pct: int) -> tuple:
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


# 5x7 bitmap glyphs, 5-bit rows MSB=left. Covers every char produced by the
# firmware render paths.
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
    "A": G([0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001]),
    "B": G([0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110]),
    "C": G([0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110]),
    "D": G([0b11100, 0b10010, 0b10001, 0b10001, 0b10001, 0b10010, 0b11100]),
    "E": G([0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111]),
    "F": G([0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000]),
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
    "V": G([0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100]),
    "W": G([0b10001, 0b10001, 0b10001, 0b10001, 0b10101, 0b11011, 0b10001]),
    "X": G([0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001]),
    "Y": G([0b10001, 0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100]),
    "Z": G([0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111]),
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
    "x": G([0b00000, 0b00000, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001]),
    "y": G([0b00000, 0b00000, 0b10001, 0b10001, 0b01111, 0b00001, 0b01110]),
    "z": G([0b00000, 0b00000, 0b11111, 0b00010, 0b00100, 0b01000, 0b11111]),
    " ": G([0, 0, 0, 0, 0, 0, 0]),
    "%": G([0b11001, 0b11010, 0b00010, 0b00100, 0b01000, 0b01011, 0b10011]),
    "—": G([0, 0, 0, 0b11111, 0, 0, 0]),
    "-": G([0, 0, 0, 0b11111, 0, 0, 0]),
    ".": G([0, 0, 0, 0, 0, 0, 0b00100]),
    "/": G([0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0, 0]),
    "_": G([0, 0, 0, 0, 0, 0, 0b11111]),
    ":": G([0, 0b00100, 0b00100, 0, 0b00100, 0b00100, 0]),
    "(": G([0b00010, 0b00100, 0b01000, 0b01000, 0b01000, 0b00100, 0b00010]),
    ")": G([0b01000, 0b00100, 0b00010, 0b00010, 0b00010, 0b00100, 0b01000]),
}


def draw_char(img: Image.Image, x: int, y: int, ch: str, size: int, color: tuple) -> int:
    rows = FONT5x7.get(ch)
    if rows is None:
        ImageDraw.Draw(img).rectangle([x, y, x + 5 * size - 1, y + 7 * size - 1], outline=color)
        return 6 * size
    px = img.load()
    for ry, row in enumerate(rows):
        for rx in range(5):
            if (row >> (4 - rx)) & 1:
                for dy in range(size):
                    for dx in range(size):
                        xx = x + rx * size + dx
                        yy = y + ry * size + dy
                        if 0 <= xx < img.width and 0 <= yy < img.height:
                            px[xx, yy] = color
    return 6 * size


def draw_text(img: Image.Image, x: int, y: int, text: str, size: int, color: tuple) -> int:
    cur_x = x
    for ch in text:
        cur_x += draw_char(img, cur_x, y, ch, size, color)
    return cur_x


def draw_text_centered(img: Image.Image, y: int, text: str, size: int, color: tuple,
                       width: int = W) -> None:
    text_w = len(text) * 6 * size
    x = max(0, (width - text_w) // 2)
    draw_text(img, x, y, text, size, color)


def draw_bar(draw: ImageDraw.ImageDraw, x: int, y: int, w: int, h: int, pct: int, fg: tuple) -> None:
    draw.rectangle([x, y, x + w - 1, y + h - 1], fill=BAR_BG)
    fill = max(0, min(w, (w * pct) // 100))
    if fill > 0:
        draw.rectangle([x, y, x + fill - 1, y + h - 1], fill=fg)
    draw.rectangle([x, y, x + w - 1, y + h - 1], outline=WHITE)


# Layout constants from ClawdManager.cpp
HEADER_H = 34
FOOTER_Y = 213
EDGE = 10


def render_header(img: Image.Image, clock: str) -> None:
    draw = ImageDraw.Draw(img)
    draw.rectangle([0, 0, W - 1, HEADER_H - 1], fill=BLACK)
    title = "ClaudeCubeMeter"
    title_w = len(title) * 6 * 2
    draw_text(img, (W - title_w) // 2, 2, title, 2, WHITE)
    if clock:
        clock_w = 8 * 6
        draw_text(img, (W - clock_w) // 2, 22, clock, 1, GRAY_MID)
    draw.line([(EDGE, HEADER_H - 2), (W - EDGE - 1, HEADER_H - 2)], fill=BAR_BG)


def render_footer(img: Image.Image, status: str, ago: str) -> None:
    draw = ImageDraw.Draw(img)
    draw.rectangle([0, FOOTER_Y, W - 1, H - 1], fill=BLACK)
    draw.line([(EDGE, FOOTER_Y - 1), (W - EDGE - 1, FOOTER_Y - 1)], fill=BAR_BG)
    if status == "allowed_warning":
        dot_color = ORANGE
    elif status in ("allowed", "ok"):
        dot_color = GREEN
    else:
        dot_color = RED
    draw.ellipse([14 - 3, 224 - 3, 14 + 3, 224 + 3], fill=dot_color)
    draw_text(img, 22, 220, f"{status} - {ago}", 1, GRAY_LIGHT)


def render_main(session_pct: int, session_reset_min: int, weekly_pct: int,
                weekly_reset_min: int, status: str, clock: str, ago: str) -> Image.Image:
    img = Image.new("RGB", (W, H), BLACK)
    draw = ImageDraw.Draw(img)

    render_header(img, clock)

    draw_text(img, EDGE, 36, "5h SESSION", 1, GRAY_MID)
    draw_text_centered(img, 50, f"{session_pct}%", 5, color_for_pct(session_pct))
    draw_bar(draw, EDGE, 92, W - 2 * EDGE, 10, session_pct, color_for_pct(session_pct))
    draw_text(img, EDGE, 108, f"resets in {format_reset(session_reset_min)}", 1, GRAY_LIGHT)

    draw_text(img, EDGE, 128, "7d WEEKLY", 1, GRAY_MID)
    draw_text_centered(img, 138, f"{weekly_pct}%", 3, color_for_pct(weekly_pct))
    draw_bar(draw, EDGE, 168, W - 2 * EDGE, 8, weekly_pct, color_for_pct(weekly_pct))
    draw_text(img, EDGE, 180, f"resets in {format_reset(weekly_reset_min)}", 1, GRAY_LIGHT)

    render_footer(img, status, ago)
    return img


def render_no_token() -> Image.Image:
    img = Image.new("RGB", (W, H), BLACK)
    draw_text_centered(img, 80, "Claude Code", 2, WHITE)
    draw_text_centered(img, 120, "no token", 2, RED)
    draw_text_centered(img, 150, "open /clawd.html", 1, GRAY_MID)
    return img


def render_waiting() -> Image.Image:
    img = Image.new("RGB", (W, H), BLACK)
    draw_text_centered(img, 70, "ClaudeCubeMeter", 2, WHITE)
    draw_text_centered(img, 110, "waiting for", 2, YELLOW)
    draw_text_centered(img, 135, "proxy data", 2, YELLOW)
    draw_text_centered(img, 170, "run claude_proxy.py", 1, GRAY_MID)
    return img


def render_polling_failed(err: str, http_code: int) -> Image.Image:
    img = Image.new("RGB", (W, H), BLACK)
    draw_text_centered(img, 70, "ClaudeCubeMeter", 2, WHITE)
    draw_text_centered(img, 110, "polling failed", 2, RED)
    draw_text_centered(img, 140, err, 1, GRAY_MID)
    if http_code:
        draw_text_centered(img, 160, f"HTTP {http_code}", 1, GRAY_MID)
    return img


def render_splash(ip: str, version: str) -> Image.Image:
    """Matches DisplayManager::drawStartup() — boot screen visible ~5 s."""
    img = Image.new("RGB", (W, H), BLACK)
    draw = ImageDraw.Draw(img)

    title = "ClaudeCubeMeter"
    title_w = len(title) * 6 * 2
    draw_text(img, (W - title_w) // 2, 30, title, 2, WHITE)
    draw_text_centered(img, 60, "Claude Cube Meter", 1, GRAY_LIGHT)
    draw_text_centered(img, 75, "by Dariusz Koryto", 1, GRAY_LIGHT)
    draw_text_centered(img, 100, version, 1, GRAY_MID)

    ip_str = f"IP: {ip}"
    ip_w = len(ip_str) * 6 * 2
    draw_text(img, (W - ip_w) // 2, 130, ip_str, 2, WHITE)

    box, gap, box_y = 36, 16, 180
    total_w = box * 3 + gap * 2
    box_x = (W - total_w) // 2
    draw.rectangle([box_x, box_y, box_x + box - 1, box_y + box - 1], fill=RED)
    draw.rectangle([box_x + box + gap, box_y, box_x + 2 * box + gap - 1, box_y + box - 1], fill=GREEN)
    draw.rectangle([box_x + 2 * (box + gap), box_y, box_x + 3 * box + 2 * gap - 1, box_y + box - 1], fill=BLUE)
    return img


def save(img: Image.Image, path: Path) -> None:
    img.resize((img.width * UPSCALE, img.height * UPSCALE), Image.NEAREST).save(path)
    print(f"Wrote {path}")


def render_grid(panels: list, out_path: Path) -> None:
    """Lay out the screen panels in a 2x4 grid with captions, native 240x240 each."""
    cols, rows = 4, 2
    cell_pad = 12
    label_h = 22
    cell_w = W + 2 * cell_pad
    cell_h = H + 2 * cell_pad + label_h
    grid_w = cell_w * cols
    grid_h = cell_h * rows

    grid = Image.new("RGB", (grid_w, grid_h), (24, 24, 24))
    for i, (caption, panel) in enumerate(panels):
        cx = (i % cols) * cell_w
        cy = (i // cols) * cell_h
        grid.paste(panel, (cx + cell_pad, cy + cell_pad + label_h))
        caption_w = len(caption) * 6 * 2
        draw_text(grid, cx + (cell_w - caption_w) // 2, cy + 6, caption, 2, WHITE)

    save(grid, out_path)


if __name__ == "__main__":
    out_dir = Path(__file__).parent.parent / "screenshots"
    out_dir.mkdir(exist_ok=True)

    normal = render_main(45, 135, 28, 7200, "allowed", "14:46:58", "3s ago")
    save(normal, out_dir / "mockup_normal.png")

    warning = render_main(87, 43, 65, 4320, "allowed_warning", "18:02:11", "12s ago")
    save(warning, out_dir / "mockup_warning.png")

    limited = render_main(98, 12, 92, 2880, "rate_limited", "22:17:04", "27s ago")
    save(limited, out_dir / "mockup_limited.png")

    live = render_main(16, 205, 10, 6420, "allowed", "16:47:46", "4s ago")
    save(live, out_dir / "mockup_live.png")

    save(render_no_token(), out_dir / "mockup_no_token.png")
    save(render_waiting(), out_dir / "mockup_waiting.png")
    save(render_polling_failed("unauthorized", 401), out_dir / "mockup_error.png")
    save(render_splash("192.168.10.43", "v1.0.0"), out_dir / "mockup_splash.png")

    render_grid(
        [
            ("splash",  render_splash("192.168.10.43", "v1.0.0")),
            ("no_token", render_no_token()),
            ("waiting", render_waiting()),
            ("error",   render_polling_failed("unauthorized", 401)),
            ("normal",  render_main(45, 135, 28, 7200, "allowed", "14:46:58", "3s ago")),
            ("warning", render_main(87, 43, 65, 4320, "allowed_warning", "18:02:11", "12s ago")),
            ("limited", render_main(98, 12, 92, 2880, "rate_limited", "22:17:04", "27s ago")),
            ("live",    render_main(16, 205, 10, 6420, "allowed", "16:47:46", "4s ago")),
        ],
        out_dir / "mockup_grid.png",
    )
