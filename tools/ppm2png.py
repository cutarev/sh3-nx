#!/usr/bin/env python3
"""ppm2png.py in.ppm out.png: view SH3_SHOT frame dumps without extra packages."""
import struct
import sys
import zlib

d = open(sys.argv[1], 'rb').read()
magic, w, h, mx, px = d.split(maxsplit=4)
w, h = int(w), int(h)
rows = b''.join(b'\0' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))


def chunk(t, data):
    return struct.pack('>I', len(data)) + t + data + struct.pack('>I', zlib.crc32(t + data))


open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                              chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
