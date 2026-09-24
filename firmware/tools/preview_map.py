#!/usr/bin/env python3
"""Draw a packed map on the PC, exactly the way the board will.

Two reasons this exists. The obvious one: look at the data before spending a
flash cycle on it. The better one: the board's renderer is arithmetic that is
easy to get subtly wrong - a sign flip, a swapped axis - and wrong maths on a
2.4 inch screen in a pocket is miserable to diagnose. Getting the same maths
right here first, where it can be looked at, is far cheaper.

    python tools/preview_map.py map.bin --at 17.4455 78.3489 --heading 45
"""
import argparse
import math
import struct
from pathlib import Path

HEADER = "<6sIiiHHI"
SCREEN_W, SCREEN_H = 320, 214       # the board's map area
METRES_ACROSS = 700.0               # how much world fits across the screen
WIDTHS = {1: 5, 2: 4, 3: 3, 4: 2}   # by road class
SHADES = {1: (255, 214, 120), 2: (245, 245, 245), 3: (200, 205, 215), 4: (120, 128, 140)}


class Map:
    def __init__(self, path):
        data = Path(path).read_bytes()
        size = struct.calcsize(HEADER)
        magic, tile_deg, lat0, lon0, rows, cols, count = struct.unpack(HEADER, data[:size])
        if magic != b"BNMAP1":
            raise SystemExit("not a BikeNav map: %r" % magic)
        self.data = data
        self.tile_deg = tile_deg / 1e7
        self.lat0 = lat0 / 1e7
        self.lon0 = lon0 / 1e7
        self.rows, self.cols = rows, cols
        self.index = []
        for i in range(count):
            off, length = struct.unpack_from("<II", data, size + i * 8)
            self.index.append((off, length))

    def ways_near(self, lat, lon, radius_tiles=2):
        """Every way in the tiles around a point, as (class, [(lat, lon)...])."""
        r0 = int((lat - self.lat0) / self.tile_deg)
        c0 = int((lon - self.lon0) / self.tile_deg)
        scale = self.tile_deg / 65536.0
        for r in range(r0 - radius_tiles, r0 + radius_tiles + 1):
            for c in range(c0 - radius_tiles, c0 + radius_tiles + 1):
                if not (0 <= r < self.rows and 0 <= c < self.cols):
                    continue
                off, length = self.index[r * self.cols + c]
                if not length:
                    continue
                tlat = self.lat0 + r * self.tile_deg
                tlon = self.lon0 + c * self.tile_deg
                pos, end = off, off + length
                while pos < end:
                    cls = self.data[pos]
                    if cls == 0:
                        break
                    n = self.data[pos + 1]
                    pos += 2
                    pts = []
                    for i in range(n):
                        x, y = struct.unpack_from("<HH", self.data, pos + i * 4)
                        pts.append((tlat + y * scale, tlon + x * scale))
                    pos += n * 4
                    yield cls, pts


def to_screen(lat, lon, origin, heading, px_per_m):
    """The board's transform: metres from the rider, turned so forward is up."""
    olat, olon = origin
    north = (lat - olat) * 111_320.0
    east = (lon - olon) * 111_320.0 * math.cos(math.radians(olat))
    ch = math.cos(math.radians(heading))
    sh = math.sin(math.radians(heading))
    x = east * ch - north * sh
    y = north * ch + east * sh
    return (SCREEN_W / 2 + x * px_per_m, SCREEN_H - 46 - y * px_per_m)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("map")
    ap.add_argument("--at", nargs=2, type=float, required=True, metavar=("LAT", "LON"))
    ap.add_argument("--heading", type=float, default=0.0)
    ap.add_argument("--out", default="preview.png")
    ap.add_argument("--scale", type=int, default=3, help="magnify, so it is visible on a monitor")
    args = ap.parse_args()

    from PIL import Image, ImageDraw

    m = Map(args.map)
    lat, lon = args.at
    px_per_m = SCREEN_W / METRES_ACROSS
    k = args.scale
    img = Image.new("RGB", (SCREEN_W * k, SCREEN_H * k), (10, 12, 16))
    draw = ImageDraw.Draw(img)

    drawn = 0
    for cls, pts in m.ways_near(lat, lon):
        screen = [to_screen(p[0], p[1], (lat, lon), args.heading, px_per_m) for p in pts]
        # Skip what is nowhere near the screen, as the board will.
        if all(x < -50 or x > SCREEN_W + 50 or y < -50 or y > SCREEN_H + 50 for x, y in screen):
            continue
        draw.line([(x * k, y * k) for x, y in screen],
                  fill=SHADES[cls], width=max(1, WIDTHS[cls] * k // 2), joint="curve")
        drawn += 1

    # The rider, where the board puts them.
    cx, cy = SCREEN_W / 2 * k, (SCREEN_H - 46) * k
    r = 5 * k
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(90, 176, 255), outline=(255, 255, 255), width=k)

    img.save(args.out)
    print("%s: %d ways drawn, %.0f m across, heading %.0f" %
          (args.out, drawn, METRES_ACROSS, args.heading))


if __name__ == "__main__":
    main()
