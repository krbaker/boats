#!/usr/bin/env python3
"""AIS NMEA 0183 test server for the AIS display.

Serves a stream of simulated AIS traffic over TCP (like a real AIS network
feed). Vessels move around a centre point; Class A ships send message types
1 and 5, Class B boats send types 18 and 24.

    python3 ais_sim.py --lat 47.6005 --lon -122.3550            # port 10110
    python3 ais_sim.py --timestamps tag --noise                 # stress the parser
    python3 ais_sim.py --replay capture.nmea                    # replay a capture
    python3 ais_sim.py --dump 20                                # print sentences and exit

Then point the display's "AIS data server" at this machine's IP and port.
Only the Python standard library is needed.
"""

import argparse
import datetime
import math
import random
import socket
import sys
import threading
import time

# ---------------------------------------------------------------------------
# AIVDM encoding
# ---------------------------------------------------------------------------


class Bits:
    def __init__(self):
        self.bits = []

    def u(self, value, width):
        value = int(value) & ((1 << width) - 1)
        self.bits.extend((value >> (width - 1 - i)) & 1 for i in range(width))
        return self

    def s(self, value, width):
        return self.u(int(value) & ((1 << width) - 1), width)

    def text(self, value, chars):
        value = value.upper()[:chars].ljust(chars, "@")
        for ch in value:
            c = ord(ch)
            self.u(c - 64 if c >= 64 else c, 6)
        return self

    def armour(self):
        fill = (6 - len(self.bits) % 6) % 6
        bits = self.bits + [0] * fill
        out = []
        for i in range(0, len(bits), 6):
            v = 0
            for b in bits[i:i + 6]:
                v = (v << 1) | b
            out.append(chr(v + 48 if v < 40 else v + 56))
        return "".join(out), fill


def checksum(body):
    c = 0
    for ch in body:
        c ^= ord(ch)
    return f"{c:02X}"


_seq = 0


def sentences(payload, fill, channel="A"):
    """Split an armoured payload into !AIVDM sentences."""
    global _seq
    parts = [payload[i:i + 60] for i in range(0, len(payload), 60)] or [""]
    seq = ""
    if len(parts) > 1:
        _seq = (_seq + 1) % 10
        seq = str(_seq)
    out = []
    for i, part in enumerate(parts):
        f = fill if i == len(parts) - 1 else 0
        body = f"AIVDM,{len(parts)},{i + 1},{seq},{channel},{part},{f}"
        out.append(f"!{body}*{checksum(body)}")
    return out


def msg_position_a(v):
    b = Bits().u(1, 6).u(0, 2).u(v.mmsi, 30).u(v.status, 4).s(-128, 8)
    b.u(round(v.sog * 10), 10).u(1, 1)
    b.s(round(v.lon * 600000), 28).s(round(v.lat * 600000), 27)
    b.u(round(v.cog * 10) % 3600, 12).u(round(v.heading) % 360 if v.heading is not None else 511, 9)
    b.u(int(time.time()) % 60, 6).u(0, 2).u(0, 3).u(0, 1).u(0, 19)
    return b.armour()


def msg_static_a(v):
    b = Bits().u(5, 6).u(0, 2).u(v.mmsi, 30).u(0, 2).u(v.imo, 30)
    b.text(v.callsign, 7).text(v.name, 20).u(v.ship_type, 8)
    b.u(v.dim[0], 9).u(v.dim[1], 9).u(v.dim[2], 6).u(v.dim[3], 6)
    b.u(1, 4).u(10, 4).u(3, 5).u(12, 5).u(0, 6).u(60, 8)
    b.text(v.destination, 20).u(0, 1).u(0, 1)
    return b.armour()


def msg_position_b(v):
    b = Bits().u(18, 6).u(0, 2).u(v.mmsi, 30).u(0, 8)
    b.u(round(v.sog * 10), 10).u(0, 1)
    b.s(round(v.lon * 600000), 28).s(round(v.lat * 600000), 27)
    b.u(round(v.cog * 10) % 3600, 12).u(round(v.heading) % 360 if v.heading is not None else 511, 9)
    b.u(int(time.time()) % 60, 6).u(0, 2).u(1, 1).u(0, 1).u(1, 1).u(1, 1).u(0, 1).u(0, 1).u(0, 1).u(0, 20)
    return b.armour()


def msg_static_b(v, part):
    b = Bits().u(24, 6).u(0, 2).u(v.mmsi, 30).u(part, 2)
    if part == 0:
        b.text(v.name, 20).u(0, 8)
    else:
        b.u(v.ship_type, 8).text("SIMVEND", 7).text(v.callsign, 7)
        b.u(v.dim[0], 9).u(v.dim[1], 9).u(v.dim[2], 6).u(v.dim[3], 6).u(0, 6)
    return b.armour()


def msg_base_station():
    # Type 4: valid AIS the display should ignore.
    b = Bits().u(4, 6).u(0, 2).u(3669702, 30).u(2026, 14).u(10, 4).u(3, 5).u(12, 5).u(0, 6).u(0, 6)
    b.u(1, 1).s(0, 28).s(0, 27).u(7, 4).u(0, 10).u(0, 1).u(0, 19)
    return b.armour()


# ---------------------------------------------------------------------------
# Simulated vessels
# ---------------------------------------------------------------------------

NAMES = ["OLYMPIC SPIRIT", "SALISH TRADER", "PUGET STAR", "MV KITSAP", "SEA OTTER", "ORCA DANCER",
         "EVERGREEN DAWN", "TUG RAINIER", "BLUE HERON", "WINDWARD", "LADY ELLIOTT", "NORTHERN LIGHT",
         "HARBOR PILOT", "CASCADE", "ALKI POINT", "STORMY PETREL", "PACIFIC ROSE", "DUWAMISH"]

PROFILES = [  # (class, ship type, length range m, speed range kn)
    ("A", 70, (150, 300), (8, 16)),    # cargo
    ("A", 80, (180, 250), (8, 14)),    # tanker
    ("A", 60, (90, 140), (12, 18)),    # passenger ferry
    ("A", 52, (25, 35), (5, 10)),      # tug
    ("A", 30, (20, 40), (2, 8)),       # fishing
    ("B", 37, (8, 16), (3, 15)),       # pleasure
    ("B", 36, (9, 15), (3, 7)),        # sailing
]


class Vessel:
    def __init__(self, i, lat0, lon0, radius_nm, rng):
        cls, self.ship_type, lrange, srange = rng.choice(PROFILES)
        self.cls = cls
        self.mmsi = 366000000 + rng.randrange(100000, 999999)
        self.imo = 9000000 + rng.randrange(100000, 999999) if cls == "A" else 0
        self.name = NAMES[i % len(NAMES)] + ("" if i < len(NAMES) else f" {i // len(NAMES)}")
        self.callsign = f"W{rng.randrange(1000, 9999)}"
        self.destination = rng.choice(["SEATTLE", "TACOMA", "VANCOUVER BC", "BREMERTON", "EVERETT", ""])
        length = rng.uniform(*lrange)
        beam = length * rng.uniform(0.14, 0.2)
        a = length * rng.uniform(0.55, 0.85)
        c = beam / 2
        self.dim = (round(a), round(length - a), round(c), round(beam - c))
        self.status = 0
        self.lat0, self.lon0, self.radius_nm = lat0, lon0, radius_nm
        ang, r = rng.uniform(0, 2 * math.pi), radius_nm * math.sqrt(rng.uniform(0, 1))
        self.lat = lat0 + r / 60 * math.cos(ang)
        self.lon = lon0 + r / 60 * math.sin(ang) / math.cos(math.radians(lat0))
        self.cog = rng.uniform(0, 360)
        self.sog = 0.0 if rng.random() < 0.15 else rng.uniform(*srange)
        if self.sog == 0:
            self.status = 1 if cls == "A" else 0
        self.heading = None if (cls == "B" and rng.random() < 0.5) else self.cog
        self.next_pos = 0.0
        self.next_static = rng.uniform(0, 5)
        self.rng = rng

    def step(self, dt):
        if self.sog > 0:
            if self.rng.random() < 0.02:
                self.cog = (self.cog + self.rng.uniform(-30, 30)) % 360
            # Turn back towards the centre when straying outside the area.
            dn = (self.lat - self.lat0) * 60
            de = (self.lon - self.lon0) * 60 * math.cos(math.radians(self.lat0))
            if math.hypot(dn, de) > self.radius_nm:
                home = math.degrees(math.atan2(-de, -dn)) % 360
                diff = (home - self.cog + 540) % 360 - 180
                self.cog = (self.cog + max(-10, min(10, diff))) % 360
            dist_nm = self.sog * dt / 3600
            self.lat += dist_nm / 60 * math.cos(math.radians(self.cog))
            self.lon += dist_nm / 60 * math.sin(math.radians(self.cog)) / math.cos(math.radians(self.lat))
            if self.heading is not None:
                self.heading = (self.cog + self.rng.uniform(-3, 3)) % 360

    def due_messages(self, now):
        msgs = []
        if now >= self.next_pos:
            msgs += sentences(*(msg_position_a(self) if self.cls == "A" else msg_position_b(self)),
                              channel=self.rng.choice("AB"))
            moving = self.sog > 0.5
            self.next_pos = now + (self.rng.uniform(2, 6) if moving else self.rng.uniform(20, 40))
        if now >= self.next_static:
            if self.cls == "A":
                msgs += sentences(*msg_static_a(self))
            else:
                msgs += sentences(*msg_static_b(self, 0)) + sentences(*msg_static_b(self, 1))
            self.next_static = now + self.rng.uniform(30, 60)
        return msgs


# ---------------------------------------------------------------------------
# Decoration (timestamps / noise) and serving
# ---------------------------------------------------------------------------

def decorate(line, mode):
    now = time.time()
    if mode == "tag":
        body = f"s:sim,c:{int(now)}"
        return f"\\{body}*{checksum(body)}\\{line}"
    if mode == "iso":
        return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="milliseconds") + " " + line
    if mode == "epoch":
        return f"{now:.3f} {line}"
    if mode == "suffix":
        return f"{line},rsim,{int(now)}"
    return line


def noise_lines(rng):
    lines = ["$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47",
             "!AIVDM,1,1,,A,15RTgt0PAso;90TKcjM8h6g208CQ,0*4B",  # bad checksum
             "garbage line without a sentence"]
    lines += sentences(*msg_base_station())
    return [rng.choice(lines)]


class Simulator:
    def __init__(self, args):
        self.args = args
        self.rng = random.Random(args.seed)
        self.vessels = [Vessel(i, args.lat, args.lon, args.radius_nm, self.rng) for i in range(args.vessels)]
        self.lock = threading.Lock()
        self.clients = []
        self.last = time.time()

    def tick(self):
        now = time.time()
        dt, self.last = now - self.last, now
        out = []
        for v in self.vessels:
            v.step(dt)
            out += v.due_messages(now)
        if self.args.noise and self.rng.random() < 0.3:
            out += noise_lines(self.rng)
        return [decorate(line, self.args.timestamps) for line in out]

    def broadcast(self, lines):
        if not lines:
            return
        data = ("\r\n".join(lines) + "\r\n").encode()
        with self.lock:
            for c in list(self.clients):
                try:
                    c.sendall(data)
                except OSError:
                    self.clients.remove(c)
                    print("client disconnected", file=sys.stderr)

    def accept_loop(self, srv):
        while True:
            conn, addr = srv.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            with self.lock:
                self.clients.append(conn)
            print(f"client connected from {addr[0]}:{addr[1]}", file=sys.stderr)


def replay_lines(path, rate):
    while True:
        with open(path, errors="replace") as f:
            for line in f:
                line = line.strip()
                if line:
                    yield line
                    time.sleep(1.0 / rate)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lat", type=float, default=47.6005)
    ap.add_argument("--lon", type=float, default=-122.3550)
    ap.add_argument("--radius-nm", type=float, default=3.0, help="area the vessels move in")
    ap.add_argument("--vessels", type=int, default=15)
    ap.add_argument("--port", type=int, default=10110)
    ap.add_argument("--timestamps", choices=["none", "tag", "iso", "epoch", "suffix"], default="none",
                    help="decorate sentences with timestamps the way some servers do")
    ap.add_argument("--noise", action="store_true", help="mix in non-AIS, unsupported and corrupt lines")
    ap.add_argument("--replay", metavar="FILE", help="replay lines from a capture file instead of simulating")
    ap.add_argument("--rate", type=float, default=20, help="lines per second for --replay")
    ap.add_argument("--dump", type=int, metavar="N", help="print N simulated sentences to stdout and exit")
    ap.add_argument("--seed", type=int, default=None)
    args = ap.parse_args()

    sim = Simulator(args)
    if args.dump:
        n = 0
        while n < args.dump:
            for line in sim.tick():
                print(line)
                n += 1
            for v in sim.vessels:  # make every vessel due again
                v.next_pos = 0
        return 0

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(4)
    threading.Thread(target=sim.accept_loop, args=(srv,), daemon=True).start()
    print(f"serving AIS on TCP port {args.port} "
          f"({'replaying ' + args.replay if args.replay else f'{args.vessels} simulated vessels'})", file=sys.stderr)

    try:
        if args.replay:
            for line in replay_lines(args.replay, args.rate):
                sim.broadcast([decorate(line, args.timestamps)])
        else:
            while True:
                sim.broadcast(sim.tick())
                time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
