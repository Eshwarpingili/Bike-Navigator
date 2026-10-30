#!/usr/bin/env python3
"""Build the board's text fonts from Inter, into main/fonts/.

Why Inter rather than the Montserrat that LVGL ships with: Montserrat is a
geometric display face. Its round, wide letterforms are handsome on a poster
and mediocre at 16 px on a panel being read at a glance in sunlight. Inter was
drawn for screen UI - taller x-height, shorter extenders, open apertures, and
tabular figures that stop a counting-down distance from jittering as the digits
change width. It is the single biggest reason the old screens read as dated.

Montserrat is NOT removed. Every icon on the board (play, volume, settings, the
back chevron) is an LV_SYMBOL glyph living in LVGL's built-in Montserrat, so
that font stays, used for icons only. Inter carries all the text.

The TTFs are not in this repository. Fetch them once:

    https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
    extras/ttf/Inter-Regular.ttf, Inter-SemiBold.ttf, Inter-Bold.ttf

Inter is licensed under the SIL Open Font License 1.1, which is why a copy of
that licence sits beside the generated files.

    npm install lv_font_conv
    python firmware/tools/gen_fonts.py --ttf <dir with the three .ttf files>

Flash cost of the set below is around 40 KB, against roughly 2.5 MB free in the
4 MB application partition.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / "main" / "fonts"

# Weight is picked per role, not per size. The numerals a rider reads at speed
# carry weight; the supporting text does not, because bold body text on a small
# panel turns into a grey block.
# ASCII, plus Latin-1 for the accented characters that turn up in road names.
# Nothing wider: every glyph is flash, and the board has no other script.
TEXT = ["0x20-0x7F", "0xA0-0xFF"]
# The two big faces only ever draw numbers and a couple of words, and a glyph
# at 48 px costs about eight times what it does at 16. Latin-1 there would be
# 90 KB of flash holding accented capitals that nothing can put on screen.
NUMS = ["0x20-0x7F"]

FONTS = [
    ("inter_12", "Inter-Regular.ttf", 12, TEXT, 14),   # diagnostics, deliberately quiet
    ("inter_16", "Inter-Regular.ttf", 16, TEXT, 16),   # artist, street, secondary lines
    ("inter_16b", "Inter-SemiBold.ttf", 16, TEXT, 16), # track title, tile headings
    ("inter_20", "Inter-SemiBold.ttf", 20, TEXT, 20),  # the street you turn onto
    ("inter_28", "Inter-SemiBold.ttf", 28, NUMS, 28),  # home-tile distance, readouts
    ("inter_48", "Inter-Bold.ttf", 48, NUMS, 28),      # the distance to the manoeuvre
]


def find_converter() -> list[str]:
    for base in (Path.cwd(), HERE, HERE.parent, Path("C:/bnfont")):
        js = base / "node_modules" / "lv_font_conv" / "lv_font_conv.js"
        if js.exists():
            return ["node", str(js)]
    exe = shutil.which("lv_font_conv")
    if exe:
        return [exe]
    sys.exit("lv_font_conv not found. Run: npm install lv_font_conv")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ttf", required=True, help="directory holding the Inter .ttf files")
    args = ap.parse_args()

    ttf_dir = Path(args.ttf)
    conv = find_converter()
    OUT.mkdir(parents=True, exist_ok=True)

    names = []
    for name, ttf, size, ranges, icon in FONTS:
        src = ttf_dir / ttf
        if not src.exists():
            sys.exit(f"missing {src} - see the header of this file")
        dst = OUT / f"{name}.c"
        cmd = conv + [
            "--font", str(src),
            "--size", str(size),
            "--bpp", "4",
            "--format", "lvgl",
            # Uncompressed on purpose: decompressing a glyph costs CPU on every
            # draw, and this board has flash to spare and cycles it does not.
            "--no-compress",
            "--force-fast-kern-format",
            "--lv-include", "lvgl.h",
            "-o", str(dst),
        ]
        for r in ranges:
            cmd += ["-r", r]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)

        # Point the font at Montserrat for anything Inter does not have. That
        # is the LV_SYMBOL range - every icon on the board - and LVGL resolves
        # it per glyph, so one label can hold an icon and a word and get the
        # right face for each. It has to be written into the initializer
        # rather than assigned at startup: these structs are const, and on
        # this part const means flash, which cannot be written to.
        text = dst.read_text(encoding="utf-8")
        marker = "    .fallback = NULL,"
        if marker not in text:
            sys.exit(f"{dst.name}: no .fallback field - lv_font_conv output changed")
        text = text.replace(marker, f"    .fallback = &lv_font_montserrat_{icon},")
        decl = "\n".join([
            '#include "lvgl.h"',
            "",
            "/* Icons come from LVGL's own Montserrat; see gen_fonts.py. */",
            f"LV_FONT_DECLARE(lv_font_montserrat_{icon});",
        ])
        text = text.replace('#include "lvgl.h"', decl, 1)
        dst.write_text(text, encoding="utf-8")
        names.append((name, dst.stat().st_size))

    header = OUT / "fonts.h"
    lines = [
        "/* Generated by firmware/tools/gen_fonts.py - do not edit. */",
        "#ifndef BIKENAV_FONTS_H",
        "#define BIKENAV_FONTS_H",
        "",
        '#include "lvgl.h"',
        "",
        "/* Inter, for every piece of text on the board. LVGL's built-in",
        " * Montserrat stays for the LV_SYMBOL icons and nothing else. */",
    ]
    for name, _ in names:
        lines.append(f"LV_FONT_DECLARE({name});")
    lines += ["", "#endif", ""]
    header.write_text("\n".join(lines), encoding="utf-8")

    total = sum(s for _, s in names)
    print(f"wrote {len(names)} fonts + fonts.h to {OUT} ({total // 1024} KB of C source)")
    for name, s in names:
        print(f"  {name:10s} {s // 1024:3d} KB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
