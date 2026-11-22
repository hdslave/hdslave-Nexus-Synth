"""Rebuild src/ui/wordmark.h with Python and Pillow. Coordinates are native MPC pixels."""

from math import cos, sin, pi, floor
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter

WIDTH, HEIGHT, SCALE = 166, 40, 4
SIZE = WIDTH * SCALE, HEIGHT * SCALE
ROOT = Path(__file__).resolve().parents[1]
# Five cut-metal letters. The X has two long spurs that cross the orbit.
LETTERS = [
    [
        (1, 31),
        (11, 26),
        (18, 10),
        (12, 8),
        (26, 8),
        (35, 23),
        (41, 9),
        (49, 6),
        (37, 32),
        (31, 32),
        (22, 17),
        (16, 30),
    ],
    [
        (39, 31),
        (49, 9),
        (74, 8),
        (66, 14),
        (53, 14),
        (51, 18),
        (66, 17),
        (61, 22),
        (49, 23),
        (46, 27),
        (63, 26),
        (58, 31),
    ],
    [
        (57, 36),
        (76, 20),
        (70, 11),
        (64, 5),
        (80, 10),
        (84, 16),
        (100, 5),
        (96, 12),
        (87, 21),
        (94, 32),
        (103, 38),
        (87, 33),
        (81, 25),
        (71, 32),
    ],
    [
        (96, 12),
        (107, 8),
        (98, 25),
        (100, 27),
        (110, 26),
        (118, 10),
        (128, 7),
        (117, 29),
        (112, 32),
        (94, 32),
        (89, 27),
    ],
    [
        (115, 32),
        (121, 26),
        (138, 26),
        (140, 23),
        (124, 22),
        (121, 18),
        (127, 10),
        (154, 8),
        (161, 5),
        (153, 14),
        (132, 15),
        (131, 17),
        (147, 18),
        (151, 21),
        (146, 28),
        (139, 32),
    ],
]
ENVIRONMENT = [
    (0.0, (39, 23, 91)),
    (0.16, (126, 112, 214)),
    (0.33, (225, 230, 255)),
    (0.46, (252, 255, 253)),
    (0.51, (140, 153, 200)),
    (0.54, (27, 22, 52)),
    (0.67, (47, 39, 97)),
    (0.79, (137, 126, 192)),
    (0.91, (224, 242, 185)),
    (1.0, (70, 64, 115)),
]


def blend(a, b, t):
    return tuple(round(i + (j - i) * t) for i, j in zip(a, b))


def reflection(t):
    for (x, a), (y, b) in zip(ENVIRONMENT, ENVIRONMENT[1:]):
        if t <= y:
            return blend(a, b, max(0, (t - x) / (y - x)))
    return ENVIRONMENT[-1][1]


def scaled(points):
    return [(round(x * SCALE), round(y * SCALE)) for x, y in points]


def artwork():
    art = Image.new("RGB", SIZE, (9, 7, 15))
    pixels = art.load()
    # A twisted strip of checkerboard behind the cut metal.
    for xx in range(SIZE[0]):
        x = xx / SCALE
        phase = x / 23
        center = 24 - 11 * sin(phase - 0.8)
        thick = 2.2 + 6.2 * (0.5 + 0.5 * cos(phase))
        for yy in range(
            max(0, int((center - thick) * SCALE)),
            min(SIZE[1], int((center + thick) * SCALE) + 1),
        ):
            v = ((yy / SCALE - center) / thick + 1) * 0.5
            checked = (floor(x / 6 + 2 * v) + floor(3 * v)) % 2
            pixels[xx, yy] = blend(
                (20, 11, 39), (145, 83, 214), (0.22 + 0.50 * v) if checked else 0.02
            )
    draw = ImageDraw.Draw(art)
    orbit = []
    for i in range(241):
        a = 2 * pi * i / 240
        orbit.append((82 + 74 * cos(a), 20 + 10 * sin(a) - 11 * cos(a)))
    draw.line(scaled(orbit), fill=(117, 152, 68), width=2)
    mask = Image.new("L", SIZE)
    draw_mask = ImageDraw.Draw(mask)
    for outline in LETTERS:
        draw_mask.polygon(scaled(outline), fill=255)
    # The bevel follows each silhouette, including internal corners.
    mask_px = mask.load()
    bevel = mask.filter(ImageFilter.GaussianBlur(3.2)).load()
    for yy in range(2, SIZE[1] - 2):
        for xx in range(2, SIZE[0] - 2):
            if not mask_px[xx, yy]:
                continue
            x, y = xx / SCALE, yy / SCALE
            edge = (
                bevel[xx + 2, yy]
                - bevel[xx - 2, yy]
                + 1.2 * (bevel[xx, yy + 2] - bevel[xx, yy - 2])
            ) / 150
            t = (y - 6 + 2 * sin(x / 12) + 1.6 * cos(x / 5.8)) / 28
            c = reflection(t)
            c = blend(
                c, (249, 255, 247) if edge > 0 else (24, 10, 44), min(1, abs(edge))
            )
            pixels[xx, yy] = c
    for x, y, r in [(22, 8, 3.8), (83, 17, 2.4), (148, 12, 3.5)]:
        draw.polygon(
            scaled(
                [
                    (x - r, y),
                    (x - 0.6, y - 0.5),
                    (x, y - r),
                    (x + 0.6, y - 0.5),
                    (x + r, y),
                    (x + 0.6, y + 0.5),
                    (x, y + r),
                    (x - 0.6, y + 0.5),
                ]
            ),
            fill=(248, 255, 236),
        )
    return art.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)


def write_header():
    bitmap = artwork().quantize(
        colors=96, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE
    )
    raw = bitmap.getpalette()
    palette = [
        f"0xff{r:02x}{g:02x}{b:02x}" for r, g, b in zip(raw[::3], raw[1::3], raw[2::3])
    ]

    # One palette index per character. Offset into the printable ASCII range, escaping C++ punctuation.
    def encode(row):
        return "".join(
            "\\" + chr(32 + n)
            if chr(32 + n) in '\\"?'
            else ("\\177" if n == 95 else chr(32 + n))
            for n in row
        )

    lines = [
        "// Generated by tools/draw_wordmark.py.",
        "#pragma once",
        "#include <cstdint>",
        "namespace hdui::wordmark {",
        f"constexpr int width = {WIDTH}, height = {HEIGHT};",
        "inline constexpr uint32_t palette[] = {",
    ]
    lines.extend(
        "    " + ", ".join(palette[i : i + 8]) + "," for i in range(0, len(palette), 8)
    )
    lines.extend(["};", "inline constexpr char rows[][width + 1] = {"])
    data = bitmap.tobytes()
    lines.extend(
        '    "' + encode(data[i : i + WIDTH]) + '",' for i in range(0, len(data), WIDTH)
    )
    lines.extend(["};", "} // namespace hdui::wordmark", ""])
    (ROOT / "src/ui/wordmark.h").write_text("\n".join(lines))


if __name__ == "__main__":
    write_header()
