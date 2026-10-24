#!/usr/bin/env python3
"""Generate the original YOUYOU wordmark tile and NACP icon.

Only Pillow is used. Shapes are drawn at 4x and downsampled for clean edges.
"""
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets"
ASSETS.mkdir(exist_ok=True)
S = 4
SIZE = 64
W = SIZE * S
BLUE = (58, 123, 252, 255)
WHITE = (255, 255, 255, 255)

def mark(size_px: int) -> Image.Image:
    """Return an RGBA rounded-square mark with a white Y, transparent corners."""
    image = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    pad = 2 * S
    draw.rounded_rectangle((pad, pad, W - 1 - pad, W - 1 - pad), radius=14 * S, fill=BLUE)
    lw = 8 * S
    # Two upper arms and one lower stem, with rounded caps.
    strokes = [((24 * S, 17 * S), (32 * S, 30 * S)),
               ((40 * S, 17 * S), (32 * S, 30 * S)),
               ((32 * S, 30 * S), (32 * S, 47 * S))]
    for a, b in strokes:
        draw.line((a, b), fill=WHITE, width=lw)
        for x, y in (a, b):
            draw.ellipse((x - lw // 2, y - lw // 2, x + lw // 2, y + lw // 2), fill=WHITE)
    if size_px != W:
        image = image.resize((size_px, size_px), Image.Resampling.LANCZOS)
    return image

def smooth_icon(draw_kind: str) -> Image.Image:
    """Original smooth 64px fallback for a hand or mouse, 4x supersampled."""
    canvas = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(canvas)
    if draw_kind == "touch":
        d.rounded_rectangle((20 * S, 27 * S, 45 * S, 51 * S), radius=8 * S, fill=WHITE)
        for x in (25, 33, 41):
            d.line((x * S, 27 * S, x * S, 11 * S), fill=WHITE, width=6 * S)
            d.ellipse(((x - 3) * S, 8 * S, (x + 3) * S, 14 * S), fill=WHITE)
        d.line((20 * S, 35 * S, 12 * S, 31 * S), fill=WHITE, width=6 * S)
        d.ellipse((9 * S, 28 * S, 15 * S, 34 * S), fill=WHITE)
    else:
        d.rounded_rectangle((21 * S, 10 * S, 43 * S, 53 * S), radius=11 * S,
                            outline=WHITE, width=5 * S)
        d.line((32 * S, 13 * S, 32 * S, 31 * S), fill=WHITE, width=4 * S)
        d.line((21 * S, 31 * S, 43 * S, 31 * S), fill=WHITE, width=4 * S)
    return canvas.resize((SIZE, SIZE), Image.Resampling.LANCZOS)

mark_px = mark(SIZE)
(ASSETS / "youyou-mark.rgba").write_bytes(mark_px.tobytes())
icon = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
icon_draw = ImageDraw.Draw(icon)
icon_draw.rounded_rectangle((0, 0, 255, 255), radius=56, fill=BLUE)
icon_draw.rounded_rectangle((18, 18, 238, 238), radius=44, outline=(255, 255, 255, 255), width=6)
icon.alpha_composite(mark_px.resize((202, 202), Image.Resampling.LANCZOS), (27, 27))
icon.convert("RGB").save(ASSETS / "youyou-icon.jpg", quality=95)
(ASSETS / "touch.rgba").write_bytes(smooth_icon("touch").tobytes())
(ASSETS / "mouse.rgba").write_bytes(smooth_icon("mouse").tobytes())
print("generated", ASSETS / "youyou-mark.rgba", ASSETS / "youyou-icon.jpg", ASSETS / "touch.rgba", ASSETS / "mouse.rgba")
