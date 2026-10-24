#!/usr/bin/env python3
"""Host test: Gate D compiles exactly Gate B's shared test hooks plus its own; no build, no VOS execution.

As for Gate C (test-gatec-case-guards.py), a shared hook that names Gate B but not Gate D would silently drop
instrumentation or a self-test from the Gate D image. Checked:
  * source: every conditional naming VOS_TEST_CASE_storage_stress_b also names ..._d on the same side; each Gate D
    hook site exists exactly once; arch.mk links Gate D's objects;
  * no line moves: Gate D adds no line to a compiled source. Its hooks extend existing guard lines, trail existing
    code lines (kernel.c, storage_stress.c), or are the one directive line in process.c that replaced a comment
    line (the comment moved onto its #if line); its header blocks are the last thing in their headers;
  * preprocessed: each shared unit's own lines under Gate D equal those under Gate B once the gate-specific lines
    and Gate D's hook expansions are set aside; Gate C's hooks never reach the Gate D image;
  * disk self-tests: Gate D compiles exactly Gate A's selftest_vblk.c and selftest_blk.c.
Paths are relative to this script, so mutation controls can run it against a modified copy of the tree.
"""
from pathlib import Path
import re
import shutil
import subprocess
from unittest import TestCase, main, skipUnless

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / 'arch/x86_64'
CC = ['gcc', '-std=gnu11', '-ffreestanding', '-m64', '-mgeneral-regs-only', '-E']
SHARED = ('io_scope.c', 'process.c', 'virtio_blk.c', 'kernel.c', 'selftest_vblk.c', 'selftest_blk.c', 'file.c',
          'storage_stress.c')
GATE_SOURCES = ('storage_stress_b.c', 'storage_stress_c.c', 'storage_stress_d.c')
GATE_B_ONLY = ('storage_stress_b_progress', 'selftest_storage_stress_b', 'Gate B')
GATE_D_ONLY = ('storage_stress_d_progress', 'storage_stress_d_event', 'selftest_storage_stress_d', 'Gate D')
GATE_C_EXPANSIONS = ('storage_stress_c_progress', 'storage_stress_c_hold', 'selftest_storage_stress_c')
DISPATCH = ('do { if (first_failure) panic("Gate D prerequisite self-test failed"); '
            'check(selftest_storage_stress_d(&bi) == 0, "N3.6 selected Gate D stress"); } while (0);')
EXPANSIONS = ('storage_stress_d_progress(p, f, res);', 'storage_stress_d_event(kind, identity);', DISPATCH)
COMMENT = ' /* Scalar progress only, before the existing post-dispatch kill check. */\n#include "stress_d_progress.inc"\n'
SITES = {
    'process.c': [COMMENT + '        storage_stress_event(STRESS_PROGRESS, p->pid); STORAGE_STRESS_C_PROGRESS(p, f, res)\n'],
    'kernel.c': ['    selftest_sched_case(&bi, first_failure != 0); STORAGE_STRESS_D_DISPATCH(&bi)\n'],
    'storage_stress.c': ['    struct stress_trace *t = &storage_stress_trace; STORAGE_STRESS_D_EVENT(kind, identity)\n'],
    'storage_stress.h': ['#ifdef VOS_TEST_CASE_storage_stress_d\n'],
}
GATE_D_TEXT = re.compile(r'storage_stress_d|STORAGE_STRESS_D_|stress_d_progress\.inc')
CONDITIONAL = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif)\b')


def sources(src):
    return [p for p in sorted(Path(src).glob('*.[ch]')) if p.name not in GATE_SOURCES]


def source_problems(src):
    problems = []
    for p in sources(src):
        for n, line in enumerate(p.read_text().splitlines(), 1):
            if not CONDITIONAL.match(line) or 'VOS_TEST_CASE_storage_stress_b' not in line or \
                    line.strip() == '#ifdef VOS_TEST_CASE_storage_stress_b':
                continue
            if 'VOS_TEST_CASE_storage_stress_d' not in line:
                problems.append('%s:%d names Gate B but not Gate D' % (p.name, n))
                continue
            b = re.search(r'(!?)defined\(VOS_TEST_CASE_storage_stress_b\)', line).group(1)
            d = re.search(r'(!?)defined\(VOS_TEST_CASE_storage_stress_d\)', line).group(1)
            if b != d:
                problems.append('%s:%d Gate D guard polarity differs from Gate B' % (p.name, n))
    for name, wants in SITES.items():
        text = (Path(src) / name).read_text()
        for w in wants:
            if text.count(w) != 1:
                problems.append('%s: Gate D hook site missing or duplicated' % name)
    if 'ifeq ($(VOS_TEST_CASE),storage-stress-d)\nARCH_OBJS += storage_stress_d.o storage_stress.o\nendif\n' not in \
            (Path(src) / 'arch.mk').read_text():
        problems.append('arch.mk: Gate D objects not linked')
    inc = [l.strip() for l in (Path(src) / 'stress_d_progress.inc').read_text().splitlines()
           if l.strip() and not l.lstrip().startswith(('/*', '*'))]
    if inc != ['#ifdef VOS_TEST_CASE_storage_stress_d', 'storage_stress_d_progress(p, f, res);', '#endif']:
        problems.append('stress_d_progress.inc: hook not confined to the Gate D image')
    return problems


def added_line_problems(src):
    problems = []
    for p in sources(src):
        text = p.read_text()
        lines = text.splitlines()
        if p.suffix == '.h':
            gate = [i for i, l in enumerate(lines) if GATE_D_TEXT.search(l) and
                    not (CONDITIONAL.match(l) and 'VOS_TEST_CASE_storage_stress_b' in l)]   # guard extensions stay put
            if gate:
                start = max(i for i in range(gate[0] + 1) if i == 0 or lines[i - 1].strip() in ('#endif', ''))
                tail = [l.strip() for l in lines[start:] if not GATE_D_TEXT.search(l)]
                if any(t and not t.startswith(('#', '/*', '*')) for t in tail):
                    problems.append('%s: Gate D block is not at the end of the header' % p.name)
            continue
        for n, line in enumerate(lines, 1):
            m = GATE_D_TEXT.search(line)
            if not m:
                continue
            if CONDITIONAL.match(line) and 'VOS_TEST_CASE_storage_stress_b' in line:
                continue                                 # same-line guard extension
            if line.strip() == '#include "stress_d_progress.inc"' and p.name == 'process.c':
                continue                                 # the one directive line (checked with its moved comment)
            if line[:m.start()].strip():
                continue                                 # trails an existing code line
            problems.append('%s:%d is a line of its own for Gate D' % (p.name, n))
    return problems


def preprocess(src, unit, case):
    p = subprocess.run(CC + ['-DVOS_TEST', '-DVOS_TEST_CASE_' + case, '-I', str(src), str(Path(src) / unit)],
                       capture_output=True, text=True, timeout=120)
    if p.returncode:
        raise AssertionError(unit + ': ' + p.stderr[:400])
    own, current = [], None
    for line in p.stdout.splitlines():
        m = re.match(r'^# \d+ "([^"]*)"', line)
        if m:
            current = Path(m.group(1)).name
        elif current == unit and line.strip():
            own.append(re.sub(r'\s+', ' ', line.strip()))
    return own


def without_gate_d(line):
    for e in EXPANSIONS:
        line = line.replace(e, '')
    return line.strip()


def count(src, unit, case, text):
    p = subprocess.run(CC + ['-P', '-DVOS_TEST', '-DVOS_TEST_CASE_' + case, '-I', str(src), str(Path(src) / unit)],
                       capture_output=True, text=True, timeout=60)
    if p.returncode:
        raise AssertionError(unit + ': ' + p.stderr[:400])
    return re.sub(r'\s+', ' ', p.stdout).count(text)


def header(src, case):
    p = subprocess.run(CC + ['-P', '-DVOS_TEST', '-DVOS_TEST_CASE_' + case, '-I', str(src), '-include',
                             str(Path(src) / 'storage_stress.h'), '-x', 'c', '/dev/null'],
                       capture_output=True, text=True, timeout=60)
    if p.returncode:
        raise AssertionError(p.stderr[:400])
    return [x.strip() for x in p.stdout.splitlines() if x.strip()]


def self_tests(src, case):
    out = []
    for f in ('selftest_vblk.c', 'selftest_blk.c'):
        p = subprocess.run(CC + ['-P', '-DVOS_TEST', '-DVOS_TEST_CASE_' + case, '-I', str(src), str(Path(src) / f)],
                           capture_output=True, text=True, timeout=60)
        if p.returncode:
            raise AssertionError(f + ': ' + p.stderr[:300])
        out.append(p.stdout)
    return out


@skipUnless(shutil.which('gcc'), 'gcc required')
class GateDCaseGuards(TestCase):
    src = SRC

    def test_every_gate_b_conditional_covers_gate_d(self):
        self.assertEqual(source_problems(self.src), [])

    def test_gate_d_adds_no_line_to_compiled_sources(self):
        self.assertEqual(added_line_problems(self.src), [])

    def test_shared_units_identical_apart_from_gate_specific_lines(self):
        for unit in SHARED:
            with self.subTest(unit):
                b = [x for x in preprocess(self.src, unit, 'storage_stress_b') if not any(t in x for t in GATE_B_ONLY)]
                d = [without_gate_d(x) for x in preprocess(self.src, unit, 'storage_stress_d')]
                self.assertTrue(b)
                self.assertEqual([x for x in d if x and not any(t in x for t in GATE_D_ONLY)], b)

    def test_gate_d_hooks_compiled_exactly_once_and_nowhere_else(self):
        for unit, text in (('process.c', EXPANSIONS[0]), ('storage_stress.c', EXPANSIONS[1]), ('kernel.c', DISPATCH)):
            with self.subTest(unit):
                self.assertEqual(count(self.src, unit, 'storage_stress_d', text), 1)
                for case in ('storage_stress', 'storage_stress_b', 'storage_stress_c', 'file', 'accounting'):
                    self.assertEqual(count(self.src, unit, case, text), 0)
        for unit in ('process.c', 'kernel.c', 'file.c'):
            with self.subTest('no Gate C hook in the Gate D image: ' + unit):
                for t in GATE_C_EXPANSIONS:
                    self.assertEqual(count(self.src, unit, 'storage_stress_d', t), 0)

    def test_header_identical_apart_from_gate_declarations(self):
        b = [x for x in header(self.src, 'storage_stress_b') if not any(t in x for t in GATE_B_ONLY)]
        d = [x for x in header(self.src, 'storage_stress_d') if not any(t in x for t in GATE_D_ONLY)]
        self.assertEqual(d, b)
        self.assertEqual(len([x for x in header(self.src, 'storage_stress_d') if any(t in x for t in GATE_D_ONLY)]), 3)

    def test_gate_d_compiles_gate_a_disk_self_tests(self):
        a = self_tests(self.src, 'storage_stress')
        d = self_tests(self.src, 'storage_stress_d')
        dispatch = 'int selftest_storage_stress_d(const struct boot_info *bi);\n'   # selftest.h's Gate D tail only
        self.assertEqual([x.count(dispatch) for x in d], [1, 1])
        self.assertEqual([x.replace(dispatch, '', 1) for x in d], a)


if __name__ == '__main__':
    main()
