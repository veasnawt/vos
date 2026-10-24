#!/usr/bin/env python3
"""Host test: the C20 larger-RAM wrapper; no build, no media, no VOS execution.

Checked: the exact 15-run matrix; that the QEMU command equals each accepted Gate B/C/D runner's own command except
for the guest memory arguments (captured by running the accepted run_profile against a stub); the sparse 8 GiB
backing matches scripts/exit-test.sh; the pmm memory record rules; that judgement is each gate's unchanged serial
contract (golden logs and their exact rejections); the family requirement; the sparse bound in the guard; and, end
to end against a stub supervisor, that a sparse guest file is measured, bounded and removed on success and failure.
"""
from importlib.util import module_from_spec, spec_from_file_location
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import resource
from types import SimpleNamespace
from unittest import TestCase, main
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))


def load(name, file):
    spec = spec_from_file_location(name, HERE / file)
    m = module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


ram = load('ram_wrapper', 'test-storage-stress-ram.py')
RUNNERS = {g: ram.gate_runner(g) for g in ram.GATES}
GOLDEN_C = load('gatec_parser_test', 'test-storage-stress-c-parser.py').serial
GOLDEN_D = load('gated_parser_test', 'test-storage-stress-d-parser.py').serial


def golden_b(groups, transport, checks=4321):
    """A golden Gate B boot log written from its record contract (SC01 epochs and CPU progress, SC02 cases)."""
    out = ['VOS x86_64 boot', 'storage-stress: TRANSPORT %s PASS' % transport]
    if 'SC01' in groups:
        out.append('storage-stress: SC01 BEGIN seed=0x4E360001 epochs=68 reads=136')
        for e in range(68):
            if e in (16, 33, 50, 67):
                out.append('storage-stress: SC01 CPU epoch=%d PASS' % e)
            out.append('storage-stress: SC01 EPOCH %d region=%d lead=%d reads=2 overlap=1 accounting=PASS' % (e, 5 * e % 17, e % 2))
        out.append('storage-stress: SC01 PASS epochs=68 reads=136 overlaps=68 cpu=4 ipc=68 accounting=PASS')
    if 'SC02' in groups:
        out.append('storage-stress: SC02 BEGIN seed=0x4E360002 cases=64')
        for c in range(64):
            out.append('storage-stress: SC02 CASE %d slot=%d grant=%d term=%d errors=PASS incarnation=PASS accounting=PASS'
                       % (c, c // 8, c // 4 % 2, c % 4))
        out.append('storage-stress: SC02 PASS cases=64 user=48 ready=16 slots=8 grants=2 accounting=PASS')
    out += ['storage-stress: SC03 SC04 SC05 SC10 SC11 NOT RUN', 'storage-stress: CHECKS %d PASS' % checks,
            'storage-stress: SELECTED PASS', 'VOS-TEST: PASS']
    return '\r\n'.join(out) + '\r\n'


GOLDEN = {'b': golden_b, 'c': GOLDEN_C, 'd': GOLDEN_D}
_FSIZE = resource.getrlimit(resource.RLIMIT_FSIZE)[0]
# The stub guest's sparse backing: 8 GiB apparent, or just under this process's own file-size limit (the storage guard
# runs suites with RLIMIT_FSIZE 64 MiB); only its allocation, never its apparent size, is bounded by the wrapper.
APPARENT = 8 << 30 if _FSIZE == resource.RLIM_INFINITY or _FSIZE > (8 << 30) else _FSIZE - (1 << 20)


def pmm_line(ignored):
    return 'pmm: 262144 frames tracked below 1 GiB, 28610 usable (114440 KiB), %d bytes of RAM above 1 GiB ignored' % ignored


def with_pmm(text, ignored):
    head, rest = text.split('\r\n', 1)
    return head + '\r\n' + pmm_line(ignored) + '\r\n' + rest


class Stop(Exception):
    pass


class FakeRoot:
    """A stub evidence root: admitted media, inputs and build environment, and a stub supervisor."""

    def __init__(self, gate, firmware):
        self.dir = Path(tempfile.mkdtemp(prefix='ram-wrapper-'))
        groups = ram.GATES[gate][1]
        key = firmware.lower() + '-' + '-'.join(groups)
        iso = self.dir / 'media' / key / 'vos.iso'
        iso.parent.mkdir(parents=True)
        iso.write_bytes(b'iso ' + key.encode())
        (self.dir / 'inputs').mkdir()
        for n in ('OVMF_VARS_4M.fd', 'OVMF_CODE_4M.fd', 'stress.img'):
            (self.dir / 'inputs' / n).write_bytes(n.encode())
        (self.dir / 'build-env.json').write_text('{}')
        (self.dir / 'preboot.json').write_text(json.dumps(dict(production_isolation=True, frozen_hashes=139,
                                                               sha256={'media/%s/vos.iso' % key: ram.digest(iso)})))
        self.argv = None

    def supervisor(self, runner, on_run):
        root = self

        class S:
            def __init__(self):
                self.root = root.dir
                self.runner = runner
                self.family = ram.FINAL

            def guard(self, extra=0):
                return {}

            def copy(self, a, b):
                shutil.copyfile(a, b)

            def run(self, label, argv, **kw):
                root.argv = argv
                root.kw = kw
                on_run(Path(root.dir) / label, argv)
        return S()

    def close(self):
        shutil.rmtree(self.dir)


def accepted_argv(gate, firmware, cpu):
    """The QEMU command the accepted, unchanged gate runner builds for this profile at 128 MiB."""
    runner = RUNNERS[gate]
    fr = FakeRoot(gate, firmware)
    try:
        def stop(out, argv):
            raise Stop
        s = fr.supervisor(runner, stop)
        with patch.object(runner, 'validate_iso', lambda *a, **k: None):
            try:
                runner.run_profile(s, ram.GATES[gate][1], firmware, cpu)
            except Stop:
                pass
        argv = fr.argv[3:]                                   # drop: timeout --kill-after=2 SECONDS
        return [a.replace(str(fr.dir), 'ROOT') for a in argv]
    finally:
        fr.close()


def wrapper_argv(firmware, cpu, memory):
    r = Path('ROOT')
    key = 'KEY'
    return ram.qemu_argv(r, r / 'media' / key / 'vos.iso', r / ('run-' + 'LABEL'), firmware, cpu, memory)


def strip_memory(argv):
    out, i = [], 0
    while i < len(argv):
        if argv[i] in ('-m', '-object') or (argv[i] == '-machine' and argv[i + 1] == 'memory-backend=ram'):
            i += 2
            continue
        out.append(argv[i])
        i += 1
    return out


def normalize(argv, gate, firmware, cpu, label_memory):
    groups = '-'.join(ram.GATES[gate][1])
    key = firmware.lower() + '-' + groups
    label = firmware.lower() + '-' + cpu + ('-' + label_memory if label_memory else '') + '-' + groups
    return [a.replace('media/' + key, 'media/KEY').replace('run-' + label, 'run-LABEL') for a in argv]


class Matrix(TestCase):
    def test_exact_fifteen_runs(self):
        self.assertEqual(ram.PROFILES, (('BIOS', 'default', '2G'), ('UEFI', 'default', '1G'), ('UEFI', 'max', '1G'),
                                        ('UEFI', 'default', '8G'), ('UEFI', 'max', '8G')))
        p = ram.plan()
        self.assertEqual(len(p), 15)
        self.assertEqual([x[0] for x in p], ['b'] * 5 + ['c'] * 5 + ['d'] * 5)
        self.assertEqual(ram.SPARSE, ('8G',))

    def test_profiles_outside_the_matrix_rejected(self):
        for prof in (('BIOS', 'max', '2G'), ('BIOS', 'default', '1G'), ('BIOS', 'default', '8G'), ('UEFI', 'default', '2G'),
                     ('UEFI', 'default', '128M'), ('UEFI', 'max', '512M')):
            with self.subTest(prof), self.assertRaisesRegex(RuntimeError, '^profile outside the C20 larger-RAM matrix$'):
                ram.admitted_profile(*prof)

    def test_gates_are_the_accepted_runners(self):
        for g, (file, groups) in ram.GATES.items():
            self.assertEqual(Path(RUNNERS[g].__file__).name, file)
            self.assertEqual(RUNNERS[g].selection(','.join(groups)), groups)


class Command(TestCase):
    def test_equals_accepted_command_apart_from_memory(self):
        for g in ram.GATES:
            for fw in ('BIOS', 'UEFI'):
                for cpu in ('default', 'max'):
                    with self.subTest(gate=g, firmware=fw, cpu=cpu):
                        acc = normalize(accepted_argv(g, fw, cpu), g, fw, cpu, None)
                        self.assertEqual(acc[acc.index('-m') + 1], '128M')
                        self.assertEqual(wrapper_argv(fw, cpu, '128M'), acc)        # identical at 128 MiB
                        for mem in ('1G', '2G', '8G'):
                            self.assertEqual(strip_memory(wrapper_argv(fw, cpu, mem)), strip_memory(acc))

    def test_memory_arguments(self):
        out = Path('/r/run-x')
        self.assertEqual(ram.memory_args('1G', out), ['-m', '1G'])
        self.assertEqual(ram.memory_args('2G', out), ['-m', '2G'])
        self.assertEqual(ram.memory_args('8G', out), ['-m', '8G', '-object',
                         'memory-backend-file,id=ram,size=8G,mem-path=/r/run-x/guest.ram,share=on', '-machine', 'memory-backend=ram'])

    def test_sparse_backing_is_exit_test_method(self):
        text = (HERE / 'exit-test.sh').read_text()
        self.assertIn('memory-backend-file,id=ram,size=$MEM,mem-path=$DIR/$NAME.ram,share=on', text)
        self.assertIn('-machine memory-backend=ram', text)


class Memory(TestCase):
    def tl(self):
        return RUNNERS['d'].trusted_lines

    def test_record_rules(self):
        for mem, ignored, ok in (('1G', 0, True), ('1G', 4096, False), ('2G', 0, False), ('2G', 1 << 30, True),
                                 ('8G', 0, False), ('8G', 7 << 30, True)):
            with self.subTest(mem=mem, ignored=ignored):
                text = 'boot\r\n' + pmm_line(ignored) + '\r\n'
                if ok:
                    self.assertEqual(ram.check_memory(text, self.tl(), mem), ignored)
                else:
                    with self.assertRaisesRegex(RuntimeError, '^guest memory not as configured'):
                        ram.check_memory(text, self.tl(), mem)

    def test_record_must_be_single_and_trusted(self):
        for text in ('boot\r\n', 'user[forge]: ' + pmm_line(0) + '\r\n',
                     pmm_line(0) + '\r\n' + pmm_line(0) + '\r\n', pmm_line(0)[:-3] + '\r\n'):
            with self.subTest(text[:30]), self.assertRaisesRegex(RuntimeError, '^missing/duplicate pmm memory record$'):
                ram.check_memory(text, self.tl(), '1G')


class Judgement(TestCase):
    def test_gate_contracts_unchanged(self):
        for g in ram.GATES:
            groups = ram.GATES[g][1]
            for transport, mem, ignored in (('IRQ', '2G', 1 << 30), ('POLL', '1G', 0), ('POLL', '8G', 7 << 30)):
                with self.subTest(gate=g, transport=transport, mem=mem):
                    text = with_pmm(GOLDEN[g](groups, transport), ignored)
                    summary, got = ram.judge(RUNNERS[g], text, groups, transport, mem)
                    self.assertEqual(summary, RUNNERS[g].validate_serial(text, groups, transport, 33))
                    self.assertEqual(got, ignored)

    def test_gate_rejections_unchanged(self):
        for g in ram.GATES:
            groups = ram.GATES[g][1]
            text = with_pmm(GOLDEN[g](groups, 'IRQ'), 1 << 30).replace('storage-stress: SELECTED PASS', 'storage-stress: SELECTED FAIL')
            with self.subTest(gate=g):
                with self.assertRaises(RuntimeError) as want:
                    RUNNERS[g].validate_serial(text, groups, 'IRQ', 33)
                with self.assertRaises(RuntimeError) as got:
                    ram.judge(RUNNERS[g], text, groups, 'IRQ', '2G')
                self.assertEqual(str(got.exception), str(want.exception))

    def test_family_required(self):
        ram.require_final(SimpleNamespace(family=ram.FINAL))
        for fam in ('n36-stage3-gated-*', 'n36-stage3-gatec-*', 'n36-stage3-gateb-*'):
            with self.subTest(fam), self.assertRaisesRegex(RuntimeError, 'requires an N3 final-campaign evidence root'):
                ram.require_final(SimpleNamespace(family=fam))


class Guard(TestCase):
    def test_sparse_bound_enforced_by_the_gate_guard(self):
        class Base:
            def __init__(self):
                self.ram = None

            def guard(self, extra=0):
                return {'ok': 1}
        g = ram.ram_guard(SimpleNamespace(RunGuard=Base))()
        tmp = Path(tempfile.mkdtemp(prefix='ram-guard-'))
        try:
            f = tmp / 'guest.ram'
            with f.open('wb') as h:
                h.truncate(APPARENT)
                h.write(b'x' * 4096)
            g.ram = f
            self.assertEqual(g.guard(), {'ok': 1})
            with patch.object(ram, 'RAM_ALLOCATION_LIMIT', 1):
                with self.assertRaisesRegex(RuntimeError, '^sparse guest RAM allocation bound$'):
                    g.guard()
        finally:
            shutil.rmtree(tmp)


class EndToEnd(TestCase):
    def run_stub(self, gate, firmware, cpu, memory, ignored, write_ram=4096, serial=None):
        runner = RUNNERS[gate]
        fr = FakeRoot(gate, firmware)
        groups = ram.GATES[gate][1]
        transport = ram.transport_of(firmware)

        def on_run(log, argv):
            out = Path(argv[argv.index('-serial') + 1][len('file:'):]).parent
            (out / 'serial.log').write_text(serial or with_pmm(GOLDEN[gate](groups, transport), ignored))
            (out / 'qemu.log').write_text('CPU Reset (CPU 0)\n')
            if memory in ram.SPARSE:
                with (out / 'guest.ram').open('wb') as h:
                    h.truncate(APPARENT)
                    h.write(b'x' * write_ram)
        s = fr.supervisor(runner, on_run)
        out = fr.dir / ('run-' + firmware.lower() + '-' + cpu + '-' + memory + '-' + '-'.join(groups))
        return fr, s, out

    def test_passes_and_removes_sparse_ram(self):
        for gate in ram.GATES:
            with self.subTest(gate=gate):
                fr, s, out = self.run_stub(gate, 'UEFI', 'max', '8G', 7 << 30)
                try:
                    with patch.object(ram, 'validate_iso', lambda *a, **k: None):
                        res = ram.run_profile(s, gate, 'UEFI', 'max', '8G')
                    self.assertTrue(res['passed'])
                    self.assertEqual(res['ignored_bytes_above_1gib'], 7 << 30)
                    self.assertEqual(res['guest_ram_apparent'], APPARENT)
                    self.assertLess(res['guest_ram_allocated'], 1 << 20)
                    self.assertTrue(res['guest_ram_removed'])
                    self.assertFalse((out / 'guest.ram').exists())
                    self.assertEqual(json.loads((out / 'result.json').read_text())['memory'], '8G')
                finally:
                    fr.close()

    def test_sparse_runs_lift_only_the_file_size_limit(self):
        """QEMU creates the 8 GiB backing file under the guard's RLIMIT_FSIZE, so a sparse run needs a file-size limit above
        8 GiB (allocation stays bounded separately); every other run keeps the guard's default limit."""
        self.assertGreater(ram.SPARSE_FILE_LIMIT, 8 << 30)
        fr, s, out = self.run_stub('c', 'UEFI', 'default', '8G', 7 << 30)
        try:
            with patch.object(ram, 'validate_iso', lambda *a, **k: None):
                ram.run_profile(s, 'c', 'UEFI', 'default', '8G')
            self.assertEqual(fr.kw.get('file_limit'), ram.SPARSE_FILE_LIMIT)
        finally:
            fr.close()
        for fw, cpu, mem in (('BIOS', 'default', '2G'), ('UEFI', 'max', '1G')):
            fr, s, out = self.run_stub('c', fw, cpu, mem, 0 if mem == '1G' else 1 << 30)
            try:
                with patch.object(ram, 'validate_iso', lambda *a, **k: None):
                    ram.run_profile(s, 'c', fw, cpu, mem)
                self.assertNotIn('file_limit', fr.kw)
            finally:
                fr.close()

    def test_bios_2g_has_no_sparse_backing(self):
        fr, s, out = self.run_stub('b', 'BIOS', 'default', '2G', 1 << 30)
        try:
            res = ram.run_profile(s, 'b', 'BIOS', 'default', '2G')
            self.assertTrue(res['passed'])
            self.assertNotIn('-object', res['argv'])
            self.assertNotIn('guest_ram_removed', res)
        finally:
            fr.close()

    def test_failures_still_remove_sparse_ram(self):
        cases = {'allocation over the bound': dict(write_ram=1 << 20, limit=4096, why='^sparse guest RAM allocation bound$'),
                 'memory record wrong': dict(ignored=0, why='^guest memory not as configured'),
                 'gate contract broken': dict(serial_edit=True, why='unexpected execution/duplicate/out-of-order tail')}
        for name, c in cases.items():
            with self.subTest(name):
                serial = None
                if c.get('serial_edit'):
                    serial = with_pmm(GOLDEN['d'](('SC10', 'SC11'), 'POLL'), 7 << 30).replace(
                        'storage-stress: SELECTED PASS\r\n', 'storage-stress: SELECTED PASS\r\nstorage-stress: SELECTED PASS\r\n')
                fr, s, out = self.run_stub('d', 'UEFI', 'default', '8G', c.get('ignored', 7 << 30),
                                           write_ram=c.get('write_ram', 4096), serial=serial)
                try:
                    with patch.object(ram, 'validate_iso', lambda *a, **k: None), \
                            patch.object(ram, 'RAM_ALLOCATION_LIMIT', c.get('limit', ram.RAM_ALLOCATION_LIMIT)):
                        with self.assertRaisesRegex(RuntimeError, c['why']):
                            ram.run_profile(s, 'd', 'UEFI', 'default', '8G')
                    res = json.loads((out / 'result.json').read_text())
                    self.assertFalse(res['passed'])
                    self.assertTrue(res['guest_ram_removed'])
                    self.assertFalse((out / 'guest.ram').exists())
                finally:
                    fr.close()


if __name__ == '__main__':
    main()
