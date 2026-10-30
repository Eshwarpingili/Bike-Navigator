#!/usr/bin/env python3
"""The CarPlay-dashboard layout, on a 320x240 panel.

What is borrowed from CarPlay is the STRUCTURE, not the picture: one panel
that dominates, a column of cards beside it with deliberately unequal heights,
everything heavily rounded, and colour bleeding out from behind the cards so
the screen reads as lit rather than printed.

What is not borrowed: the map. The dominant panel here is the next turn, which
is the thing a rider on a bike actually needs the screen for.

    python firmware/tools/preview_dash.py
"""

import os
import sys
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import gen_wallpaper as wp              # noqa: E402
import preview_screens as ps            # noqa: E402
from preview_home import glass_card     # noqa: E402

W, H = ps.W, ps.H
BAR = 22            # thinner than before: the dashboard needs the height
GAP = 14            # MIN_GAP, so no two hit areas can touch
R = ps.RADIUS

# Left panel takes the width a turn arrow and a big number need; the column
# gets what is left. 170/108 rather than a half-and-half split, because the
# arrow and the distance are what gets read at speed.
PANEL_W = 170
COL_W = W - PANEL_W - 3 * GAP
COL_X = GAP + PANEL_W + GAP
BODY_Y = BAR + GAP
BODY_H = H - BAR - 2 * GAP

# Unequal on purpose. Music needs two lines, the clock needs one big one, the
# status row needs almost nothing - so they get 76 / 54 / 42 of the 186.
H_MUSIC, H_CLOCK, H_STAT = 76, 54, 42


def dash(p, bg, turn_col, code=25, dist="450", street="3rd exit, right",
         pressed=None, no_route=False):
    img = bg.copy()
    d = ImageDraw.Draw(img, "RGBA")

    # Status strip: translucent, so the colour behind runs under it.
    strip = img.crop((0, 0, W, BAR))
    img.paste(Image.blend(strip, Image.new("RGB", (W, BAR), ps.rgb(p["bar"])), 0.55), (0, 0))
    d.text((10, 3), "BikeNav", font=ps.font(12), fill=ps.rgb(p["dim"]))
    f = ps.font(13, True)
    d.text((W - 10 - d.textlength("21:14", font=f), 2), "21:14", font=f, fill=ps.rgb(p["text"]))

    # ---- the panel that dominates: the next turn ----
    box = [GAP, BODY_Y, GAP + PANEL_W, BODY_Y + BODY_H]
    glass_card(img, p, box, pressed=(pressed == "nav"))
    x0, y0, x1, y1 = box
    if no_route:
        ps.centre(d, box, "No route", ps.font(20), ps.rgb(p["dim"]))
    else:
        # The approach bar lives on the panel's own top edge now, where it
        # belongs to the instruction rather than floating above everything.
        ps.arrow(img, code, [x0 + 4, y0 + 16, x0 + 78, y0 + 94], turn_col)
        # Sized to the widest real case ("1.2 km"), measured rather than
        # guessed: a distance clipped by the panel edge is worse than a
        # smaller one that always fits.
        ff = ps.font(38, True)
        uf = ps.font(15)
        avail = (x1 - 10) - (x0 + 82)
        while d.textlength(dist, font=ff) + d.textlength("m", font=uf) + 4 > avail and ff.size > 22:
            ff = ps.font(ff.size - 2, True)
        d.text((x0 + 82, y0 + 26), dist, font=ff, fill=ps.rgb(p["text"]))
        d.text((x0 + 82 + d.textlength(dist, font=ff) + 4, y0 + 26 + ff.size - 15), "m",
               font=uf, fill=ps.rgb(p["dim"]))
        d.text((x0 + 12, y0 + 104), street, font=ps.font(17, True), fill=ps.rgb(p["text"]))
        d.text((x0 + 12, y0 + 128), "onto NH-65", font=ps.font(14), fill=ps.rgb(p["dim"]))
        # Trip line along the bottom of the panel, not a separate bar.
        d.line([x0 + 12, y1 - 32, x1 - 12, y1 - 32], fill=ps.rgb(p["edge"]))
        for dx, s in ((12, "4.2 km"), (66, "11 min"), (120, "21:25")):
            d.text((x0 + dx, y1 - 25), s, font=ps.font(13, True), fill=ps.rgb(p["dim"]))

    # ---- the column: three cards, three different heights ----
    y = BODY_Y
    for name, hgt in (("media", H_MUSIC), ("clock", H_CLOCK), ("set", H_STAT)):
        b = [COL_X, y, COL_X + COL_W, y + hgt]
        glass_card(img, p, b, pressed=(pressed == name), radius=R)
        cx0, cy0, cx1, cy1 = b
        if name == "media":
            ps.centre(d, [cx0, cy0 + 12, cx1, cy0 + 34], "Sahiba",
                      ps.font(14, True), ps.rgb(p["music"]))
            ps.centre(d, [cx0, cy0 + 32, cx1, cy0 + 52], "Jasleen Royal",
                      ps.font(12), ps.rgb(p["dim"]))
            for dx, g in ((-30, "⏮"), (0, "⏸"), (30, "⏭")):
                ps.centre(d, [cx0 + dx, cy1 - 26, cx1 + dx, cy1 - 6], g,
                          ps.sysfont(15), ps.rgb(p["text"]))
        elif name == "clock":
            ps.centre(d, [cx0, cy0 + 6, cx1, cy1], "21:14", ps.font(32, True),
                      ps.rgb(p["text"]))
        else:
            ps.centre(d, [cx0, cy0 + 2, cx1 - 14, cy1], "70%", ps.font(16, True),
                      ps.rgb(p["dim"]))
            ps.centre(d, [cx0 + 26, cy0 + 2, cx1 + 26, cy1], "⚙",
                      ps.sysfont(17), ps.rgb(p["dim"]))
        y += hgt + GAP
    return img


def main():
    night = Image.open(os.path.join(HERE, "wp_night.png")).convert("RGB")
    plum = night
    day = Image.open(os.path.join(HERE, "wp_day.png")).convert("RGB")
    rows = [
        ("night / roundabout, 450 m", dash(ps.DARK, night, ps.DARK["warn"])),
        ("night / sharp left, 90 m", dash(ps.DARK, night, 0xE53935, 11, "90", "Sharp left")),
        ("night / arriving, 40 m", dash(ps.DARK, plum, ps.DARK["good"], 4, "40", "You have arrived")),
        ("day / roundabout, 150 m", dash(ps.LIGHT, day, ps.LIGHT["warn"], 25, "150")),
        ("day / music under thumb", dash(ps.LIGHT, day, ps.LIGHT["warn"], 25, "150",
                                         pressed="media")),
        ("night / no route", dash(ps.DARK, night, 0, no_route=True)),
    ]
    cols, pad, cap = 3, 16, 20
    rowc = (len(rows) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (W + pad) + pad, rowc * (H + cap + pad) + pad), "#4A4F57")
    d = ImageDraw.Draw(sheet)
    for i, (name, im) in enumerate(rows):
        x = pad + (i % cols) * (W + pad)
        y = pad + (i // cols) * (H + cap + pad)
        sheet.paste(ps.to565(im), (x, y))
        d.text((x, y + H + 4), name, font=ps.font(13), fill="#FFFFFF")
    out = os.path.join(HERE, "dash.png")
    sheet.save(out)
    print("wrote", out, sheet.size)


if __name__ == "__main__":
    main()
