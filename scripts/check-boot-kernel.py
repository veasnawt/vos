#!/usr/bin/env python3
"""Prove a packaged ELF64 kernel preserves entry and every PT_LOAD byte/flag."""
import pathlib
import struct
import sys


def image(path):
    data = pathlib.Path(path).read_bytes()
    assert data[:7] == b"\x7fELF\x02\x01\x01", path
    header = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
    kind, machine, version, entry, phoff = header[:5]
    assert (kind, machine, version) == (2, 62, 1), path
    phsize, count = header[8:10]
    assert phsize == 56 and count and phoff + phsize * count <= len(data), path
    loads = []
    for i in range(count):
        ptype, flags, offset, va, pa, filesz, memsz, align = struct.unpack_from(
            "<IIQQQQQQ", data, phoff + phsize * i)
        if ptype == 1:
            assert filesz <= memsz and offset + filesz <= len(data), path
            loads.append((flags, va, pa, filesz, memsz, align, data[offset:offset+filesz]))
    assert loads, path
    return entry, loads


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: check-boot-kernel.py ORIGINAL PACKAGED")
    assert image(sys.argv[1]) == image(sys.argv[2]), "boot kernel load image changed"
    print("check-boot-kernel: entry and all PT_LOAD bytes, sizes, addresses and flags unchanged")
