#!/usr/bin/env python3
"""Generate the HUD-style 9-slice border assets.

The design quotes assets/test.png: a 3px outer rule with 7px cut corners.
Source is 40x40 with 10px corners, so it renders 1:1 at border_thickness=10.

    python3 tools/make-hud-assets.py

Requires Pillow (only for regenerating the assets).
"""
import math
from pathlib import Path

from PIL import Image

SIZE = 40
CUT = 7
THICK = 3

COLORS = {
    "hud-40-focused.png": "#ffffff",
    "hud-40-unfocused.png": "#6e7681",
}


def dist_to_edge(x: int, y: int) -> float:
    cx, cy = x + 0.5, y + 0.5
    return min(
        cx, cy, SIZE - cx, SIZE - cy,
        (cx + cy - CUT) / math.sqrt(2),
        ((SIZE - cx) + cy - CUT) / math.sqrt(2),
        (cx + (SIZE - cy) - CUT) / math.sqrt(2),
        ((SIZE - cx) + (SIZE - cy) - CUT) / math.sqrt(2),
    )


def inside(x: int, y: int) -> bool:
    return (x + y >= CUT and (SIZE - 1 - x) + y >= CUT and
            x + (SIZE - 1 - y) >= CUT and
            (SIZE - 1 - x) + (SIZE - 1 - y) >= CUT)


def make(color: str) -> Image.Image:
    r = int(color[1:3], 16)
    g = int(color[3:5], 16)
    b = int(color[5:7], 16)
    img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    px = img.load()
    for y in range(SIZE):
        for x in range(SIZE):
            if inside(x, y) and dist_to_edge(x, y) < THICK:
                px[x, y] = (r, g, b, 255)
    return img


def main() -> None:
    out = Path(__file__).resolve().parent.parent / "assets"
    out.mkdir(exist_ok=True)
    for name, color in COLORS.items():
        make(color).save(out / name)
        print(f"wrote {out / name}")


if __name__ == "__main__":
    main()
