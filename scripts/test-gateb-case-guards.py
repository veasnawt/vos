#!/usr/bin/env python3
"""Host test: which disk self-tests each image compiles (selftest_vblk.c, selftest_blk.c); no build, no VOS execution.

Images that attach the immutable ext2 disk (ext2, file, accounting, N3.6 Gate A and Gate B) must compile the
attached-disk check and neither the absent-device test nor the no-disk expectation; every other test image
must compile the absent-device checks. Gate B must compile exactly Gate A's self-test code.
"""
from pathlib import Path
import shutil
import subprocess
from unittest import TestCase, main, skipUnless

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / 'arch/x86_64'
CC = ['gcc', '-std=gnu11', '-ffreestanding', '-m64', '-mgeneral-regs-only', '-E', '-P', '-I', str(SRC)]
ATTACHED = ('ext2', 'file', 'accounting', 'storage_stress', 'storage_stress_b')
NO_DISK = ('modules', 'suite', 'irq', 'stress')


def compiled(case):
    defs = ['-DVOS_TEST'] + (['-DVOS_TEST_CASE_' + case] if case else [])
    out = []
    for f in ('selftest_vblk.c', 'selftest_blk.c'):
        p = subprocess.run(CC + defs + [str(SRC / f)], capture_output=True, text=True, timeout=60)
        if p.returncode:
            raise AssertionError(f + ': ' + p.stderr[:300])
        out.append(p.stdout)
    vblk, blk = out
    return dict(absent='test_absent();' in vblk, attached='virtio-blk device ready (ext2 image)' in vblk,
                no_disk='no usable disk: no device registered' in blk), out


@skipUnless(shutil.which('gcc'), 'gcc required')
class CaseGuards(TestCase):
    def test_attached_disk_images(self):
        for case in ATTACHED:
            with self.subTest(case):
                self.assertEqual(compiled(case)[0], dict(absent=False, attached=True, no_disk=False))

    def test_no_disk_images(self):
        for case in (None,) + NO_DISK:
            with self.subTest(case):
                self.assertEqual(compiled(case)[0], dict(absent=True, attached=False, no_disk=True))

    def test_gate_b_compiles_gate_a_self_tests(self):
        self.assertEqual(compiled('storage_stress_b')[1], compiled('storage_stress')[1])


if __name__ == '__main__':
    main()
