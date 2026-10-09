#!/usr/bin/env python3
"""Differential fuzzing of the lifter on sh3.exe's own instructions.

One sample of every instruction form (mnemonic + operand shapes) found in the game is lifted with
xboxrecomp's real pipeline (lift_basic_block) and run as C, and the same bytes run on Unicorn; both
start from the same random registers and memory. Registers, the scratch memory and every condition
code the instruction defines (read back with setcc) must agree.

    python tools/fuzz.py [-k mnemonic] [--vectors 24]
"""
import argparse
import json
import os
import random
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
XR = os.path.join(ROOT, 'third_party', 'xboxrecomp')
sys.path.insert(0, XR)
from capstone import Cs, CS_ARCH_X86, CS_MODE_32  # noqa: E402
from capstone import x86_const as X  # noqa: E402
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_PROT_ALL  # noqa: E402
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX,  # noqa: E402
                               UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI,
                               UC_X86_REG_EFLAGS)
from tools.recomp.disasm import BasicBlock, Disassembler  # noqa: E402
from tools.recomp.lifter import Lifter, lift_basic_block  # noqa: E402
from tools.recomp.translator import FunctionTranslator, FP_STACK_MACROS  # noqa: E402

EXE = os.path.join(ROOT, 'game', 'sh3.exe')
BASE = 0x400000
CODE = 0x00100000          # where each snippet runs (Unicorn) / is said to live (lifter)
SCRATCH = 0x10000000       # 4 KB every memory operand points into
FLAGS_AT = 0x10002000      # setcc results, one byte per condition
REGS = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
UREGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP,
         UC_X86_REG_ESI, UC_X86_REG_EDI]
# Condition codes in setcc opcode order (0F 90+cc) and the flags each reads.
CONDS = [('o', 'O'), ('no', 'O'), ('b', 'C'), ('ae', 'C'), ('e', 'Z'), ('ne', 'Z'), ('be', 'CZ'), ('a', 'CZ'),
         ('s', 'S'), ('ns', 'S'), ('p', 'P'), ('np', 'P'), ('l', 'SO'), ('ge', 'SO'), ('le', 'ZSO'), ('g', 'ZSO')]
FLAG_BITS = {'C': (X.X86_EFLAGS_MODIFY_CF, X.X86_EFLAGS_UNDEFINED_CF, X.X86_EFLAGS_RESET_CF, X.X86_EFLAGS_SET_CF),
             'Z': (X.X86_EFLAGS_MODIFY_ZF, X.X86_EFLAGS_UNDEFINED_ZF, X.X86_EFLAGS_RESET_ZF, X.X86_EFLAGS_SET_ZF),
             'S': (X.X86_EFLAGS_MODIFY_SF, X.X86_EFLAGS_UNDEFINED_SF, X.X86_EFLAGS_RESET_SF, X.X86_EFLAGS_SET_SF),
             'O': (X.X86_EFLAGS_MODIFY_OF, X.X86_EFLAGS_UNDEFINED_OF, X.X86_EFLAGS_RESET_OF, X.X86_EFLAGS_SET_OF),
             'P': (X.X86_EFLAGS_MODIFY_PF, X.X86_EFLAGS_UNDEFINED_PF, X.X86_EFLAGS_RESET_PF, X.X86_EFLAGS_SET_PF)}
SKIP_PREFIX = ('j', 'call', 'ret', 'loop', 'f', 'int', 'in', 'out', 'ins', 'outs', 'lcall', 'ljmp', 'iret',
               'cpuid', 'rdtsc', 'hlt', 'cli', 'sti', 'pf', 'pi2f', 'pswap', 'emms', 'femms', 'lods', 'stos',
               'movs', 'cmps', 'scas', 'rep', 'enter', 'leave', 'pushf', 'popf', 'pusha', 'popa', 'bound',
               'arpl', 'les', 'lds', 'lea', 'nop', 'wait', 'cld', 'std', 'sahf', 'lahf', 'xlat', 'daa',
               'das', 'aaa', 'aas', 'aam', 'aad', 'into', 'salc', 'lock', 'cmpxchg', 'xadd')


def form_key(ins):
    ops = []
    for op in ins.operands:
        if op.type == X.X86_OP_REG:
            ops.append(f'r{op.size}')
        elif op.type == X.X86_OP_IMM:
            ops.append(f'i{op.size}')
        else:
            ops.append(f'm{op.size}' + ('i' if op.mem.index else '') + ('s' if op.mem.segment else ''))
    return ins.mnemonic + ' ' + ','.join(ops)


def usable(ins, carry_ok=False):
    if ins.mnemonic in CARRY_USERS and not carry_ok:
        return False
    if ins.mnemonic in ('div', 'idiv') or consumer_cc(ins) is not None:
        return False
    if ins.mnemonic.startswith(SKIP_PREFIX) or any(p in (0xF2, 0xF3, 0xF0) for p in ins.prefix if p):
        return False
    for op in ins.operands:
        if op.type == X.X86_OP_REG and ins.reg_name(op.reg) not in (
                REGS + ['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di', 'al', 'cl', 'dl', 'bl', 'ah', 'ch', 'dh', 'bh']):
            return False  # mmx/xmm/x87/segment registers
        if op.type == X.X86_OP_MEM and op.mem.segment:
            return False
    return True


CC_INDEX = {cc: i for i, (cc, _) in enumerate(CONDS)}
FLAG_MASK = 0
for _bits in FLAG_BITS.values():
    FLAG_MASK |= _bits[0] | _bits[2] | _bits[3]
CARRY_USERS = ('adc', 'sbb', 'rcl', 'rcr')


def consumer_cc(ins):
    """The condition a jcc/setcc/cmovcc reads, or None."""
    for pre in ('cmov', 'set', 'j'):
        if ins.mnemonic.startswith(pre) and ins.mnemonic[len(pre):] in CC_INDEX:
            return CC_INDEX[ins.mnemonic[len(pre):]]
    return None


def collect(filter_m, per_form):
    """singles: instructions whose results do not depend on incoming flags.
    pairs: a flag-setting instruction and the next flag reader in the game's own code, keyed by
    (setter form, what reads it)."""
    d = open(EXE, 'rb').read()
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    singles, pairs = {}, {}
    for f in json.load(open(os.path.join(ROOT, 'build', 'disasm', 'functions.json'))):
        s, e = int(f['start'], 16), int(f['end'], 16)
        if not (0x401000 <= s < e <= 0x68A000):
            continue
        setter = None
        for ins in md.disasm(d[s - BASE:e - BASE], s):
            cc = consumer_cc(ins)
            reads_carry = ins.mnemonic in CARRY_USERS
            if (cc is not None or reads_carry) and setter is not None and usable(setter):
                if not filter_m or filter_m in (setter.mnemonic, ins.mnemonic):
                    if cc is not None:
                        key = (form_key(setter), 'set' + CONDS[cc][0])
                        item = (setter.address, bytes(setter.bytes), setter, cc, None)
                    elif usable(ins, carry_ok=True) and all(op.type != X.X86_OP_MEM for op in ins.operands):
                        key = (form_key(setter), form_key(ins))
                        item = (setter.address, bytes(setter.bytes), setter, None, ins)
                    else:
                        key = None
                    if key:
                        lst = pairs.setdefault(key, [])
                        if len(lst) < per_form:
                            lst.append(item)
            if ins.eflags & FLAG_MASK:
                setter = ins
            elif ins.mnemonic.startswith(('j', 'call', 'ret')):
                setter = None
            if cc is None and not reads_carry and usable(ins) and (not filter_m or ins.mnemonic == filter_m):
                lst = singles.setdefault(form_key(ins), [])
                if len(lst) < per_form and bytes(ins.bytes) not in (x[1] for x in lst):
                    lst.append((ins.address, bytes(ins.bytes), ins, None, None))
    return singles, pairs


def collect_fpu(per_form):
    """MSVC's float compare: [fld.. fcomp..] fnstsw ax; test ah, imm (or sahf); jcc. The whole run, from
    the game's code, with the jcc turned into a setcc."""
    d = open(EXE, 'rb').read()
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    seqs = {}
    for f in json.load(open(os.path.join(ROOT, 'build', 'disasm', 'functions.json'))):
        s, e = int(f['start'], 16), int(f['end'], 16)
        if not (0x401000 <= s < e <= 0x68A000):
            continue
        insns = list(md.disasm(d[s - BASE:e - BASE], s))
        for i, ins in enumerate(insns):
            if ins.mnemonic != 'fnstsw' or i + 2 >= len(insns):
                continue
            t, j = insns[i + 1], insns[i + 2]
            if not ((t.mnemonic == 'test' and t.op_str.startswith('ah')) or t.mnemonic == 'sahf'):
                continue
            cc = consumer_cc(j)
            if cc is None:
                continue
            k = i - 1  # back to the fld that starts the comparison, through x87 instructions only
            while k >= 0 and i - k <= 4 and insns[k].mnemonic.startswith('f') and not insns[k].mnemonic.startswith(('fld', 'fild')):
                k -= 1
            if k < 0 or not insns[k].mnemonic.startswith(('fld', 'fild')) or i - k > 4:
                continue
            run = insns[k:i + 2]
            if not all(usable_fpu(x) for x in run) or not stack_valid(run):
                continue
            key = ' ; '.join(form_key(x) for x in run) + ' ; set' + CONDS[cc][0]
            lst = seqs.setdefault(key, [])
            if len(lst) < per_form:
                lst.append((run[0].address, b''.join(bytes(x.bytes) for x in run), run[0], cc, run))
    return seqs


def stack_valid(run):
    """The run never reads an x87 register it did not load itself (the test starts from an empty stack)."""
    depth = 0
    for ins in run:
        m = ins.mnemonic
        sts = [int(ins.reg_name(op.reg)[3:-1]) if ins.reg_name(op.reg).startswith('st(') else 0
               for op in ins.operands if op.type == X.X86_OP_REG and ins.reg_name(op.reg).startswith('st')]
        need = max(sts) + 1 if sts else (2 if m == 'fcompp' else 1)
        if m.startswith(('fld', 'fild')) and m != 'fldcw':
            if sts and depth < need:
                return False
            depth += 1
            continue
        if m in ('fnstsw', 'test', 'sahf'):
            continue
        if depth < need:
            return False
        if m == 'fcompp':
            depth -= 2
        elif m.endswith('p') or m in ('fcomp', 'fistp'):
            depth -= 1
    return True


def usable_fpu(ins):
    for op in ins.operands:
        if op.type == X.X86_OP_MEM and op.mem.segment:
            return False
    return ins.mnemonic not in ('fstsw', 'fnstcw', 'fldcw', 'fnstenv', 'fldenv', 'fsave', 'frstor')


def defined_conds(ins):
    ef = ins.eflags
    out = []
    for i, (cc, flags) in enumerate(CONDS):
        ok = True
        for fl in flags:
            mod, und, reset, setb = FLAG_BITS[fl]
            if ef & und or not (ef & (mod | reset | setb)):
                ok = False
        if ok:
            out.append(i)
    return out


def snippet(ins_bytes, conds):
    code = ins_bytes
    for i in conds:
        code += bytes([0x0F, 0x90 + i, 0x05]) + struct.pack('<I', FLAGS_AT + i)
    return code


def lift(code):
    dis = Disassembler()
    insns = [dis._decode_instruction(i) for i in dis._cs.disasm(code, CODE)]
    lifter = Lifter()
    lifter.needs_cf = FunctionTranslator._function_needs_cf(insns)
    lines, _ = lift_basic_block(lifter, BasicBlock(start=CODE, instructions=insns))
    return list(lines)


NICE_FLOATS = [0.0, 1.0, -1.0, 0.5, 2.0, 1e-5, 100.0, -100.0, 3.25, -0.0]


def make_inputs(ins, rnd, n, run=None):
    """Registers chosen so every memory operand lands in SCRATCH; boundary values otherwise."""
    special = [0, 1, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678]
    vecs = []
    for _ in range(n):
        regs = [rnd.choice(special) if rnd.random() < 0.5 else rnd.getrandbits(32) for _ in REGS]
        regs[4] = SCRATCH + 0x800  # esp
        absmem = []
        ops = [op for x in (run or [ins]) for op in x.operands]
        regs_fixed = set()
        for op in ops:
            if op.type != X.X86_OP_MEM:
                continue
            m = op.mem
            base = ins.reg_name(m.base) if m.base else None
            index = ins.reg_name(m.index) if m.index else None
            if base in regs_fixed:
                continue
            if base:
                regs_fixed.add(base)
            off = rnd.randrange(0x100, 0x700) & ~3
            if index:
                regs[REGS.index(index)] = rnd.randrange(0, 8) if base else 0
            if base:
                iv = regs[REGS.index(index)] * m.scale if index and index != base else 0
                regs[REGS.index(base)] = (SCRATCH + off - iv - m.disp) & 0xFFFFFFFF
                if index == base:  # [r + r*s]: solve for r
                    regs[REGS.index(base)] = ((SCRATCH + off - m.disp) // (1 + m.scale)) & 0xFFFFFFFF
            else:
                absmem.append(m.disp & 0xFFFFFFFF)
        if run:  # x87: floats a comparison can tell apart, and repeats so some compare equal
            dbl = any(op.type == X.X86_OP_MEM and op.size == 8 for op in ops)
            vals = [rnd.choice(NICE_FLOATS[:4] if rnd.random() < 0.5 else NICE_FLOATS) for _ in range(0x1000 // (8 if dbl else 4))]
            mem = struct.pack(f'<{len(vals)}{"d" if dbl else "f"}', *vals)
        else:
            mem = bytes(rnd.getrandbits(8) for _ in range(0x1000))
        flags = 0x2 | rnd.choice([0, 1]) | rnd.choice([0, 0x40]) | rnd.choice([0, 0x80]) | rnd.choice([0, 0x800])
        vecs.append((regs, mem, flags, absmem))
    return vecs


def run_unicorn(code, vec):
    regs, mem, flags, absmem = vec
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(CODE, 0x1000, UC_PROT_ALL)
    uc.mem_write(CODE, code)
    uc.mem_map(SCRATCH, 0x3000, UC_PROT_ALL)
    uc.mem_write(SCRATCH, mem)
    for a in absmem:
        page = a & ~0xFFF
        try:
            uc.mem_map(page, 0x2000, UC_PROT_ALL)
        except Exception:
            pass
        uc.mem_write(a, mem[:8])
    for r, v in zip(UREGS, regs):
        uc.reg_write(r, v)
    uc.reg_write(UC_X86_REG_EFLAGS, flags)
    uc.emu_start(CODE, CODE + len(code))
    out = [uc.reg_read(r) for r in UREGS]
    return out, bytes(uc.mem_read(SCRATCH, 0x1000)), bytes(uc.mem_read(FLAGS_AT, 16)), \
        [bytes(uc.mem_read(a, 8)) for a in absmem]


HARNESS_HEAD = r'''
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include <stdio.h>
#include <sys/mman.h>
ptrdiff_t g_xbox_mem_offset; uint32_t g_xbox_code_lo, g_xbox_code_hi; int g_force_return;
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_fs_base, g_seh_ebp;
RECOMP_TLS double g_fp_stack[8]; RECOMP_TLS int g_fp_top, g_fp_cmp, g_df;
RECOMP_TLS uint16_t g_fp_control_word = 0x027F, g_fp_cc;
RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3, g_xmm4, g_xmm5, g_xmm6, g_xmm7;
RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3, g_mm4, g_mm5, g_mm6, g_mm7;
volatile uint32_t g_icall_trace[ICALL_TRACE_SIZE]; volatile uint32_t g_icall_trace_idx; volatile uint64_t g_icall_count;
void recomp_unimpl(const char *t, uint32_t va) { fprintf(stderr, "UNIMPL %s\n", t); }
void recomp_cpuid(void) {}
recomp_func_t recomp_lookup(uint32_t va) { return 0; }
recomp_func_t recomp_lookup_kernel(uint32_t va) { return 0; }
recomp_func_t recomp_lookup_manual(uint32_t va) { return 0; }
void recomp_icall_fail_log(uint32_t va) {} void recomp_icall_not_code_log(uint32_t va) {}
static uint32_t in_regs[8], in_flags; static uint8_t in_mem[0x1000]; static uint32_t in_abs[4]; static int n_abs;
static void setup(void) {
    g_eax = in_regs[0]; g_ecx = in_regs[1]; g_edx = in_regs[2]; g_ebx = in_regs[3]; g_esp = in_regs[4];
    g_esi = in_regs[6]; g_edi = in_regs[7];
    memcpy((void *)XBOX_PTR(0x10000000u), in_mem, 0x1000);
    memset((void *)XBOX_PTR(0x10002000u), 0, 16);
    for (int i = 0; i < n_abs; i++) memcpy((void *)XBOX_PTR(in_abs[i]), in_mem, 8);
}
static void report(int t, int v, uint32_t ebp_out) {
    printf("%d %d %08X %08X %08X %08X %08X %08X %08X %08X ", t, v, g_eax, g_ecx, g_edx, g_ebx, g_esp, ebp_out, g_esi, g_edi);
    for (int i = 0; i < 0x1000; i++) printf("%02X", ((uint8_t *)XBOX_PTR(0x10000000u))[i]);
    printf(" ");
    for (int i = 0; i < 16; i++) printf("%02X", ((uint8_t *)XBOX_PTR(0x10002000u))[i]);
    for (int i = 0; i < n_abs; i++) { printf(" "); for (int j = 0; j < 8; j++) printf("%02X", ((uint8_t *)XBOX_PTR(in_abs[i]))[j]); }
    printf("\n");
}
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-k', help='only this mnemonic')
    ap.add_argument('--vectors', type=int, default=24)
    ap.add_argument('--per-form', type=int, default=2)
    a = ap.parse_args()
    rnd = random.Random(1234)
    singles, pairs = collect(a.k, a.per_form)
    fpu = collect_fpu(a.per_form) if not a.k or a.k == 'fpu' else {}
    if a.k == 'fpu':
        singles, pairs = {}, {}
    tests = []
    for key, samples in sorted(fpu.items()):
        for addr, b, ins, cc, run in samples:
            code = snippet(b, [cc])
            try:
                lines = lift(code)
            except Exception as e:  # noqa: BLE001
                print(f'lift failed {key} @{addr:08X}: {e}')
                continue
            tests.append((key, addr, b, ins, [cc], b'\xdb\xe3' + code, lines, make_inputs(ins, rnd, a.vectors, run)))
    for key, samples in sorted(singles.items()) + sorted(pairs.items(), key=lambda kv: str(kv[0])):
        for addr, b, ins, cc, consumer in samples:
            if cc is not None:
                conds, code = [cc], snippet(b, [cc])
            elif consumer is not None:
                conds, code = [], b + bytes(consumer.bytes)
            else:
                conds, code = [], b
            try:
                lines = lift(code)
            except Exception as e:  # noqa: BLE001
                print(f'lift failed {key} @{addr:08X}: {e}')
                continue
            tests.append((str(key), addr, b, ins, conds, code, lines, make_inputs(ins, rnd, a.vectors)))
    forms = list(singles) + list(pairs)
    print(f'{len(singles)} single forms, {len(pairs)} setter/reader pairs, {len(fpu)} x87 compares, {len(tests)} samples')
    work = os.path.join(ROOT, 'build', 'fuzz')
    os.makedirs(work, exist_ok=True)
    src = [HARNESS_HEAD, '\n'.join(FP_STACK_MACROS)]
    main_lines = ['int main(void) {',
                  '    g_xbox_mem_offset = (ptrdiff_t)mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, '
                  'MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);']
    for t, (key, addr, b, ins, conds, code, lines, vecs) in enumerate(tests):
        body = '\n'.join('    ' + l for l in lines)
        src.append(f'static uint32_t lif_{t}(uint32_t ebp, int _cf) {{ int _flags = 0;\n'
                   f'    uint32_t _fa = 0, _fb = 0, _zr = 0; int32_t _fas = 0, _fbs = 0; (void)_zr;\n'
                   f'    (void)_cf; (void)_flags; (void)_fa; (void)_fb; (void)_fas; (void)_fbs;\n'
                   f'{body}\n    return ebp; }}')
        for v, (regs, mem, flags, absmem) in enumerate(vecs):
            main_lines.append('    { static const uint8_t m[] = {' + ','.join(str(x) for x in mem) + '};')
            main_lines.append('      memcpy(in_mem, m, sizeof m); ' +
                              ' '.join(f'in_regs[{i}] = {v}u;' for i, v in enumerate(regs)) +
                              f' n_abs = {len(absmem)}; ' + ' '.join(f'in_abs[{i}] = {v}u;' for i, v in enumerate(absmem)) +
                              f' setup(); report({t}, {v}, lif_{t}({regs[5]}u, {flags & 1})); }}')
    main_lines.append('    return 0; }')
    src.append('\n'.join(main_lines))
    open(os.path.join(work, 'fuzz.c'), 'w').write('\n'.join(src))
    gen = os.path.join(ROOT, 'src', 'recomp', 'gen')
    r = subprocess.run(['clang', '-O0', '-w', '-fwrapv', '-fno-strict-aliasing', '-I', gen, '-o',
                        os.path.join(work, 'fuzz'), os.path.join(work, 'fuzz.c'), '-lm'], capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[:4000])
        sys.exit(1)
    run = subprocess.run([os.path.join(work, 'fuzz')], capture_output=True, text=True)
    out = run.stdout.splitlines()
    unimpl = sorted(set(l for l in run.stderr.splitlines() if l.startswith('UNIMPL')))
    if unimpl:
        print('lifted to RECOMP_UNIMPL:', ', '.join(u[7:] for u in unimpl))
    results = {}
    for line in out:
        f = line.split()
        if len(f) >= 12:
            results[(int(f[0]), int(f[1]))] = f[2:]
    bad = 0
    rare = set()
    for t, (key, addr, b, ins, conds, code, lines, vecs) in enumerate(tests):
        shown = False
        for v, vec in enumerate(vecs):
            got = results.get((t, v))
            if got is None:
                print(f'no C result for {key} @{addr:08X} (the lifted code crashed?)')
                break
            try:
                ur, umem, uflags, uabs = run_unicorn(code, vec)
            except Exception as e:  # noqa: BLE001
                print(f'unicorn could not run {key} @{addr:08X}: {e}')
                break
            lregs = [int(x, 16) for x in got[:8]]
            lmem, lflags = bytes.fromhex(got[8]), bytes.fromhex(got[9])
            labs = [bytes.fromhex(x) for x in got[10:]]
            diffs = []
            x87 = code[:2] == b'\xdb\xe3'
            for i, n in enumerate(REGS):
                if x87 and n == 'eax':
                    if (lregs[0] >> 8) & 0x45 != (ur[0] >> 8) & 0x45:
                        diffs.append(f'ah&45 {(ur[0] >> 8) & 0x45:02X}!={(lregs[0] >> 8) & 0x45:02X}')
                elif not x87 and lregs[i] != ur[i]:
                    diffs.append(f'{n} {ur[i]:08X}!={lregs[i]:08X}')
            if lmem != umem and not x87:
                i = next(j for j in range(0x1000) if lmem[j] != umem[j])
                diffs.append(f'mem+{i:X} {umem[i]:02X}!={lmem[i]:02X}')
            for i in conds:
                if lflags[i] != uflags[i]:
                    if CONDS[i][1] in ('O', 'P') and not x87:
                        rare.add(f'{key}: set{CONDS[i][0]}')
                    else:
                        diffs.append(f'set{CONDS[i][0]} {uflags[i]}!={lflags[i]}')
            if labs != uabs:
                diffs.append('absmem')
            if diffs and not shown:
                bad += 1
                shown = True
                print(f'MISMATCH {key:28s} @{addr:08X} {ins.mnemonic} {ins.op_str}  in={[hex(x) for x in vec[0]]}'
                      f' flags={vec[2]:X}\n    {"; ".join(diffs)}\n    lifted: {" | ".join(l for l in lines if "set" not in l)[:300]}')
    print(f'{len(tests)} samples, {bad} with mismatches')
    if rare:
        print('overflow/parity conditions only (rarely read):', ', '.join(sorted(rare)))


if __name__ == '__main__':
    main()
