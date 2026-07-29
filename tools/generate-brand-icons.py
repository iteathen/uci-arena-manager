#!/usr/bin/env python3
"""Regenerate the committed UCI Arena Manager raster application icons."""

from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "packaging" / "icons"


def icon(size: int) -> Image.Image:
    scale = size / 256
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    def points(values: list[tuple[int, int]]) -> list[tuple[int, int]]:
        return [(round(x * scale), round(y * scale)) for x, y in values]

    draw.rounded_rectangle(
        (round(12 * scale), round(12 * scale), round(244 * scale), round(244 * scale)),
        radius=round(52 * scale),
        fill="#101426",
    )
    crown = points([(52, 184), (65, 168), (79, 87), (108, 115), (128, 51),
                    (148, 115), (177, 87), (191, 168), (204, 184)])
    draw.polygon(crown, fill="#35b9df")
    draw.rectangle(
        (round(52 * scale), round(184 * scale), round(204 * scale), round(208 * scale)),
        fill="#6d5dfc",
    )
    draw.ellipse(
        (round(112 * scale), round(130 * scale), round(144 * scale), round(162 * scale)),
        fill="#101426",
    )
    width = max(1, round(8 * scale))
    draw.line(points([(112, 146), (144, 146)]), fill="white", width=width)
    draw.line(points([(128, 130), (128, 162)]), fill="white", width=width)
    return image


def main() -> None:
    OUTPUT.mkdir(parents=True, exist_ok=True)
    largest = icon(256)
    largest.save(OUTPUT / "uci-arena-manager.png", optimize=True)
    largest.save(
        OUTPUT / "uci-arena-manager.ico",
        sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)],
    )


if __name__ == "__main__":
    main()
