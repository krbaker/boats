#!/usr/bin/env python3
"""Build nautical chart tiles for the AIS display from NOAA ENC data.

Charts are rendered by NOAA's ENC-based Chart Display Service (official ENC
data drawn with S-52 nautical symbology) in Web Mercator, at one scale per
zoom level, covering the whole area the display can pan over (by default
10 nm either side of the centre). Each level is rendered as one large image,
then cut into 256 px tiles and stored in one pack file. The output directory gets:

    ais_map/manifest.json       centre point and per-level georeference
    ais_map/z0.pak ... zN.pak   the tiles for each zoom level (z0 = most zoomed in)
    map_set.zip                 the ais_map files, ready to upload on the
                                display's configuration page

Instead of uploading, you can copy the ais_map folder to the root of the
display's microSD card (FAT32).

Needs Pillow (pip install -r requirements.txt).

Example:
    python3 build_map.py --lat 47.6005 --lon -122.3550 -o seattle/
"""

import argparse
import hashlib
import io
import json
import math
import os
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

try:
    from PIL import Image
except ImportError:
    sys.exit("The map builder needs Pillow: pip install -r requirements.txt")

WMS_URL = ("https://gis.charttools.noaa.gov/arcgis/rest/services/MCS/NOAAChartDisplay/"
           "MapServer/exts/MaritimeChartService/WMSServer")

# Layer ids published by the WMS service. The defaults leave out the
# chart-information, data-quality and overscale overlays, which add clutter
# (magenta "i" markers, hatching) without helping on a small display.
LAYER_TITLES = {
    0: "Information about the chart display",
    1: "Natural and man-made features, port features",
    2: "Depths, currents, etc",
    3: "Seabed, obstructions, pipelines",
    4: "Traffic routes",
    5: "Special areas",
    6: "Buoys, beacons, lights, fog signals, radar",
    7: "Services and small craft facilities",
    8: "Data quality",
    9: "Low accuracy",
    10: "Additional chart information",
    11: "Shallow water pattern",
    12: "Overscale warning",
}
DEFAULT_LAYERS = [1, 2, 3, 4, 5, 6, 7]

SCREEN_PX = 800         # the display is 800 px across; a level's radius spans half of that
TILE_PX = 256
MAX_RENDER_PX = 16384   # largest chart request; each zoom level is rendered in one piece if it fits
PALETTE_COLORS = 128
PACK_MAGIC = b"AISPAK1\0"
MERC_RADIUS_M = 6378137.0
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def mercator(lat, lon):
    """Lat/lon to Web Mercator (EPSG:3857) metres."""
    return (MERC_RADIUS_M * math.radians(lon),
            MERC_RADIUS_M * math.log(math.tan(math.pi / 4 + math.radians(lat) / 2)))


def fetch_chart(bbox, width, height, layers, retries=4):
    """Render bbox (EPSG:3857 metres: x_min, y_min, x_max, y_max) as a width x height image.

    Charts are requested in EPSG:3857 because the chart service keeps pixels
    square in whatever CRS it is asked for: an EPSG:4326 request comes back
    covering more latitude than asked (by 1/cos(lat)), which put vessels on land.
    """
    Image.MAX_IMAGE_PIXELS = None  # a whole zoom level is one image of up to 16k x 16k
    x_min, y_min, x_max, y_max = bbox
    params = {
        "SERVICE": "WMS",
        "VERSION": "1.3.0",
        "REQUEST": "GetMap",
        "LAYERS": ",".join(str(layer) for layer in layers),
        "STYLES": "",
        "CRS": "EPSG:3857",
        "BBOX": f"{x_min:.2f},{y_min:.2f},{x_max:.2f},{y_max:.2f}",
        "WIDTH": str(width),
        "HEIGHT": str(height),
        "FORMAT": "image/png",
        "TRANSPARENT": "FALSE",
    }
    url = WMS_URL + "?" + urllib.parse.urlencode(params)
    delay = 2.0
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "ais-display-mapbuilder/2.0"})
            with urllib.request.urlopen(req, timeout=180) as resp:
                data = resp.read()
            if not data.startswith(PNG_SIGNATURE):
                raise RuntimeError("chart service did not return a PNG: " + data[:300].decode("utf-8", "replace"))
            img = Image.open(io.BytesIO(data))
            img.load()
            img = img.convert("RGB")
            if img.size != (width, height):
                raise RuntimeError(f"chart service returned {img.size}, expected {width}x{height}")
            return img
        except (urllib.error.URLError, TimeoutError, RuntimeError, OSError) as e:
            if attempt == retries - 1:
                raise
            print(f"  request failed ({e}); retrying in {delay:.0f}s", file=sys.stderr)
            time.sleep(delay)
            delay *= 2
    raise AssertionError("unreachable")


def tile_png(img):
    """Encode one tile (already in the level's palette) as a PNG."""
    buf = io.BytesIO()
    img.save(buf, "PNG", optimize=True)
    return buf.getvalue()


def write_pack(path, cols, rows, tiles):
    """Pack format (little-endian):
        magic "AISPAK1\\0", u16 cols, u16 rows, u16 tile_px, u16 reserved,
        cols*rows x (u32 offset, u32 length), row-major from the top-left tile,
        then the PNG data. Identical tiles share one copy; length 0 = no tile.
    """
    header = PACK_MAGIC + struct.pack("<HHHH", cols, rows, TILE_PX, 0)
    data_start = len(header) + cols * rows * 8
    blobs, where, index = [], {}, []
    pos = data_start
    for r in range(rows):
        for c in range(cols):
            png = tiles.get((c, r))
            if png is None:
                index.append((0, 0))
                continue
            key = hashlib.sha1(png).digest()
            if key not in where:
                where[key] = pos
                blobs.append(png)
                pos += len(png)
            index.append((where[key], len(png)))
    with open(path, "wb") as f:
        f.write(header)
        for off, length in index:
            f.write(struct.pack("<II", off, length))
        for b in blobs:
            f.write(b)
    return len(blobs)


def build_level(i, radius, args, layers, cx, cy, out_dir):
    """Render one zoom level over the pan area and write z{i}.pak. Returns its manifest entry."""
    coslat = math.cos(math.radians(args.lat))
    m_per_px = 2 * radius * 1852.0 / SCREEN_PX          # ground metres per pixel at the centre
    merc_px = m_per_px / coslat                          # Web Mercator metres per pixel
    cover_nm = max(args.pan_nm, radius)                  # the view must always be fully covered
    span_px = 2 * cover_nm * 1852.0 / coslat / merc_px
    cols = rows = math.ceil(span_px / TILE_PX)
    origin_x = cx - cols * TILE_PX * merc_px / 2         # top-left corner of the tile grid
    origin_y = cy + rows * TILE_PX * merc_px / 2

    # Render the whole level as one big image and cut it into tiles, so every
    # tile comes from the same render (the chart service picks symbols, labels
    # and level of detail per request). A level too big for one request is
    # split into as few equal blocks as possible.
    per_block = max(1, args.max_render_px // TILE_PX)
    nblk = math.ceil(cols / per_block)
    size = math.ceil(cols / nblk)  # tiles per block side
    print(f"zoom {i}: radius {radius} nm ({m_per_px:.2f} m/px), {cols}x{rows} tiles, "
          f"{cols * TILE_PX} px across{f' in {nblk * nblk} blocks' if nblk > 1 else ''}", flush=True)
    if nblk > 1:
        print(f"  warning: too big for one {args.max_render_px} px render; detail may differ between blocks",
              file=sys.stderr)
    tiles, palette = {}, None
    # Middle block first, so the palette comes from the area people look at most.
    order = sorted(((br, bc) for br in range(nblk) for bc in range(nblk)),
                   key=lambda p: (p[0] - (nblk - 1) / 2) ** 2 + (p[1] - (nblk - 1) / 2) ** 2)
    for n, (br, bc) in enumerate(order, 1):
        c0, r0 = bc * size, br * size
        nc, nr = min(size, cols - c0), min(size, rows - r0)
        x0 = origin_x + c0 * TILE_PX * merc_px
        y1 = origin_y - r0 * TILE_PX * merc_px
        bbox = (x0, y1 - nr * TILE_PX * merc_px, x0 + nc * TILE_PX * merc_px, y1)
        t0 = time.time()
        img = fetch_chart(bbox, nc * TILE_PX, nr * TILE_PX, layers)
        if palette is None:
            # Chosen from a reduced copy: the full render can be 16k x 16k.
            sample = img.resize((min(img.width, 2048), min(img.height, 2048)), Image.Resampling.NEAREST)
            palette = sample.quantize(colors=PALETTE_COLORS, method=Image.Quantize.MEDIANCUT,
                                      dither=Image.Dither.NONE)
        img = img.quantize(palette=palette, dither=Image.Dither.NONE)
        for r in range(nr):
            for c in range(nc):
                box = (c * TILE_PX, r * TILE_PX, (c + 1) * TILE_PX, (r + 1) * TILE_PX)
                tiles[(c0 + c, r0 + r)] = tile_png(img.crop(box))
        del img
        print(f"  {'block ' + str(n) + '/' + str(nblk * nblk) + ' ' if nblk > 1 else ''}rendered and cut in "
              f"{time.time() - t0:.0f} s", flush=True)

    name = f"z{i}.pak"
    unique = write_pack(os.path.join(out_dir, name), cols, rows, tiles)
    size = os.path.getsize(os.path.join(out_dir, name))
    print(f"  wrote {name}: {unique} unique of {cols * rows} tiles, {size / 1e6:.1f} MB")
    return {
        "file": name,
        "radius_nm": radius,
        "m_per_px": round(m_per_px, 4),
        "merc_px": round(merc_px, 6),
        "origin": [round(origin_x, 2), round(origin_y, 2)],  # EPSG:3857 metres, top-left of the tile grid
        "cols": cols,
        "rows": rows,
    }


def parse_radii(text):
    radii = sorted({float(r) for r in text.split(",") if r.strip()})
    if not radii or any(r <= 0 or r > 60 for r in radii) or len(radii) > 8:
        raise argparse.ArgumentTypeError("give 1 to 8 radii between 0 and 60 nm")
    return radii


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lat", type=float, required=True, help="centre latitude, decimal degrees (north positive)")
    ap.add_argument("--lon", type=float, required=True, help="centre longitude, decimal degrees (east positive)")
    ap.add_argument("--radii-nm", type=parse_radii, default=parse_radii("0.5,1,2,4,10"),
                    help="comma-separated view radius for each zoom level in nautical miles (default 0.5,1,2,4,10)")
    ap.add_argument("--pan-nm", type=float, default=10.0,
                    help="how far from the centre the display can pan, in nautical miles (default 10)")
    ap.add_argument("--max-render-px", type=int, default=MAX_RENDER_PX,
                    help=f"largest image to request from the chart service (default {MAX_RENDER_PX}; the service "
                         "advertises 14400, so try that if big renders fail)")
    ap.add_argument("--layers", default=",".join(map(str, DEFAULT_LAYERS)),
                    help="comma-separated chart layer ids (see --list-layers)")
    ap.add_argument("--list-layers", action="store_true", help="print the available layer ids and exit")
    ap.add_argument("-o", "--out", default="map_out", help="output directory (default map_out)")
    args = ap.parse_args()

    if args.list_layers:
        for k, v in LAYER_TITLES.items():
            print(f"{k:3d}  {v}{'  (default)' if k in DEFAULT_LAYERS else ''}")
        return 0
    if not -80 <= args.lat <= 80 or not -180 <= args.lon <= 180:
        ap.error("lat must be within +/-80 and lon within +/-180")
    if args.max_render_px < TILE_PX:
        ap.error(f"--max-render-px must be at least {TILE_PX}")
    if not 0 < args.pan_nm <= 30:
        ap.error("--pan-nm must be between 0 and 30")
    layers = [int(x) for x in args.layers.split(",") if x.strip()]

    map_dir = os.path.join(args.out, "ais_map")
    os.makedirs(map_dir, exist_ok=True)
    cx, cy = mercator(args.lat, args.lon)
    manifest = {
        "version": 3,
        "created": int(time.time()),
        "center_lat": round(args.lat, 7),
        "center_lon": round(args.lon, 7),
        "projection": "EPSG:3857",
        "pan_nm": args.pan_nm,
        "tile_px": TILE_PX,
        "levels": [build_level(i, r, args, layers, cx, cy, map_dir) for i, r in enumerate(args.radii_nm)],
    }
    manifest_json = json.dumps(manifest, indent=2)
    with open(os.path.join(map_dir, "manifest.json"), "w") as f:
        f.write(manifest_json + "\n")

    # Entries are STORED (uncompressed) with sizes in the local headers so the
    # display can unpack the upload as it streams in, without an inflater.
    zip_path = os.path.join(args.out, "map_set.zip")
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_STORED) as z:
        z.write(os.path.join(map_dir, "manifest.json"), "manifest.json")
        for level in manifest["levels"]:
            z.write(os.path.join(map_dir, level["file"]), level["file"])
    print(f"wrote {zip_path} ({os.path.getsize(zip_path) / 1e6:.1f} MB) - upload this on the display's web page,")
    print(f"or copy {map_dir} to the root of the display's microSD card")
    return 0


if __name__ == "__main__":
    sys.exit(main())
