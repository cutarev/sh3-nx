#!/usr/bin/env python3
"""Merges back the functions the disassembler started inside an instruction of the function before them.

A sweep that runs out of phase reads a displacement byte as a call (0x5BCB47: E8 00 00 00 00 inside
`mov [esp+0xE8], 0`) or a data value as a function pointer (0x545350, inside `test ah, 5`); the bogus start then
cuts the real function short, which is lifted up to the cut and falls into garbage (SH3 crashed when Heather took
damage). Decoding each function from its own start, a function that begins inside one of its instructions
(before a ret, or a jmp past it) is dropped and the parent extended over it. Parents that are themselves decoded data
(gap_prologue, tail_jump_alias) are left alone.

Usage: fix_functions.py build/sh3_ws.exe build/disasm/functions.json
"""
import bisect, json, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

B = 0x400000  # .text file offsets equal RVAs in this exe
d = open(sys.argv[1], 'rb').read()
fs = json.load(open(sys.argv[2]))
by = {int(f['start'], 16): f for f in fs}
starts = sorted(by)
md = Cs(CS_ARCH_X86, CS_MODE_32)
drop = set()
for i, a in enumerate(starts):
    f = by[a]
    if a in drop or f['detection_method'] in ('gap_prologue', 'tail_jump_alias'):
        continue
    inner = starts[i + 1:bisect.bisect_left(starts, int(f['end'], 16))]
    if not inner:
        continue
    bounds, pc = set(), a
    for ins in md.disasm(d[a - B:inner[-1] + 16 - B], a):
        if ins.address > inner[-1]:
            break
        bounds.add(ins.address)
        pc = ins.address + ins.size
        if ins.mnemonic in ('ret', 'int3') or (ins.mnemonic == 'jmp' and ins.address > inner[0]):
            break
    for s in inner:
        if s < pc and s not in bounds:
            drop.add(s)
            end = max(int(f['end'], 16), int(by[s]['end'], 16))
            f['end'], f['size'] = f'0x{end:08X}', end - a
            print(f'0x{s:08X} starts inside an instruction of 0x{a:08X}: merged, now ends at 0x{end:08X}')
json.dump([f for f in fs if int(f['start'], 16) not in drop], open(sys.argv[2], 'w'), indent=1)
