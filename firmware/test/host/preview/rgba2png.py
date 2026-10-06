#!/usr/bin/env python3
"""icons.rgba (int width, int height, then 0xAARRGGBB words) -> PNG."""
import struct, sys, zlib

data = open(sys.argv[1], "rb").read()
w, h = struct.unpack_from("<ii", data)
raw = bytearray()
for y in range(h):
    raw.append(0)
    for x in range(w):
        v, = struct.unpack_from("<I", data, 8 + (y * w + x) * 4)
        raw += bytes(((v >> 16) & 255, (v >> 8) & 255, v & 255))
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[2], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                              chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
