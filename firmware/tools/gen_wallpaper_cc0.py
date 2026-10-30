#!/usr/bin/env python3
"""Turn a CC0 gradient into the board's two backgrounds.

Source: "Colorpaper JM01" by AV Reference, OpenGameArt, licensed CC0 (public
domain, redistribution and modification permitted, no attribution required).
We credit it anyway, because that is the decent thing and it costs nothing.

    https://opengameart.org/content/colorpaper-jm01

Why a downloaded gradient rather than one generated here: the hand-written mesh
was too polite. These are saturation 0.75-0.81 against about 0.35, which is the
whole difference between "tinted" and "lit".

Three of the ten were usable, and the test was not taste. Red, amber, green and
blue mean exactly one thing on this board - how hard the next manoeuvre is - so
a background may not wander into those hues. Measured across the pack:

    jm05  0% of pixels in a reserved hue band   <- night
    jm08  2%                                    <- day
    jm02  3%
    jm07 68%, jm10 62%, jm09 57%  ... rejected, whatever they look like

    python firmware/tools/gen_wallpaper_cc0.py --zip <path to the pack>
"""

from __future__ import annotations

import argparse
import colorsys
import zipfile
from pathlib import Path

from PIL import Image, ImageEnhance

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / "main" / "wallpaper"
W, H = 320, 240

# (member, target mean brightness, saturation boost)
# Night is pulled well down: a card has to sit ON this, not glow through it.
# Day is lifted instead, because the same picture at noon has to stay pale
# enough that black text on a white card still wins.
FACES = {
    "wp_night": ("jm05_360dpi.png", 0.34, 1.30),
    "wp_day": ("jm08_360dpi.png", 0.88, 0.62),
}

BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def severity_fraction(img: Image.Image) -> float:
    """Share of pixels sitting in the hue bands the turn colours own."""
    small = img.resize((32, 24))
    n = bad = 0
    for r, g, b in small.getdata():
        h, s, _ = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
        n += 1
        if s > 0.25 and (h < 0.18 or 0.25 < h < 0.47):
            bad += 1
    return bad / n


def tone(img: Image.Image, target_v: float, sat: float) -> Image.Image:
    img = ImageEnhance.Color(img).enhance(sat)
    px = img.resize((32, 24)).getdata()
    mean = sum(max(p) for p in px) / (len(px) * 255)
    img = ImageEnhance.Brightness(img).enhance(max(0.05, target_v / max(mean, 0.01)))
    return img


def dither565(img: Image.Image) -> Image.Image:
    """Floyd-Steinberg to 5/6/5. Done once here so the panel never bands and
    the board never spends a cycle on it."""
    w, h = img.size
    px = [list(p) for p in img.getdata()]
    err = [[0.0, 0.0, 0.0] for _ in range(w * h)]
    bits = (5, 6, 5)
    out = [(0, 0, 0)] * (w * h)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            vals = []
            for c in range(3):
                v = px[i][c] + err[i][c]
                step = 255.0 / ((1 << bits[c]) - 1)
                q = min(255.0, max(0.0, round(v / step) * step))
                e = v - q
                vals.append(int(round(q)))
                for dx, dy, f in ((1, 0, 7 / 16), (-1, 1, 3 / 16),
                                  (0, 1, 5 / 16), (1, 1, 1 / 16)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < w and 0 <= ny < h:
                        err[ny * w + nx][c] += e * f
            out[i] = tuple(vals)
    im = Image.new("RGB", (w, h))
    im.putdata(out)
    return im


def prepare(src: Image.Image, target_v: float, sat: float) -> Image.Image:
    # Centre-crop to 4:3 before scaling, so nothing is stretched.
    w, h = src.size
    want = W / H
    if w / h > want:
        nw = int(h * want)
        src = src.crop(((w - nw) // 2, 0, (w + nw) // 2, h))
    else:
        nh = int(w / want)
        src = src.crop((0, (h - nh) // 2, w, (h + nh) // 2))
    src = src.resize((W, H), Image.LANCZOS)
    return dither565(tone(src, target_v, sat))


def to565_bytes(img: Image.Image) -> bytes:
    out = bytearray()
    for r, g, b in img.getdata():
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        out += bytes((v & 0xFF, (v >> 8) & 0xFF))
    return bytes(out)


def emit_c(name: str, data: bytes) -> str:
    rows = ["    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 24])
            for i in range(0, len(data), 24)]
    return (f"static const uint8_t {name}_map[] = {{\n" + ",\n".join(rows) + ",\n};\n\n"
            f"const lv_img_dsc_t {name} = {{\n"
            f"    .header.cf = LV_IMG_CF_TRUE_COLOR,\n"
            f"    .header.always_zero = 0,\n    .header.reserved = 0,\n"
            f"    .header.w = {W},\n    .header.h = {H},\n"
            f"    .data_size = {len(data)},\n    .data = {name}_map,\n}};\n")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--zip", required=True)
    ap.add_argument("--preview-only", action="store_true")
    args = ap.parse_args()

    z = zipfile.ZipFile(args.zip)
    made = {}
    for var, (member, v, s) in FACES.items():
        with z.open(member) as f:
            img = prepare(Image.open(f).convert("RGB"), v, s)
        frac = severity_fraction(img)
        print(f"{var:9s} from {member}  severity-hue {frac*100:.1f}%")
        if frac > 0.08:
            raise SystemExit(f"{member} puts {frac*100:.0f}% of the screen in a "
                             "reserved turn colour; pick another")
        made[var] = img
        img.save(HERE / f"{var}.png")

    if args.preview_only:
        return 0

    OUT.mkdir(parents=True, exist_ok=True)
    parts = ["/* Generated by firmware/tools/gen_wallpaper_cc0.py - do not edit.",
             " *",
             " * From 'Colorpaper JM01' by AV Reference (OpenGameArt), CC0.",
             " * https://opengameart.org/content/colorpaper-jm01",
             " */",
             '#include "lvgl.h"', ""]
    total = 0
    for var, img in made.items():
        data = to565_bytes(img)
        total += len(data)
        parts.append(emit_c(var, data))
    (OUT / "wallpaper.c").write_text("\n".join(parts), encoding="utf-8")
    (OUT / "wallpaper.h").write_text("\n".join([
        "/* Generated by firmware/tools/gen_wallpaper_cc0.py - do not edit. */",
        "#ifndef BIKENAV_WALLPAPER_H", "#define BIKENAV_WALLPAPER_H", "",
        '#include "lvgl.h"', "",
        "/* The background behind the cards, one per face. Pre-dithered to",
        " * RGB565, so the panel shows no banding and the board does nothing",
        " * but blit. Source is CC0; see the .c file. */",
        "LV_IMG_DECLARE(wp_night);", "LV_IMG_DECLARE(wp_day);", "", "#endif", "",
    ]), encoding="utf-8")
    print(f"wrote {OUT}/wallpaper.c  ({total // 1024} KB of pixels)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
