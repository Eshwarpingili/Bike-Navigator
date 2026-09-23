"""Generate the maneuver arrow images for the firmware.

Draws every arrow with Pillow at 4x resolution, downsamples for anti-aliasing and
writes 8-bit alpha masks as LVGL v8 image descriptors (LV_IMG_CF_ALPHA_8BIT), so the
firmware can tint them any colour. Also writes a PNG contact sheet for checking.

    python gen_arrows.py            # writes ../main/arrows.c and arrows_preview.png
"""

from __future__ import annotations

import math
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
OUT_C = HERE.parent / "main" / "arrows.c"
OUT_PREVIEW = HERE / "arrows_preview.png"

SIZES = {"big": 132, "small": 40}
SS = 4  # supersampling factor
DIM = 0.32  # alpha of the "other way" parts (ring, untaken branch)

STROKE = 0.19  # stroke width, in path units (paths span roughly 1.0)
HEAD_LEN = 0.36
HEAD_HALF = 0.26


class Shape:
    """Strokes and filled polygons in abstract units, rendered later to fit a box."""

    def __init__(self):
        self.items = []  # (kind, points, alpha, width)

    def stroke(self, pts, alpha=1.0, width=STROKE):
        self.items.append(("line", pts, alpha, width))

    def poly(self, pts, alpha=1.0):
        self.items.append(("poly", pts, alpha, 0))

    def circle(self, c, r, alpha=1.0):
        self.items.append(("circle", [c], alpha, r))

    def ring(self, c, r, alpha=1.0, width=STROKE):
        self.items.append(("ring", [c], alpha, (r, width)))

    def arrow(self, pts, alpha=1.0):
        """Polyline whose last segment ends in an arrowhead at the last point."""
        (x0, y0), (x1, y1) = pts[-2], pts[-1]
        dx, dy = x1 - x0, y1 - y0
        n = math.hypot(dx, dy)
        ux, uy = dx / n, dy / n
        bx, by = x1 - ux * HEAD_LEN, y1 - uy * HEAD_LEN
        self.stroke(pts[:-1] + [(bx + ux * 0.02, by + uy * 0.02)], alpha)
        px, py = -uy, ux
        self.poly([(x1, y1), (bx + px * HEAD_HALF, by + py * HEAD_HALF),
                   (bx - px * HEAD_HALF, by - py * HEAD_HALF)], alpha)

    def bounds(self):
        xs, ys = [], []
        for kind, pts, _a, w in self.items:
            if kind in ("circle", "ring"):
                (cx, cy), = pts
                r = w if kind == "circle" else w[0] + w[1] / 2
                xs += [cx - r, cx + r]
                ys += [cy - r, cy + r]
            else:
                pad = w / 2 if kind == "line" else 0
                for x, y in pts:
                    xs += [x - pad, x + pad]
                    ys += [y - pad, y + pad]
        return min(xs), min(ys), max(xs), max(ys)

    def render(self, size: int, margin: float = 0.06) -> Image.Image:
        big = size * SS
        x0, y0, x1, y1 = self.bounds()
        span = max(x1 - x0, y1 - y0)
        scale = big * (1 - 2 * margin) / span
        ox = (big - (x1 - x0) * scale) / 2 - x0 * scale
        oy = (big - (y1 - y0) * scale) / 2 - y0 * scale

        def tf(p):
            return (p[0] * scale + ox, p[1] * scale + oy)

        # Draw each alpha level on its own layer and combine with max(), so dim and
        # bright parts overlap cleanly instead of adding up.
        out = Image.new("L", (big, big), 0)
        for kind, pts, alpha, w in self.items:
            layer = Image.new("L", (big, big), 0)
            d = ImageDraw.Draw(layer)
            if kind == "line":
                lw = max(1, round(w * scale))
                tp = [tf(p) for p in pts]
                d.line(tp, fill=255, width=lw, joint="curve")
                r = lw / 2
                for x, y in (tp[0], tp[-1]):
                    d.ellipse([x - r, y - r, x + r, y + r], fill=255)
            elif kind == "poly":
                d.polygon([tf(p) for p in pts], fill=255)
            elif kind == "circle":
                (cx, cy), = [tf(pts[0])]
                r = w * scale
                d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)
            elif kind == "ring":
                (cx, cy), = [tf(pts[0])]
                r, rw = w[0] * scale, w[1] * scale
                d.ellipse([cx - r - rw / 2, cy - r - rw / 2, cx + r + rw / 2, cy + r + rw / 2], fill=255)
                d.ellipse([cx - r + rw / 2, cy - r + rw / 2, cx + r - rw / 2, cy + r - rw / 2], fill=0)
            if alpha < 1:
                layer = layer.point(lambda v, a=alpha: round(v * a))
            out = ImageChops.lighter(out, layer)
        return out.resize((size, size), Image.LANCZOS)


def heading(deg: float):
    """Unit vector for a compass-style angle: 0 = up, +90 = right."""
    r = math.radians(deg)
    return math.sin(r), -math.cos(r)


def turn(angle: float) -> Shape:
    """Stem going up, then a bend of `angle` degrees (negative = left)."""
    s = Shape()
    hx, hy = heading(angle)
    if abs(angle) > 100:  # sharp: bend high up so the arrow clears the stem
        pivot, leg, stem = (0.0, -0.45), 1.05, 0.9
    else:
        pivot, leg, stem = (0.0, 0.0), 0.95, 1.0 if abs(angle) < 60 else 0.85
    s.arrow([(0.0, stem), pivot, (pivot[0] + hx * leg, pivot[1] + hy * leg)])
    return s


def straight() -> Shape:
    s = Shape()
    s.arrow([(0.0, 1.0), (0.0, -0.9)])
    return s


def uturn(left: bool) -> Shape:
    s = Shape()
    sign = -1 if left else 1
    r = 0.36
    cx = sign * r
    arc = [(cx - sign * r * math.cos(math.radians(t)), -r * math.sin(math.radians(t)) - 0.1)
           for t in range(0, 181, 10)]
    s.arrow([(0.0, 0.9)] + arc + [(2 * cx, 0.55)])
    return s


def fork(left: bool, exit_ramp: bool) -> Shape:
    """Keep left/right (Y fork) or exit left/right (ramp leaving a straight road)."""
    s = Shape()
    sign = -1 if left else 1
    if exit_ramp:
        s.stroke([(0.0, 1.0), (0.0, -0.9)], alpha=DIM)
        s.arrow([(0.0, 1.0), (0.0, 0.35), (sign * 0.6, -0.35)])
    else:
        s.stroke([(0.0, 1.0), (0.0, 0.3), (-sign * 0.5, -0.6)], alpha=DIM)
        s.arrow([(0.0, 1.0), (0.0, 0.3), (sign * 0.5, -0.75)])
    return s


EXIT_ANGLES = {"se": 135, "e": 90, "ne": 45, "n": 0, "nw": -45, "w": -90, "sw": -135, "s": 180}


def roundabout(exit_name: str, clockwise: bool) -> Shape:
    """Entry from the bottom, ring, bright arc along the driving direction, exit arrow.

    Right-hand traffic drives counter-clockwise (compass angle decreasing from the
    south entry); left-hand traffic, e.g. India, drives clockwise.
    """
    s = Shape()
    R = 0.42
    s.ring((0.0, 0.0), R, alpha=DIM, width=STROKE * 0.75)
    target = EXIT_ANGLES[exit_name]
    start = 180.0
    if exit_name == "s":
        # Going back the way you came: split entry and exit so they don't overlap.
        # Traffic keeps right (entry bottom-right) or left (entry bottom-left).
        start, target = (220.0, 140.0) if clockwise else (140.0, 220.0)
    if clockwise:
        end = target if target > start else target + 360
    else:
        end = target if target < start else target - 360
    if abs(end - start) < 1:
        end = start + (360 if clockwise else -360)
    steps = max(4, int(abs(end - start) / 10))
    arc = []
    for i in range(steps + 1):
        a = start + (end - start) * i / steps
        hx, hy = heading(a)
        arc.append((hx * R, hy * R))
    ex, ey = heading(target)
    exit_tip = (ex * (R + 0.62), ey * (R + 0.62))
    entry_x, entry_y = arc[0]
    s.stroke([(entry_x, R + 0.55), (entry_x, entry_y)])
    if exit_name == "s":  # straight down, parallel to the entry
        s.arrow(arc + [(arc[-1][0], R + 0.62)])
    else:
        s.arrow(arc + [exit_tip])
    return s


def destination() -> Shape:
    """Map pin."""
    s = Shape()
    r = 0.42
    s.circle((0.0, -0.2), r)
    s.poly([(-r * 0.86, 0.02), (r * 0.86, 0.02), (0.0, 0.95)])
    return s


def off_route() -> Shape:
    """Warning triangle; the exclamation mark is cut out when rendering."""
    s = Shape()
    s.poly([(0.0, -0.9), (0.98, 0.8), (-0.98, 0.8)])
    return s


def render_off_route(size: int) -> Image.Image:
    img = off_route().render(size)
    d = ImageDraw.Draw(img)
    w = size
    bar_w = max(2, round(w * 0.09))
    cx = w / 2
    d.rounded_rectangle([cx - bar_w / 2, w * 0.36, cx + bar_w / 2, w * 0.66], radius=bar_w / 2, fill=0)
    d.ellipse([cx - bar_w * 0.6, w * 0.71, cx + bar_w * 0.6, w * 0.71 + bar_w * 1.2], fill=0)
    return img


# name -> Shape factory. Order matters only for the preview sheet.
SHAPES = {
    "straight": straight,
    "slight_left": lambda: turn(-45),
    "slight_right": lambda: turn(45),
    "left": lambda: turn(-90),
    "right": lambda: turn(90),
    "sharp_left": lambda: turn(-135),
    "sharp_right": lambda: turn(135),
    "uturn_left": lambda: uturn(True),
    "uturn_right": lambda: uturn(False),
    "keep_left": lambda: fork(True, False),
    "keep_right": lambda: fork(False, False),
    "exit_left": lambda: fork(True, True),
    "exit_right": lambda: fork(False, True),
    "destination": destination,
}
EXITS = ["se", "e", "ne", "n", "nw", "w", "sw", "s"]
for _e in EXITS:
    SHAPES[f"rb_rht_{_e}"] = (lambda e: lambda: roundabout(e, clockwise=False))(_e)
    SHAPES[f"rb_lht_{_e}"] = (lambda e: lambda: roundabout(e, clockwise=True))(_e)

# Direction code (PROTOCOL.md) -> image name. Codes not listed draw no arrow.
DIRECTION_IMAGE = {
    1: "straight", 2: "slight_left", 3: "slight_right", 4: "destination",
    5: "destination", 6: "keep_left", 7: "keep_right", 8: "left", 9: "off_route",
    10: "right", 11: "sharp_left", 12: "sharp_right", 13: "straight",
    14: "uturn_left", 15: "uturn_right", 16: "straight", 17: "straight",
    18: "straight", 19: "straight", 20: "straight", 21: "exit_left", 22: "exit_right",
}
for _i, _e in enumerate(EXITS):
    DIRECTION_IMAGE[23 + _i] = f"rb_rht_{_e}"
    DIRECTION_IMAGE[31 + _i] = f"rb_lht_{_e}"


def render(name: str, size: int) -> Image.Image:
    if name == "off_route":
        return render_off_route(size)
    return SHAPES[name]().render(size)


def c_array(name: str, img: Image.Image) -> str:
    data = img.tobytes()
    rows = []
    for i in range(0, len(data), 24):
        rows.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",")
    w, h = img.size
    return (
        f"static const uint8_t {name}_map[] = {{\n" + "\n".join(rows) + "\n};\n"
        f"static const lv_img_dsc_t {name} = {{\n"
        f"    .header.cf = LV_IMG_CF_ALPHA_8BIT,\n"
        f"    .header.always_zero = 0,\n"
        f"    .header.reserved = 0,\n"
        f"    .header.w = {w},\n"
        f"    .header.h = {h},\n"
        f"    .data_size = sizeof({name}_map),\n"
        f"    .data = {name}_map,\n"
        f"}};\n"
    )


def main():
    names = list(SHAPES) + ["off_route"]
    parts = [
        "/* Generated by firmware/tools/gen_arrows.py - do not edit. */\n",
        '#include "arrows.h"\n\n',
    ]
    previews = []
    for size_name, size in SIZES.items():
        for name in names:
            img = render(name, size)
            parts.append(c_array(f"img_{name}_{size_name}", img))
            if size_name == "big":
                previews.append((name, img))
        parts.append("\n")

    for size_name in SIZES:
        parts.append(f"static const lv_img_dsc_t *const direction_{size_name}[] = {{\n")
        for code in range(0, 39):
            name = DIRECTION_IMAGE.get(code)
            parts.append(f"    /* {code:2d} */ {'&img_' + name + '_' + size_name if name else 'NULL'},\n")
        parts.append("};\n\n")

    parts.append(
        "const lv_img_dsc_t *arrow_for_direction(uint8_t direction, bool small)\n"
        "{\n"
        "    const lv_img_dsc_t *const *table = small ? direction_small : direction_big;\n"
        "    if (direction >= sizeof(direction_big) / sizeof(direction_big[0])) {\n"
        "        return NULL;\n"
        "    }\n"
        "    return table[direction];\n"
        "}\n"
    )
    OUT_C.parent.mkdir(parents=True, exist_ok=True)
    OUT_C.write_text("".join(parts), encoding="ascii", newline="\n")

    cols = 8
    cell = SIZES["big"] + 24
    rows = math.ceil(len(previews) / cols)
    sheet = Image.new("RGB", (cols * cell, rows * cell), (18, 18, 18))
    font = ImageFont.load_default()
    for i, (name, img) in enumerate(previews):
        x, y = (i % cols) * cell, (i // cols) * cell
        sheet.paste(Image.new("RGB", img.size, (255, 255, 255)), (x + 12, y + 4), img)
        ImageDraw.Draw(sheet).text((x + 12, y + cell - 18), name, fill=(150, 150, 150), font=font)
    sheet.save(OUT_PREVIEW)
    print(f"wrote {OUT_C} ({OUT_C.stat().st_size // 1024} KB) and {OUT_PREVIEW}")


if __name__ == "__main__":
    main()
