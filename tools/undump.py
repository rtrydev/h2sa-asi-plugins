#!/usr/bin/env python3
"""Turn a raw in-memory dump of hitman2.exe (from H2SADump.asi) into a
PE that pefile/analysis can read.

The dump is the mapped image verbatim (byte i == *(base+i)), so RVAs already
equal file offsets. We only need to rewrite the section table so each
section's PointerToRawData == VirtualAddress and SizeOfRawData >= VirtualSize
(i.e. a "memory-aligned" PE), and mark the .text section readable. Imports
are already resolved in the dump; that is fine for code analysis.

Usage:
  python3 tools/undump.py hitman2_dump.bin [-o hitman2.unpacked.exe]
"""
import sys, os, struct, argparse


def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def p16(b, o, v): struct.pack_into("<H", b, o, v)
def p32(b, o, v): struct.pack_into("<I", b, o, v)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("-o", "--out", default=None)
    args = ap.parse_args()

    data = bytearray(open(args.dump, "rb").read())
    if data[:2] != b"MZ":
        sys.exit("not an MZ image (dump must start at the module base)")
    e_lfanew = u32(data, 0x3C)
    if data[e_lfanew:e_lfanew + 4] != b"PE\x00\x00":
        sys.exit("no PE header at e_lfanew")
    coff = e_lfanew + 4
    n_sec = u16(data, coff + 2)
    opt_size = u16(data, coff + 16)
    opt = coff + 20
    magic = u16(data, opt)
    if magic != 0x10B:
        sys.exit("not a PE32 image")
    sec_tab = opt + opt_size

    IMAGE_SCN_MEM_READ = 0x40000000
    IMAGE_SCN_MEM_EXECUTE = 0x20000000
    print(f"sections: {n_sec}")
    for i in range(n_sec):
        s = sec_tab + i * 40
        name = data[s:s + 8].rstrip(b"\x00").decode("latin1", "replace")
        vsz = u32(data, s + 8)
        va = u32(data, s + 12)
        # memory-align: raw data now lives at file offset == VA
        raw_size = (vsz + 0xFFF) & ~0xFFF
        p32(data, s + 16, raw_size)      # SizeOfRawData
        p32(data, s + 20, va)            # PointerToRawData
        chars = u32(data, s + 36)
        # ensure the code section is analyzable/readable
        if name.lower().startswith(".text") or (chars & IMAGE_SCN_MEM_EXECUTE):
            chars |= IMAGE_SCN_MEM_READ
        p32(data, s + 36, chars)
        print(f"  {name:8} va=0x{va:06x} vsz=0x{vsz:06x} "
              f"-> raw@0x{va:06x} size=0x{raw_size:06x} chars=0x{chars:08x}")

    # FileAlignment must divide the new PointerToRawData values (== VA, which
    # is SectionAlignment-aligned). Set FileAlignment = SectionAlignment.
    sect_align = u32(data, opt + 32)
    p32(data, opt + 36, sect_align)      # FileAlignment

    out = args.out or (os.path.splitext(args.dump)[0] + ".unpacked.exe")
    open(out, "wb").write(data)
    print(f"wrote {out} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
