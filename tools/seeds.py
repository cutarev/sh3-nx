#!/usr/bin/env python3
"""Function entry points the disassembler cannot reach by calls: code addresses that appear as
data (callback tables) or as immediates (push offset thread_proc), and that start right after
padding or a ret, the way MSVC lays functions out. Writes build/seeds.json for tools.disasm.

    python tools/seeds.py
"""
import bisect
import itertools
import json
import struct
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM

from pe2xbe import parse_pe

EXE, XREFS, OUT = 'build/sh3_ws.exe', 'build/disasm/xrefs.json', 'build/seeds.json'  # tools/patch_exe.py's output
d = open(EXE, 'rb').read()
PE = parse_pe(d)
BASE = PE['base']
TEXT = next(s for s in PE['secs'] if s['name'] == '.text')
T0, T1 = BASE + TEXT['va'], BASE + TEXT['va'] + TEXT['vs']


def off(va):  # file offset of a .text address
    return va - BASE + TEXT['ro'] - TEXT['va']


def byte_at(va):
    return d[off(va)]


def looks_like_entry(va):
    return T0 <= va < T1 and byte_at(va - 1) in (0xCC, 0x90, 0xC3) and byte_at(va) not in (0xCC, 0x90, 0x00)


# Inside a function the first pass found, an address is usually a label of it (a loop MSVC aligned with
# a nop, a switch case): seeding 0x4A9100 split 0x4A8D70 in two and skipped half of it.
funcs = sorted((int(f['start'], 16), int(f['end'], 16)) for f in json.load(open('build/disasm/functions.json')))
starts = [a for a, _ in funcs]


def containing(va):
    i = bisect.bisect_right(starts, va) - 1
    return funcs[i] if i >= 0 and va < funcs[i][1] else None


md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True


def follows_transfer(va, fn_start):
    """Decoding the function from its start, va is an instruction boundary whose last non-padding
    predecessor is a ret or an unconditional jmp: no path falls into it, so it starts a function
    the first pass swallowed (they sit after `jmp tail; nop...` too)."""
    last = None
    for ins in md.disasm(d[off(fn_start):off(va)], fn_start):
        if ins.mnemonic not in ('nop', 'int3'):
            last = ins
        end = ins.address + ins.size
    return last is not None and end == va and last.mnemonic in ('ret', 'jmp')


def padded_entry(va):
    """16-byte aligned after a run of 2+ nop/int3: how MSVC pads between functions. Inside a function it
    aligns loops with one nop or with lea filler, never a run of nops."""
    run = 0
    while byte_at(va - 1 - run) in (0x90, 0xCC):
        run += 1
    return va % 16 == 0 and run >= 2


_reach_cache = {}


def reached(fn):
    """Instruction addresses reachable from the function's start by falling through and direct jumps
    (calls not followed, nothing past the function's end): its own code, unlike a linear sweep that
    runs out of phase on an embedded jump table."""
    if fn in _reach_cache:
        return _reach_cache[fn]
    start, end = fn
    seen, todo = set(), [start]
    while todo:
        a = todo.pop()
        while start <= a < end and a not in seen:
            ins = next(md.disasm(d[off(a):off(a) + 16], a), None)
            if ins is None:
                break
            seen.add(a)
            seen.update(range(a + 1, a + ins.size))  # inside an instruction counts as reached code too
            m = ins.mnemonic
            if m.startswith('j') and ins.operands and ins.operands[0].type == X86_OP_IMM:
                todo.append(ins.operands[0].imm)
                if m == 'jmp':
                    break
            elif m == 'jmp' and ins.operands and ins.operands[0].type == X86_OP_MEM:
                mem = ins.operands[0].mem
                if not mem.base and mem.index and mem.scale == 4:  # switch: jmp [reg*4 + table]
                    t = mem.disp & 0xFFFFFFFF
                    while T0 <= t < T1:
                        target = struct.unpack_from('<I', d, off(t))[0]
                        if not start <= target < end:
                            break
                        todo.append(target)
                        t += 4
                break
            elif m in ('ret', 'jmp', 'int3') or m.startswith('ret'):
                break
            a += ins.size
    _reach_cache[fn] = seen
    return seen


def boundary_in_code(va, fn_start):
    """The function's own code reaches va, as a label or inside an instruction (a constant in .data
    that happens to equal an address in the middle of a jne): not a function of its own. Every
    function spanning va counts: a jcc alias (0x530200) overlaps its parent (0x52FE90), and only the
    parent's switch reaches the call at 0x5307FF that a .data constant 0x530800 points into."""
    return any(va in reached(f) for f in funcs if f[0] <= va < f[1])


def gap_entry(va):
    return containing(va) is None and T0 <= va < T1 and va % 16 == 0 and byte_at(va) not in (0xCC, 0x90, 0x00)


def gap_label(va):
    """An earlier seed in the same gap reaches va: a label of that undetected function, like 0x4B7690,
    the target of a jmp inside 0x4B75F0."""
    i = bisect.bisect_right(starts, va)
    hi = starts[i] if i < len(starts) else T1
    lo = ends_before[i - 1] if i else T0
    return any(va in reached((g, hi)) for g in gap_entries[bisect.bisect_left(gap_entries, lo):
                                                           bisect.bisect_left(gap_entries, va)])


def is_seed(va):
    fn = containing(va)
    if fn is not None and va in data_ptrs and va % 16 == 0 and not boundary_in_code(va, fn[0]):
        return True  # a function pointer table names it and decoding never reaches it: data sits before it
    if gap_entry(va):
        return not gap_label(va)  # aligned like a function in a gap: a jmp thunk after a jump table (0x54D6A0)
    if not looks_like_entry(va):
        return False
    # MSVC emits int3 only between functions (after a noreturn call too: 0x6731B8 follows `call exit; int3`)
    return fn is None or padded_entry(va) or byte_at(va - 1) == 0xCC or follows_transfer(va, fn[0])


data_ptrs = set()
for sec in PE['secs']:
    if sec is not TEXT:
        for o in range(0, min(sec['rs'], sec['vs']) - 3, 4):
            data_ptrs.add(struct.unpack_from('<I', d, sec['ro'] + o)[0])
cands = set(data_ptrs)
cands |= {int(x['to'], 16) for x in json.load(open(XREFS)) if x['type'] in ('data_imm', 'call', 'jump')}
# Every E8 rel32 in .text, decoded or not: the sweep's xrefs miss a call it decoded out of phase after a byte
# table (0x4A9C60's first instruction calls 0x4DF840). is_seed keeps the targets that look like entries.
for o in range(off(T0), off(T1) - 5):
    if d[o] == 0xE8:
        t = (T0 + o - off(T0) + 5 + struct.unpack_from('<i', d, o + 1)[0]) & 0xFFFFFFFF
        if T0 <= t < T1 and t % 16 == 0 and looks_like_entry(t):
            cands.add(t)
ends_before = list(itertools.accumulate((e for _, e in funcs), max))  # aliases overlap their parent
gap_entries = sorted(a for a in cands if gap_entry(a))
observed = {int(l.split()[0], 16) for l in open('tools/observed_seeds.txt') if l.strip() and not l.startswith('#')}
seeds = sorted(a for a in cands if is_seed(a))
trusted = {a for a in seeds if containing(a) and a in data_ptrs} | observed  # may split a mis-swept function
json.dump([{'start': f'0x{a:08X}', **({'observed': True} if a in trusted else {})}
           for a in sorted(set(seeds) | observed)], open(OUT, 'w'), indent=0)
print(f'{len(seeds)} seeds -> {OUT}')
