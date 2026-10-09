#!/usr/bin/env python3
"""Wrap a 32-bit Windows PE (sh3.exe) in an XBE so the xboxrecomp pipeline reads it unchanged.

Every section keeps its PE virtual address, so the lifted code addresses match the original exe.
The XBE has no kernel imports: the PE's import table stays in .idata and the runtime fills the
IAT with host bridges. The import list is written to a JSON file for that.

    python tools/pe2xbe.py sh3.exe build/sh3.xbe --imports build/imports.json
"""
import argparse
import json
import struct

ENTRY_RETAIL_XOR = 0xA8FC57AB
THUNK_RETAIL_XOR = 0x5B6D40B6
XBE_WRITABLE, XBE_PRELOAD, XBE_EXECUTABLE = 1, 2, 4
PE_EXECUTE, PE_WRITE = 0x20000000, 0x80000000

# Offsets inside the 4 KB XBE header page (mapped at the image base).
CERT, SECHDR, NAMES, ZEROS, DBGPATH = 0x180, 0x360, 0x4C0, 0x560, 0x580


def parse_pe(d):
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    assert d[pe:pe + 4] == b'PE\0\0' and struct.unpack_from('<H', d, pe + 4)[0] == 0x14C, 'not a PE32 i386'
    nsec, = struct.unpack_from('<H', d, pe + 6)
    optsz, = struct.unpack_from('<H', d, pe + 20)
    opt = pe + 24
    ep, = struct.unpack_from('<I', d, opt + 16)
    base, = struct.unpack_from('<I', d, opt + 28)
    size_image, size_hdrs = struct.unpack_from('<II', d, opt + 56)
    stack_commit, heap_res, heap_commit = struct.unpack_from('<4xIII', d, opt + 72)
    timestamp, = struct.unpack_from('<I', d, pe + 8)
    imp_rva, = struct.unpack_from('<I', d, opt + 96 + 8)
    tls_rva, = struct.unpack_from('<I', d, opt + 96 + 9 * 8)
    assert tls_rva == 0, 'PE TLS directory not supported'
    secs = []
    for i in range(nsec):
        name, vs, va, rs, ro, _, _, _, _, ch = struct.unpack_from('<8sIIIIIIHHI', d, opt + optsz + i * 40)
        secs.append(dict(name=name.rstrip(b'\0').decode(), vs=vs, va=va, rs=rs, ro=ro, ch=ch))
    assert size_hdrs <= 0x1000 and all(s['ro'] >= 0x1000 for s in secs)
    return dict(base=base, ep=ep, size_image=size_image, stack_commit=stack_commit, heap_res=heap_res,
                heap_commit=heap_commit, timestamp=timestamp, imp_rva=imp_rva, secs=secs)


def parse_imports(d, pe):
    def off(rva):
        for s in pe['secs']:
            if s['va'] <= rva < s['va'] + max(s['vs'], s['rs']):
                return rva - s['va'] + s['ro']
        raise ValueError(hex(rva))

    def cstr(o):
        return d[o:d.index(b'\0', o)].decode()

    out, o = [], off(pe['imp_rva'])
    while True:
        ilt, _, _, name_rva, iat = struct.unpack_from('<IIIII', d, o)
        o += 20
        if not name_rva:
            return out
        t, slot = off(ilt or iat), pe['base'] + iat
        while (e := struct.unpack_from('<I', d, t)[0]):
            fn = f'#{e & 0xFFFF}' if e & 0x80000000 else cstr(off(e) + 2)
            out.append(dict(dll=cstr(off(name_rva)), name=fn, iat=f'0x{slot:08X}'))
            t, slot = t + 4, slot + 4


def build_xbe(d, pe):
    base = pe['base']
    h = bytearray(0x1000)
    h[0:4] = b'XBEH'
    struct.pack_into('<IIIIIIIIII', h, 0x104,
                     base, 0x1000, pe['size_image'], 0x178, pe['timestamp'],
                     base + CERT, len(pe['secs']), base + SECHDR, 0,
                     (base + pe['ep']) ^ ENTRY_RETAIL_XOR)
    struct.pack_into('<IIIIIIIIIIII', h, 0x12C,
                     0, pe['stack_commit'], pe['heap_res'], pe['heap_commit'], base, pe['size_image'], 0,
                     pe['timestamp'], base + DBGPATH, base + DBGPATH, 0,
                     (base + ZEROS) ^ THUNK_RETAIL_XOR)  # empty kernel thunk table
    # Certificate: size, timestamp, title id, title name.
    struct.pack_into('<III', h, CERT, 0x1D0, pe['timestamp'], 0x4B4E0002)
    title = 'Silent Hill 3'.encode('utf-16-le')
    h[CERT + 12:CERT + 12 + len(title)] = title
    h[DBGPATH:DBGPATH + 8] = b'sh3.exe\0'
    for i, s in enumerate(pe['secs']):
        flags = XBE_PRELOAD
        flags |= XBE_EXECUTABLE if s['ch'] & PE_EXECUTE else 0
        flags |= XBE_WRITABLE if s['ch'] & PE_WRITE else 0
        name_va = base + NAMES + i * 16
        h[NAMES + i * 16:NAMES + i * 16 + len(s['name'])] = s['name'].encode()
        struct.pack_into('<IIIIIIIII', h, SECHDR + i * 56,
                         flags, base + s['va'], s['vs'], s['ro'], min(s['rs'], s['vs']),
                         name_va, 0, base + ZEROS, base + ZEROS)
    # PE file offsets of the sections are kept as they are, so the XBE is the new header page
    # followed by the PE file from 0x1000 on.
    return bytes(h) + d[0x1000:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe')
    ap.add_argument('xbe')
    ap.add_argument('--imports')
    a = ap.parse_args()
    d = open(a.exe, 'rb').read()
    pe = parse_pe(d)
    open(a.xbe, 'wb').write(build_xbe(d, pe))
    if a.imports:
        json.dump(parse_imports(d, pe), open(a.imports, 'w'), indent=1)
    print(f"{a.xbe}: base 0x{pe['base']:08X} entry 0x{pe['base'] + pe['ep']:08X} "
          f"{len(pe['secs'])} sections")


if __name__ == '__main__':
    main()
