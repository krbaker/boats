#!/usr/bin/env python3
"""Write zip files for test_map_store into build/zips and print '<expect> <path>' pairs."""

import json
import os
import struct
import zipfile
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "build", "zips")
os.makedirs(OUT, exist_ok=True)


def png(w, h):
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    chunk = b"IHDR" + ihdr
    return (b"\x89PNG\r\n\x1a\n" + struct.pack(">I", 13) + chunk + struct.pack(">I", zlib.crc32(chunk))
            + b"\0" * 500)


def manifest(levels=(2.0, 0.5, 1.0), **over):
    m = {"version": 2, "center_lat": 47.6, "center_lon": -122.35, "projection": "EPSG:3857", "width": 800,
         "height": 800, "levels": [{"file": f"map_z{i}.png", "radius_nm": r, "m_per_px": 2 * r * 1852 / 800,
                                    "bbox": [-13630000.0, 6030000.0, -13610000.0, 6050000.0]} for i, r in enumerate(levels)]}
    m.update(over)
    return json.dumps(m)


def make(name, files, compression=zipfile.ZIP_STORED, expect="ok"):
    path = os.path.join(OUT, name)
    with zipfile.ZipFile(path, "w", compression=compression) as z:
        for fname, data in files.items():
            z.writestr(fname, data)
    print(expect, path)


good = {"manifest.json": manifest(), "map_z0.png": png(800, 800), "map_z1.png": png(800, 800),
        "map_z2.png": png(800, 800)}
make("good.zip", good)
make("folder.zip", {"maps/" + k: v for k, v in good.items()})
make("deflated.zip", good, zipfile.ZIP_DEFLATED, "fail")
make("missing_png.zip", {k: v for k, v in good.items() if k != "map_z2.png"}, expect="fail")
make("wrong_size.zip", {**good, "map_z1.png": png(640, 640)}, expect="fail")
make("bad_manifest.zip", {**good, "manifest.json": "{not json"}, expect="fail")
make("no_manifest.zip", {k: v for k, v in good.items() if k != "manifest.json"}, expect="fail")
make("old_projection.zip", {**good, "manifest.json": manifest(projection="EPSG:4326")}, expect="fail")
make("bad_center.zip", {**good, "manifest.json": manifest(center_lat=123.0)}, expect="fail")
make("traversal.zip", {**good, "../../x.png": png(800, 800)})  # reduced to its base name, stays inside
make("bad_name.zip", {**good, "bad name!.png": png(800, 800)}, expect="fail")
with open(os.path.join(OUT, "truncated.zip"), "wb") as f:
    f.write(open(os.path.join(OUT, "good.zip"), "rb").read()[:700])
print("fail", os.path.join(OUT, "truncated.zip"))
with open(os.path.join(OUT, "not_zip.zip"), "wb") as f:
    f.write(png(800, 800))
print("fail", os.path.join(OUT, "not_zip.zip"))
