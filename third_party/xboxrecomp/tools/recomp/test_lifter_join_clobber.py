"""Merged ZF must survive a destination overwrite (SH3's window triangles).

The two paths of signed remainder by two leave flags from AND or INC.
MOV ECX,9 preserves those flags before JNE selects the triangle's winding.
"""
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

from . import config
from .translator import FunctionTranslator


@pytest.mark.parametrize("jcc", ("je", "jne"))
@pytest.mark.parametrize("width", (8, 16, 32))
def test_merged_zero_flag_survives_mov(jcc, width):
    # and ecx,80000001h; jns join; dec ecx; or ecx,-2; inc ecx;
    # join: mov ecx,9; je/jne taken; xor eax,eax; ret;
    # taken: mov eax,1; ret
    opcode = "74" if jcc == "je" else "75"
    mask, negative = {
        8: ("80e181", "fec980c9fefec1"),
        16: ("6681e10180", "66496683c9fe6641"),
        32: ("81e101000080", "4983c9fe41"),
    }[width]
    negative = bytes.fromhex(negative)
    image = (bytes.fromhex(mask) + bytes((0x79, len(negative))) + negative
             + bytes.fromhex("b909000000" + opcode + "0331c0c3b801000000c3"))
    base = 0x10000
    config._install([config.Section(".text", base, len(image), 0,
                                    len(image), True)],
                    entry_point=base, kernel_thunk_addr=base,
                    origin="join-clobber-test")
    db = {base: {"start": hex(base), "end": base + len(image),
                 "_addr": base, "size": len(image)}}
    code = FunctionTranslator(image, db).translate_function(base, db[base])
    cc = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        pytest.skip("C compiler unavailable")
    comparison = "==" if jcc == "je" else "!="
    source = """#include <stdint.h>
static uint32_t eax, ecx, esp;
#define LO8(r) ((uint8_t)(r))
#define LO16(r) ((uint16_t)(r))
#define SET_LO8(r, v) ((r) = ((r) & 0xFFFFFF00u) | (uint8_t)(v))
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | (uint16_t)(v))
""" + code + """
int main(void) {
    static const uint32_t values[] = {
        0, 1, 2, 3, 8, 9, 0x7FFFFFFE, 0x7FFFFFFF,
        0x80000000, 0x80000001, 0xFFFFFFFE, 0xFFFFFFFF
    };
    for (unsigned i = 0; i < sizeof values / sizeof values[0]; i++) {
        ecx = values[i]; eax = 99; esp = 0;
        sub_00010000();
        if (eax != ((values[i] & 1u) """ + comparison + """ 0)) return 1;
    }
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="join-clobber-") as tmp:
        c, exe = Path(tmp) / "test.c", Path(tmp) / "test"
        c.write_text(source)
        built = subprocess.run([cc, "-O2", str(c), "-o", str(exe)],
                               capture_output=True, text=True)
        assert built.returncode == 0, built.stderr
        ran = subprocess.run([str(exe)], capture_output=True, text=True)
        assert ran.returncode == 0, code
