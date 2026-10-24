#!/usr/bin/env python3
"""Host test: the reconstructed N3.5 Stage 5 additional UEFI runs; no build, no media, no VOS execution.

Checked against independent sources, never against the module's own tables: the Stage 5 report's run table (names,
RAM, CPU, marker counts and order), the report's marker rules, make test-uefi's own file/ext2/8 GiB runs in arch.mk
(EXT2_DISK, markers, OVMF/sparse variables), the kernel's printed literals, scripts/exit-test.sh's sparse RAM method,
and the strict pmm record rule.
"""
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import re
from unittest import TestCase, main

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
spec = spec_from_file_location('uefi_additional', HERE / 'test-uefi-additional.py')
ua = module_from_spec(spec)
spec.loader.exec_module(ua)


def stage5_rows():
    text = (ROOT / 'docs/results/n3.5-stage5.md').read_text()
    section = text.split('### Additional fresh UEFI configurations: all 19 runs', 1)[1].split('\n## ', 1)[0]
    return [(m[1], m[2], m[3], int(m[4])) for m in
            re.finditer(r'^\| `([^`]+)` \| (\S+) / (\S+) \| PASS — exit status 33 and (\d+) marker\(s\) found \|$', section, re.M)]


def pmm(ignored):
    return 'pmm: 262144 frames tracked below 1 GiB, 28610 usable (114440 KiB), %d bytes of RAM above 1 GiB ignored' % ignored


class Report(TestCase):
    def test_runs_equal_the_stage5_table(self):
        rows = stage5_rows()
        self.assertEqual(len(rows), 19)
        cmds = ua.commands()
        self.assertEqual([(c['name'], c['memory'], c['cpu']) for c in cmds], [(n, m, c) for n, m, c, _ in rows])
        markers = [c['argv'][4:c['argv'].index('--')] for c in cmds]
        self.assertEqual([len(m) for m in markers], [k for _, _, _, k in rows])

    def test_marker_rules_of_the_report(self):
        for c in ua.commands():
            with self.subTest(c['name']):
                m = c['argv'][4:c['argv'].index('--')]
                self.assertIn('selftest: io-scope 18 cases, 0 failed', m)
                self.assertEqual(m[-1] if c['memory'] != '8G' else m[-2], 'VOS-TEST: PASS')
                if c['image'] == 'file':
                    self.assertIn('selftest: file 845 cases, 0 failed', m)
                if c['image'] == 'ext2':
                    self.assertIn('selftest: ext2 97 cases, 0 failed, ', m)
                self.assertEqual('bytes of RAM above 1 GiB ignored' in m, c['memory'] == '8G')


class Commands(TestCase):
    def arch(self):
        return (ROOT / 'arch/x86_64/arch.mk').read_text()

    def test_ext2_disk_is_make_test_uefi_disk(self):
        mk = self.arch()
        ext2_dir = re.search(r'^EXT2_DIR\s*:=\s*(\S+)', mk, re.M)[1]
        disk = re.search(r'^EXT2_DISK\s*:=\s*(.*?)\n(?!\s)', mk, re.M | re.S)[1].replace('\\\n', ' ')
        self.assertEqual(disk.replace('$(EXT2_DIR)', ext2_dir).split(), ua.EXT2_DISK)

    def test_isos_are_make_test_uefi_images(self):
        mk = self.arch()
        self.assertIn('$(TEST_ISO:vos.iso=vos-uefi.iso)', mk)
        for image in ('file', 'ext2'):
            self.assertIn('build/$(ARCH)-test-%s/vos-uefi.iso' % image, mk)
            self.assertEqual(ua.ISO[image], 'build/x86_64-test-%s/vos-uefi.iso' % image)
        self.assertEqual(ua.ISO['base'], 'build/x86_64-test/vos-uefi.iso')

    def test_markers_are_make_test_uefi_and_kernel_literals(self):
        mk = self.arch()
        for literal in ("'selftest: file 845 cases, 0 failed'", "'bytes of RAM above 1 GiB ignored'", "'selftest: pmm-kernel '",
                        'TIMEOUT=240 MEM=8G SPARSE_MEM=1 scripts/exit-test.sh uefi-test-8G'):
            self.assertIn(literal, mk)
        src = ROOT / 'arch/x86_64'
        self.assertIn('serial_puts("selftest: io-scope ");', (src / 'selftest_io_scope.c').read_text())
        self.assertIn('serial_puts("selftest: ext2 ");', (src / 'selftest_ext2.c').read_text())
        self.assertIn('serial_puts(" bytes of RAM above 1 GiB ignored\\r\\n");', (src / 'kernel.c').read_text())

    def test_exact_commands(self):
        cmds = {c['name']: c for c in ua.commands()}
        base = cmds['i5-base-1G']
        self.assertEqual(base['env'], {'OVMF': '1', 'MEM': '1G', 'TIMEOUT': '240'})
        self.assertEqual(base['argv'], ['scripts/exit-test.sh', 'i5-base-1G', 'build/x86_64-test/vos-uefi.iso', '33',
                                        'selftest: pmm-kernel ', 'selftest: io-scope 18 cases, 0 failed', 'VOS-TEST: PASS', '--'])
        f8 = cmds['i5-file-max-8G']
        self.assertEqual(f8['env'], {'OVMF': '1', 'MEM': '8G', 'TIMEOUT': '240', 'SPARSE_MEM': '1'})
        self.assertEqual(f8['argv'][-7:], ['--', *ua.EXT2_DISK, '-cpu', 'max'])
        e128 = cmds['i5-ext2-default-128M']
        self.assertEqual(e128['argv'][-5:], ['--', *ua.EXT2_DISK])
        self.assertEqual(e128['serial'], 'build/x86_64-test-ext2/i5-ext2-default-128M.serial.log')
        for c in ua.commands():
            with self.subTest(c['name']):
                self.assertEqual('SPARSE_MEM' in c['env'], c['memory'] == '8G')
                self.assertEqual(c['argv'][-2:] == ['-cpu', 'max'], c['cpu'] == 'max')
                self.assertEqual(c['argv'][:4], ['scripts/exit-test.sh', c['name'], ua.ISO[c['image']], '33'])

    def test_exit_test_sparse_method(self):
        text = (HERE / 'exit-test.sh').read_text()
        self.assertIn('if [ "${SPARSE_MEM:-0}" = 1 ]; then', text)
        self.assertIn('rm -f "$DIR/$NAME.ram"', text)


class Serial(TestCase):
    def test_pmm_rules(self):
        for mem, ignored, ok in (('128M', 0, True), ('512M', 0, True), ('1G', 0, True), ('1G', 4096, False),
                                 ('8G', 0, False), ('8G', 7 << 30, True)):
            with self.subTest(mem=mem, ignored=ignored):
                text = 'boot\r\n' + pmm(ignored) + '\r\nVOS-TEST: PASS\r\n'
                if ok:
                    self.assertEqual(ua.check_serial(text, mem), ignored)
                else:
                    with self.assertRaisesRegex(RuntimeError, '^guest memory not as configured'):
                        ua.check_serial(text, mem)

    def test_pmm_record_single_and_trusted(self):
        for text in ('boot\r\n', 'user[forge]: ' + pmm(0) + '\r\n', pmm(0) + '\r\n' + pmm(0) + '\r\n'):
            with self.subTest(text[:20]), self.assertRaisesRegex(RuntimeError, '^missing/duplicate pmm memory record$'):
                ua.check_serial(text, '1G')


if __name__ == '__main__':
    main()
