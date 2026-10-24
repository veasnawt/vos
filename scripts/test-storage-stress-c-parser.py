#!/usr/bin/env python3
"""Host test: Gate C selector and strict SC03/SC04/SC05 serial parser; no build, no media, no VOS execution.

Every negative case must fail for its intended reason (the exact require() message), not merely fail.
"""
from importlib.util import module_from_spec, spec_from_file_location
from itertools import combinations
from pathlib import Path
import sys
from unittest import TestCase, main

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
spec = spec_from_file_location('gatec_runner', HERE / 'test-storage-stress-c.py')
runner = module_from_spec(spec)
spec.loader.exec_module(runner)

SUBSETS = [c for n in (1, 2, 3) for c in combinations(('SC03', 'SC04', 'SC05'), n)]


def serial(groups, transport, checks=1234, tail=True):
    """A golden Gate C boot log built independently of the parser's own case generators."""
    out = ['VOS x86_64 boot', 'virtio-blk device ready (ext2 image)', 'storage-stress: TRANSPORT %s PASS' % transport]
    irq = transport == 'IRQ'
    for g in groups:
        if g == 'SC03':
            out.append('storage-stress: SC03 BEGIN seed=0x4E360003 cases=32')
            n = 0
            for op in ('open', 'read', 'stat', 'enum'):
                for site in ('device', 'reader'):
                    for order in ('kill-first', 'complete-first'):
                        for reuse in ('same', 'other'):
                            orphan = int(irq and site == 'device' and order == 'kill-first')
                            out.append('process 7 killed: Gate C SC03 %s' % order)
                            out.append('storage-stress: SC03 CASE %d op=%s site=%s order=%s reuse=%s orphan=%d '
                                       'kill=PASS publication=NONE accounting=PASS' % (n, op, site, order, reuse, orphan))
                            n += 1
            out.append('storage-stress: SC03 PASS cases=32 kills=32 publication=NONE accounting=PASS')
        elif g == 'SC04':
            out.append('storage-stress: SC04 BEGIN seed=0x4E360004 cases=12')
            n = applicable = 0
            for mix, orphans in (('AA', 0), ('AO', 1), ('OO', 2)):
                for order in (0, 1):
                    for rnd in (0, 1):
                        head = 'storage-stress: SC04 CASE %d mix=%s order=%d round=%d' % (n, mix, order, rnd)
                        if orphans and not irq:
                            out.append(head + ' NOT APPLICABLE (polling transport: a killed polling waiter leaves no orphan)')
                        else:
                            out.append(head + ' orphans=%d retire=EXACT accounting=PASS' % orphans)
                            applicable += 1
                        n += 1
            out.append('storage-stress: SC04 PASS applicable=%d not_applicable=%d accounting=PASS' % (applicable, 12 - applicable))
        else:
            out.append('storage-stress: SC05 BEGIN seed=0x4E360005 cases=12')
            n = 0
            for rnd in (0, 1):
                for fault, dest in (('line-fallback', 'EXACT'), ('prefix-eio', 'UNCHANGED'), ('eintr', 'UNCHANGED')):
                    for role in ('owner', 'waiter'):
                        out.append('storage-stress: SC05 CASE %d fault=%s role=%s round=%d dest=%s retry=PASS accounting=PASS'
                                   % (n, fault, role, rnd, dest))
                        n += 1
            out.append('storage-stress: SC05 PASS cases=12 accounting=PASS')
    missing = [g for g in ('SC01', 'SC02', 'SC03', 'SC04', 'SC05', 'SC10', 'SC11') if g not in groups]
    out.append('storage-stress: ' + ' '.join(missing) + ' NOT RUN')
    out.append('storage-stress: CHECKS %d PASS' % checks)
    out.append('storage-stress: SELECTED PASS')
    if tail:
        out.append('VOS-TEST: PASS')
    return '\r\n'.join(out) + '\r\n'


class Selector(TestCase):
    def test_every_fixed_order_subset_accepted(self):
        for groups in SUBSETS:
            self.assertEqual(runner.selection(','.join(groups)), groups)

    def test_rejected_selections(self):
        for text, why in (('', 'unsupported, duplicate or empty group'),
                          ('SC03,SC03', 'unsupported, duplicate or empty group'),
                          ('SC01', 'unsupported, duplicate or empty group'),
                          ('SC02,SC03', 'unsupported, duplicate or empty group'),
                          ('SC03,SC10', 'unsupported, duplicate or empty group'),
                          ('sc03', 'unsupported, duplicate or empty group'),
                          ('SC03, SC04', 'unsupported, duplicate or empty group'),
                          ('SC03,', 'unsupported, duplicate or empty group'),
                          ('SC04,SC03', 'out-of-order selection'),
                          ('SC05,SC04', 'out-of-order selection'),
                          ('SC03,SC05,SC04', 'out-of-order selection')):
            with self.subTest(text), self.assertRaisesRegex(RuntimeError, '^' + why + '$'):
                runner.selection(text)

    def test_configuration_exact(self):
        modules, cfg = runner.configuration(('SC03', 'SC04', 'SC05'), 'BIOS')
        self.assertEqual(modules, [('hello.elf', 'hello'), ('stress.img', 'storage-stress.img'), ('gatec.elf', 'stress-gatec')])
        self.assertEqual(cfg, "set timeout=0\nset default=0\nmenuentry 'VOS N3.6 Gate C' {\n"
                              "  multiboot2 /boot/vos.bin n36-groups=SC03,SC04,SC05\n"
                              "  module2 /boot/hello.elf hello\n  module2 /boot/stress.img storage-stress.img\n"
                              "  module2 /boot/gatec.elf stress-gatec\n  boot\n}\n")
        with self.assertRaisesRegex(RuntimeError, '^out-of-order selection$'):
            runner.configuration(('SC05', 'SC03'), 'UEFI')
        with self.assertRaisesRegex(RuntimeError, '^unknown firmware$'):
            runner.configuration(('SC03',), 'ARM')

    def test_transport_by_firmware(self):
        self.assertEqual((runner.transport_of('BIOS'), runner.transport_of('UEFI')), ('IRQ', 'POLL'))


class Parser(TestCase):
    def ok(self, groups, transport, text=None):
        return runner.validate_serial(text if text is not None else serial(groups, transport), groups, transport, 33)

    def bad(self, text, why, groups=('SC03', 'SC04', 'SC05'), transport='IRQ', exit_status=33):
        with self.assertRaisesRegex(RuntimeError, '^' + why + '$'):
            runner.validate_serial(text, groups, transport, exit_status)

    def test_golden_every_subset_and_transport(self):
        for groups in SUBSETS:
            for transport in ('IRQ', 'POLL'):
                with self.subTest(groups=groups, transport=transport):
                    s = self.ok(groups, transport)
                    irq = transport == 'IRQ'
                    self.assertEqual(s['groups'], list(groups))
                    self.assertEqual(s['checks'], 1234)
                    self.assertEqual((s['SC03_cases'], s['SC03_kills'], s['SC03_orphans']),
                                     (32, 32, 8 if irq else 0) if 'SC03' in groups else (0, 0, 0))
                    self.assertEqual((s['SC04_applicable'], s['SC04_not_applicable'], s['SC04_orphans']),
                                     ((12, 0, 12) if irq else (4, 8, 0)) if 'SC04' in groups else (0, 0, 0))
                    self.assertEqual(s['SC05_cases'], 12 if 'SC05' in groups else 0)

    def test_not_applicable_never_counted_as_pass(self):
        s = self.ok(('SC04',), 'POLL')
        self.assertEqual((s['SC04_applicable'], s['SC04_not_applicable']), (4, 8))
        # A polling log that claims an orphan case passed is rejected, not counted.
        text = serial(('SC04',), 'POLL').replace(
            'SC04 CASE 4 mix=AO order=0 round=0 NOT APPLICABLE (polling transport: a killed polling waiter leaves no orphan)',
            'SC04 CASE 4 mix=AO order=0 round=0 orphans=1 retire=EXACT accounting=PASS')
        self.bad(text, 'incomplete/out-of-order/inexact SC04 case 4', ('SC04',), 'POLL')
        # An interrupt log may never mark an orphan case NOT APPLICABLE.
        text = serial(('SC04',), 'IRQ').replace(
            'SC04 CASE 11 mix=OO order=1 round=1 orphans=2 retire=EXACT accounting=PASS',
            'SC04 CASE 11 mix=OO order=1 round=1 NOT APPLICABLE (polling transport: a killed polling waiter leaves no orphan)')
        self.bad(text, 'incomplete/out-of-order/inexact SC04 case 11', ('SC04',), 'IRQ')
        # The group verdict must carry the transport's exact counts.
        text = serial(('SC04',), 'POLL').replace('applicable=4 not_applicable=8', 'applicable=12 not_applicable=0')
        self.bad(text, 'missing/duplicate/incomplete group result', ('SC04',), 'POLL')

    def test_transport_mismatch(self):
        self.bad(serial(('SC03',), 'POLL'), 'missing/duplicate/wrong transport', ('SC03',), 'IRQ')
        self.bad(serial(('SC03',), 'IRQ'), 'missing/duplicate/wrong transport', ('SC03',), 'POLL')
        self.bad(serial(('SC03',), 'IRQ'), 'unknown transport', ('SC03',), 'irq')

    def test_orphan_accounting_exact(self):
        full = serial(('SC03',), 'IRQ')
        self.bad(full.replace('SC03 CASE 0 op=open site=device order=kill-first reuse=same orphan=1',
                              'SC03 CASE 0 op=open site=device order=kill-first reuse=same orphan=0'),
                 'incomplete/out-of-order/inexact SC03 case 0', ('SC03',))
        self.bad(serial(('SC03',), 'POLL').replace('reuse=same orphan=0 kill', 'reuse=same orphan=1 kill', 1),
                 'incomplete/out-of-order/inexact SC03 case 0', ('SC03',), 'POLL')
        self.bad(full.replace('site=reader order=kill-first reuse=same orphan=0',
                              'site=reader order=kill-first reuse=same orphan=1', 1),
                 'incomplete/out-of-order/inexact SC03 case 4', ('SC03',))

    def test_every_case_field_is_exact(self):
        full = serial(('SC03', 'SC04', 'SC05'), 'IRQ')
        for old, new, why in (
                ('kill=PASS publication=NONE accounting=PASS', 'kill=PASS publication=SOME accounting=PASS',
                 'incomplete/out-of-order/inexact SC03 case 0'),
                ('SC03 CASE 9 op=read', 'SC03 CASE 9 op=open', 'incomplete/out-of-order/inexact SC03 case 9'),
                ('order=complete-first reuse=other', 'order=complete-first reuse=same',
                 'incomplete/out-of-order/inexact SC03 case 3'),
                ('retire=EXACT', 'retire=LATE', 'incomplete/out-of-order/inexact SC04 case 0'),
                ('fault=prefix-eio role=owner round=0 dest=UNCHANGED', 'fault=prefix-eio role=owner round=0 dest=EXACT',
                 'incomplete/out-of-order/inexact SC05 case 2'),
                ('fault=eintr role=waiter round=1 dest=UNCHANGED retry=PASS', 'fault=eintr role=waiter round=1 dest=UNCHANGED retry=SKIP',
                 'incomplete/out-of-order/inexact SC05 case 11'),
                ('SC05 PASS cases=12', 'SC05 PASS cases=11', 'missing/duplicate/incomplete group result')):
            with self.subTest(old):
                self.assertIn(old, full)
                self.bad(full.replace(old, new, 1), why)

    def test_missing_duplicate_reordered_cases(self):
        full = serial(('SC03', 'SC04', 'SC05'), 'IRQ').split('\r\n')
        line = next(i for i, x in enumerate(full) if x.startswith('storage-stress: SC03 CASE 17 '))
        self.bad('\r\n'.join(full[:line] + full[line + 1:]), 'incomplete/out-of-order/inexact SC03 case 17')
        self.bad('\r\n'.join(full[:line + 1] + full[line:]), 'incomplete/out-of-order/inexact SC03 case 18')
        swapped = full[:]
        a = next(i for i, x in enumerate(full) if x.startswith('storage-stress: SC05 CASE 3 '))
        swapped[a], swapped[a + 1] = swapped[a + 1], swapped[a]
        self.bad('\r\n'.join(swapped), 'incomplete/out-of-order/inexact SC05 case 3')
        cut = next(i for i, x in enumerate(full) if x.startswith('storage-stress: SC04 CASE 6 '))
        self.bad('\r\n'.join(full[:cut]), 'incomplete/out-of-order/inexact SC04 case 6')

    def test_group_order_and_selection_contract(self):
        self.bad(serial(('SC03', 'SC05'), 'IRQ'), 'missing/duplicate/out-of-order group begin', ('SC03', 'SC04', 'SC05'))
        self.bad(serial(('SC03', 'SC04'), 'IRQ'), 'missing/inexact explicit nonexecution record', ('SC03',))
        self.bad(serial(('SC04',), 'IRQ'), 'missing/duplicate/out-of-order group begin', ('SC03',))
        self.bad(serial(('SC03',), 'IRQ'), 'out-of-order selection', ('SC04', 'SC03'))

    def test_nonexecution_record_exact(self):
        text = serial(('SC03',), 'IRQ')
        self.bad(text.replace('SC01 SC02 SC04 SC05 SC10 SC11 NOT RUN', 'SC01 SC02 SC10 SC11 NOT RUN'),
                 'missing/inexact explicit nonexecution record', ('SC03',))
        self.bad(text.replace('storage-stress: SC01 SC02 SC04 SC05 SC10 SC11 NOT RUN\r\n', ''),
                 'missing/inexact explicit nonexecution record', ('SC03',))
        # Gate B's fixed record is not Gate C's.
        self.bad(serial(('SC03', 'SC04', 'SC05'), 'IRQ').replace('SC01 SC02 SC10 SC11 NOT RUN', 'SC03 SC04 SC05 SC10 SC11 NOT RUN'),
                 'missing/inexact explicit nonexecution record')

    def test_tail_and_verdict(self):
        text = serial(('SC05',), 'POLL')
        for old, new, why in (('CHECKS 1234 PASS', 'CHECKS 0 PASS', 'missing exact trusted assertion count'),
                              ('CHECKS 1234 PASS', 'CHECKS PASS', 'missing exact trusted assertion count'),
                              ('storage-stress: SELECTED PASS\r\n', 'storage-stress: SELECTED PASS\r\nstorage-stress: SC03 CASE 0\r\n',
                               'unexpected execution/duplicate/out-of-order tail'),
                              ('storage-stress: SELECTED PASS\r\n', 'storage-stress: SELECTED PASS\r\nstorage-stress: SELECTED PASS\r\n',
                               'unexpected execution/duplicate/out-of-order tail'),
                              ('VOS-TEST: PASS\r\n', '', 'missing/duplicate whole-boot verdict'),
                              ('VOS-TEST: PASS\r\n', 'VOS-TEST: PASS\r\nVOS-TEST: PASS\r\n', 'missing/duplicate whole-boot verdict')):
            with self.subTest(old=old, new=new):
                self.bad(text.replace(old, new), why, ('SC05',), 'POLL')
        early = 'VOS-TEST: PASS\r\n' + serial(('SC05',), 'POLL', tail=False)
        self.bad(early, 'premature whole-boot verdict', ('SC05',), 'POLL')
        self.bad(text, 'wrong whole-boot exit', ('SC05',), 'POLL', exit_status=35)

    def test_guest_failures(self):
        text = serial(('SC03',), 'IRQ')
        for line in ('storage-stress: FAIL: exact killed system call', 'panic: Gate C failed; architectural review required',
                     'check failed: N3.6 selected Gate C stress', 'VOS-TEST: FAIL', 'self-test FAILED'):
            with self.subTest(line):
                self.bad(text.replace('storage-stress: SC03 CASE 5 ', line + '\r\nstorage-stress: SC03 CASE 5 '),
                         'trusted guest failure', ('SC03',))

    def test_untrusted_user_lines_ignored_and_cannot_forge(self):
        text = serial(('SC03',), 'IRQ').replace('storage-stress: SC03 CASE 1 ',
                                                'user[7]: storage-stress: FAIL: forged\r\nstorage-stress: SC03 CASE 1 ')
        self.assertEqual(runner.validate_serial(text, ('SC03',), 'IRQ', 33)['SC03_cases'], 32)
        forged = serial(('SC03',), 'IRQ').replace('storage-stress: SC03 CASE 2 ', 'user[7]: storage-stress: SC03 CASE 2 ')
        self.bad(forged, 'incomplete/out-of-order/inexact SC03 case 2', ('SC03',))

    def test_transport_record_first(self):
        text = serial(('SC03',), 'IRQ').replace('storage-stress: TRANSPORT IRQ PASS\r\n', '')
        self.bad(text, 'missing/duplicate/wrong transport', ('SC03',))
        lines = serial(('SC03',), 'IRQ').split('\r\n')
        i = lines.index('storage-stress: TRANSPORT IRQ PASS')
        moved = lines[:i] + lines[i + 1:i + 2] + [lines[i]] + lines[i + 2:]
        self.bad('\r\n'.join(moved), 'transport/selection record order', ('SC03',))

    def test_watchdog_uses_transport_specific_result(self):
        self.assertEqual(runner.end('SC04', 'IRQ'), 'storage-stress: SC04 PASS applicable=12 not_applicable=0 accounting=PASS')
        self.assertEqual(runner.end('SC04', 'POLL'), 'storage-stress: SC04 PASS applicable=4 not_applicable=8 accounting=PASS')
        with self.assertRaisesRegex(RuntimeError, '^unknown transport$'):
            runner.end('SC04', 'MSI')

    def test_case_generators_match_kernel_loop_order(self):
        # SC05 loops round, fault, role; SC03 op, site, order, reuse; SC04 mix, order, round.
        sc05 = [line for _, line in runner.sc05_cases()]
        self.assertTrue(sc05[1].endswith('fault=line-fallback role=waiter round=0 dest=EXACT retry=PASS accounting=PASS'))
        self.assertTrue(sc05[6].startswith('storage-stress: SC05 CASE 6 fault=line-fallback role=owner round=1'))
        sc03 = [line for _, line in runner.sc03_cases('IRQ')]
        self.assertIn('CASE 31 op=enum site=reader order=complete-first reuse=other orphan=0', sc03[31])
        self.assertEqual(sum(f['orphan'] for f, _ in runner.sc03_cases('IRQ')), 8)
        self.assertEqual(sum(f['orphan'] for f, _ in runner.sc03_cases('POLL')), 0)


class KernelContract(TestCase):
    """The parser's lines are the ones storage_stress_c.c prints, in the kernel's loop order."""
    src = (HERE.parent / 'arch/x86_64/storage_stress_c.c').read_text()

    def test_record_literals(self):
        for literal in ('storage-stress: SC03 BEGIN seed=0x4E360003 cases=32\\r\\n',
                        'storage-stress: SC04 BEGIN seed=0x4E360004 cases=12\\r\\n',
                        'storage-stress: SC05 BEGIN seed=0x4E360005 cases=12\\r\\n',
                        'storage-stress: SC03 PASS cases=32 kills=32 publication=NONE accounting=PASS\\r\\n',
                        'storage-stress: SC05 PASS cases=12 accounting=PASS\\r\\n',
                        '"storage-stress: SC04 PASS applicable="', '" not_applicable="', '" accounting=PASS\\r\\n"',
                        '"storage-stress: SC03 CASE "', '" op="', '" site=device"', '" site=reader"',
                        '" order=kill-first"', '" order=complete-first"', '" reuse=same"', '" reuse=other"',
                        '" orphan=1"', '" orphan=0"', '" kill=PASS publication=NONE accounting=PASS\\r\\n"',
                        '"storage-stress: SC04 CASE "', '" mix="', '" order="', '" round="', '" orphans="',
                        '" retire=EXACT accounting=PASS\\r\\n"', '"storage-stress: SC05 CASE "', '" fault="',
                        '" role=owner"', '" role=waiter"', '" dest=EXACT"', '" dest=UNCHANGED"',
                        '" retry=PASS accounting=PASS\\r\\n"', '"storage-stress: TRANSPORT "', '" PASS\\r\\n"',
                        '"storage-stress: SC01 SC02"', '" SC03"', '" SC04"', '" SC05"', '" SC10 SC11 NOT RUN\\r\\n"',
                        '"storage-stress: CHECKS "', '"storage-stress: SELECTED PASS\\r\\n"',
                        '{"open","read","stat","enum"}', '{"AA","AO","OO"}', '{"line-fallback","prefix-eio","eintr"}',
                        '" NOT APPLICABLE (polling transport: a killed polling waiter leaves no orphan)\\r\\n"'):
            with self.subTest(literal):
                self.assertIn(literal, self.src)

    def test_loop_order_and_orphan_rule(self):
        self.assertIn('for(uint64_t op=OP_OPEN;op<=OP_ENUM;op++) for(unsigned s=0;s<2;s++) for(unsigned k=0;k<2;k++) '
                      'for(unsigned u=0;u<2;u++)\n        sc03_case(bi,n++,op,s,k,u);', self.src)
        self.assertIn('enum { SITE_DEVICE, SITE_READER };\nenum { KILL_FIRST, COMPLETE_FIRST };\nenum { REUSE_SAME, REUSE_OTHER };',
                      self.src)
        self.assertIn('for(unsigned mix=0;mix<3;mix++) for(unsigned order=0;order<2;order++) for(unsigned round=0;round<2;round++)',
                      self.src)
        self.assertIn('for(unsigned round=0;round<2;round++) for(unsigned fault=0;fault<3;fault++) '
                      'for(unsigned role=0;role<2;role++)', self.src)
        # The only orphan in SC03: an interrupt-mode kill of a request held at the device.
        self.assertEqual(self.src.count('orphan=1;'), 1)
        self.assertIn('if(order==KILL_FIRST) {\n        proc_kill(&procs[V],"Gate C SC03 kill-first");\n'
                      '        if(site==SITE_DEVICE && irq) {', self.src)
        # SC04 orphan mixes are skipped (and labelled) only under polling.
        self.assertIn('if(mix && vblk_info()->mode!=VBLK_MODE_IRQ) {', self.src)


if __name__ == '__main__':
    main()
