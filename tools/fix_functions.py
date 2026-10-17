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
# A gap_prologue is only a guess at code nobody claimed; when the function before it jumps into it, it is that
# function's tail. 0x5BEBB0 (a callback in the table at 0x6D7A68) stopped at its first ret and its jcc's into
# 0x5BEBD8 fell to stubs: SH3 crashed breaking the wall with the silencer in the construction site.
def jumps_into(f, lo, hi):
    a = int(f['start'], 16)
    return any(ins.mnemonic.startswith('j') and ins.op_str.startswith('0x') and lo <= int(ins.op_str, 16) < hi
               for ins in md.disasm(d[a - B:int(f['end'], 16) - B], a))


for n in starts:  # in address order: a merged tail can be jumped into the next gap
    nf = by[n]
    if n in drop or nf['detection_method'] != 'gap_prologue':
        continue
    end = int(nf['end'], 16)
    # every function ending there: an alias overlaps its parent and both jump in (0x583500 in 0x583410)
    enders = [f for a, f in by.items() if a < n and a not in drop and int(f['end'], 16) == n and jumps_into(f, n, end)]
    for f in enders:
        f['end'], f['size'] = f'0x{end:08X}', end - int(f['start'], 16)
        print(f'0x{n:08X} (gap_prologue) is jumped into by {f["start"]}: merged, now ends at 0x{end:08X}')
    if enders:
        drop.add(n)
json.dump([f for f in fs if int(f['start'], 16) not in drop], open(sys.argv[2], 'w'), indent=1)
