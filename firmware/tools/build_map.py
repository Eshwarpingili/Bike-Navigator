#!/usr/bin/env python3
"""Turn OpenStreetMap roads into a map the board can draw.

The board has no GPU and no network worth speaking of, so the map lives in
flash as geometry rather than as pictures: lines it can rotate by arithmetic
instead of pixels it would have to resample. This is what a bike computer
actually does.

Output format (all little endian). Coordinates are degrees scaled by 1e7 in the
header, and 16-bit offsets within a tile, which on a tile this size is well
under a metre - far finer than a 320x240 screen can show.

    magic   "BNMAP1"          6 bytes
    tile_deg                  u32   tile size, degrees * 1e7
    lat0, lon0                i32   south-west corner of the grid
    rows, cols                u16
    tile_count                u32
    index[tile_count]         u32 offset, u32 length   (0 length = empty tile)
    ...tile blobs, each a run of ways:
        class                 u8    1 = major .. 4 = minor, 0 ends the tile
        points                u8
        (x, y) * points       u16 each, offset within the tile

Usage:
    python tools/build_map.py --bbox 17.30 78.25 17.55 78.60 --out map.bin

Data is OpenStreetMap, ODbL. Fetched from Overpass in tiles, cached on disk so
a re-run costs nothing.
"""
import argparse
import json
import math
import ssl
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

# Several, because Overpass instances throttle and go down independently.
MIRRORS = [
    "https://overpass-api.de/api/interpreter",
    "https://overpass.kumi.systems/api/interpreter",
    "https://overpass.osm.ch/api/interpreter",
]


def ssl_context():
    """Python's trust store on this machine is out of date, so certificate
    verification fails against perfectly good servers. certifi ships a current
    bundle; use it rather than turning verification off, which would make this
    tool quietly accept anything that answered."""
    try:
        import certifi
        return ssl.create_default_context(cafile=certifi.where())
    except ImportError:
        return ssl.create_default_context()

# Road classes worth the flash they take. Anything smaller than this is noise at
# 320x240 and would double the file for streets a rider cannot read anyway.
CLASSES = {
    "motorway": 1, "trunk": 1, "motorway_link": 1, "trunk_link": 1,
    "primary": 2, "primary_link": 2,
    "secondary": 3, "secondary_link": 3,
    "tertiary": 4, "tertiary_link": 4, "unclassified": 4, "residential": 4,
}

TILE_DEG = 0.01           # about 1.1 km north-south
FETCH_DEG = 0.05          # Overpass request size; bigger times out
SIMPLIFY_M = 8.0          # below what the screen can resolve anyway
MAX_POINTS_PER_WAY = 255


def fetch(bbox, cache_dir, retries=3):
    """One Overpass request, cached on disk by bounding box."""
    south, west, north, east = bbox
    name = "%.3f_%.3f_%.3f_%.3f.json" % bbox
    path = cache_dir / name
    if path.exists():
        return json.loads(path.read_text(encoding="utf-8"))

    classes = "|".join(sorted(CLASSES))
    query = (
        "[out:json][timeout:120];"
        'way["highway"~"^(%s)$"](%f,%f,%f,%f);'
        "out geom;"
    ) % (classes, south, west, north, east)

    ctx = ssl_context()
    last = None
    for attempt in range(retries):
        for url in MIRRORS:
            try:
                req = urllib.request.Request(
                    url,
                    data=urllib.parse.urlencode({"data": query}).encode(),
                    headers={"User-Agent": "BikeNav map builder (OSM ODbL)"},
                )
                with urllib.request.urlopen(req, timeout=180, context=ctx) as resp:
                    raw = resp.read().decode("utf-8")
                data = json.loads(raw)
                path.write_text(raw, encoding="utf-8")
                return data
            except (urllib.error.URLError, TimeoutError, json.JSONDecodeError,
                    OSError) as exc:
                last = exc
                print("  %s: %s" % (urllib.parse.urlsplit(url).netloc, exc), file=sys.stderr)
        # Overpass throttles hard; backing off is expected, not an error.
        if attempt < retries - 1:
            wait = 20 * (attempt + 1)
            print("  all mirrors busy, retry in %ds" % wait, file=sys.stderr)
            time.sleep(wait)
    raise RuntimeError("no Overpass mirror answered: %s" % last)


def simplify(points, tolerance_m):
    """Douglas-Peucker. A road drawn on a 2.4 inch screen does not need every
    node the surveyor walked."""
    if len(points) < 3:
        return points
    # Degrees to metres, near enough at city scale.
    lat_scale = 111_320.0
    lon_scale = 111_320.0 * math.cos(math.radians(points[0][0]))

    def perpendicular(p, a, b):
        ax, ay = (a[1] - p[1]) * lon_scale, (a[0] - p[0]) * lat_scale
        bx, by = (b[1] - p[1]) * lon_scale, (b[0] - p[0]) * lat_scale
        dx, dy = bx - ax, by - ay
        den = math.hypot(dx, dy)
        if den < 1e-9:
            return math.hypot(ax, ay)
        return abs(dx * ay - dy * ax) / den

    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        lo, hi = stack.pop()
        worst, worst_i = 0.0, -1
        for i in range(lo + 1, hi):
            d = perpendicular(points[i], points[lo], points[hi])
            if d > worst:
                worst, worst_i = d, i
        if worst_i >= 0 and worst > tolerance_m:
            keep[worst_i] = True
            stack.append((lo, worst_i))
            stack.append((worst_i, hi))
    return [p for p, k in zip(points, keep) if k]


def clip_to_tiles(points, cls, grid, tiles):
    """Drop a way into every tile it crosses, splitting where it leaves one.

    Split rather than assign-to-one, so a road never vanishes when the rider is
    in the next tile along - which is exactly when they would be looking at it.
    """
    lat0, lon0, rows, cols = grid
    current, current_key = [], None
    for lat, lon in points:
        r = int((lat - lat0) / TILE_DEG)
        c = int((lon - lon0) / TILE_DEG)
        if not (0 <= r < rows and 0 <= c < cols):
            if len(current) > 1:
                tiles.setdefault(current_key, []).append((cls, current))
            current, current_key = [], None
            continue
        key = r * cols + c
        if current_key is None:
            current_key = key
        elif key != current_key:
            # Carry the crossing point into both tiles so the line joins up.
            current.append((lat, lon))
            if len(current) > 1:
                tiles.setdefault(current_key, []).append((cls, current))
            current = [current[-2]] if len(current) > 1 else []
            current_key = key
        current.append((lat, lon))
    if len(current) > 1 and current_key is not None:
        tiles.setdefault(current_key, []).append((cls, current))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bbox", nargs=4, type=float, required=True,
                    metavar=("SOUTH", "WEST", "NORTH", "EAST"))
    ap.add_argument("--out", default="map.bin")
    ap.add_argument("--cache", default="map_cache")
    args = ap.parse_args()

    south, west, north, east = args.bbox
    cache_dir = Path(args.cache)
    cache_dir.mkdir(exist_ok=True)

    lat0 = math.floor(south / TILE_DEG) * TILE_DEG
    lon0 = math.floor(west / TILE_DEG) * TILE_DEG
    rows = int(math.ceil((north - lat0) / TILE_DEG))
    cols = int(math.ceil((east - lon0) / TILE_DEG))
    grid = (lat0, lon0, rows, cols)
    print("grid %d x %d tiles of %.3f deg" % (rows, cols, TILE_DEG))

    tiles = {}
    ways_in = points_in = points_out = 0

    lat = south
    while lat < north:
        lon = west
        while lon < east:
            box = (round(lat, 3), round(lon, 3),
                   round(min(lat + FETCH_DEG, north), 3),
                   round(min(lon + FETCH_DEG, east), 3))
            print("fetch %.3f,%.3f .. %.3f,%.3f" % box)
            data = fetch(box, cache_dir)
            for el in data.get("elements", []):
                geom = el.get("geometry")
                if not geom:
                    continue
                cls = CLASSES.get(el.get("tags", {}).get("highway", ""))
                if cls is None:
                    continue
                pts = [(g["lat"], g["lon"]) for g in geom]
                ways_in += 1
                points_in += len(pts)
                pts = simplify(pts, SIMPLIFY_M)
                points_out += len(pts)
                clip_to_tiles(pts, cls, grid, tiles)
            lon += FETCH_DEG
        lat += FETCH_DEG

    print("ways %d, points %d -> %d after simplifying" % (ways_in, points_in, points_out))

    # Pack.
    scale = 65536.0 / TILE_DEG
    tile_count = rows * cols
    blobs = []
    for key in range(tile_count):
        ways = tiles.get(key, [])
        if not ways:
            blobs.append(b"")
            continue
        r, c = divmod(key, cols)
        tlat = lat0 + r * TILE_DEG
        tlon = lon0 + c * TILE_DEG
        out = bytearray()
        for cls, pts in ways:
            for start in range(0, len(pts), MAX_POINTS_PER_WAY - 1):
                chunk = pts[start:start + MAX_POINTS_PER_WAY]
                if len(chunk) < 2:
                    continue
                out.append(cls)
                out.append(len(chunk))
                for plat, plon in chunk:
                    x = int(round((plon - tlon) * scale))
                    y = int(round((plat - tlat) * scale))
                    out += struct.pack("<HH", max(0, min(65535, x)), max(0, min(65535, y)))
        out.append(0)  # end of tile
        blobs.append(bytes(out))

    header = struct.pack("<6sIiiHHI", b"BNMAP1", int(TILE_DEG * 1e7),
                         int(round(lat0 * 1e7)), int(round(lon0 * 1e7)),
                         rows, cols, tile_count)
    index_size = tile_count * 8
    offset = len(header) + index_size
    index = bytearray()
    for blob in blobs:
        if blob:
            index += struct.pack("<II", offset, len(blob))
            offset += len(blob)
        else:
            index += struct.pack("<II", 0, 0)

    out_path = Path(args.out)
    with out_path.open("wb") as fh:
        fh.write(header)
        fh.write(index)
        for blob in blobs:
            fh.write(blob)

    used = sum(1 for b in blobs if b)
    print("%s: %.2f MB, %d of %d tiles have roads"
          % (out_path, out_path.stat().st_size / 1e6, used, tile_count))


if __name__ == "__main__":
    main()
