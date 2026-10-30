#!/usr/bin/env python3
"""Generate the home screen's background, and emit it as an LVGL image.

These are ORIGINAL mesh gradients, written here. Apple's own wallpapers are
copyrighted, and so are the copies that wallpaper sites give away, so none of
them are used or shipped. What is borrowed is the idea: a few wide, soft colour
blobs summed together, low in chroma, dark at the edges.

Why a stored image rather than an LVGL gradient: LVGL can only do two stops in
one direction here, which is a ramp, not a wallpaper. A mesh of blobs is what
makes a background look designed rather than filled in, and once it is
pre-dithered to RGB565 it costs the board nothing but the blit.

Two rules the hues obey, both about safety rather than taste:

  * Nothing may read as amber, red or green. Those four colours mean exactly
    one thing on this board - how hard the next manoeuvre is - and a background
    that borrows them teaches the rider that they can also mean nothing.
  * Chroma stays low and lightness stays at one end. The dark face must not
    lift blacks behind a card; the light face must not dim in sunlight.

    python firmware/tools/gen_wallpaper.py --preview   # a sheet to choose from
    python firmware/tools/gen_wallpaper.py --emit      # main/fonts/.. the C
"""

from __future__ import annotations

import argparse
import math
import os
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / "main" / "wallpaper"

W, H = 320, 240

# (x, y, radius, (r, g, b), weight) in fractions of the screen.
# Hue notes are in the names: indigo 260 deg, violet 285, plum 310, slate 220.
# Nothing between 0 and 200 degrees, which is where the severity colours live.
SCHEMES = {
    "midnight": dict(
        base=(6, 8, 14),
        blobs=[
            (0.18, 0.12, 0.62, (46, 32, 104), 1.00),   # indigo, top left
            (0.86, 0.26, 0.55, (78, 34, 96), 0.80),    # violet, top right
            (0.62, 0.92, 0.70, (20, 44, 78), 0.70),    # slate blue, below
            (0.04, 0.80, 0.45, (36, 24, 62), 0.55),
        ],
    ),
    "aurora": dict(
        base=(5, 9, 16),
        blobs=[
            (0.10, 0.20, 0.58, (24, 52, 92), 0.95),    # deep slate blue
            (0.78, 0.08, 0.50, (62, 30, 104), 0.85),   # violet
            (0.92, 0.78, 0.62, (18, 60, 82), 0.65),    # teal, kept dark
            (0.38, 0.98, 0.55, (40, 26, 78), 0.60),
        ],
    ),
    "plum": dict(
        base=(8, 6, 12),
        blobs=[
            (0.24, 0.86, 0.68, (84, 28, 86), 0.95),    # plum, low
            (0.90, 0.16, 0.52, (44, 32, 100), 0.85),   # indigo, high
            (0.52, 0.30, 0.60, (30, 24, 70), 0.55),
        ],
    ),
    # --- light faces: pale, cool, and bright enough to stay bright in sun ---
    "daybreak": dict(
        base=(228, 234, 242),
        blobs=[
            (0.14, 0.14, 0.62, (206, 214, 240), 1.00),  # cool lavender
            (0.88, 0.22, 0.56, (214, 208, 238), 0.85),  # pale violet
            (0.64, 0.94, 0.66, (204, 220, 236), 0.70),  # pale slate
        ],
    ),
    "porcelain": dict(
        base=(232, 236, 243),
        blobs=[
            (0.10, 0.84, 0.60, (210, 222, 238), 0.90),
            (0.86, 0.10, 0.58, (218, 212, 236), 0.85),
            (0.46, 0.44, 0.70, (226, 230, 244), 0.50),
        ],
    ),
    "mist": dict(
        base=(226, 232, 240),
        blobs=[
            (0.20, 0.18, 0.66, (200, 216, 234), 1.00),
            (0.84, 0.76, 0.60, (214, 206, 232), 0.80),
            (0.56, 0.06, 0.50, (220, 226, 240), 0.55),
        ],
    ),
}

DARK_SET = ("midnight", "aurora", "plum")
LIGHT_SET = ("daybreak", "porcelain", "mist")


def render(scheme: dict) -> Image.Image:
    """Sum of Gaussian blobs over a base. Floats until the dither."""
    base = scheme["base"]
    buf = [[list(base) for _ in range(W)] for _ in range(H)]
    for bx, by, br, colour, weight in scheme["blobs"]:
        cx, cy = bx * W, by * H
        rad = br * W
        inv = 1.0 / (2.0 * rad * rad)
        for y in range(H):
            dy2 = (y - cy) ** 2
            row = buf[y]
            for x in range(W):
                f = math.exp(-((x - cx) ** 2 + dy2) * inv) * weight
                if f < 0.004:
                    continue
                px = row[x]
                for i in range(3):
                    px[i] += (colour[i] - base[i]) * f
    img = Image.new("RGB", (W, H))
    img.putdata([tuple(min(255, max(0, int(v))) for v in buf[y][x])
                 for y in range(H) for x in range(W)])
    return img


def dither565(img: Image.Image) -> Image.Image:
    """Floyd-Steinberg down to 5/6/5. Done here, once, so the board never
    pays for it and the panel never shows a band."""
    px = [list(p) for p in img.getdata()]
    out = Image.new("RGB", (W, H))
    data = [0] * (W * H)
    err = [[0.0, 0.0, 0.0] for _ in range(W * H)]
    bits = (5, 6, 5)
    for y in range(H):
        for x in range(W):
            i = y * W + x
            vals = []
            for c in range(3):
                v = px[i][c] + err[i][c]
                step = 255.0 / ((1 << bits[c]) - 1)
                q = round(v / step) * step
                q = min(255.0, max(0.0, q))
                e = v - q
                vals.append(int(round(q)))
                for dx, dy, f in ((1, 0, 7 / 16), (-1, 1, 3 / 16),
                                  (0, 1, 5 / 16), (1, 1, 1 / 16)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < W and 0 <= ny < H:
                        err[ny * W + nx][c] += e * f
            data[i] = tuple(vals)
    out.putdata(data)
    return out


def to565_bytes(img: Image.Image) -> bytes:
    """Little-endian uint16, which is what LV_COLOR_DEPTH 16 with
    LV_COLOR_16_SWAP 0 expects. Get this backwards and every colour is wrong."""
    out = bytearray()
    for r, g, b in img.getdata():
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        out.append(v & 0xFF)
        out.append((v >> 8) & 0xFF)
    return bytes(out)


def emit_c(name: str, data: bytes) -> str:
    rows = []
    for i in range(0, len(data), 24):
        rows.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",")
    return (
        f"static const uint8_t {name}_map[] = {{\n" + "\n".join(rows) + "\n};\n\n"
        f"const lv_img_dsc_t {name} = {{\n"
        f"    .header.cf = LV_IMG_CF_TRUE_COLOR,\n"
        f"    .header.always_zero = 0,\n"
        f"    .header.reserved = 0,\n"
        f"    .header.w = {W},\n"
        f"    .header.h = {H},\n"
        f"    .data_size = {len(data)},\n"
        f"    .data = {name}_map,\n"
        f"}};\n"
    )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", action="store_true")
    ap.add_argument("--emit", action="store_true")
    ap.add_argument("--dark", default="midnight")
    ap.add_argument("--light", default="daybreak")
    args = ap.parse_args()

    if args.preview:
        cell_w, cell_h, pad = W, H, 14
        names = list(DARK_SET) + list(LIGHT_SET)
        sheet = Image.new("RGB", (3 * (cell_w + pad) + pad,
                                  2 * (cell_h + pad) + pad), "#3C4047")
        for i, n in enumerate(names):
            im = dither565(render(SCHEMES[n]))
            sheet.paste(im, (pad + (i % 3) * (cell_w + pad),
                             pad + (i // 3) * (cell_h + pad)))
        out = HERE / "wallpapers.png"
        sheet.save(out)
        print("wrote", out, "order:", ", ".join(names))
        return 0

    if args.emit:
        OUT.mkdir(parents=True, exist_ok=True)
        parts = ['/* Generated by firmware/tools/gen_wallpaper.py - do not edit. */',
                 '#include "lvgl.h"', ""]
        total = 0
        for var, scheme in (("wp_night", args.dark), ("wp_day", args.light)):
            data = to565_bytes(dither565(render(SCHEMES[scheme])))
            total += len(data)
            parts.append(f"/* {scheme} */")
            parts.append(emit_c(var, data))
        (OUT / "wallpaper.c").write_text("\n".join(parts), encoding="utf-8")
        (OUT / "wallpaper.h").write_text("\n".join([
            "/* Generated by firmware/tools/gen_wallpaper.py - do not edit. */",
            "#ifndef BIKENAV_WALLPAPER_H",
            "#define BIKENAV_WALLPAPER_H",
            "",
            '#include "lvgl.h"',
            "",
            "/* The home screen's background, one per face. Original mesh",
            " * gradients, pre-dithered to RGB565 so the panel shows no bands",
            " * and the board does no work beyond the blit. */",
            "LV_IMG_DECLARE(wp_night);",
            "LV_IMG_DECLARE(wp_day);",
            "",
            "#endif",
            "",
        ]), encoding="utf-8")
        print(f"wrote {OUT}/wallpaper.c  ({total // 1024} KB of pixels, "
              f"{total * 6 // 1024} KB of C source)")
        return 0

    ap.error("pass --preview or --emit")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
