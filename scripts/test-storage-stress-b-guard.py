#!/usr/bin/env python3
"""Gate B session-admission guard: fail-closed host tests on a fake /proc; no VOS execution."""
from contextlib import ExitStack
import hashlib
import inspect
import json
import os
from pathlib import Path
import shutil
from types import SimpleNamespace
import subprocess
import tempfile
from unittest import TestCase, main
from unittest.mock import patch
import storage_stress_b_host as g

CTL = '/home/veasnawt/.claude/remote/ccd-cli/2.1.304-test'
BASH, TIMEOUT = '/usr/bin/bash', '/usr/lib/cargo/bin/coreutils/timeout'
START_MONO, START_REAL = 1000.0, 1700000000.0


class Admission(TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='gateb-guard-'))
        self.root = self.tmp / 'evidence'
        self.root.mkdir()
        self.state = dict(label='test', limit_bytes=g.LIMIT, evidence_copy=str(self.tmp / 'copy.tar.gz'),
                          boot_id='b-1', session_pid=1920, session_start_ticks='161210',
                          started_monotonic=START_MONO, started_real=START_REAL)
        (self.root / 'state.json').write_text(json.dumps(self.state, indent=2) + '\n')
        (self.root / 'baseline-source.json').write_text('{}')
        self.proc = self.tmp / 'proc'
        (self.proc / 'sys/kernel/random').mkdir(parents=True)
        (self.proc / 'sys/kernel/random/boot_id').write_text('b-1\n')
        self.process(1, 0, '182', None, 'systemd')
        self.process(2122, 1, '200', '/init', 'init')
        self.process(557, 1, '945', '/home/veasnawt/.claude/remote/srv/x/server', 'server')
        self.process(4000, 557, '5000', CTL, '2.1.304-test')
        self.process(4100, 4000, '6000', BASH, 'bash')
        self.process(4200, 4100, '6100', TIMEOUT, 'timeout')
        self.process(4300, 4200, '6200', BASH, 'bash')
        self.process(4400, 1, '6300', BASH, 'bash')
        self.process(4500, 2122, '6400', BASH, 'bash')
        self.journal = []
        self.argv = []
        self.mono = 2000.0
        self.used = None
        self.sizes = {}
        self.host = dict(C=50 * g.GIB, E=100 * g.GIB)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def process(self, pid, ppid, ticks, exe, comm):
        d = self.proc / str(pid)
        if d.exists():
            shutil.rmtree(d)
        d.mkdir()
        (d / 'stat').write_text('%d (%s) S %d %s %s 0 0 0\n' % (pid, comm, ppid, ' '.join(['0'] * 17), ticks))
        if exe:
            os.symlink(exe, d / 'exe')

    def run_fake(self, argv, **kw):
        self.argv.append(argv)
        out = ''.join(json.dumps(dict(MESSAGE=m)) + '\n' for m in self.journal)
        return subprocess.CompletedProcess(argv, 0, out, '')

    def env(self, ppid):
        stack = ExitStack()
        stack.enter_context(patch.object(g, 'PROC', self.proc))
        stack.enter_context(patch.object(g, 'REPO', self.tmp))
        stack.enter_context(patch.object(g, 'host_free', side_effect=lambda: dict(self.host)))
        stack.enter_context(patch.object(g.os, 'getppid', return_value=ppid))
        stack.enter_context(patch.object(g.time, 'monotonic', side_effect=lambda: self.mono))
        stack.enter_context(patch.object(g.subprocess, 'run', side_effect=self.run_fake))
        stack.enter_context(patch.object(g.subprocess, 'check_output', return_value=b''))
        stack.enter_context(patch.object(g, 'EVIDENCE', self.tmp / 'vos-evidence'))
        if getattr(self, 'linux', None) is not None:   # final-campaign tests only: deterministic Linux free space
            stack.enter_context(patch.object(g.os, 'statvfs', return_value=SimpleNamespace(f_bavail=self.linux, f_frsize=1)))
        if self.used is not None or self.sizes:
            stack.enter_context(patch.object(g, 'allocated', side_effect=lambda p: self.used or 0 if Path(p) == self.root
                                             else self.sizes.get(Path(p).name, 0)))
        return stack

    def guard(self, ppid, extra=0):
        with self.env(ppid):
            return g.Guard(self.root).guard(extra)

    def admit(self, ppid, reason='handoff test'):
        with self.env(ppid):
            return g.Guard(self.root).admit('claude-code', reason)

    def admissions(self):
        return (self.root / g.ADMISSIONS).read_bytes()

    def samples(self):
        p = self.root / 'storage-samples.jsonl'
        return p.read_bytes() if p.exists() else b''

    def rejects(self, ppid, why=None):
        before = self.samples()
        with self.assertRaises(RuntimeError) as e:
            self.guard(ppid)
        if why:
            self.assertIn(why, str(e.exception))
        self.assertEqual(self.samples(), before, 'rejected guard must not record a sample')

    def rewrite(self, lines):
        (self.root / g.ADMISSIONS).write_bytes(b''.join(json.dumps(r).encode() + b'\n' for r in lines))

    # Positive: the admitted controller, its child, verified shell layers.
    def test_admitted_controller_direct(self):
        row = self.admit(4100)
        self.assertEqual((row['seq'], row['session_pid'], row['session_start_ticks'], row['session_exe']), (1, 4000, '5000', CTL))
        self.assertEqual(row['supersedes'], '0:1920:161210')
        self.assertEqual(self.guard(4000)['session_pid'], 4000)

    def test_child_through_shell_layer(self):
        self.admit(4100)
        self.assertEqual(self.guard(4100)['admission'], 1)

    def test_two_verified_layers(self):
        self.admit(4100)
        self.assertEqual(self.guard(4200)['session_pid'], 4000)

    def test_three_layers_rejected(self):
        self.admit(4100)
        self.rejects(4300, 'beyond verified shell layers')

    # Negative: ancestry.
    def test_unrelated_processes_rejected(self):
        self.admit(4100)
        self.rejects(4400, 'session lost')     # orphan shell reparented to init
        self.rejects(4500, 'session lost')     # another terminal's shell
        self.rejects(557, 'session lost')      # controller's own parent, not the admitted process
        self.rejects(1, 'session lost')

    def test_old_session_alive_blocks_transfer(self):
        self.process(1920, 1, '161210', BASH, 'bash')
        with self.assertRaises(RuntimeError) as e:
            self.admit(4100)
        self.assertIn('previous controlling session still alive', str(e.exception))
        self.assertFalse((self.root / g.ADMISSIONS).exists())

    def test_old_pid_reused_by_other_process_is_dead(self):
        self.process(1920, 1, '999999', BASH, 'bash')
        self.assertEqual(self.admit(4100)['seq'], 1)

    def test_admitted_session_alive_blocks_second_and_readmission(self):
        self.admit(4100)
        with self.assertRaises(RuntimeError):
            self.admit(4100)
        self.process(4600, 557, '8000', CTL, '2.1.304-test')
        self.process(4700, 4600, '8100', BASH, 'bash')
        with self.assertRaises(RuntimeError) as e:
            self.admit(4700)
        self.assertIn('previous controlling session still alive', str(e.exception))
        self.assertEqual(len(self.admissions().splitlines()), 1)

    def test_dead_controller_cannot_authorize(self):
        self.admit(4100)
        shutil.rmtree(self.proc / '4000')
        self.rejects(4100, 'ancestry unreadable')
        self.process(4100, 1, '6000', BASH, 'bash')   # orphaned shell
        self.rejects(4100, 'session lost')

    def test_pid_reuse_with_different_ticks_or_exe(self):
        self.admit(4100)
        self.process(4000, 557, '7777', CTL, '2.1.304-test')
        self.rejects(4100, 'session lost')
        self.process(4000, 557, '5000', CTL + '-other', 'x')
        self.rejects(4100, 'session lost')

    def test_non_agent_controller_rejected(self):
        for ppid in (4400, 4500):
            with self.assertRaises(RuntimeError) as e:
                self.admit(ppid)
            self.assertIn('not the named agent', str(e.exception))
        self.assertFalse((self.root / g.ADMISSIONS).exists())

    def test_no_caller_supplied_identity(self):
        self.assertEqual(list(inspect.signature(g.Guard.admit).parameters), ['self', 'agent', 'reason'])
        with self.assertRaises(RuntimeError):
            with self.env(4100):
                g.Guard(self.root).admit('codex', 'unknown agent')

    def test_wrong_boot_rejected(self):
        (self.proc / 'sys/kernel/random/boot_id').write_text('b-2\n')
        with self.assertRaises(RuntimeError):
            self.admit(4100)
        (self.proc / 'sys/kernel/random/boot_id').write_text('b-1\n')
        self.admit(4100)
        (self.proc / 'sys/kernel/random/boot_id').write_text('b-2\n')
        self.rejects(4100, 'boot ID changed')

    # Time: the original 10,000-second bound is never restarted.
    def test_expired_original_deadline(self):
        self.mono = START_MONO + 10000
        with self.assertRaises(RuntimeError):
            self.admit(4100)
        self.assertFalse((self.root / g.ADMISSIONS).exists())

    def test_new_admission_does_not_restart_deadline(self):
        self.mono = START_MONO + 9000
        row = self.admit(4100)
        self.assertEqual(row['started_monotonic'], START_MONO + 9000)
        self.mono = START_MONO + 9999
        self.guard(4100)
        self.mono = START_MONO + 10000
        self.rejects(4100, 'bounded execution session')

    def test_execution_window_not_reset(self):
        window = self.root / 'execution-window.json'
        window.write_text(json.dumps(dict(started_monotonic=2000.0, utc=0)) + '\n')
        before = window.read_bytes()
        self.mono = 2000.0 + 5399
        self.admit(4100)
        self.assertEqual(window.read_bytes(), before)
        self.mono = 2000.0 + 5400
        self.rejects(4100, '90-minute execution window')

    # Journal: scan still starts at the original Gate B admission.
    def test_original_journal_coverage(self):
        self.mono = 5000.0
        self.admit(4100)
        self.assertTrue(self.argv and all('--since=@' + str(START_REAL) in a for a in self.argv))
        self.journal = ['Reached target shutdown.target - System Shutdown.']
        self.rejects(4100, 'shutdown/filesystem warning')

    # Storage: cumulative evidence accounting, no reset by admission.
    def test_cumulative_storage_preserved(self):
        self.used = 100 * g.MIB
        row = self.admit(4100)
        sample = json.loads(self.samples().splitlines()[-1])
        self.assertEqual((sample['allocated'], sample['charged'], sample['admission']), (100 * g.MIB, 102 * g.MIB, row['seq']))
        self.used = g.LIMIT - 2 * g.MIB + 1
        self.rejects(4100, 'cumulative allocation')

    def test_admission_refused_without_storage_headroom(self):
        self.used = g.LIMIT - 2 * g.MIB - 1024
        with self.assertRaises(RuntimeError) as e:
            self.admit(4100)
        self.assertIn('cumulative allocation', str(e.exception))
        self.assertFalse((self.root / g.ADMISSIONS).exists())

    # Records: state.json byte-identical, prior records preserved, any defect fails closed.
    def test_state_and_prior_records_preserved(self):
        state = (self.root / 'state.json').read_bytes()
        self.admit(4100)
        first = self.admissions()
        shutil.rmtree(self.proc / '4000')
        self.process(4600, 557, '8000', CTL, '2.1.304-test')
        self.process(4700, 4600, '8100', BASH, 'bash')
        self.mono = 3000.0
        row = self.admit(4700)
        self.assertTrue(self.admissions().startswith(first))
        self.assertEqual((row['seq'], row['supersedes']), (2, '1:4000:5000'))
        self.assertEqual((self.root / 'state.json').read_bytes(), state)
        self.guard(4700)
        self.process(4000, 557, '5000', CTL, '2.1.304-test')   # an older admitted identity is no longer current
        self.rejects(4100, 'session lost')

    def test_missing_records_no_bypass(self):
        self.rejects(4100, 'session lost')
        self.admit(4100)
        (self.root / g.ADMISSIONS).unlink()
        self.rejects(4100, 'session lost')

    def test_original_session_semantics_unchanged(self):
        self.process(1920, 1, '161210', BASH, 'bash')
        self.assertEqual(self.guard(1920)['session_pid'], 1920)
        self.rejects(4100, 'session lost')

    def test_malformed_records_fail_closed(self):
        self.admit(4100)
        good = self.admissions()
        rec = json.loads(good)
        def variant(**change):
            r = dict(rec)
            for k, v in change.items():
                if v is KeyError:
                    del r[k]
                else:
                    r[k] = v
            return [r]
        cases = {
            'no trailing newline': good[:-1],
            'empty file': b'',
            'garbage line': good + b'{not json\n',
            'blank line': good + b'\n',
            'duplicate record': good + good,
            'list not object': b'[1]\n',
        }
        for name, data in cases.items():
            with self.subTest(name):
                (self.root / g.ADMISSIONS).write_bytes(data)
                self.rejects(4100)
        lines = {
            'missing field': variant(reason=KeyError),
            'extra field': variant(extra=1),
            'bool sequence': variant(seq=True),
            'sequence gap': variant(seq=2),
            'wrong boot': variant(boot_id='b-2'),
            'wrong state hash': variant(state_sha256='0' * 64),
            'wrong predecessor': variant(supersedes='0:1920:1'),
            'wrong prev hash': variant(prev_sha256='0' * 64),
            'unknown agent': variant(agent='codex'),
            'agent exe mismatch': variant(session_exe='/usr/bin/bash'),
            'string pid': variant(session_pid='4000'),
            'nondigit ticks': variant(session_start_ticks='5000x'),
            'empty reason': variant(reason=' '),
            'time before original': variant(started_monotonic=START_MONO - 1),
            'nonfinite time': variant(started_real=float('inf')),
            'original session reused': variant(session_pid=1920, session_start_ticks='161210'),
        }
        for name, rows in lines.items():
            with self.subTest(name):
                self.rewrite(rows)
                self.rejects(4100)
        (self.root / g.ADMISSIONS).write_bytes(good)
        self.guard(4100)

    def test_out_of_order_and_tampered_history(self):
        self.admit(4100)
        shutil.rmtree(self.proc / '4000')
        self.process(4600, 557, '8000', CTL, '2.1.304-test')
        self.process(4700, 4600, '8100', BASH, 'bash')
        self.mono = 3000.0
        self.admit(4700)
        r1, r2 = [json.loads(x) for x in self.admissions().splitlines()]
        good = self.admissions()
        for name, rows in {
            'swapped order': [r2, r1],
            'second earlier in time': [r1, dict(r2, started_monotonic=r1['started_monotonic'] - 1)],
            'first record edited': [dict(r1, reason='edited'), r2],
            'duplicate session': [r1, dict(r2, session_pid=4000, session_start_ticks='5000')],
        }.items():
            with self.subTest(name):
                self.rewrite(rows)
                self.rejects(4700)
        (self.root / g.ADMISSIONS).write_bytes(good)
        self.guard(4700)

    def test_state_change_after_admission_fails_closed(self):
        self.admit(4100)
        with (self.root / 'state.json').open('a') as f:
            f.write(' ')
        self.rejects(4100, 'boot/state mismatch')


    # Storage extension v1: 384 MiB only with a cumulative carry of every other Gate B root and archive.
    def authorize(self, limit, **fields):
        rec = dict(version=g.VERSIONS.get(limit, 'none'), limit_bytes=limit, previous_limit_bytes=g.LIMIT)
        rec.update(fields)
        path = self.root / 'storage-authorization.json'
        path.write_text(json.dumps(rec) + '\n')
        return hashlib.sha256(path.read_bytes()).hexdigest()

    def restate(self, auth=True, **change):
        limit = change.get('limit_bytes', g.LIMIT)
        if auth and limit != g.LIMIT and 'storage_authorization_sha256' not in change:
            change['storage_authorization_sha256'] = self.authorize(limit)
        (self.root / 'state.json').write_text(json.dumps(dict(self.state, **change), indent=2) + '\n')

    def prior(self):
        ev = self.tmp / 'vos-evidence'
        roots = [ev / 'n36-stage3-gateb-old', ev / 'n36-stage3-gateb-rerun1-x']
        for d in roots:
            d.mkdir(parents=True)
        archive = self.tmp / 'n36-stage3-gateb-old.tar.gz'
        archive.write_bytes(b'x')
        self.sizes = {'n36-stage3-gateb-old': 150 * g.MIB, 'n36-stage3-gateb-rerun1-x': 80 * g.MIB,
                      'n36-stage3-gateb-old.tar.gz': 20 * g.MIB}
        return [str(p) for p in roots + [archive]]

    def test_unknown_allowance_rejected(self):
        self.restate(limit_bytes=300 * g.MIB, carried=[])
        with self.assertRaises(RuntimeError):
            g.Guard(self.root)

    def test_extension_requires_carry(self):
        self.restate(limit_bytes=384 * g.MIB)
        with self.assertRaises(RuntimeError) as e:
            g.Guard(self.root)
        self.assertIn('without cumulative carry', str(e.exception))

    def test_extension_is_cumulative(self):
        self.restate(limit_bytes=384 * g.MIB, carried=self.prior())
        self.used = 100 * g.MIB
        self.admit(4100)
        row = json.loads(self.samples().splitlines()[-1])
        self.assertEqual((row['carried'], row['charged'], row['limit']), (250 * g.MIB, 352 * g.MIB, 384 * g.MIB))
        self.used = 132 * g.MIB + 1                 # 132 + 250 + 2 MiB margin > 384 MiB
        self.rejects(4100, 'cumulative allocation')

    def test_extension_rejects_unadmitted_root(self):
        self.restate(limit_bytes=384 * g.MIB, carried=self.prior())
        self.admit(4100)
        (self.tmp / 'vos-evidence' / 'n36-stage3-gateb-new').mkdir()
        self.rejects(4100, 'differs from the admitted carry')

    def test_extension_rejects_missing_carried(self):
        self.restate(limit_bytes=384 * g.MIB, carried=self.prior() + [str(self.tmp / 'vos-evidence/n36-stage3-gateb-gone')])
        with self.assertRaises(RuntimeError) as e:
            self.admit(4100)
        self.assertIn('differs from the admitted carry', str(e.exception))
        self.assertFalse((self.root / g.ADMISSIONS).exists())

    def test_own_root_and_archive_not_carried(self):
        (self.tmp / 'evidence-copy.tar.gz').write_bytes(b'x')
        self.restate(limit_bytes=384 * g.MIB, carried=[], evidence_copy=str(self.tmp / 'evidence-copy.tar.gz'))
        with patch.object(g, 'GATEB', 'evidence*'):
            self.admit(4100)
            self.assertEqual(json.loads(self.samples().splitlines()[-1])['carried'], 0)

    def test_original_allowance_unchanged(self):
        self.prior()
        self.used = 10 * g.MIB
        self.admit(4100)
        row = json.loads(self.samples().splitlines()[-1])
        self.assertEqual((row['carried'], row['limit']), (0, g.LIMIT))
        self.used = g.LIMIT - 2 * g.MIB + 1
        self.rejects(4100, 'cumulative allocation')


    # Storage extension v2: 448 MiB cumulative, versioned authorization record.
    def test_448_is_cumulative(self):
        self.restate(limit_bytes=448 * g.MIB, carried=self.prior())
        self.used = 150 * g.MIB
        self.admit(4100)
        row = json.loads(self.samples().splitlines()[-1])
        self.assertEqual((row['carried'], row['charged'], row['limit']), (250 * g.MIB, 402 * g.MIB, 448 * g.MIB))
        self.used = 196 * g.MIB                      # 196 + 250 + 2 MiB margin = exactly 448 MiB
        self.assertEqual(self.guard(4100)['charged'], 448 * g.MIB)
        self.used = 196 * g.MIB + 1
        self.rejects(4100, 'cumulative allocation')

    def test_448_admission_extra_counts(self):
        self.restate(limit_bytes=448 * g.MIB, carried=self.prior())
        self.used = 100 * g.MIB
        self.admit(4100)
        with self.assertRaises(RuntimeError):
            self.guard(4100, extra=97 * g.MIB)       # 100 + 250 + 2 + 97 > 448
        self.guard(4100, extra=96 * g.MIB)

    def test_unsupported_allowances_rejected(self):
        carried = self.prior()
        for limit in (0, 300 * g.MIB, 448 * g.MIB + 1, 512 * g.MIB, 1 << 40):
            with self.subTest(limit):
                self.restate(limit_bytes=limit, carried=carried)
                with self.assertRaises(RuntimeError) as e:
                    g.Guard(self.root)
                self.assertIn('wrong Gate B allowance', str(e.exception))

    def test_invalid_authorization_rejected(self):
        carried = self.prior()
        bad = {'v1 version for 448': dict(version=g.VERSIONS[384 * g.MIB]),
               'unknown version': dict(version='N3.6 Gate B storage extension v3'),
               'limit mismatch': dict(limit_bytes=384 * g.MIB),
               'previous not lower': dict(previous_limit_bytes=448 * g.MIB),
               'previous not an allowance': dict(previous_limit_bytes=300 * g.MIB),
               'previous not an integer': dict(previous_limit_bytes=True)}
        for name, fields in bad.items():
            with self.subTest(name):
                self.restate(limit_bytes=448 * g.MIB, carried=carried,
                             storage_authorization_sha256=self.authorize(448 * g.MIB, **fields))
                with self.assertRaises(RuntimeError) as e:
                    g.Guard(self.root)
                self.assertIn('invalid storage authorization version', str(e.exception))
        with self.subTest('record edited after admission'):
            self.restate(limit_bytes=448 * g.MIB, carried=carried)
            with (self.root / 'storage-authorization.json').open('a') as f:
                f.write(' ')
            with self.assertRaises(RuntimeError) as e:
                g.Guard(self.root)
            self.assertIn('without its authorization record', str(e.exception))
        with self.subTest('record missing'):
            self.restate(limit_bytes=448 * g.MIB, carried=carried)
            (self.root / 'storage-authorization.json').unlink()
            with self.assertRaises(RuntimeError):
                g.Guard(self.root)
        with self.subTest('record not an object'):
            p = self.root / 'storage-authorization.json'
            p.write_text('[1]\n')
            self.restate(auth=False, limit_bytes=448 * g.MIB, carried=carried,
                         storage_authorization_sha256=hashlib.sha256(p.read_bytes()).hexdigest())
            with self.assertRaises(RuntimeError):
                g.Guard(self.root)

    def test_448_carried_set_exact(self):
        self.restate(limit_bytes=448 * g.MIB, carried=self.prior())
        self.admit(4100)
        (self.tmp / 'vos-evidence' / 'n36-stage3-gateb-new').mkdir()
        self.rejects(4100, 'differs from the admitted carry')
        (self.tmp / 'vos-evidence' / 'n36-stage3-gateb-new').rmdir()
        shutil.rmtree(self.tmp / 'vos-evidence' / 'n36-stage3-gateb-rerun1-x')
        self.rejects(4100, 'differs from the admitted carry')

    def test_448_reserves_enforced(self):
        self.restate(limit_bytes=448 * g.MIB, carried=self.prior())
        self.used = 10 * g.MIB
        self.admit(4100)
        remaining = 448 * g.MIB - (10 + 250 + 2) * g.MIB
        self.host = dict(C=10 * g.GIB + remaining + 256 * g.MIB - 1, E=100 * g.GIB)
        self.rejects(4100, 'C reserve')
        self.host = dict(C=50 * g.GIB, E=256 * g.MIB - 1)
        self.rejects(4100, 'E reserve')
        self.host = dict(C=10 * g.GIB + remaining + 256 * g.MIB, E=256 * g.MIB)
        self.guard(4100)

    def test_384_records_still_valid(self):
        self.restate(limit_bytes=384 * g.MIB, carried=self.prior())
        self.used = 10 * g.MIB
        self.admit(4100)
        self.assertEqual(json.loads(self.samples().splitlines()[-1])['limit'], 384 * g.MIB)


    # Gate C family (proposed): separate cumulative allowance, versioned record, no cross-family carry.
    def gatec_root(self, name='n36-stage3-gatec-x'):
        ev = self.tmp / 'vos-evidence'
        ev.mkdir(exist_ok=True)
        r = ev / name
        shutil.copytree(self.root, r)
        self.root = r
        return r

    def gatec_state(self, carried, previous=0, version=None, limit=256 * g.MIB):
        p = self.root / 'storage-authorization.json'
        p.write_text(json.dumps(dict(version=version or g.GATEC_VERSIONS.get(limit, 'none'), limit_bytes=limit,
                                     previous_limit_bytes=previous)) + '\n')
        st = dict(self.state, limit_bytes=limit, carried=carried, evidence_copy=str(self.tmp / (self.root.name + '.tar.gz')),
                  storage_authorization_sha256=hashlib.sha256(p.read_bytes()).hexdigest())
        (self.root / 'state.json').write_text(json.dumps(st, indent=2) + '\n')

    def test_gatec_admitted_and_cumulative(self):
        self.prior()                                  # Gate B roots exist; they must not count towards Gate C
        self.gatec_root()
        self.gatec_state([])
        self.used = 100 * g.MIB
        self.admit(4100)
        row = json.loads(self.samples().splitlines()[-1])
        self.assertEqual((row['carried'], row['charged'], row['limit']), (0, 102 * g.MIB, 256 * g.MIB))
        self.used = 254 * g.MIB + 1
        self.rejects(4100, 'cumulative allocation')

    def test_gatec_carries_only_gatec(self):
        self.prior()
        ev = self.tmp / 'vos-evidence'
        (ev / 'n36-stage3-gatec-first').mkdir()
        (self.tmp / 'n36-stage3-gatec-first.tar.gz').write_bytes(b'x')
        self.sizes.update({'n36-stage3-gatec-first': 60 * g.MIB, 'n36-stage3-gatec-first.tar.gz': 15 * g.MIB})
        self.gatec_root('n36-stage3-gatec-second')
        self.gatec_state([str(ev / 'n36-stage3-gatec-first'), str(self.tmp / 'n36-stage3-gatec-first.tar.gz')])
        self.used = 10 * g.MIB
        self.admit(4100)
        self.assertEqual(json.loads(self.samples().splitlines()[-1])['carried'], 75 * g.MIB)
        (ev / 'n36-stage3-gatec-third').mkdir()
        self.rejects(4100, 'differs from the admitted carry')

    def test_gatec_rejects_gateb_allowances_and_bad_records(self):
        self.gatec_root()
        for limit in (128 * g.MIB, 384 * g.MIB, 448 * g.MIB, 512 * g.MIB):   # 256 MiB is Gate C v1 itself
            with self.subTest(limit=limit):
                self.gatec_state([], limit=limit, version='N3.6 Gate C allowance v1')
                with self.assertRaises(RuntimeError):
                    g.Guard(self.root)
        for name, kw in {'gate B version': dict(version=g.VERSIONS[448 * g.MIB]), 'nonzero previous': dict(previous=g.LIMIT),
                         'unknown version': dict(version='N3.6 Gate C allowance v2')}.items():
            with self.subTest(name):
                self.gatec_state([], **kw)
                with self.assertRaises(RuntimeError) as e:
                    g.Guard(self.root)
                self.assertIn('invalid storage authorization version', str(e.exception))
        with self.subTest('no carry list'):
            self.gatec_state([])
            st = json.loads((self.root / 'state.json').read_text())
            del st['carried']
            (self.root / 'state.json').write_text(json.dumps(st) + '\n')
            with self.assertRaises(RuntimeError):
                g.Guard(self.root)

    def test_gateb_ignores_gatec_roots(self):
        self.restate(limit_bytes=448 * g.MIB, carried=self.prior())
        (self.tmp / 'vos-evidence' / 'n36-stage3-gatec-x').mkdir()
        (self.tmp / 'n36-stage3-gatec-x.tar.gz').write_bytes(b'x')
        self.used = 10 * g.MIB
        self.admit(4100)
        self.assertEqual(json.loads(self.samples().splitlines()[-1])['carried'], 250 * g.MIB)

    # Gate C allowance v2 (user-approved 2026-10-22): a cumulative extension of v1, never a reset.
    def gatec_v1_evidence(self):
        ev = self.tmp / 'vos-evidence'
        ev.mkdir(exist_ok=True)
        (ev / 'n36-stage3-gatec-first').mkdir()
        (self.tmp / 'n36-stage3-gatec-first.tar.gz').write_bytes(b'x')
        self.sizes.update({'n36-stage3-gatec-first': 200 * g.MIB, 'n36-stage3-gatec-first.tar.gz': 18 * g.MIB})
        return [str(ev / 'n36-stage3-gatec-first'), str(self.tmp / 'n36-stage3-gatec-first.tar.gz')]

    def test_gatec_v2_extends_v1_cumulatively(self):
        self.prior()                                  # Gate B roots exist; they must not count towards Gate C
        carried = self.gatec_v1_evidence()
        self.gatec_root('n36-stage3-gatec-second')
        self.gatec_state(carried, previous=256 * g.MIB, limit=400 * g.MIB)
        self.used = 100 * g.MIB
        self.admit(4100)
        row = json.loads(self.samples().splitlines()[-1])
        self.assertEqual((row['carried'], row['charged'], row['limit']), (218 * g.MIB, 320 * g.MIB, 400 * g.MIB))
        self.used = 180 * g.MIB + 1                   # 180 + 218 carried + 2 headroom: one byte over 400 MiB
        self.rejects(4100, 'cumulative allocation')

    def test_gatec_v2_requires_v1_as_previous(self):
        self.gatec_root()
        for name, kw in {'reset to zero': dict(previous=0), 'Gate B 384': dict(previous=384 * g.MIB),
                         'Gate B 448': dict(previous=448 * g.MIB), 'non-integer': dict(previous=str(256 * g.MIB)),
                         'v1 version string': dict(previous=256 * g.MIB, version='N3.6 Gate C allowance v1'),
                         'Gate B version string': dict(previous=256 * g.MIB, version=g.VERSIONS[448 * g.MIB])}.items():
            with self.subTest(name):
                self.gatec_state([], limit=400 * g.MIB, **kw)
                with self.assertRaises(RuntimeError) as e:
                    g.Guard(self.root)
                self.assertIn('invalid storage authorization version', str(e.exception))
        with self.subTest('v1 must still start from zero'):
            self.gatec_state([], limit=256 * g.MIB, previous=256 * g.MIB)
            with self.assertRaises(RuntimeError):
                g.Guard(self.root)

    def test_gatec_v2_carry_must_be_complete(self):
        carried = self.gatec_v1_evidence()
        self.gatec_root('n36-stage3-gatec-second')
        self.gatec_state(carried[:1], previous=256 * g.MIB, limit=400 * g.MIB)       # the v1 archive left out
        self.used = 10 * g.MIB
        with self.assertRaises(RuntimeError) as e:                                  # refused at admission itself
            self.admit(4100)
        self.assertIn('differs from the admitted carry', str(e.exception))
        self.assertFalse((self.root / g.ADMISSIONS).exists())

    # Gate D (user-approved 2026-10-22): an independent family, 320 MiB v1, carrying only earlier Gate D evidence.
    def gated_state(self, carried, previous=0, version=None, limit=320 * g.MIB):
        p = self.root / 'storage-authorization.json'
        p.write_text(json.dumps(dict(version=version or g.GATED_VERSIONS.get(limit, 'none'), limit_bytes=limit,
                                     previous_limit_bytes=previous)) + '\n')
        st = dict(self.state, limit_bytes=limit, carried=carried, evidence_copy=str(self.tmp / (self.root.name + '.tar.gz')),
                  storage_authorization_sha256=hashlib.sha256(p.read_bytes()).hexdigest())
        (self.root / 'state.json').write_text(json.dumps(st, indent=2) + '\n')

    def other_gates(self):
        """Gate B and Gate C evidence, which never counts towards Gate D."""
        carried = self.prior()
        ev = self.tmp / 'vos-evidence'
        (ev / 'n36-stage3-gatec-x').mkdir()
        (self.tmp / 'n36-stage3-gatec-x.tar.gz').write_bytes(b'x')
        self.sizes.update({'n36-stage3-gatec-x': 90 * g.MIB, 'n36-stage3-gatec-x.tar.gz': 18 * g.MIB})
        return carried + [str(ev / 'n36-stage3-gatec-x'), str(self.tmp / 'n36-stage3-gatec-x.tar.gz')]

    def gated_first(self):
        ev = self.tmp / 'vos-evidence'
        (ev / 'n36-stage3-gated-first').mkdir(parents=True)
        (self.tmp / 'n36-stage3-gated-first.tar.gz').write_bytes(b'x')
        self.sizes.update({'n36-stage3-gated-first': 70 * g.MIB, 'n36-stage3-gated-first.tar.gz': 18 * g.MIB})
        return [str(ev / 'n36-stage3-gated-first'), str(self.tmp / 'n36-stage3-gated-first.tar.gz')]

    def test_gated_admitted_and_cumulative(self):
        self.other_gates()                            # Gate B and Gate C evidence exists; none of it counts
        self.gatec_root('n36-stage3-gated-first')
        self.gated_state([])
        self.used = 100 * g.MIB
        self.admit(4100)
        row = json.loads(self.samples().splitlines()[-1])
        self.assertEqual((row['carried'], row['charged'], row['limit']), (0, 102 * g.MIB, 320 * g.MIB))
        self.assertEqual(g.Guard(self.root).family, g.GATED)
        self.used = 318 * g.MIB + 1                   # + 2 headroom: one byte over 320 MiB
        self.rejects(4100, 'cumulative allocation')

    def test_gated_carries_only_gated(self):
        self.other_gates()
        carried = self.gated_first()
        self.gatec_root('n36-stage3-gated-second')
        self.gated_state(carried)
        self.used = 10 * g.MIB
        self.admit(4100)
        self.assertEqual(json.loads(self.samples().splitlines()[-1])['carried'], 88 * g.MIB)
        (self.tmp / 'vos-evidence' / 'n36-stage3-gated-third').mkdir()
        self.rejects(4100, 'differs from the admitted carry')

    def test_gated_refuses_incomplete_carry_and_other_gates_evidence(self):
        other = self.other_gates()
        first = self.gated_first()
        self.gatec_root('n36-stage3-gated-second')
        for name, carried in {'incomplete': first[:1], 'Gate C evidence charged to Gate D': first + other[-2:],
                              'Gate B evidence charged to Gate D': first + other[:1]}.items():
            with self.subTest(name):
                self.gated_state(carried)
                self.used = 10 * g.MIB
                with self.assertRaises(RuntimeError) as e:
                    self.admit(4100)
                self.assertIn('differs from the admitted carry', str(e.exception))
                self.assertFalse((self.root / g.ADMISSIONS).exists())

    def test_gated_rejects_other_allowances_and_records(self):
        self.gatec_root('n36-stage3-gated-x')
        for limit in (256 * g.MIB, 384 * g.MIB, 400 * g.MIB, 448 * g.MIB, 512 * g.MIB):   # other gates' limits
            with self.subTest(limit=limit):
                self.gated_state([], limit=limit, version='N3.6 Gate D allowance v1')
                with self.assertRaises(RuntimeError) as e:
                    g.Guard(self.root)
                self.assertIn('wrong Gate B allowance', str(e.exception))
        for name, kw in {'Gate C v1 version': dict(version='N3.6 Gate C allowance v1'),
                         'Gate C v2 version': dict(version='N3.6 Gate C allowance v2'),
                         'Gate B version': dict(version=g.VERSIONS[448 * g.MIB]),
                         'previous Gate C v1 limit': dict(previous=256 * g.MIB),
                         'previous Gate C v2 limit': dict(previous=400 * g.MIB),
                         'non-integer previous': dict(previous='0')}.items():
            with self.subTest(name):
                self.gated_state([], **kw)
                with self.assertRaises(RuntimeError) as e:
                    g.Guard(self.root)
                self.assertIn('invalid storage authorization version', str(e.exception))
        with self.subTest('no carry list'):
            self.gated_state([])
            st = json.loads((self.root / 'state.json').read_text())
            del st['carried']
            (self.root / 'state.json').write_text(json.dumps(st) + '\n')
            with self.assertRaises(RuntimeError) as e:
                g.Guard(self.root)
            self.assertIn('without cumulative carry', str(e.exception))
        with self.subTest('no authorization record'):
            self.gated_state([])
            (self.root / 'storage-authorization.json').unlink()
            with self.assertRaises(RuntimeError) as e:
                g.Guard(self.root)
            self.assertIn('without its authorization record', str(e.exception))

    def test_family_identification(self):
        base = self.root
        for name, family in (('n36-stage3-gated-x', g.GATED), ('n36-stage3-gatec-x', g.GATEC), ('n36-stage3-gateb-x', g.GATEB)):
            with self.subTest(name):
                self.root = base
                self.gatec_root(name)
                if family == g.GATED:
                    self.gated_state([])
                elif family == g.GATEC:
                    self.gatec_state([])
                self.assertEqual(g.Guard(self.root).family, family)
        for name in ('n36-stage3-gatedx', 'n36-stage3-gate-d-x', 'x-n36-stage3-gated-y'):   # near misses are not Gate D
            with self.subTest(name):
                self.root = base
                self.gatec_root(name)
                self.gated_state([])
                with self.assertRaises(RuntimeError) as e:
                    g.Guard(self.root)
                self.assertIn('wrong Gate B allowance', str(e.exception))

    def test_gatec_ignores_gated_roots(self):
        self.gated_first()
        self.gatec_root('n36-stage3-gatec-y')
        self.gatec_state([])
        self.used = 10 * g.MIB
        self.admit(4100)
        self.assertEqual(json.loads(self.samples().splitlines()[-1])['carried'], 0)

    def test_gateb_ignores_gated_roots(self):
        self.restate(limit_bytes=448 * g.MIB, carried=self.prior())
        self.gated_first()
        self.used = 10 * g.MIB
        self.admit(4100)
        self.assertEqual(json.loads(self.samples().splitlines()[-1])['carried'], 250 * g.MIB)

    # N3 final campaign (user-approved 2026-10-23): an independent family, 2560 MiB v1, carrying only earlier
    # final-campaign evidence; no gate's evidence counts towards it, nor it towards any gate.
    def final_state(self, carried, previous=0, version=None, limit=2560 * g.MIB):
        if getattr(self, 'linux', None) is None:
            self.linux = 100 * g.GIB
        p = self.root / 'storage-authorization.json'
        p.write_text(json.dumps(dict(version=version or g.FINAL_VERSIONS.get(limit, 'none'), limit_bytes=limit,
                                     previous_limit_bytes=previous)) + '\n')
        st = dict(self.state, limit_bytes=limit, carried=carried, evidence_copy=str(self.tmp / (self.root.name + '.tar.gz')),
                  storage_authorization_sha256=hashlib.sha256(p.read_bytes()).hexdigest())
        (self.root / 'state.json').write_text(json.dumps(st, indent=2) + '\n')

    def all_gates(self):
        """Gate B, C and D evidence, none of which counts towards the final campaign."""
        carried = self.other_gates()
        carried += self.gated_first()
        return carried

    def final_first(self):
        ev = self.tmp / 'vos-evidence'
        (ev / 'n36-final-first').mkdir(parents=True)
        (self.tmp / 'n36-final-first.tar.gz').write_bytes(b'x')
        self.sizes.update({'n36-final-first': 1100 * g.MIB, 'n36-final-first.tar.gz': 60 * g.MIB})
        return [str(ev / 'n36-final-first'), str(self.tmp / 'n36-final-first.tar.gz')]

    def test_final_admitted_and_cumulative(self):
        self.all_gates()                              # Gate B, C and D evidence exists; none of it counts
        self.gatec_root('n36-final-first')
        self.final_state([])
        self.used = 1000 * g.MIB
        self.admit(4100)
        row = json.loads(self.samples().splitlines()[-1])
        self.assertEqual((row['carried'], row['charged'], row['limit']), (0, 1002 * g.MIB, 2560 * g.MIB))
        self.assertEqual(g.Guard(self.root).family, g.FINAL)
        self.used = 2558 * g.MIB + 1                  # + 2 headroom: one byte over 2560 MiB
        self.rejects(4100, 'cumulative allocation')

    def test_final_reserve_follows_remaining_allowance(self):
        self.gatec_root('n36-final-first')
        self.final_state([])
        self.used = 100 * g.MIB                       # remaining 2458 MiB: C must hold 10 GiB + 2458 MiB + 256 MiB
        self.host = dict(C=10 * g.GIB + 2458 * g.MIB + 256 * g.MIB, E=100 * g.GIB)
        self.admit(4100)
        self.host = dict(C=10 * g.GIB + 2458 * g.MIB + 256 * g.MIB - 1, E=100 * g.GIB)
        self.rejects(4100, 'C reserve')
        self.host = dict(C=50 * g.GIB, E=100 * g.GIB)
        self.linux = g.GIB + 2458 * g.MIB - 1           # Linux must hold 1 GiB + the remaining allowance
        self.rejects(4100, 'Linux reserve')
        self.linux = g.GIB + 2458 * g.MIB
        self.guard(4100)

    def test_final_carries_only_final(self):
        self.all_gates()
        carried = self.final_first()
        self.gatec_root('n36-final-second')
        self.final_state(carried)
        self.used = 10 * g.MIB
        self.admit(4100)
        self.assertEqual(json.loads(self.samples().splitlines()[-1])['carried'], 1160 * g.MIB)
        (self.tmp / 'vos-evidence' / 'n36-final-third').mkdir()
        self.rejects(4100, 'differs from the admitted carry')

    def test_final_refuses_incomplete_carry_and_other_gates_evidence(self):
        other = self.all_gates()                      # B roots + archive, C root + archive, D root + archive
        first = self.final_first()
        self.gatec_root('n36-final-second')
        for name, carried in {'incomplete': first[:1], 'archive only': first[1:],
                              'Gate D evidence charged to the final campaign': first + other[-2:],
                              'Gate C evidence charged to the final campaign': first + other[-4:-2],
                              'Gate B evidence charged to the final campaign': first + other[:1]}.items():
            with self.subTest(name):
                self.final_state(carried)
                self.used = 10 * g.MIB
                with self.assertRaises(RuntimeError) as e:
                    self.admit(4100)
                self.assertIn('differs from the admitted carry', str(e.exception))
                self.assertFalse((self.root / g.ADMISSIONS).exists())

    def test_final_rejects_other_allowances_and_records(self):
        self.gatec_root('n36-final-x')
        for limit in (256 * g.MIB, 320 * g.MIB, 384 * g.MIB, 400 * g.MIB, 448 * g.MIB, 2048 * g.MIB, 3072 * g.MIB):
            with self.subTest(limit=limit):
                self.final_state([], limit=limit, version='N3 final campaign allowance v1')
                with self.assertRaises(RuntimeError) as e:
                    g.Guard(self.root)
                self.assertIn('wrong Gate B allowance', str(e.exception))
        for name, kw in {'Gate D version': dict(version='N3.6 Gate D allowance v1'),
                         'Gate C v2 version': dict(version='N3.6 Gate C allowance v2'),
                         'Gate B version': dict(version=g.VERSIONS[448 * g.MIB]),
                         'unknown version': dict(version='N3 final campaign allowance v2'),
                         'previous Gate D limit': dict(previous=320 * g.MIB),
                         'previous Gate C v2 limit': dict(previous=400 * g.MIB),
                         'non-integer previous': dict(previous='0')}.items():
            with self.subTest(name):
                self.final_state([], **kw)
                with self.assertRaises(RuntimeError) as e:
                    g.Guard(self.root)
                self.assertIn('invalid storage authorization version', str(e.exception))
        with self.subTest('no carry list'):
            self.final_state([])
            st = json.loads((self.root / 'state.json').read_text())
            del st['carried']
            (self.root / 'state.json').write_text(json.dumps(st) + '\n')
            with self.assertRaises(RuntimeError) as e:
                g.Guard(self.root)
            self.assertIn('without cumulative carry', str(e.exception))
        with self.subTest('no authorization record'):
            self.final_state([])
            (self.root / 'storage-authorization.json').unlink()
            with self.assertRaises(RuntimeError) as e:
                g.Guard(self.root)
            self.assertIn('without its authorization record', str(e.exception))

    def test_final_family_identification(self):
        base = self.root
        self.root = base
        self.gatec_root('n36-final-x')
        self.final_state([])
        self.assertEqual(g.Guard(self.root).family, g.FINAL)
        for name in ('n36-finalx', 'n36-final', 'x-n36-final-y', 'n36-stage3-final-x', 'n36-fina-l-x'):   # near misses
            with self.subTest(name):
                self.root = base
                self.gatec_root(name)
                self.final_state([])
                with self.assertRaises(RuntimeError) as e:
                    g.Guard(self.root)
                self.assertIn('wrong Gate B allowance', str(e.exception))

    def test_gates_ignore_final_roots(self):
        for name, setup in (('Gate B', lambda: self.restate(limit_bytes=448 * g.MIB, carried=self.prior())),
                            ('Gate C', lambda: (self.gatec_root('n36-stage3-gatec-y'), self.gatec_state([]))),
                            ('Gate D', lambda: (self.gatec_root('n36-stage3-gated-y'), self.gated_state([])))):
            with self.subTest(name):
                self.tearDown()
                self.setUp()
                self.final_first()
                setup()
                self.used = 10 * g.MIB
                self.admit(4100)
                want = 250 * g.MIB if name == 'Gate B' else 0
                self.assertEqual(json.loads(self.samples().splitlines()[-1])['carried'], want)


if __name__ == '__main__':
    main()
