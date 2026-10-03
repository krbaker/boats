#!/usr/bin/env python3
"""Encode simulated vessels with tools/ais_sim.py, decode them with the C
parser (ais_dump) and check the fields survive the round trip, including
with timestamp decorations and noise mixed in."""

import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "tools"))
import ais_sim  # noqa: E402

failures = 0


def check(cond, msg):
    global failures
    if not cond:
        failures += 1
        print("FAIL", msg)


rng = random.Random(42)
vessels = [ais_sim.Vessel(i, 47.6, -122.35, 3.0, rng) for i in range(40)]
by_mmsi = {v.mmsi: v for v in vessels}

for mode in ["none", "tag", "iso", "epoch", "suffix"]:
    lines = []
    for v in vessels:
        v.next_pos = v.next_static = 0
        lines += v.due_messages(1e12)
        lines += ais_sim.noise_lines(rng)
    lines = [ais_sim.decorate(line, mode) for line in lines]
    out = subprocess.run([os.path.join(HERE, "build", "ais_dump")], input="\n".join(lines) + "\n",
                         capture_output=True, text=True, check=True)
    seen = set()
    for row in out.stdout.splitlines():
        f = row.split("\t")
        mmsi, mtype = int(f[0]), int(f[1])
        v = by_mmsi.get(mmsi)
        check(v is not None, f"[{mode}] unknown mmsi {mmsi}")
        if not v:
            continue
        seen.add((mmsi, mtype))
        check(f[2] == v.cls, f"[{mode}] {mmsi} class {f[2]} != {v.cls}")
        if mtype in (1, 18):
            check(abs(float(f[4]) - v.lat) < 1e-5 and abs(float(f[5]) - v.lon) < 1e-5, f"[{mode}] {mmsi} position")
            check(abs(float(f[6]) - round(v.sog, 1)) < 0.051, f"[{mode}] {mmsi} sog {f[6]} vs {v.sog}")
            check(abs(float(f[7]) - round(v.cog, 1) % 360) < 0.051, f"[{mode}] {mmsi} cog {f[7]} vs {v.cog}")
            if v.heading is not None:
                check(int(f[8]) == round(v.heading) % 360, f"[{mode}] {mmsi} heading")
        if mtype == 5 or (mtype == 24 and f[14]):
            check(f[14] == v.name.upper()[:20], f"[{mode}] {mmsi} name {f[14]!r} vs {v.name!r}")
        if mtype == 5 or (mtype == 24 and f[15]):
            check(int(f[9]) == v.ship_type, f"[{mode}] {mmsi} ship type")
            check(tuple(int(x) for x in f[10:14]) == v.dim, f"[{mode}] {mmsi} dims {f[10:14]} vs {v.dim}")
            check(f[15] == v.callsign, f"[{mode}] {mmsi} callsign")
        if mtype == 5:
            check(f[16] == v.destination, f"[{mode}] {mmsi} destination {f[16]!r} vs {v.destination!r}")
    for v in vessels:
        types = (1, 5) if v.cls == "A" else (18, 24)
        for t in types:
            check((v.mmsi, t) in seen, f"[{mode}] {v.mmsi} message type {t} not decoded")

if failures:
    print(f"{failures} round-trip failure(s)")
    sys.exit(1)
print("AIS round-trip test passed")
