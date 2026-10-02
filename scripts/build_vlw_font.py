#!/usr/bin/env python3
"""Build the embedded anti-aliased UI fonts (Processing/TFT_eSPI VLW format).

Renders fonts/NotoSans-Bold.ttf with FreeType into data/ui_font_small.vlw and
data/ui_font_large.vlw. The firmware only ever scales these down (0.5x-1.0x),
so each text height is drawn from the smallest file at least that tall.

Requires: pip install freetype-py
"""

from __future__ import annotations

import struct
from pathlib import Path

import freetype

ROOT = Path(__file__).resolve().parents[1]
FONT_PATH = ROOT / "fonts" / "NotoSans-Bold.ttf"

# (output file, FreeType pixel size)
OUTPUTS = [
    (ROOT / "data" / "ui_font_small.vlw", 32),
    (ROOT / "data" / "ui_font_large.vlw", 64),
]

# Printable ASCII, plus the ellipsis (SSID truncation) and the degree sign.
CODEPOINTS = sorted(set(range(0x20, 0x7F)) | {0x2026, 0x00B0})

VLW_VERSION = 11


def render_glyph(face: freetype.Face, cp: int) -> tuple[list[int], bytes]:
    """Return the 7 VLW metrics and the 8-bit alpha bitmap for one code point."""
    if face.get_char_index(cp) == 0:
        raise SystemExit(f"{FONT_PATH.name} has no glyph for U+{cp:04X}")
    face.load_char(cp, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_LIGHT)
    g = face.glyph
    bm = g.bitmap
    width, height = bm.width, bm.rows
    # The row pitch can be padded; copy row by row.
    rows = bytearray()
    for y in range(height):
        start = y * bm.pitch
        rows += bytes(bm.buffer[start : start + width])
    x_advance = (g.advance.x + 32) >> 6
    # height, width, xAdvance, dY (top above baseline), dX (left bearing), padding
    metrics = [cp, height, width, x_advance, g.bitmap_top, g.bitmap_left, 0]
    return metrics, bytes(rows)


def build(out_path: Path, pixel_size: int) -> None:
    face = freetype.Face(str(FONT_PATH))
    face.set_pixel_sizes(0, pixel_size)

    glyphs = [render_glyph(face, cp) for cp in CODEPOINTS]
    by_cp = {m[0]: m for m, _ in glyphs}

    # Same convention as Processing: ascent of "d", descent of "p".
    ascent = by_cp[ord("d")][4]
    descent = by_cp[ord("p")][1] - by_cp[ord("p")][4]

    out = bytearray()
    out += struct.pack(">6i", len(glyphs), VLW_VERSION, pixel_size, 0, ascent, descent)
    for metrics, _ in glyphs:
        out += struct.pack(">7i", *metrics)
    for _, bitmap in glyphs:
        out += bitmap

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(out)

    # LovyanGFX line height (fontHeight at size 1): tallest ascent plus deepest
    # descent over all glyphs, starting from the header values.
    max_ascent = max([ascent] + [m[4] for m, _ in glyphs])
    max_descent = max([descent] + [m[1] - m[4] for m, _ in glyphs])
    cap_height = by_cp[ord("H")][4]
    print(
        f"{out_path.relative_to(ROOT)}: {len(glyphs)} glyphs, {len(out)} bytes, "
        f"pixel size {pixel_size}, cap height {cap_height}, "
        f"line height {max_ascent + max_descent}"
    )


def main() -> None:
    for out_path, pixel_size in OUTPUTS:
        build(out_path, pixel_size)


if __name__ == "__main__":
    main()
