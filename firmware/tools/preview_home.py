#!/usr/bin/env python3
"""The home screen over each candidate wallpaper, with glass cards.

Same numbers ui.c uses, same real Inter, and the whole thing quantized to
RGB565 at the end, so what comes out is what the panel will show.

    python firmware/tools/preview_home.py
"""

import os
import sys
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import gen_wallpaper as wp              # noqa: E402
import preview_screens as ps            # noqa: E402

W, H = ps.W, ps.H
BAR_H, MIN_GAP, RADIUS = ps.BAR_H, ps.MIN_GAP, ps.RADIUS

# How solid the glass is. Far more opaque than a web glass panel, and
# deliberately: this is read in sunlight at 25 km/h, so the wallpaper is
# allowed to tint the card, not to show through it.
GLASS = 0.86


def glass_card(img, p, box, pressed=False, radius=RADIUS):
    """A translucent card over whatever is behind it, with a lit top edge."""
    x0, y0, x1, y1 = [int(v) for v in box]
    size = (x1 - x0, y1 - y0)
    top = p["press"] if pressed else p["card"]
    bot = p["press_grad"] if pressed else p["card_grad"]

    fill = wp.Image.new("RGB", size)
    fill.paste(ps.vgrad(size, top, bot), (0, 0))

    behind = img.crop((x0, y0, x1, y1))
    blended = Image.blend(behind, fill, GLASS)
    img.paste(blended, (x0, y0), ps.rounded_mask(size, radius))

    # Ingredient three: an edge brighter than the fill, lit along the top.
    ps.outline(img, (x0, y0, x1, y1), radius, ps.rgb(p["edge"]), 255)
    d = ImageDraw.Draw(img, "RGBA")
    light = (255, 255, 255, 90) if p is ps.DARK else (255, 255, 255, 190)
    d.line([x0 + radius - 4, y0 + 1, x1 - radius + 4, y0 + 1], fill=light, width=1)


def home(p, bg, route_colour, code=25, pressed=None, no_route=False):
    img = bg.copy()
    d = ImageDraw.Draw(img, "RGBA")

    # The top bar is glass too, so the wallpaper runs under it.
    bar = img.crop((0, 0, W, BAR_H))
    tint = Image.new("RGB", (W, BAR_H), ps.rgb(p["bar"]))
    img.paste(Image.blend(bar, tint, 0.72), (0, 0))
    d.line([0, BAR_H, W, BAR_H], fill=ps.rgb(p["edge"]))
    d.text((12, 5), "BikeNav", font=ps.font(14), fill=ps.rgb(p["dim"]))
    f = ps.font(15, True)
    d.text((W - 12 - d.textlength("21:14", font=f), 4), "21:14", font=f,
           fill=ps.rgb(p["text"]))

    tw = (W - 3 * MIN_GAP) // 2
    th = (H - BAR_H - 3 * MIN_GAP) // 2
    col2 = MIN_GAP + tw + MIN_GAP
    row2 = BAR_H + MIN_GAP + th + MIN_GAP
    tiles = [
        ("nav", [MIN_GAP, BAR_H + MIN_GAP, MIN_GAP + tw, BAR_H + MIN_GAP + th],
         p["dim"] if no_route else route_colour),
        ("media", [col2, BAR_H + MIN_GAP, col2 + tw, BAR_H + MIN_GAP + th], p["music"]),
        ("clock", [MIN_GAP, row2, MIN_GAP + tw, row2 + th], p["clock"]),
        ("set", [col2, row2, col2 + tw, row2 + th], p["edge"]),
    ]
    for name, box, col in tiles:
        hit = pressed == name
        s = 2 if hit else 0
        b = [box[0] + s, box[1] + s, box[2] - s, box[3] - s]
        glass_card(img, p, b, pressed=hit)
        ps.strip(img, b, col, tw)
        x0, y0, x1, y1 = b
        if name == "nav":
            if no_route:
                ps.centre(d, [x0, y0, x1, y1], "No route", ps.font(18), ps.rgb(p["dim"]))
            else:
                ps.arrow(img, code, [x0 + 2, y0 + 12, x0 + 62, y1 - 16], route_colour)
                ff = ps.font(27, True)
                d.text((x1 - 10 - d.textlength("450", font=ff), y0 + 16), "450",
                       font=ff, fill=ps.rgb(p["text"]))
                ps.centre(d, [x0, y1 - 26, x1, y1 - 7], "NH-65 exit 3",
                          ps.font(12), ps.rgb(p["dim"]))
        elif name == "media":
            ps.centre(d, [x0, y0 + 14, x1, y0 + 36], "Sahiba", ps.font(14, True),
                      ps.rgb(p["music"]))
            ps.centre(d, [x0, y0 + 38, x1, y0 + 58], "Jasleen Royal", ps.font(13),
                      ps.rgb(p["dim"]))
        elif name == "clock":
            ps.centre(d, [x0, y0 + 6, x1, y1], "21:14" if no_route else "Map",
                      ps.font(40 if no_route else 19, True),
                      ps.rgb(p["text"] if no_route else p["clock"]))
        else:
            ps.centre(d, [x0, y0 + 10, x1, y1 - 20], "⚙", ps.sysfont(26),
                      ps.rgb(p["dim"]))
            ps.centre(d, [x0, y1 - 30, x1, y1 - 8], "70%", ps.font(13), ps.rgb(p["dim"]))
    return img


def main():
    rows = []
    for name in wp.DARK_SET:
        bg = wp.dither565(wp.render(wp.SCHEMES[name]))
        rows.append((f"night / {name}", home(ps.DARK, bg, ps.DARK["warn"])))
    for name in wp.LIGHT_SET:
        bg = wp.dither565(wp.render(wp.SCHEMES[name]))
        rows.append((f"day / {name}", home(ps.LIGHT, bg, ps.LIGHT["warn"])))
    # The two states worth seeing beside the choice.
    bg = wp.dither565(wp.render(wp.SCHEMES[wp.DARK_SET[0]]))
    rows.append(("night / no route", home(ps.DARK, bg, 0, no_route=True)))
    rows.append(("night / music under thumb",
                 home(ps.DARK, bg, ps.DARK["warn"], pressed="media")))
    bgl = wp.dither565(wp.render(wp.SCHEMES[wp.LIGHT_SET[0]]))
    rows.append(("day / settings under thumb",
                 home(ps.LIGHT, bgl, ps.LIGHT["warn"], pressed="set")))

    cols, pad, cap = 3, 16, 20
    rowc = (len(rows) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (W + pad) + pad, rowc * (H + cap + pad) + pad),
                      "#4A4F57")
    d = ImageDraw.Draw(sheet)
    for i, (name, im) in enumerate(rows):
        x = pad + (i % cols) * (W + pad)
        y = pad + (i // cols) * (H + cap + pad)
        sheet.paste(ps.to565(im), (x, y))
        d.text((x, y + H + 4), name, font=ps.font(13), fill="#FFFFFF")
    out = os.path.join(HERE, "home.png")
    sheet.save(out)
    print("wrote", out, sheet.size)


if __name__ == "__main__":
    main()
