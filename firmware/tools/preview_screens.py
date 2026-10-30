#!/usr/bin/env python3
"""Draw the board's screens on the PC, so a look at the design costs an image
instead of a BURN-and-replug cycle and a photograph.

The colours and the geometry are the same numbers ui.c uses - palette hexes,
MIN_GAP, the tile arithmetic, the strip offsets - so what comes out is a fair
picture of the layout. It is not an LVGL renderer: fonts and glyph metrics are
approximate, and anything that depends on real text measurement (the scrolling
title, LV_LABEL_LONG_DOT) will differ by a few pixels on the board.

    python firmware/tools/preview_screens.py            # writes screens.png
"""

import os
from PIL import Image, ImageDraw, ImageFont

W, H = 320, 240
BAR_H = 26          # the top bar, which every screen sits under
MIN_GAP = 14        # 2 * HIT_PAD + 2, from ui.c
NEAR_FROM_M = 400

# set_palette() in ui.c, verbatim.
DARK = dict(
    bg="#000000", card="#161A21", text="#FFFFFF", dim="#AAB3BF",
    accent="#5AB0FF", warn="#FFB300", good="#3DDC84", bar="#161A21",
    edge="#161A21", music="#A78BFA", map="#2DD4BF", press="#2E3540",
)
LIGHT = dict(
    bg="#D8DFE7", card="#FFFFFF", text="#000000", dim="#333C47",
    accent="#0A44C4", warn="#8A4200", good="#04602F", bar="#C2CBD6",
    edge="#7C8A9A", music="#5B21B6", map="#0F766E", press="#AEBAC8",
)
RED = "#E53935"


def font(size, bold=False):
    for name in (("seguisb.ttf", "segoeui.ttf") if bold else ("segoeui.ttf",)):
        path = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", name)
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    try:
        return ImageFont.truetype("DejaVuSans.ttf", size)
    except OSError:
        return ImageFont.load_default()


def centre(d, box, text, f, fill):
    x0, y0, x1, y1 = box
    w = d.textlength(text, font=f)
    a = f.getbbox(text)
    d.text((x0 + (x1 - x0 - w) / 2, y0 + (y1 - y0 - (a[3] - a[1])) / 2 - a[1]),
           text, font=f, fill=fill)


def top_bar(d, p, clock="21:14", title="BikeNav"):
    d.rectangle([0, 0, W, BAR_H], fill=p["bar"])
    d.text((10, 4), title, font=font(15), fill=p["dim"])
    w = d.textlength(clock, font=font(15, True))
    d.text((W - 10 - w, 4), clock, font=font(15, True), fill=p["text"])


def card(d, p, box, radius=12):
    d.rounded_rectangle(box, radius, fill=p["card"],
                        outline=p["edge"] if p is LIGHT else None,
                        width=2 if p is LIGHT else 0)


def strip(d, box, colour, tile_w):
    """tile_strip(): (tile_w - 28) x 3, aligned TOP_MID with y offset 5."""
    x0, y0, x1, _ = box
    w = tile_w - 28
    cx = (x0 + x1) / 2
    d.rounded_rectangle([cx - w / 2, y0 + 5, cx + w / 2, y0 + 8], 2, fill=colour)


def arrow_glyph(d, box, colour, kind="right"):
    """Stands in for the real arrow asset: direction and weight, not the art."""
    x0, y0, x1, y1 = box
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    s = min(x1 - x0, y1 - y0) * 0.40
    t = max(3, s * 0.30)
    if kind == "right":                      # shaft up, then turn right
        d.line([cx - s * 0.5, cy + s, cx - s * 0.5, cy - s * 0.2], fill=colour, width=int(t))
        d.line([cx - s * 0.5, cy - s * 0.2, cx + s * 0.35, cy - s * 0.2], fill=colour, width=int(t))
        d.polygon([(cx + s * 0.25, cy - s * 0.2 - t), (cx + s * 0.25, cy - s * 0.2 + t),
                   (cx + s * 0.8, cy - s * 0.2)], fill=colour)
    else:                                    # roundabout: ring plus exit
        r = s * 0.55
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=colour, width=int(t))
        d.line([cx, cy + r, cx, cy + s * 1.1], fill=colour, width=int(t))
        d.line([cx + r, cy, cx + s * 1.1, cy], fill=colour, width=int(t))
        d.polygon([(cx + s * 0.9, cy - t * 1.6), (cx + s * 0.9, cy + t * 1.6),
                   (cx + s * 1.4, cy)], fill=colour)


def home(p, route_colour, pressed=None):
    img = Image.new("RGB", (W, H), p["bg"])
    d = ImageDraw.Draw(img)
    top_bar(d, p)

    tw = (W - 3 * MIN_GAP) // 2
    th = (H - BAR_H - 3 * MIN_GAP) // 2
    col2 = MIN_GAP + tw + MIN_GAP
    row2 = BAR_H + MIN_GAP + th + MIN_GAP

    tiles = [
        ("nav",   [MIN_GAP, BAR_H + MIN_GAP, MIN_GAP + tw, BAR_H + MIN_GAP + th], route_colour),
        ("media", [col2, BAR_H + MIN_GAP, col2 + tw, BAR_H + MIN_GAP + th], p["music"]),
        ("clock", [MIN_GAP, row2, MIN_GAP + tw, row2 + th], p["map"]),
        ("set",   [col2, row2, col2 + tw, row2 + th], p["edge"]),
    ]
    for name, box, col in tiles:
        was = p["card"]
        if pressed == name:
            d.rounded_rectangle(box, 12, fill=p["press"])
        else:
            card(d, p, box)
        strip(d, box, col, tw)
        x0, y0, x1, y1 = box
        if name == "nav":
            arrow_glyph(d, [x0 - 4, y0 + 6, x0 + 62, y1 - 18], route_colour, "round")
            t = "450"
            f = font(26, True)
            d.text((x1 - 8 - d.textlength(t, font=f), y0 + 14), t, font=f, fill=p["text"])
            centre(d, [x0, y1 - 24, x1, y1 - 6], "NH-65 exit 3", font(13), p["dim"])
        elif name == "media":
            centre(d, [x0, y0 + 14, x1, y0 + 34], "Sahiba", font(14, True), p["music"])
            centre(d, [x0, y0 + 38, x1, y0 + 56], "Jasleen Royal", font(13), p["dim"])
        elif name == "clock":
            centre(d, [x0, y0 + 6, x1, y1], "Map", font(19, True), p["map"])
        else:
            centre(d, [x0, y0 + 8, x1, y1 - 18], "\u2699", font(26), p["dim"])
            centre(d, [x0, y1 - 28, x1, y1 - 8], "70%", font(13), p["dim"])
        _ = was
    return img


def nav(p, dist_m, dir_colour, zoom=1.0, slide=0):
    img = Image.new("RGB", (W, H), p["bg"])
    d = ImageDraw.Draw(img)
    top_bar(d, p)
    y0 = BAR_H

    if dist_m < NEAR_FROM_M:
        w = W - (W * dist_m // NEAR_FROM_M)
        d.rectangle([0, y0, w, y0 + 4], fill=dir_colour)

    side = 136 * zoom
    off = (136 - side) / 2
    arrow_glyph(d, [6 + off + slide, y0 + 8 + off, 6 + off + side + slide, y0 + 8 + off + side],
                dir_colour, "round")

    t = str(dist_m) if dist_m >= 100 else str(dist_m)
    f = font(46, True)
    d.text((148 + slide, y0 + 10), t, font=f, fill=p["text"])
    d.text((148 + slide + d.textlength(t, font=f) + 4, y0 + 36), "m", font=font(18), fill=p["text"])
    d.text((150, y0 + 70), "3rd exit, right", font=font(19, True), fill=p["text"])
    d.text((150, y0 + 96), "onto NH-65", font=font(17), fill=p["text"])
    d.text((150, y0 + 132), "then", font=font(14), fill=p["dim"])

    d.rectangle([0, H - 42, W, H], fill=p["bar"])
    for x, s in ((10, "4.2 km"), (125, "11 min"), (240, "21:25")):
        d.text((x, H - 30), s, font=font(17, True), fill=p["text"])
    return img


def main():
    rows = [
        ("night / home, roundabout ahead", home(DARK, DARK["warn"])),
        ("night / home, settings pressed", home(DARK, DARK["warn"], pressed="set")),
        ("night / nav 420 m, bar empty", nav(DARK, 420, DARK["warn"])),
        ("night / nav 200 m, pulse", nav(DARK, 200, DARK["warn"], zoom=1.17)),
        ("night / nav 50 m, nearly there", nav(DARK, 50, DARK["warn"])),
        ("night / nav sharp left, 90 m", nav(DARK, 90, RED)),
        ("day / home, no route", home(LIGHT, LIGHT["dim"])),
        ("day / home, roundabout ahead", home(LIGHT, LIGHT["warn"])),
        ("day / nav 150 m", nav(LIGHT, 150, LIGHT["warn"])),
        ("day / nav slight right, 300 m", nav(LIGHT, 300, LIGHT["accent"])),
        ("day / nav arriving, 40 m", nav(LIGHT, 40, LIGHT["good"])),
        ("day / nav new turn sliding in", nav(LIGHT, 260, LIGHT["warn"], slide=30)),
    ]
    cols, pad, cap = 3, 16, 20
    rowc = (len(rows) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (W + pad) + pad, rowc * (H + cap + pad) + pad),
                      "#5A5F66")
    d = ImageDraw.Draw(sheet)
    for i, (name, im) in enumerate(rows):
        x = pad + (i % cols) * (W + pad)
        y = pad + (i // cols) * (H + cap + pad)
        sheet.paste(im, (x, y))
        d.text((x, y + H + 4), name, font=font(13), fill="#FFFFFF")
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "screens.png")
    sheet.save(out)
    print("wrote", out, sheet.size)


if __name__ == "__main__":
    main()
