#!/usr/bin/env python3
"""Byte patches applied to sh3.exe before recompilation: the parts of ThirteenAG's SilentHill3.WidescreenFix that
change code in the middle of a function (the rest are hand-written functions in src/game/sh3.c). Every site is
checked against the original bytes.

Usage: patch_exe.py game/sh3.exe build/sh3_ws.exe
"""
import struct, sys

BASE = 0x400000
ZERO = 0x68A868        # a 0.0f constant in .rdata
V_BORDER = 0x0EC00000  # sh3.c game_init: the cutscene borders' half height, 0 on a wide screen
CAVE_HEALTH = 0x401ED8  # nop padding after a ret; sh3.c defines sub_00401ED8


def p32(v):
    return struct.pack('<I', v & 0xFFFFFFFF)


def call(at, target):
    return b'\xE8' + struct.pack('<i', target - (at + 5))


# (va, original bytes, replacement, what)
PATCHES = [
    (0x41BB2D, p32(0x68E6D4), p32(ZERO), 'black bars at the sides of the screen'),
    (0x41BE66, p32(0x68E6D4), p32(ZERO), 'black bars at the sides of the screen'),
    (0x41BB4D, p32(0x68A8F0), p32(V_BORDER), 'cutscene borders'),
    (0x5DAFE6, p32(0x100), p32(0x400), 'inventory background width'),
    (0x5DAFEB, p32(-0x100), p32(-0x400), 'inventory background width'),
    (CAVE_HEALTH, b'\x90', b'\xC3', 'cave: health indicator position'),
    (0x5F1D15, bytes.fromhex('d959e8d9c0'), call(0x5F1D15, CAVE_HEALTH), 'health indicator position'),
    (0x5E82FD, b'\x6a\x00', b'\x6a\x01', 'pause menu box: alpha blending on'),
    (0x5E8390, p32(0xFF000000), p32(0x00000000), 'pause menu box: fully transparent'),
]


def main():
    d = bytearray(open(sys.argv[1], 'rb').read())
    for va, old, new, what in PATCHES:
        o = va - BASE  # .text file offsets equal RVAs in this exe
        if d[o:o + len(old)] != old:
            sys.exit(f'{va:#x} ({what}): expected {old.hex()}, found {d[o:o + len(old)].hex()}')
        d[o:o + len(new)] = new
    open(sys.argv[2], 'wb').write(d)
    print(f'{len(PATCHES)} patches -> {sys.argv[2]}')


if __name__ == '__main__':
    main()
