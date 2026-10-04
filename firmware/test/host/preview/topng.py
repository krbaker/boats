#!/usr/bin/env python3
"""Convert the preview's raw RGB565 800x800 frames to PNG.

    topng.py frame.raw frame.png       (outside the round screen blacked out)
"""
import struct
import sys
import zlib

SIZE = 800


def to_png(src, dst):
    px = open(src, "rb").read()
    rr = (SIZE / 2) ** 2
    raw = bytearray()
    for y in range(SIZE):
        raw.append(0)
        for x in range(SIZE):
            v, = struct.unpack_from("<H", px, (y * SIZE + x) * 2)
            if (x - SIZE / 2 + .5) ** 2 + (y - SIZE / 2 + .5) ** 2 > rr:
                raw += b"\0\0\0"
            else:
                raw += bytes(((v >> 8) & 0xF8, (v >> 3) & 0xFC, (v << 3) & 0xF8))

    def chunk(t, c):
        return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c))
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))
    open(dst, "wb").write(png)


if __name__ == "__main__":
    to_png(sys.argv[1], sys.argv[2])
