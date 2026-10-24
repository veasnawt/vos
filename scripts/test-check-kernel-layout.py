#!/usr/bin/env python3
"""Host tests for check-kernel-layout.py on synthetic ELF64 images; no build, no VOS execution."""
import importlib.util
from pathlib import Path
import struct
from unittest import TestCase, main

spec = importlib.util.spec_from_file_location('layout', Path(__file__).with_name('check-kernel-layout.py'))
layout = importlib.util.module_from_spec(spec)
spec.loader.exec_module(layout)

VMA = layout.KERNEL_VMA
# The corrected production geometry: (paddr, filesz, memsz, flags, higher half).
GOOD = [(0x100000, 0x1000, 0x1000, 5, False), (0x101000, 0x28, 0x6000, 6, False),
        (0x107000, 0x18000, 0x18000, 5, True), (0x11f000, 0x7000, 0x7000, 4, True),
        (0x126000, 0x98, 0x8c000, 6, True)]


def elf(segs=GOOD, entry=0x100038, symbols=None, extra_loads=(), machine=62):
    phdrs = [(1, f, 0x1000 + (p & 0xfff), (VMA + p) if hi else p, p, fs, ms, 0x1000) for p, fs, ms, f, hi in segs]
    phdrs += list(extra_loads)
    sym = {'_start': entry, '__kernel_phys_start': segs[0][0],
           '__kernel_phys_end': segs[-1][0] + segs[-1][2] if segs else 0x200000}
    for (name, *_), seg in zip([('__kernel_phys_start',), ('__boot_data_start',), ('__text_start',),
                                ('__rodata_start',), ('__data_start',)], phdrs):
        sym.setdefault(name, seg[3])
    sym.update(symbols or {})
    strtab = b'\0'
    entries = [b'\0' * 24]
    for name, value in sym.items():
        entries.append(struct.pack('<IBBHQQ', len(strtab), 0x10, 0, 0xfff1, value, 0))
        strtab += name.encode() + b'\0'
    symtab = b''.join(entries)
    phoff = 64
    symoff = phoff + 56 * len(phdrs)
    stroff = symoff + len(symtab)
    shoff = (stroff + len(strtab) + 7) & ~7
    head = struct.pack('<4sBBBBB7sHHIQQQIHHHHHH', b'\x7fELF', 2, 1, 1, 0, 0, b'\0' * 7, 2, machine, 1,
                       entry, phoff, shoff, 0, 64, 56, len(phdrs), 64, 3, 0)
    body = b''.join(struct.pack('<IIQQQQQQ', *p) for p in phdrs) + symtab + strtab
    body += b'\0' * (shoff - 64 - len(body))
    shdrs = struct.pack('<IIQQQQIIQQ', *([0] * 10))
    shdrs += struct.pack('<IIQQQQIIQQ', 0, 2, 0, 0, symoff, len(symtab), 2, 1, 8, 24)
    shdrs += struct.pack('<IIQQQQIIQQ', 0, 3, 0, 0, stroff, len(strtab), 0, 0, 1, 0)
    return head + body + shdrs


def edit(i, **change):
    segs = [list(s) for s in GOOD]
    for k, v in change.items():
        segs[i][('paddr', 'filesz', 'memsz', 'flags', 'high').index(k)] = v
    return [tuple(s) for s in segs]


class Layout(TestCase):
    def rejects(self, data, words):
        with self.assertRaises(layout.LayoutError) as e:
            layout.check(data)
        self.assertIn(words, str(e.exception))

    def test_corrected_layout_passes(self):
        self.assertEqual(len(layout.check(elf())), 5)

    def test_original_boot_text_tail_rejected(self):          # production before the correction
        self.rejects(elf(edit(0, filesz=0x21a, memsz=0x21a)), 'not page-aligned')

    def test_original_rodata_tail_rejected(self):
        self.rejects(elf(edit(3, filesz=0x6fb0, memsz=0x6fb0)), 'not page-aligned')

    def test_original_bss_tail_rejected(self):
        self.rejects(elf(edit(4, memsz=0x8b7f8), symbols={'__kernel_phys_end': 0x1b2000}), 'not page-aligned')

    def test_hole_rejected(self):
        self.rejects(elf(edit(3, filesz=0x6000, memsz=0x6000)), 'hole')

    def test_overlap_rejected(self):
        self.rejects(elf(edit(3, filesz=0x8000, memsz=0x8000)), 'overlaps')

    def test_misaligned_start_rejected(self):
        self.rejects(elf(edit(1, paddr=0x101800)), 'not page-aligned')

    def test_filesz_above_memsz_rejected(self):
        self.rejects(elf(edit(1, filesz=0x7000)), 'filesz')

    def test_empty_file_bytes_rejected(self):
        self.rejects(elf(edit(1, filesz=0)), 'filesz')

    def test_rwx_rejected(self):
        self.rejects(elf(edit(2, flags=7)), 'writable and executable')

    def test_wrong_role_flags_rejected(self):
        self.rejects(elf(edit(3, flags=5)), 'flags')
        self.rejects(elf(edit(4, flags=4)), 'flags')

    def test_reservation_end_mismatch_rejected(self):
        self.rejects(elf(symbols={'__kernel_phys_end': 0x1b3000}), 'reservation ends')

    def test_reservation_start_mismatch_rejected(self):
        self.rejects(elf(symbols={'__kernel_phys_start': 0x101000}), 'overlaps below 0x101000')   # segment below it
        self.rejects(elf(symbols={'__kernel_phys_start': 0xff000}), 'hole after 0xff000')         # page not loaded

    def test_segment_symbol_mismatch_rejected(self):
        self.rejects(elf(symbols={'__rodata_start': VMA + 0x120000}), 'does not start at __rodata_start')

    def test_beyond_kmap_span_rejected(self):
        self.rejects(elf(edit(4, memsz=0x2fa000), symbols={'__kernel_phys_end': 0x420000}), '4 MiB')

    def test_unaligned_reservation_rejected(self):
        self.rejects(elf(symbols={'__kernel_phys_end': 0x1b2010}), 'page-aligned')

    def test_virtual_address_relocation_rejected(self):
        segs = [list(s) for s in GOOD]
        segs[3][4] = False                         # rodata identity-mapped instead of higher half
        self.rejects(elf([tuple(s) for s in segs]), 'virtual address')

    def test_entry_outside_boot_text_rejected(self):
        self.rejects(elf(entry=0x101010), 'entry')
        self.rejects(elf(symbols={'_start': 0x100040}), 'entry')

    def test_extra_or_missing_segment_rejected(self):
        self.rejects(elf(extra_loads=[(1, 4, 0x1000, 0x1b2000, 0x1b2000, 0x10, 0x1000, 0x1000)]), 'PT_LOAD')
        self.rejects(elf(GOOD[:4], symbols={'__data_start': 0}), 'PT_LOAD')

    def test_missing_symbol_rejected(self):
        data = elf()
        self.rejects(data.replace(b'__rodata_start\0', b'__rodata_stXrt\0'), 'missing symbol')

    def test_not_x86_64_rejected(self):
        self.rejects(elf(machine=3), 'x86-64')
        self.rejects(b'\x7fELF' + b'\1' * 60, 'ELF64')
        self.rejects(b'MZ' + b'\0' * 100, 'not an ELF')

    def test_cli_status(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            good, bad = Path(d) / 'good', Path(d) / 'bad'
            good.write_bytes(elf())
            bad.write_bytes(elf(edit(3, filesz=0x6fb0, memsz=0x6fb0)))
            self.assertEqual(layout.main(['x', str(good)]), 0)
            self.assertEqual(layout.main(['x', str(good), str(bad)]), 1)
            self.assertEqual(layout.main(['x']), 2)


if __name__ == '__main__':
    main()
