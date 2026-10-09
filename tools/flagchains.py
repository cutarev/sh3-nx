#!/usr/bin/env python3
"""Classifies the lifter's unresolved-flag branches (`if (_flags /* jcc */)`) in src/recomp/gen.

A marker means the lifter found no flag-setting instruction for a conditional jump. It is harmless when
the jump is unreachable (bytes after an unconditional goto/return with no label in between, e.g. a jump
table decoded as code); a reachable one is a real missing branch. Prints the reachable ones.
"""
import glob, os, re, sys

ROOT = os.path.join(os.path.dirname(__file__), '..', 'src', 'recomp', 'gen')
marker = re.compile(r'if \(_flags /\* (\w+)')
term = re.compile(r'^\s*(goto loc_\w+;|return\b|RECOMP_TAIL|RECOMP_ABI_RET|rt_unreachable)')
label = re.compile(r'^loc_[0-9A-F]+: ;|^(static )?void sub_|^\s*case ')
dead = live = 0
for path in sorted(glob.glob(os.path.join(ROOT, '*.c'))):
    lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
    for i, line in enumerate(lines):
        m = marker.search(line)
        if not m:
            continue
        reachable = True
        for j in range(i - 1, max(i - 400, 0), -1):
            s = lines[j]
            if label.search(s):
                break
            if term.search(s):
                reachable = False
                break
        if reachable:
            live += 1
            fn = next((lines[k] for k in range(i, 0, -1) if lines[k].startswith(('void sub_', 'static void sub_'))), '?')
            print(f'{os.path.basename(path)}:{i + 1}: {m.group(1)} in {fn.split("(")[0]}')
        else:
            dead += 1
print(f'{live} reachable, {dead} unreachable (decoded data)', file=sys.stderr)
