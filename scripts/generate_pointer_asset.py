#!/usr/bin/env python3
"""Generate the local mouse pointer overlay asset (assets/pointer.rgba).

The Switch build has no pointer of its own: the pointer on screen comes from
the PC's captured desktop. Some PC applications hide or replace the system
cursor inside their own window, and then nothing is left to show. Drawing the
arrow locally keeps a pointer visible regardless of what the PC reports.

Layout: 64x64 RGBA8888, straight alpha, hotspot at (2, 2) in image pixels.
"""
import pathlib

from PIL import Image, ImageDraw

SIZE = 64
SS = 4                      # supersampling factor
W = SIZE * SS
ASSETS = pathlib.Path(__file__).resolve().parents[1] / "assets"

WHITE = (255, 255, 255, 255)
EDGE = (18, 20, 24, 235)

# Classic arrow, apex at the top-left so the hotspot is the tip.
OUTLINE = [(2, 2), (2, 47), (14, 36), (20, 56), (29, 51), (22, 33), (36, 32)]


def pointer() -> Image.Image:
    canvas = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(canvas)
    pts = [(x * SS, y * SS) for x, y in OUTLINE]
    d.polygon(pts, fill=WHITE, outline=EDGE, width=3 * SS)
    return canvas.resize((SIZE, SIZE), Image.Resampling.LANCZOS)


if __name__ == "__main__":
    image = pointer()
    (ASSETS / "pointer.rgba").write_bytes(image.tobytes())
    image.resize((256, 256), Image.Resampling.NEAREST).save(ASSETS.parent / "pointer-preview.png")
    print("generated", ASSETS / "pointer.rgba", len((ASSETS / "pointer.rgba").read_bytes()), "bytes")
