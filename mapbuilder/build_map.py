#!/usr/bin/env python3
"""Build nautical chart images for the AIS display from NOAA ENC data.

Charts are rendered by NOAA's ENC-based Chart Display Service (official ENC
data drawn with S-52 nautical symbology) and saved as square PNGs, one per
zoom level, centred on the given position. The output directory gets:

    map_z0.png ... map_zN.png   one chart per zoom level (z0 = most zoomed in)
    manifest.json               centre point and per-level georeference
    map_set.zip                 the above, ready to upload on the display's
                                configuration page

Only the Python standard library is needed.

Example:
    python3 build_map.py --lat 47.6005 --lon -122.3550 -o seattle/
"""

import argparse
import json
import math
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
import zlib

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

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


MERC_RADIUS_M = 6378137.0


def mercator(lat, lon):
    """Lat/lon to Web Mercator (EPSG:3857) metres."""
    return (MERC_RADIUS_M * math.radians(lon),
            MERC_RADIUS_M * math.log(math.tan(math.pi / 4 + math.radians(lat) / 2)))


def bbox_for(lat, lon, radius_nm):
    """Web Mercator box extending radius_nm from the centre in each direction.

    Charts are requested in EPSG:3857 because the chart service keeps pixels
    square in whatever CRS it is asked for: an EPSG:4326 request comes back
    covering more latitude than asked (by 1/cos(lat)), which put vessels on
    land. Mercator metres are 1/cos(lat) longer than ground metres.
    """
    x, y = mercator(lat, lon)
    half = radius_nm * 1852.0 / math.cos(math.radians(lat))
    return x - half, y - half, x + half, y + half


def fetch_chart(bbox, size, layers, retries=4):
    x_min, y_min, x_max, y_max = bbox
    params = {
        "SERVICE": "WMS",
        "VERSION": "1.3.0",
        "REQUEST": "GetMap",
        "LAYERS": ",".join(str(layer) for layer in layers),
        "STYLES": "",
        "CRS": "EPSG:3857",
        "BBOX": f"{x_min:.2f},{y_min:.2f},{x_max:.2f},{y_max:.2f}",
        "WIDTH": str(size),
        "HEIGHT": str(size),
        "FORMAT": "image/png",
        "TRANSPARENT": "FALSE",
    }
    url = WMS_URL + "?" + urllib.parse.urlencode(params)
    delay = 2.0
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "ais-display-mapbuilder/1.0"})
            with urllib.request.urlopen(req, timeout=120) as resp:
                data = resp.read()
            if not data.startswith(PNG_SIGNATURE):
                raise RuntimeError("chart service did not return a PNG: " + data[:300].decode("utf-8", "replace"))
            return data
        except (urllib.error.URLError, TimeoutError, RuntimeError) as e:
            if attempt == retries - 1:
                raise
            print(f"  request failed ({e}); retrying in {delay:.0f}s", file=sys.stderr)
            time.sleep(delay)
            delay *= 2
    raise AssertionError("unreachable")


def png_chunk(ctype, data):
    body = ctype + data
    return len(data).to_bytes(4, "big") + body + zlib.crc32(body).to_bytes(4, "big")


def png_size(png):
    if not png.startswith(PNG_SIGNATURE) or png[12:16] != b"IHDR":
        raise ValueError("not a PNG")
    return int.from_bytes(png[16:20], "big"), int.from_bytes(png[20:24], "big")


def add_text_chunks(png, items):
    """Insert tEXt chunks straight after IHDR."""
    ihdr_end = 8 + 4 + 4 + 13 + 4
    chunks = b"".join(png_chunk(b"tEXt", k.encode("latin-1") + b"\0" + str(v).encode("latin-1"))
                      for k, v in items.items())
    return png[:ihdr_end] + chunks + png[ihdr_end:]


def parse_radii(text):
    radii = sorted({float(r) for r in text.split(",") if r.strip()})
    if not radii or any(r <= 0 or r > 60 for r in radii):
        raise argparse.ArgumentTypeError("radii must be between 0 and 60 nm")
    return radii


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lat", type=float, required=True, help="centre latitude, decimal degrees (north positive)")
    ap.add_argument("--lon", type=float, required=True, help="centre longitude, decimal degrees (east positive)")
    ap.add_argument("--radii-nm", type=parse_radii, default=parse_radii("0.5,1,2,4"),
                    help="comma-separated chart radius for each zoom level in nautical miles (default 0.5,1,2,4)")
    ap.add_argument("--size", type=int, default=800, help="image size in pixels (default 800, the display size)")
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
    layers = [int(x) for x in args.layers.split(",") if x.strip()]

    os.makedirs(args.out, exist_ok=True)
    manifest = {
        "version": 2,
        "center_lat": round(args.lat, 7),
        "center_lon": round(args.lon, 7),
        "projection": "EPSG:3857",
        "width": args.size,
        "height": args.size,
        "levels": [],
    }

    for i, radius in enumerate(args.radii_nm):
        bbox = bbox_for(args.lat, args.lon, radius)
        m_per_px = 2 * radius * 1852.0 / args.size
        print(f"zoom {i}: radius {radius} nm ({m_per_px:.2f} m/px) ...", flush=True)
        png = fetch_chart(bbox, args.size, layers)
        w, h = png_size(png)
        if (w, h) != (args.size, args.size):
            raise RuntimeError(f"chart service returned {w}x{h}, expected {args.size}x{args.size}")
        png = add_text_chunks(png, {
            "ais:center_lat": manifest["center_lat"],
            "ais:center_lon": manifest["center_lon"],
            "ais:m_per_px": f"{m_per_px:.4f}",
            "ais:zoom_index": i,
            "ais:projection": "EPSG:3857",
            "Source": "NOAA ENC via NOAA Chart Display Service",
        })
        name = f"map_z{i}.png"
        with open(os.path.join(args.out, name), "wb") as f:
            f.write(png)
        manifest["levels"].append({
            "file": name,
            "radius_nm": radius,
            "m_per_px": round(m_per_px, 4),
            "bbox": [round(v, 2) for v in bbox],  # EPSG:3857 metres: x_min, y_min, x_max, y_max
        })
        print(f"  wrote {name} ({len(png) // 1024} KiB)")

    manifest_json = json.dumps(manifest, indent=2)
    with open(os.path.join(args.out, "manifest.json"), "w") as f:
        f.write(manifest_json + "\n")

    # Entries are STORED (uncompressed) with sizes in the local headers so the
    # display can unpack the upload as it streams in, without an inflater.
    zip_path = os.path.join(args.out, "map_set.zip")
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_STORED) as z:
        z.writestr("manifest.json", manifest_json)
        for level in manifest["levels"]:
            with open(os.path.join(args.out, level["file"]), "rb") as f:
                z.writestr(level["file"], f.read())
    print(f"wrote {zip_path} ({os.path.getsize(zip_path) // 1024} KiB) - upload this on the display's web page")
    return 0


if __name__ == "__main__":
    sys.exit(main())
