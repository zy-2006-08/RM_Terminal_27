"""Minimal PNG decoder + video-pane content classifier for F3 runtime evidence.

Why hand-rolled: the project venv has no PIL and no numpy, and F3 forbids human
visual checks, so the only way to assert "the pane shows the offline text" rather
than "the pane shows a frozen camera frame" is to read the pixels ourselves.

Only what Qt's PNG writer actually emits is supported: non-interlaced, 8-bit,
colour types 2 (RGB) and 6 (RGBA). Anything else raises instead of guessing.
"""

from __future__ import annotations

import struct
import sys
import zlib

# VideoPane::paintEvent fills its whole rect with this before drawing anything
# (cpp/dashboard.cpp:303). Status text is drawn on top of it in grey; a decoded
# frame covers nearly all of it.
PANE_BACKGROUND = (12, 12, 14)


class Png:
    def __init__(self, width: int, height: int, channels: int, rows: list[bytes]):
        self.width = width
        self.height = height
        self.channels = channels
        self.rows = rows

    def pixel(self, x: int, y: int) -> tuple[int, int, int]:
        base = x * self.channels
        row = self.rows[y]
        return (row[base], row[base + 1], row[base + 2])


def _unfilter(raw: bytes, width: int, height: int, channels: int) -> list[bytes]:
    stride = width * channels
    rows: list[bytearray] = []
    previous = bytearray(stride)
    pos = 0
    for _ in range(height):
        filter_type = raw[pos]
        pos += 1
        line = bytearray(raw[pos : pos + stride])
        pos += stride
        if filter_type == 0:
            pass
        elif filter_type == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif filter_type == 2:
            for i in range(stride):
                line[i] = (line[i] + previous[i]) & 0xFF
        elif filter_type == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 0xFF
        elif filter_type == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = previous[i]
                c = previous[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                if pa <= pb and pa <= pc:
                    pred = a
                elif pb <= pc:
                    pred = b
                else:
                    pred = c
                line[i] = (line[i] + pred) & 0xFF
        else:
            raise ValueError(f"unsupported PNG filter type {filter_type}")
        rows.append(line)
        previous = line
    return [bytes(row) for row in rows]


def read_png(path: str) -> Png:
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos = 8
    width = height = channels = 0
    idat = bytearray()
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos : pos + 4])
        kind = data[pos + 4 : pos + 8]
        body = data[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or interlace != 0 or colour not in (2, 6):
                raise ValueError(
                    f"{path}: unsupported PNG (depth={depth} colour={colour} interlace={interlace})"
                )
            channels = 3 if colour == 2 else 4
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    if not width or not height:
        raise ValueError(f"{path}: no IHDR")
    return Png(width, height, channels, _unfilter(zlib.decompress(bytes(idat)), width, height, channels))


def region_stats(png: Png, x0: int, y0: int, x1: int, y1: int) -> dict:
    """Colour statistics for a crop, enough to tell a frame from a text placard."""
    counts: dict[tuple[int, int, int], int] = {}
    total = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            pixel = png.pixel(x, y)
            counts[pixel] = counts.get(pixel, 0) + 1
            total += 1
    background = counts.get(PANE_BACKGROUND, 0)
    ranked = sorted(counts.items(), key=lambda item: item[1], reverse=True)
    return {
        "pixels": total,
        "distinct_colours": len(counts),
        "background_fraction": background / total if total else 0.0,
        "dominant": ranked[0][0] if ranked else None,
        "dominant_fraction": (ranked[0][1] / total) if total else 0.0,
        "top": [(colour, round(count / total, 4)) for colour, count in ranked[:5]],
    }


def classify(stats: dict) -> str:
    """`frame` = a decoded picture is being painted; `placard` = background+text only.

    A decoded 320x180 camera frame upscaled to the pane covers it almost entirely
    and carries hundreds of distinct colours. The offline placard is the flat fill
    plus antialiased grey glyphs: overwhelmingly background, very few colours.
    """
    if stats["background_fraction"] >= 0.80 and stats["distinct_colours"] <= 400:
        return "placard"
    if stats["background_fraction"] <= 0.30 and stats["distinct_colours"] >= 200:
        return "frame"
    return "ambiguous"


def centre_crop(png: Png, fx0=0.30, fy0=0.45, fx1=0.70, fy1=0.85) -> tuple[int, int, int, int]:
    """A box that lies inside video_full_pane in video mode.

    Geometry from --dump-layout is parent-relative, so it cannot be mapped into
    window coordinates without the parent chain. In video mode the pane fills
    everything under the two banners and the overlay row, so a central box is
    unambiguously inside it, and the pane centres both the frame and the text.
    """
    return (
        int(png.width * fx0),
        int(png.height * fy0),
        int(png.width * fx1),
        int(png.height * fy1),
    )


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print("usage: f3_png.py <screenshot.png> [more.png ...]", file=sys.stderr)
        return 2
    for path in argv[1:]:
        png = read_png(path)
        box = centre_crop(png)
        stats = region_stats(png, *box)
        verdict = classify(stats)
        print(f"{path}: {png.width}x{png.height} crop={box} -> {verdict}")
        print(
            f"  distinct_colours={stats['distinct_colours']} "
            f"background_fraction={stats['background_fraction']:.4f} "
            f"dominant={stats['dominant']} ({stats['dominant_fraction']:.4f})"
        )
        print(f"  top={stats['top']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
