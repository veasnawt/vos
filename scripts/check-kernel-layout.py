#!/usr/bin/env python3
"""x86_64 kernel layout check: the PT_LOAD memory ranges tile the page-rounded kernel reservation.

The kernel reserves and maps [__kernel_phys_start, __kernel_phys_end) by whole 4 KiB pages
(boot_reserve, kmap.c) and refuses loader objects sharing any of those pages. A loader only
honours [p_paddr, p_paddr + p_memsz) of each PT_LOAD, so every byte of every kernel page must
lie in exactly one segment, or GRUB may legally put the Multiboot2 info block in a page tail.

Derived only from the ELF program headers and symbol table:
  - five PT_LOADs in address order, starting at __kernel_phys_start, __boot_data_start,
    __text_start, __rodata_start and __data_start, flags R-X, RW-, R-X, R--, RW- (no RWX);
  - page-aligned physical starts and ends, contiguous (no hole, no overlap), the last
    ending exactly at __kernel_phys_end, at or below the 4 MiB kmap span;
  - 0 < p_filesz <= p_memsz, file offset congruent with the address, boot segments
    identity-mapped and the rest at KERNEL_VMA + paddr;
  - the entry point is _start, inside the boot text's file bytes.
usage: check-kernel-layout.py KERNEL...   (exit 0 if every kernel passes)
"""
import struct
import sys

PAGE = 0x1000
KERNEL_VMA = 0xFFFFFFFF80000000
KMAP_SPAN = 0x400000
PT_LOAD, SHT_SYMTAB = 1, 2
X, W, R = 1, 2, 4
LAYOUT = (('boot text', '__kernel_phys_start', R | X, False), ('boot data', '__boot_data_start', R | W, False),
          ('text', '__text_start', R | X, True), ('rodata', '__rodata_start', R, True),
          ('data', '__data_start', R | W, True))
SYMBOLS = ('_start', '__kernel_phys_end') + tuple(s for _, s, _, _ in LAYOUT)


class LayoutError(Exception):
    pass


def need(ok, why):
    if not ok:
        raise LayoutError(why)


def parse(data):
    need(len(data) >= 64 and data[:4] == b'\x7fELF', 'not an ELF file')
    need(data[4] == 2 and data[5] == 1, 'not ELF64 little-endian')
    e_type, e_machine = struct.unpack_from('<HH', data, 16)
    need(e_type == 2 and e_machine == 62, 'not an x86-64 executable')
    entry, phoff, shoff = struct.unpack_from('<QQQ', data, 24)
    phentsize, phnum, shentsize, shnum = struct.unpack_from('<HHHH', data, 54)
    need(phentsize == 56 and phoff + phnum * 56 <= len(data), 'bad program header table')
    loads = []
    for i in range(phnum):
        t, flags, off, va, pa, fs, ms, al = struct.unpack_from('<IIQQQQQQ', data, phoff + i * 56)
        if t == PT_LOAD:
            loads.append(dict(flags=flags, offset=off, vaddr=va, paddr=pa, filesz=fs, memsz=ms, align=al))
    need(shentsize == 64 and shoff + shnum * 64 <= len(data), 'bad section header table')
    sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * 64) for i in range(shnum)]
    symbols = {}
    for name, stype, _, _, off, size, link, _, _, entsize in sections:
        if stype != SHT_SYMTAB:
            continue
        need(entsize == 24 and link < shnum and off + size <= len(data), 'bad symbol table')
        stroff, strsize = sections[link][4], sections[link][5]
        need(stroff + strsize <= len(data), 'bad string table')
        for j in range(0, size, 24):
            st_name, _, _, _, value, _ = struct.unpack_from('<IBBHQQ', data, off + j)
            if st_name and st_name < strsize:
                end = data.index(b'\0', stroff + st_name)
                key = data[stroff + st_name:end].decode('latin1')
                if key in SYMBOLS:
                    need(key not in symbols or symbols[key] == value, 'ambiguous symbol ' + key)
                    symbols[key] = value
    return entry, loads, symbols


def check(data):
    """Return the verified segment list; raise LayoutError on any violation."""
    entry, loads, sym = parse(data)
    for s in SYMBOLS:
        need(s in sym, 'missing symbol ' + s)
    need(len(loads) == len(LAYOUT), 'expected %d PT_LOAD segments, found %d' % (len(LAYOUT), len(loads)))
    start, end = sym['__kernel_phys_start'], sym['__kernel_phys_end']
    need(start % PAGE == 0 and end % PAGE == 0 and start < end <= KMAP_SPAN,
         'kernel reservation [%#x, %#x) not page-aligned or beyond the 4 MiB kmap span' % (start, end))
    expect = start
    for seg, (role, symbol, flags, high) in zip(loads, LAYOUT):
        pa, ms, fs = seg['paddr'], seg['memsz'], seg['filesz']
        where = '%s segment [%#x, %#x)' % (role, pa, pa + ms)
        need(seg['flags'] & (W | X) != (W | X), where + ' is writable and executable')
        need(seg['flags'] == flags, where + ' has flags %#x, expected %#x' % (seg['flags'], flags))
        need(pa % PAGE == 0 and (pa + ms) % PAGE == 0, where + ' is not page-aligned')
        need(0 < fs <= ms, where + ' has filesz %#x, memsz %#x' % (fs, ms))
        need(seg['align'] >= PAGE and seg['align'] & (seg['align'] - 1) == 0 and
             seg['offset'] % PAGE == seg['vaddr'] % PAGE, where + ' has inconsistent alignment')
        need(pa == expect, where + (' leaves a hole after %#x' if pa > expect else ' overlaps below %#x') % expect)
        need(seg['vaddr'] == (KERNEL_VMA + pa if high else pa), where + ' is not at its expected virtual address')
        need(sym[symbol] == seg['vaddr'], where + ' does not start at ' + symbol)
        expect = pa + ms
    need(expect == end, 'segments end at %#x, kernel reservation ends at %#x' % (expect, end))
    boot = loads[0]
    need(entry == sym['_start'] and boot['vaddr'] <= entry < boot['vaddr'] + boot['filesz'],
         'entry %#x is not _start inside the boot text' % entry)
    return loads


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    status = 0
    for path in argv[1:]:
        try:
            with open(path, 'rb') as f:
                loads = check(f.read())
            print('kernel layout OK [%s]: %s' % (path, ' '.join('%#x-%#x/%x' % (s['paddr'], s['paddr'] + s['memsz'], s['flags'])
                                                                 for s in loads)))
        except (LayoutError, OSError) as e:
            print('kernel layout FAIL [%s]: %s' % (path, e), file=sys.stderr)
            status = 1
    return status


if __name__ == '__main__':
    sys.exit(main(sys.argv))
