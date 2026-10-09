#!/usr/bin/env python3
"""Disassemble sh3.exe at a VA: python tools/dis.py 0x41DB15 [end_or_count]"""
import sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

d = open(sys.argv[3] if len(sys.argv) > 3 else 'game/sh3.exe', 'rb').read()
va = int(sys.argv[1], 16)
n = int(sys.argv[2], 0) if len(sys.argv) > 2 else 40
end = n if n > 0x400000 else None
md = Cs(CS_ARCH_X86, CS_MODE_32)
off = va - 0x400000  # .text file offset == RVA in this exe
for i, ins in enumerate(md.disasm(d[off:off + 0x4000], va)):
    if (end and ins.address >= end) or (not end and i >= n):
        break
    print(f'{ins.address:08X}  {ins.bytes.hex():<16} {ins.mnemonic} {ins.op_str}')
