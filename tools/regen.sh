#!/bin/sh
# Regenerates src/recomp/gen from game/sh3.exe with the xboxrecomp pipeline (third_party/xboxrecomp).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
X=$ROOT/third_party/xboxrecomp
PY=$ROOT/.venv/bin/python3
B=$ROOT/build
mkdir -p "$B"
[ -z "$TRACE" ] && [ -f "$B/trace.json" ] && TRACE="$B/trace.json"  # oracle targets (tools/oracle.py)
cd "$ROOT"
if [ "$1" != --recomp-only ]; then
"$PY" tools/patch_exe.py game/sh3.exe "$B/sh3_ws.exe"  # widescreen code patches
"$PY" tools/pe2xbe.py "$B/sh3_ws.exe" "$B/sh3.xbe" --imports "$B/imports.json"
cd "$X"
"$PY" -m tools.xbe_parser "$B/sh3.xbe" --json "$B/sh3_analysis.json" --quiet
# Pass 1 finds the code; its immediates and data pointers seed the functions only data refers to.
"$PY" -m tools.disasm "$B/sh3.xbe" --text-only -o "$B/disasm" --force > "$B/disasm.log" 2>&1
(cd "$ROOT" && "$PY" tools/seeds.py)
"$PY" -m tools.disasm "$B/sh3.xbe" --text-only -o "$B/disasm" --force --seed-functions "$B/seeds.json" > "$B/disasm.log" 2>&1
(cd "$ROOT" && "$PY" tools/fix_functions.py "$B/sh3_ws.exe" "$B/disasm/functions.json")
"$PY" -m tools.func_id "$B/sh3.xbe" --functions "$B/disasm/functions.json" --strings "$B/disasm/strings.json" \
    --xrefs "$B/disasm/xrefs.json" -o "$B/func_id" > "$B/func_id.log" 2>&1
"$PY" -m tools.abi_analysis "$B/sh3.xbe" --disasm-dir "$B/disasm" --func-id-dir "$B/func_id" --output-dir "$B/abi" > "$B/abi.log" 2>&1
else cd "$X"; fi
# 0x662954 / 0x6628D8: the static CRT's _setjmp3 / longjmp.
"$PY" -m tools.recomp "$B/sh3.xbe" --all --split 250 --game-name sh3 --gen-dir "$ROOT/src/recomp/gen" \
    --setjmp 0x662954 --longjmp 0x6628D8 ${TRACE:+--trace-functions "$TRACE"} \
    --exclude-manual "$ROOT/src/game/sh3.c" \
    --disasm-dir "$B/disasm" --func-id-dir "$B/func_id" --abi-dir "$B/abi" -o "$B/recomp" > "$B/recomp.log" 2>&1
grep -E "functions \(|unresolved call targets" "$B/recomp.log"
