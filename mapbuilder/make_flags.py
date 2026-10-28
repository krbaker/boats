#!/usr/bin/env python3
"""Download the country flags the AIS display shows beside ships.

The display works out a ship's country from its MMSI and shows the flag in the
vessel list and on the vessel's detail screen, if the flag image is on the
microSD card. This writes them, one 32x24 PNG per country, to

    <out>/flags/lr.png, us.png, ...
    <out>/flags.zip             the same files, ready to upload on the
                                display's web page (Map section)

Upload flags.zip, or copy the flags folder to the root of the display's
microSD card (next to ais_map). Without it the display shows the 2-letter
country code instead.

The flags come from flagcdn.com (public domain, from Wikimedia Commons). The
country list is read from the firmware (components/ais/ais.c), so it stays in
step with what the display looks for. Needs Pillow (pip install -r requirements.txt).

Example:
    python3 make_flags.py -o seattle/
"""

import argparse
import io
import os
import re
import sys
import urllib.request
import zipfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
AIS_C = os.path.join(HERE, "..", "firmware", "components", "ais", "ais.c")
URL = "https://flagcdn.com/32x24/{}.png"
BG = (0x0D, 0x1B, 0x2A)  # UI_COLOR_BG: the display can't blend, so transparent parts get its background


def iso_codes(path):
    with open(path) as f:
        src = f.read()
    return sorted(set(re.findall(r'\{\d+, \d+, "([a-z]{2})", "[^"]+"\}', src)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--out", required=True, help="output directory (gets a flags/ folder)")
    args = ap.parse_args()

    codes = iso_codes(AIS_C)
    if not codes:
        sys.exit(f"no country codes found in {AIS_C}")
    out = os.path.join(args.out, "flags")
    os.makedirs(out, exist_ok=True)
    failed = []
    for i, iso in enumerate(codes, 1):
        print(f"\r{i}/{len(codes)} {iso}", end="", flush=True)
        try:
            req = urllib.request.Request(URL.format(iso), headers={"User-Agent": "ais-display-flags"})
            with urllib.request.urlopen(req, timeout=30) as r:
                img = Image.open(io.BytesIO(r.read())).convert("RGBA")
        except Exception as e:  # noqa: BLE001 - report and carry on with the rest
            failed.append(f"{iso} ({e})")
            continue
        flat = Image.new("RGB", img.size, BG)
        flat.paste(img, mask=img.split()[3])
        flat.save(os.path.join(out, iso + ".png"), optimize=True)
    # Uncompressed: the display unpacks the upload as it arrives (PNGs don't shrink anyway).
    zip_path = os.path.join(args.out, "flags.zip")
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_STORED) as z:
        for name in sorted(os.listdir(out)):
            if name.endswith(".png"):
                z.write(os.path.join(out, name), name)
    print(f"\r{len(codes) - len(failed)} flags written to {out} and {zip_path}")
    if failed:
        print("failed: " + ", ".join(failed))


if __name__ == "__main__":
    main()
