"""
MSVC lowers a sparse `switch` into a compare chain whose edges share one jcc:

    cmp eax, 50001h
    je  case_a
    cmp eax, 90048h
  join:
    je  case_b          ; reached by fall-through from the cmp above...
    xor eax, eax
    ret
  far:
    sub eax, 1Eh        ; ...and by `sub eax, K; jmp join` from another arm
    jmp join

The predecessors of `join` disagree on the setter (cmp snapshots its operands, sub its result), so
no single snapshot answers the je. Silent Hill 2's event and message dispatchers (0x4763F2,
0x47DE81, 0x5550A3) have this shape; the lifter used to emit the `_flags` fallback there, which is
never true, so those cases were unreachable. Every ZF producer now also writes _zr (zero exactly
when ZF is set), which the join reads.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.test_flag_join_backedge import _translate  # noqa: E402


def test_cmp_and_sub_edges_share_a_je():
    image = (b"\x3D\x48\x00\x09\x00"  # +0  cmp eax, 0x90048
             b"\x74\x05"              # +5  je +5 -> +12 (join)
             b"\x2D\x1E\x00\x00\x00"  # +7  sub eax, 0x1E   (other arm, falls into the join)
             b"\x74\x01"              # +12 join: je +1 -> +15
             b"\x90"                  # +14 nop
             b"\xC3")                 # +15 ret
    code = _translate(image)
    assert "_flags /*" not in code, code
    assert "(_zr == 0)" in code, code


def test_cmp_publishes_equality_as_zero():
    code = _translate(b"\x3D\x48\x00\x09\x00\xC3")  # cmp eax, 0x90048; ret
    assert "_zr = _fa ^ _fb;" in code, code


if __name__ == "__main__":
    test_cmp_and_sub_edges_share_a_je()
    test_cmp_publishes_equality_as_zero()
