#!/usr/bin/env python3
"""Host test: Gate D selector and strict SC10/SC11 serial parser; no build, no media, no VOS execution.

Every negative case must fail for its intended reason (the exact require() message), not merely fail.
"""
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path
import sys
from unittest import TestCase, main

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
spec = spec_from_file_location('gated_runner', HERE / 'test-storage-stress-d.py')
runner = module_from_spec(spec)
spec.loader.exec_module(runner)

SUBSETS = [('SC10',), ('SC11',), ('SC10', 'SC11')]


def serial(groups, transport, checks=4321, tail=True):
    """A golden Gate D boot log, written out independently of the parser's own generators."""
    o = int(transport == 'IRQ')
    out = ['VOS x86_64 boot', 'virtio-blk device ready (ext2 image)', 'storage-stress: TRANSPORT %s PASS' % transport]
    if 'SC10' in groups:
        out.append('storage-stress: SC10 BEGIN seed=0x4E36000A cases=14')
        rows = [(8, 74, 74, 'OPEN'), (56, 506, 506, 'OPEN'), (57, 515, 512, 'EAGAIN'), (64, 578, 512, 'EAGAIN')]
        n = 0
        for d, cost, used, res in rows:
            for cache, reads in (('cold', 'DEVICE'), ('warm', 'NONE')):
                out.append('storage-stress: SC10 CASE %d kind=depth depth=%d cache=%s cost=%d consumed=%d result=%s '
                           'reads=%s accounting=PASS' % (n, d, cache, cost, used, res, reads))
                n += 1
        for k in (1, 2, 3):
            out.append('storage-stress: SC10 CASE %d kind=recovery cycle=%d exhausted=512 recovered=506 accounting=PASS' % (7 + k, k))
        out.append('process 9 killed: Gate D SC10 kill')
        out.append('storage-stress: SC10 CASE 11 kind=kill depth=56 orphan=%d kill=PASS publication=NONE accounting=PASS' % o)
        out.append('storage-stress: SC10 CASE 12 kind=kill depth=64 orphan=%d kill=PASS publication=NONE accounting=PASS' % o)
        out.append('storage-stress: SC10 CASE 13 kind=isolation exhausted=512 independent=74 accounting=PASS')
        out.append('storage-stress: SC10 PASS cases=14 accounting=PASS')
    if 'SC11' in groups:
        out.append('storage-stress: SC11 BEGIN seed=0x4E36000B calls=814 checks=4')
        for i, mode in enumerate(('cold', 'warm', 'reopened', 'independent'), 1):
            out.append('storage-stress: SC11 MANY pass=%d mode=%s entries=200 eof=1 calls=201 cost=1408 accounting=PASS' % (i, mode))
        probes = ['cookie=0 result=EAGAIN cost=1267 consumed=512', 'cookie=1024 result=EAGAIN cost=1261 consumed=512',
                  'cookie=351232 result=EAGAIN cost=513 consumed=512',
                  'cookie=352256 result=ENTRY cost=510 consumed=510 name=tail next=524288',
                  'cookie=522240 result=ENTRY cost=12 consumed=12 name=tail next=524288',
                  'cookie=523264 result=ENTRY cost=9 consumed=9 name=tail next=524288',
                  'cookie=524288 result=EOF cost=2 consumed=2', 'cookie=524289 result=EINVAL cost=2 consumed=2',
                  'cookie=523265 result=EINVAL cost=7 consumed=7', 'cookie=24 result=EINVAL cost=5 consumed=5']
        for i, p in enumerate(probes):
            out.append('storage-stress: SC11 PROBE %d %s accounting=PASS' % (i, p))
        out.append('storage-stress: SC11 ENUMERATION calls=814 entries=803 eof=5 eagain=3 einval=3 accounting=PASS')
        for i, (k, r) in enumerate((('closed-handle', 'EBADHANDLE'), ('file-handle', 'ENOTDIR'), ('record-size', 'EINVAL'))):
            out.append('storage-stress: SC11 CHECK %d kind=%s result=%s dest=UNCHANGED accounting=PASS' % (i, k, r))
        out.append('storage-stress: SC11 CHECK 3 kind=kill-held-probe orphan=%d kill=PASS publication=NONE accounting=PASS' % o)
        out.append('storage-stress: SC11 PASS calls=814 checks=4 accounting=PASS')
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
        for text, why in (('', 'unsupported, duplicate or empty group'), ('SC10,SC10', 'unsupported, duplicate or empty group'),
                          ('SC05', 'unsupported, duplicate or empty group'), ('SC03,SC10', 'unsupported, duplicate or empty group'),
                          ('sc10', 'unsupported, duplicate or empty group'), ('SC10, SC11', 'unsupported, duplicate or empty group'),
                          ('SC11,SC10', 'out-of-order selection')):
            with self.subTest(text), self.assertRaisesRegex(RuntimeError, '^' + why + '$'):
                runner.selection(text)

    def test_configuration_exact(self):
        modules, cfg = runner.configuration(('SC10', 'SC11'), 'UEFI')
        self.assertEqual(modules, [('hello.elf', 'hello'), ('stress.img', 'storage-stress.img'), ('gated.elf', 'stress-gated')])
        self.assertEqual(cfg, "set timeout=0\nset default=0\nmenuentry 'VOS N3.6 Gate D' {\n"
                              "  multiboot2 /boot/vos.bin n36-groups=SC10,SC11\n"
                              "  module2 /boot/hello.elf hello\n  module2 /boot/stress.img storage-stress.img\n"
                              "  module2 /boot/gated.elf stress-gated\n  boot\n}\n")


class Parser(TestCase):
    def bad(self, text, why, groups=('SC10', 'SC11'), transport='IRQ', exit_status=33):
        with self.assertRaisesRegex(RuntimeError, '^' + why + '$'):
            runner.validate_serial(text, groups, transport, exit_status)

    def test_golden_every_subset_and_transport(self):
        for groups in SUBSETS:
            for transport in ('IRQ', 'POLL'):
                with self.subTest(groups=groups, transport=transport):
                    s = runner.validate_serial(serial(groups, transport), groups, transport, 33)
                    o = int(transport == 'IRQ')
                    self.assertEqual((s['SC10_cases'], s['SC10_orphans']), (14, 2 * o) if 'SC10' in groups else (0, 0))
                    self.assertEqual((s['SC11_passes'], s['SC11_calls'], s['SC11_probes'], s['SC11_checks'], s['SC11_orphans']),
                                     (4, 814, 10, 4, o) if 'SC11' in groups else (0, 0, 0, 0, 0))
                    self.assertEqual(s['checks'], 4321)

    def test_budget_boundary_and_costs_are_exact(self):
        full = serial(('SC10', 'SC11'), 'IRQ')
        for old, new, why in (
                ('depth=56 cache=cold cost=506 consumed=506 result=OPEN', 'depth=56 cache=cold cost=506 consumed=505 result=OPEN',
                 'incomplete/out-of-order/inexact SC10 case 2'),
                ('depth=57 cache=cold cost=515 consumed=512 result=EAGAIN', 'depth=57 cache=cold cost=515 consumed=512 result=OPEN',
                 'incomplete/out-of-order/inexact SC10 case 4'),
                ('depth=8 cache=warm cost=74 consumed=74 result=OPEN reads=NONE', 'depth=8 cache=warm cost=74 consumed=74 result=OPEN reads=DEVICE',
                 'incomplete/out-of-order/inexact SC10 case 1'),
                ('cycle=2 exhausted=512 recovered=506', 'cycle=2 exhausted=512 recovered=505', 'incomplete/out-of-order/inexact SC10 case 9'),
                ('independent=74', 'independent=73', 'incomplete/out-of-order/inexact SC10 case 13'),
                ('cookie=352256 result=ENTRY cost=510 consumed=510', 'cookie=352256 result=EAGAIN cost=510 consumed=510',
                 'incomplete/out-of-order/inexact SC11 case probe 3'),
                ('cookie=351232 result=EAGAIN cost=513', 'cookie=351232 result=ENTRY cost=513', 'incomplete/out-of-order/inexact SC11 case probe 2'),
                ('cookie=523264 result=ENTRY cost=9 consumed=9 name=tail next=524288', 'cookie=523264 result=ENTRY cost=9 consumed=9 name=tail next=524289',
                 'incomplete/out-of-order/inexact SC11 case probe 5'),
                ('pass=2 mode=warm entries=200 eof=1 calls=201 cost=1408', 'pass=2 mode=warm entries=199 eof=1 calls=200 cost=1401',
                 'incomplete/out-of-order/inexact SC11 case pass 2'),
                ('calls=814 entries=803 eof=5', 'calls=813 entries=802 eof=5', 'incomplete/out-of-order/inexact SC11 case enumeration'),
                ('kind=file-handle result=ENOTDIR dest=UNCHANGED', 'kind=file-handle result=ENOTDIR dest=PARTIAL',
                 'incomplete/out-of-order/inexact SC11 case check 1')):
            with self.subTest(old):
                self.assertIn(old, full)
                self.bad(full.replace(old, new, 1), why)

    def test_transport_specific_orphans(self):
        self.bad(serial(('SC10',), 'POLL').replace('depth=56 orphan=0', 'depth=56 orphan=1'),
                 'incomplete/out-of-order/inexact SC10 case 11', ('SC10',), 'POLL')
        self.bad(serial(('SC11',), 'IRQ').replace('kill-held-probe orphan=1', 'kill-held-probe orphan=0'),
                 'incomplete/out-of-order/inexact SC11 case check 3', ('SC11',), 'IRQ')
        self.bad(serial(('SC10',), 'POLL'), 'missing/duplicate/wrong transport', ('SC10',), 'IRQ')

    def test_missing_duplicate_reordered(self):
        full = serial(('SC10', 'SC11'), 'IRQ').split('\r\n')
        i = next(k for k, x in enumerate(full) if x.startswith('storage-stress: SC11 PROBE 4 '))
        self.bad('\r\n'.join(full[:i] + full[i + 1:]), 'incomplete/out-of-order/inexact SC11 case probe 4')
        self.bad('\r\n'.join(full[:i + 1] + full[i:]), 'incomplete/out-of-order/inexact SC11 case probe 5')
        j = next(k for k, x in enumerate(full) if x.startswith('storage-stress: SC10 CASE 8 '))
        swapped = full[:]
        swapped[j], swapped[j + 1] = swapped[j + 1], swapped[j]
        self.bad('\r\n'.join(swapped), 'incomplete/out-of-order/inexact SC10 case 8')

    def test_selection_and_nonexecution_contract(self):
        self.bad(serial(('SC11',), 'IRQ'), 'missing/duplicate/out-of-order group begin', ('SC10', 'SC11'))
        self.bad(serial(('SC10', 'SC11'), 'IRQ'), 'missing/inexact explicit nonexecution record', ('SC10',))
        self.bad(serial(('SC10',), 'IRQ').replace('SC05 SC11 NOT RUN', 'SC05 NOT RUN'),
                 'missing/inexact explicit nonexecution record', ('SC10',))

    def test_tail_verdict_and_failures(self):
        text = serial(('SC11',), 'POLL')
        for old, new, why in (('CHECKS 4321 PASS', 'CHECKS 0 PASS', 'missing exact trusted assertion count'),
                              ('storage-stress: SELECTED PASS\r\n', 'storage-stress: SELECTED PASS\r\nstorage-stress: SELECTED PASS\r\n',
                               'unexpected execution/duplicate/out-of-order tail'),
                              ('VOS-TEST: PASS\r\n', '', 'missing/duplicate whole-boot verdict'),
                              ('storage-stress: SC11 CHECK 0 ', 'panic: Gate D failed\r\nstorage-stress: SC11 CHECK 0 ', 'trusted guest failure')):
            with self.subTest(new):
                self.bad(text.replace(old, new), why, ('SC11',), 'POLL')
        self.bad('VOS-TEST: PASS\r\n' + serial(('SC11',), 'POLL', tail=False), 'premature whole-boot verdict', ('SC11',), 'POLL')
        self.bad(text, 'wrong whole-boot exit', ('SC11',), 'POLL', exit_status=35)
        forged = text.replace('storage-stress: SC11 PROBE 0 ', 'user[7]: storage-stress: SC11 PROBE 0 ')
        self.bad(forged, 'incomplete/out-of-order/inexact SC11 case probe 0', ('SC11',), 'POLL')


class KernelContract(TestCase):
    """The parser's records are the ones storage_stress_d.c prints, from the same fixture-model constants."""
    src = (HERE.parent / 'arch/x86_64/storage_stress_d.c').read_text()

    def test_record_literals_and_constants(self):
        for literal in ('"storage-stress: SC10 BEGIN seed=0x4E36000A cases=14\\r\\n"', '"storage-stress: SC10 PASS cases=14 accounting=PASS\\r\\n"',
                        '"storage-stress: SC11 BEGIN seed=0x4E36000B calls=814 checks=4\\r\\n"',
                        '"storage-stress: SC11 PASS calls=814 checks=4 accounting=PASS\\r\\n"',
                        '"storage-stress: SC11 ENUMERATION calls=814 entries=803 eof=5 eagain=3 einval=3 accounting=PASS\\r\\n"',
                        'static const unsigned depths[4]={8,56,57,64};',
                        'static const uint64_t probes[10]={0,1024,351232,352256,522240,523264,524288,524289,523265,24};',
                        '{"closed-handle","file-handle","record-size"}', '{-VOS_EBADHANDLE,-FILE_ENOTDIR,-VOS_EINVAL}',
                        '{"EBADHANDLE","ENOTDIR","EINVAL"}', 'sc10_line(n,"kill");', 'sc10_line(n+k,"recovery");',
                        'sc10_line(n+w,"depth");', 'sc10_line(n,"isolation");', '" kill=PASS publication=NONE accounting=PASS\\r\\n"',
                        '" exhausted=512 independent=74 accounting=PASS\\r\\n"', '" entries=200 eof=1 calls=201 cost="',
                        'sc11_pass(V,1,"cold",1)', 'sc11_pass(V,2,"warm",0)', 'sc11_pass(V,3,"reopened",0)', 'sc11_pass(H,4,"independent",0)',
                        'require(want==2+9u*depth,', '" NOT RUN\\r\\n"'):
            with self.subTest(literal):
                self.assertIn(literal, self.src)

    def test_case_order(self):
        order = ['for(unsigned i=0;i<4;i++) sc10_depth(2*i,depths[i],bi,dev);', 'sc10_recovery(8,bi,dev);',
                 'sc10_kill(11,56,bi,dev);', 'sc10_kill(12,64,bi,dev);', 'sc10_isolation(13,bi,dev);']
        pos = [self.src.index(x) for x in order]
        self.assertEqual(pos, sorted(pos))


if __name__ == '__main__':
    main()
