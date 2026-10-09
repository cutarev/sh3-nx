#!/usr/bin/env python3
"""Runs one function of sh3.exe on its original x86 code (Unicorn) from the state the recompiled
build captured at its entry, and reports where the recompiled exit state differs. Bridged calls
(imports) are not executed: they return what they returned in the recompiled run.

    tools/regen.sh with --trace-functions listing the function, rebuild, then
    SH3_ORACLE=4202F1[,n] ./build/linux/sh3 game      (n: skip the first n calls)
    python tools/oracle.py game <function hex> [address to watch]
"""
import struct
import sys
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_MEM_FETCH_UNMAPPED, UC_HOOK_MEM_WRITE, UC_PROT_ALL
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP,
                               UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EIP, UC_X86_REG_GDTR,
                               UC_X86_REG_FS, UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS)

REGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP,
        UC_X86_REG_ESI, UC_X86_REG_EDI]
NAMES = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']


def load(path):
    d = open(path, 'rb').read()
    regs = struct.unpack_from('<10I', d)
    o, ranges = 40 + 64, []
    while o < len(d):
        lo, n = struct.unpack_from('<II', d, o)
        ranges.append((lo, d[o + 8:o + 8 + n]))
        o += 8 + n
    return regs, ranges


def gdt_entry(base, limit, access, flags):
    return struct.pack('<Q', (limit & 0xFFFF) | (base & 0xFFFFFF) << 16 | access << 40 |
                       ((limit >> 16) & 0xF) << 48 | flags << 52 | ((base >> 24) & 0xFF) << 56)


def main(game_dir):
    regs, ranges = load(f'{game_dir}/oracle_in.snap')
    out_regs, out_ranges = load(f'{game_dir}/oracle_out.snap')
    calls = [tuple(int(x, 16) for x in line.split()) for line in open(f'{game_dir}/oracle_calls.txt')]
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    for lo, data in out_ranges:  # the exit state's ranges cover what the heap grew into
        base, end = lo & ~0xFFF, (lo + len(data) + 0xFFF) & ~0xFFF
        uc.mem_map(base, end - base, UC_PROT_ALL)
    for lo, data in ranges:
        uc.mem_write(lo, data)
    # Flat code/data segments and an fs segment at the guest TIB.
    gdt = 0x00300000
    uc.mem_map(gdt, 0x1000, UC_PROT_ALL)
    uc.mem_write(gdt, b'\0' * 8 + gdt_entry(0, 0xFFFFF, 0x9A, 0xC) + gdt_entry(0, 0xFFFFF, 0x92, 0xC) +
                 gdt_entry(regs[8], 0xFFFFF, 0x92, 0xC))
    uc.reg_write(UC_X86_REG_GDTR, (0, gdt, 0x1F, 0))
    uc.reg_write(UC_X86_REG_CS, 1 << 3)
    for seg in (UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS):
        uc.reg_write(seg, 2 << 3)
    uc.reg_write(UC_X86_REG_FS, 3 << 3)
    for r, v in zip(REGS, regs[:8]):
        uc.reg_write(r, v)
    esp = regs[4]
    entry_ret = struct.unpack('<I', uc.mem_read(esp, 4))[0]
    # The function was entered by the lifted call: eip is the function, [esp] its return address.
    fn = int(sys.argv[2], 16) if len(sys.argv) > 2 else None
    if fn is None:
        sys.exit('usage: oracle.py <game dir> <function hex>')

    pending = list(calls)

    def on_unmapped(uc, access, address, size, value, user):
        if address < 0xFE000000 or not pending:
            print(f'fetch from unmapped {address:08X}')
            return False
        ret, eax, edx = pending.pop(0)[:3]
        sp = uc.reg_read(UC_X86_REG_ESP)
        got = struct.unpack('<I', uc.mem_read(sp, 4))[0]
        if got != ret:
            print(f'bridged call order differs: oracle returns to {got:08X}, recompiled run to {ret:08X}')
            return False
        uc.reg_write(UC_X86_REG_EAX, eax)
        uc.reg_write(UC_X86_REG_EDX, edx)
        uc.reg_write(UC_X86_REG_ESP, sp + 4)  # ponytail: cdecl pop only; record esp deltas for stdcall imports
        uc.reg_write(UC_X86_REG_EIP, ret)
        return True

    uc.hook_add(UC_HOOK_MEM_FETCH_UNMAPPED, on_unmapped)
    watch = int(sys.argv[3], 16) if len(sys.argv) > 3 else None  # print the writes to this address

    def on_write(uc, access, address, size, value, user):
        if address <= watch < address + size:
            print(f'  write {value:#x} ({size} bytes at {address:08X}) by {uc.reg_read(UC_X86_REG_EIP):08X}')
    if watch is not None:
        uc.hook_add(UC_HOOK_MEM_WRITE, on_write)
    try:
        uc.emu_start(fn, entry_ret)
    except UcError as e:
        print(f'emulation stopped: {e} at {uc.reg_read(UC_X86_REG_EIP):08X}')
    for name, r, want in zip(NAMES, REGS, out_regs[:8]):
        got = uc.reg_read(r)
        if name == 'esp':
            want += 4  # the recompiled exit state is captured at the ret, before it pops the return address
        if name != 'ebp' and got != want:
            print(f'{name}: original {got:08X}, recompiled {want:08X}')
    diffs = 0
    for lo, data in out_ranges:
        mine = bytes(uc.mem_read(lo, len(data)))
        i = 0
        while i < len(data):
            if mine[i] != data[i]:
                j = i
                while j < len(data) and mine[j] != data[j]:
                    j += 1
                if diffs < 40:
                    print(f'mem {lo + i:08X}..{lo + j:08X}: original {mine[i:min(j, i + 16)].hex()}'
                          f' recompiled {data[i:min(j, i + 16)].hex()}')
                diffs += 1
                i = j
            i += 1
    print(f'{diffs} differing memory runs')


if __name__ == '__main__':
    main(sys.argv[1])
