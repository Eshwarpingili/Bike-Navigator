#!/usr/bin/env python3
"""Draw the board's screens on the PC, so judging a design costs an image
instead of a BURN-and-replug cycle and a photograph.

Fidelity, and its limits:
  * Colours, gradients, radii and geometry are the numbers ui.c uses.
  * Arrows are the real assets - gen_arrows renders the same alpha glyphs that
    get compiled into arrows.c, tinted the same way LVGL tints them.
  * Output is quantized to RGB565 with ordered dithering, as the panel and
    LV_DITHER_GRADIENT do, so gradient banding shows up here rather than on the
    handlebars.
  * Type is NOT exact. The board runs Montserrat compiled into LVGL as bitmaps,
    and no Montserrat TTF ships in this tree, so the nearest installed face
    stands in. Sizes and weights are right; letterforms are approximate.

    python firmware/tools/preview_screens.py
"""

import os
import sys
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_arrows  # noqa: E402

W, H = 320, 240
BAR_H = 26
MIN_GAP = 14          # 2 * HIT_PAD + 2
RADIUS = 18           # CARD_RADIUS
NEAR_FROM_M = 400

# set_palette() in ui.c, verbatim.
DARK = dict(
    bg=0x0C1017, bg_grad=0x000000, card=0x1E242E, card_grad=0x141922,
    text=0xFFFFFF, dim=0xAAB3BF, accent=0x5AB0FF, warn=0xFFB300, good=0x3DDC84,
    bar=0x151A22, edge=0x2C333F, music=0xA78BFA, map=0x2DD4BF,
    press=0x2A313C, press_grad=0x222833,
)
LIGHT = dict(
    bg=0xE7ECF2, bg_grad=0xD5DDE7, card=0xFFFFFF, card_grad=0xF1F4F9,
    text=0x000000, dim=0x333C47, accent=0x1D4ED8, warn=0xB45309, good=0x047857,
    bar=0xC2CBD6, edge=0x7C8A9A, music=0x5B21B6, map=0x115E59,
    press=0xE6ECF2, press_grad=0xDAE1EA,
)
RED = 0xE53935


def rgb(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)


def mix(c1, c2, ratio_):
    """lv_color_mix(c1, c2, ratio): c1 by ratio/255, c2 by the rest."""
    a, b = rgb(c1), rgb(c2)
    return tuple((a[i] * ratio_ + b[i] * (255 - ratio_)) // 255 for i in range(3))


def font(size, bold=False):
    names = ("seguisb.ttf", "segoeui.ttf") if bold else ("segoeui.ttf",)
    for name in names:
        p = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts", name)
        if os.path.exists(p):
            return ImageFont.truetype(p, size)
    for name in ("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf",):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def vgrad(size, top, bottom):
    """A two-stop vertical gradient, the only kind LVGL is set up for here."""
    w, h = size
    g = Image.new("RGB", (1, h))
    t, b = rgb(top), rgb(bottom)
    for y in range(h):
        f = y / max(1, h - 1)
        g.putpixel((0, y), tuple(int(t[i] + (b[i] - t[i]) * f) for i in range(3)))
    return g.resize((w, h))


def rounded_mask(size, radius):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, size[0] - 1, size[1] - 1], radius, fill=255)
    return m


def card(img, p, box, radius=RADIUS, pressed=False):
    x0, y0, x1, y1 = [int(v) for v in box]
    size = (x1 - x0, y1 - y0)
    top = p["press"] if pressed else p["card"]
    bot = p["press_grad"] if pressed else p["card_grad"]
    img.paste(vgrad(size, top, bot), (x0, y0), rounded_mask(size, radius))
    ImageDraw.Draw(img).rounded_rectangle([x0, y0, x1 - 1, y1 - 1], radius,
                                          outline=rgb(p["edge"]), width=1)


def centre(d, box, text, f, fill):
    x0, y0, x1, y1 = box
    w = d.textlength(text, font=f)
    a = f.getbbox(text)
    d.text((x0 + (x1 - x0 - w) / 2, y0 + (y1 - y0 - (a[3] - a[1])) / 2 - a[1]),
           text, font=f, fill=fill)


def arrow(img, code, box, colour, zoom=1.0):
    """The real glyph, tinted - LVGL recolours an ALPHA_8BIT image exactly so."""
    x0, y0, x1, y1 = [int(v) for v in box]
    size = max(8, int(min(x1 - x0, y1 - y0) * zoom))
    name = gen_arrows.DIRECTION_IMAGE.get(code)
    if name is None:
        return
    alpha = gen_arrows.render(name, size)
    tint = Image.new("RGB", alpha.size, colour if isinstance(colour, tuple) else rgb(colour))
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    img.paste(tint, (cx - size // 2, cy - size // 2), alpha)


def screen(p):
    img = Image.new("RGB", (W, H))
    img.paste(vgrad((W, H), p["bg"], p["bg_grad"]), (0, 0))
    return img


def top_bar(img, p, clock="21:14"):
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, W, BAR_H], fill=rgb(p["bar"]))
    d.line([0, BAR_H, W, BAR_H], fill=rgb(p["edge"]))
    d.text((12, 5), "BikeNav", font=font(14), fill=rgb(p["dim"]))
    f = font(15, True)
    d.text((W - 12 - d.textlength(clock, font=f), 4), clock, font=f, fill=rgb(p["text"]))


def strip(img, box, colour, tile_w):
    x0, y0, x1, _ = box
    w = tile_w - 28
    cx = (x0 + x1) / 2
    ImageDraw.Draw(img).rounded_rectangle([cx - w / 2, y0 + 5, cx + w / 2, y0 + 8],
                                          2, fill=rgb(colour) if isinstance(colour, int) else colour)


def home(p, route_colour, code=25, pressed=None, no_route=False):
    img = screen(p)
    top_bar(img, p)
    d = ImageDraw.Draw(img)
    tw = (W - 3 * MIN_GAP) // 2
    th = (H - BAR_H - 3 * MIN_GAP) // 2
    col2 = MIN_GAP + tw + MIN_GAP
    row2 = BAR_H + MIN_GAP + th + MIN_GAP
    tiles = [
        ("nav", [MIN_GAP, BAR_H + MIN_GAP, MIN_GAP + tw, BAR_H + MIN_GAP + th],
         p["dim"] if no_route else route_colour),
        ("media", [col2, BAR_H + MIN_GAP, col2 + tw, BAR_H + MIN_GAP + th], p["music"]),
        ("clock", [MIN_GAP, row2, MIN_GAP + tw, row2 + th], p["map"]),
        ("set", [col2, row2, col2 + tw, row2 + th], p["edge"]),
    ]
    for name, box, col in tiles:
        hit = pressed == name
        shrink = 2 if hit else 0        # transform_width/height -3, halved per side
        b = [box[0] + shrink, box[1] + shrink, box[2] - shrink, box[3] - shrink]
        card(img, p, b, pressed=hit)
        strip(img, b, col, tw)
        x0, y0, x1, y1 = b
        if name == "nav":
            if no_route:
                centre(d, [x0, y0, x1, y1], "No route", font(18), rgb(p["dim"]))
            else:
                arrow(img, code, [x0 + 2, y0 + 12, x0 + 62, y1 - 16], route_colour)
                f = font(27, True)
                d.text((x1 - 10 - d.textlength("450", font=f), y0 + 16), "450",
                       font=f, fill=rgb(p["text"]))
                centre(d, [x0, y1 - 26, x1, y1 - 7], "NH-65 exit 3", font(12), rgb(p["dim"]))
        elif name == "media":
            centre(d, [x0, y0 + 14, x1, y0 + 36], "Sahiba", font(14, True), rgb(p["music"]))
            centre(d, [x0, y0 + 38, x1, y0 + 58], "Jasleen Royal", font(13), rgb(p["dim"]))
        elif name == "clock":
            centre(d, [x0, y0 + 6, x1, y1], "21:14" if no_route else "Map",
                   font(40 if no_route else 19, True),
                   rgb(p["text"] if no_route else p["map"]))
        else:
            centre(d, [x0, y0 + 10, x1, y1 - 20], "\u2699", font(26), rgb(p["dim"]))
            centre(d, [x0, y1 - 30, x1, y1 - 8], "70%", font(13), rgb(p["dim"]))
    return img


def nav(p, dist_m, dir_colour, code=25, zoom=1.0, slide=0, street="3rd exit, right"):
    img = screen(p)
    top_bar(img, p)
    d = ImageDraw.Draw(img)
    y0 = BAR_H

    if dist_m < NEAR_FROM_M:
        w = W - (W * dist_m // NEAR_FROM_M)
        if w > 0:
            bar = Image.new("RGB", (int(w), 4))
            a, b = mix(dir_colour, p["bg"], 90), rgb(dir_colour)
            for x in range(int(w)):
                f = x / max(1, w - 1)
                col = tuple(int(a[i] + (b[i] - a[i]) * f) for i in range(3))
                bar.paste(col, (x, 0, x + 1, 4))
            img.paste(bar, (0, y0))

    arrow(img, code, [6 + slide, y0 + 10, 142 + slide, y0 + 146], dir_colour, zoom)

    t = "Arrived" if dist_m == 0 else str(dist_m)
    f = font(44 if dist_m else 30, True)
    d.text((150 + slide, y0 + 12), t, font=f, fill=rgb(p["text"]))
    if dist_m:
        d.text((150 + slide + d.textlength(t, font=f) + 5, y0 + 40), "m",
               font=font(18), fill=rgb(p["text"]))
    d.text((150, y0 + 72), street, font=font(18, True), fill=rgb(p["text"]))
    d.text((150, y0 + 97), "onto NH-65", font=font(16), fill=rgb(p["text"]))
    d.text((150, y0 + 134), "then", font=font(13), fill=rgb(p["dim"]))

    d.rectangle([0, H - 42, W, H], fill=rgb(p["bar"]))
    d.line([0, H - 42, W, H - 42], fill=rgb(p["edge"]))
    for x, s in ((12, "4.2 km"), (126, "11 min"), (238, "21:25")):
        d.text((x, H - 30), s, font=font(17, True), fill=rgb(p["text"]))
    return img


BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def to565(img):
    """What the panel shows: 5/6/5 bits, ordered-dithered the way LVGL does."""
    px = img.load()
    w, h = img.size
    out = Image.new("RGB", (w, h))
    o = out.load()
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            t = BAYER[y & 3][x & 3]
            r = min(255, r + (t >> 1)) >> 3
            g = min(255, g + (t >> 2)) >> 2
            b = min(255, b + (t >> 1)) >> 3
            o[x, y] = (r * 255 // 31, g * 255 // 63, b * 255 // 31)
    return out


def main():
    rows = [
        ("night / home, roundabout ahead", home(DARK, DARK["warn"], 25)),
        ("night / home, settings under thumb", home(DARK, DARK["warn"], 25, pressed="set")),
        ("night / home, no route", home(DARK, 0, no_route=True)),
        ("night / nav 420 m, bar empty", nav(DARK, 420, DARK["warn"], 25)),
        ("night / nav 200 m, arrow pulsing", nav(DARK, 200, DARK["warn"], 25, zoom=1.17)),
        ("night / nav 50 m", nav(DARK, 50, DARK["warn"], 25)),
        ("night / sharp left, 90 m", nav(DARK, 90, RED, 11, street="Sharp left")),
        ("night / steep bend right, 150 m", nav(DARK, 150, RED, 44, street="Steep bend right")),
        ("night / flyover, 260 m", nav(DARK, 260, DARK["warn"], 45, street="Take the flyover")),
        ("day / home, roundabout ahead", home(LIGHT, LIGHT["warn"], 25)),
        ("day / home, music under thumb", home(LIGHT, LIGHT["warn"], 25, pressed="media")),
        ("day / home, no route", home(LIGHT, 0, no_route=True)),
        ("day / nav 150 m", nav(LIGHT, 150, LIGHT["warn"], 25)),
        ("day / slight right, 300 m", nav(LIGHT, 300, LIGHT["accent"], 3, street="Bear right")),
        ("day / new turn sliding in", nav(LIGHT, 260, LIGHT["warn"], 25, slide=30)),
        ("day / underpass, 80 m", nav(LIGHT, 80, LIGHT["warn"], 46, street="Take the underpass")),
        ("day / arrived", nav(LIGHT, 0, LIGHT["good"], 4, street="You have arrived")),
        ("day / off route", nav(LIGHT, 120, RED, 9, street="Off route")),
    ]
    cols, pad, cap = 3, 16, 20
    rowc = (len(rows) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (W + pad) + pad, rowc * (H + cap + pad) + pad), "#4A4F57")
    d = ImageDraw.Draw(sheet)
    for i, (name, im) in enumerate(rows):
        x = pad + (i % cols) * (W + pad)
        y = pad + (i // cols) * (H + cap + pad)
        sheet.paste(to565(im), (x, y))
        d.text((x, y + H + 4), name, font=font(13), fill="#FFFFFF")
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "screens.png")
    sheet.save(out)
    print("wrote", out, sheet.size)


if __name__ == "__main__":
    main()
