#!/usr/bin/env python3
"""mkpak.py - pack user ELFs into BIN.PAK, the SacabambaspOS boot archive.

Layout (all little-endian, matching PakHdr/PakEnt in src/kernel/proc.c):
  0x00  char magic[8] = "SBOSPAK1"
  0x08  u32 count, u32 pad
  0x10  count * { char name[24] (NUL-padded), u64 offset, u64 size }
  data blobs, 16-aligned
Program name = source file stem, lowercased.
"""
import pathlib
import struct
import sys

def main():
    if len(sys.argv) < 3:
        sys.exit("usage: mkpak.py OUT.PAK prog1.elf [prog2.elf ...]")
    out, files = sys.argv[1], sys.argv[2:]

    entries, blobs = [], []
    off = (16 + 40 * len(files) + 15) & ~15
    for f in files:
        p = pathlib.Path(f)
        data = p.read_bytes()
        name = p.stem.lower().removeprefix("u_")[:23]
        entries.append((name, off, len(data)))
        blobs.append((off, data))
        off = (off + len(data) + 15) & ~15

    buf = bytearray(off)
    buf[0:8] = b"SBOSPAK1"
    struct.pack_into("<II", buf, 8, len(files), 0)
    pos = 16
    for name, o, size in entries:
        struct.pack_into("<24sQQ", buf, pos, name.encode(), o, size)
        pos += 40
    for o, data in blobs:
        buf[o:o + len(data)] = data

    pathlib.Path(out).write_bytes(buf)
    names = ", ".join(e[0] for e in entries)
    print(f"[mkpak] {out}  {len(files)} programs ({names})  {len(buf)} bytes")

main()
