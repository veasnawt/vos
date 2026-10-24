#!/usr/bin/env python3
"""Host test: Gate C compiles exactly Gate B's shared test hooks plus its own; no build, no VOS execution.

A shared hook that names Gate B but not Gate C silently drops instrumentation (or a whole self-test) from the
Gate C image; rerun-1 of Gate B failed on exactly such a missing case guard. Four independent views:
  * source: every conditional naming VOS_TEST_CASE_storage_stress_b also names ..._c on the same side, and
    each Gate B-only block has its declared Gate C counterpart; arch.mk links Gate C's objects;
  * no line moves: the production and Gate B images carry line numbers (.debug_line), so Gate C adds no
    line to any compiled source. Its hooks extend existing guard lines, trail existing code lines, or sit
    at the end of headers; the one directive line (file_hold.inc) replaced a comment line;
  * preprocessed: each shared unit's own lines under Gate C equal those under Gate B once the Gate B-only
    lines and the Gate C hook expansions are set aside; storage_stress.h likewise;
  * disk self-tests: Gate C compiles exactly Gate A's selftest_vblk.c and selftest_blk.c.
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
GATE_SOURCES = ('storage_stress_b.c', 'storage_stress_c.c')
GATE_B_ONLY = ('storage_stress_b_progress', 'selftest_storage_stress_b', 'Gate B')
GATE_C_ONLY = ('storage_stress_c_progress', 'selftest_storage_stress_c', 'storage_stress_c_hold', 'Gate C')
# Each Gate B-only block and the Gate C counterpart that must accompany it in the same file.
COUNTERPARTS = {
    'process.c': '        storage_stress_event(STRESS_PROGRESS, p->pid); STORAGE_STRESS_C_PROGRESS(p, f, res)\n',
    'kernel.c': '    check(selftest_io_scope() == 0, "bounded storage scope self-test"); STORAGE_STRESS_C_DISPATCH(&bi)\n',
    'storage_stress.h': '#ifdef VOS_TEST_CASE_storage_stress_c\n',
}
# Gate C hook expansions, removed from Gate C's preprocessed lines before comparing with Gate B.
EXPANSIONS = ('storage_stress_c_progress(p, f, res);', 'storage_stress_c_hold(r);',
              'do { if (first_failure) panic("Gate C prerequisite self-test failed"); '
              'check(selftest_storage_stress_c(&bi) == 0, "N3.6 selected Gate C stress"); } while (0);')
GATE_C_TEXT = re.compile(r'storage_stress_c|STORAGE_STRESS_C_|FILE_PUBLICATION_HOLD|file_hold\.inc')
CONDITIONAL = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif)\b')


def sources(src):
    return [p for p in sorted(Path(src).glob('*.[ch]')) if p.name not in GATE_SOURCES]


def conditionals(src):
    """Every preprocessor conditional naming a Gate B case, outside the gates' own sources."""
    return [(p.name, n, line.strip()) for p in sources(src)
            for n, line in enumerate(p.read_text().splitlines(), 1)
            if CONDITIONAL.match(line) and 'VOS_TEST_CASE_storage_stress_b' in line]


def source_problems(src):
    problems = []
    for name, n, line in conditionals(src):
        if line == '#ifdef VOS_TEST_CASE_storage_stress_b':
            continue
        if 'VOS_TEST_CASE_storage_stress_c' not in line:
            problems.append('%s:%d names Gate B but not Gate C' % (name, n))
            continue
        b = re.search(r'(!?)defined\(VOS_TEST_CASE_storage_stress_b\)', line).group(1)
        c = re.search(r'(!?)defined\(VOS_TEST_CASE_storage_stress_c\)', line).group(1)
        if b != c:
            problems.append('%s:%d Gate C guard polarity differs from Gate B' % (name, n))
    for p in sources(src):
        text = p.read_text()
        blocks = len(re.findall(r'^#ifdef VOS_TEST_CASE_storage_stress_b$', text, re.M))
        want = COUNTERPARTS.get(p.name)
        if blocks and (want is None or text.count(want) != 1):
            problems.append('%s: Gate B-only block without its Gate C counterpart' % p.name)
    mk = (Path(src) / 'arch.mk').read_text()
    if 'ifeq ($(VOS_TEST_CASE),storage-stress-c)\nARCH_OBJS += storage_stress_c.o storage_stress.o\nendif\n' not in mk:
        problems.append('arch.mk: Gate C objects not linked')
    return problems


def added_line_problems(src):
    """Gate C may not own a line of a compiled source: only guard extensions, trailing hooks, header tails."""
    problems = []
    for p in sources(src):
        lines = p.read_text().splitlines()
        if p.suffix == '.h':
            gate = [i for i, l in enumerate(lines) if GATE_C_TEXT.search(l)]
            rest = [i for i, l in enumerate(lines) if l.strip() and not GATE_C_TEXT.search(l)]
            if gate and p.name != 'storage_stress.h':
                # The Gate C block must follow every other declaration: only its own lines and #endifs after it.
                start = max(i for i in range(gate[0] + 1) if i == 0 or lines[i - 1].strip() in ('#endif', ''))
                tail = [l.strip() for l in lines[start:] if not GATE_C_TEXT.search(l)]
                if any(t and not t.startswith(('#', '/*', '*', 'void ', 'int ')) and not t.endswith('\\') for t in tail):
                    problems.append('%s: Gate C block is not at the end of the header' % p.name)
            continue
        for n, line in enumerate(lines, 1):
            m = GATE_C_TEXT.search(line)
            if not m:
                continue
            if CONDITIONAL.match(line) and 'VOS_TEST_CASE_storage_stress_b' in line:
                continue                                 # same-line guard extension
            if line.strip() == '#include "file_hold.inc"' and p.name == 'file.c':
                continue                                 # the one directive line (read_at), checked below
            if line[:m.start()].strip():
                continue                                 # trails an existing code line
            problems.append('%s:%d is a line of its own for Gate C' % (p.name, n))
    file_c = (Path(src) / 'file.c').read_text()
    if file_c.count('#include "file_hold.inc"') != 1 or \
            'Never copy on error. */\n#include "file_hold.inc"\n    if (!r && !(r = outcome(s, EXT2_OK))) {\n' not in file_c:
        problems.append('file.c: the read hold directive does not replace the comment\'s closing line')
    return problems


def preprocess(src, unit, case):
    """The unit's own non-blank lines (header lines are attributed away by line markers)."""
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


def without_gate_c(line):
    for e in EXPANSIONS:
        line = line.replace(e, '')
    return line.strip()


def unit_difference(src, unit):
    b = [x for x in preprocess(src, unit, 'storage_stress_b') if not any(t in x for t in GATE_B_ONLY)]
    c = [without_gate_c(x) for x in preprocess(src, unit, 'storage_stress_c')]
    return b, [x for x in c if x and not any(t in x for t in GATE_C_ONLY)]


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
class GateCCaseGuards(TestCase):
    src = SRC

    def test_every_gate_b_conditional_covers_gate_c(self):
        self.assertTrue(conditionals(self.src), 'inventory must not be empty')
        self.assertEqual(source_problems(self.src), [])

    def test_gate_c_adds_no_line_to_compiled_sources(self):
        self.assertEqual(added_line_problems(self.src), [])

    def test_shared_units_identical_apart_from_gate_specific_lines(self):
        for unit in SHARED:
            with self.subTest(unit):
                b, c = unit_difference(self.src, unit)
                self.assertTrue(b)
                self.assertEqual(c, b)

    def test_gate_c_hooks_compiled_exactly_once_each(self):
        for unit, text, n in (('process.c', 'storage_stress_c_progress(p, f, res);', 1),
                              ('kernel.c', EXPANSIONS[2], 1), ('file.c', 'storage_stress_c_hold(r);', 4)):
            with self.subTest(unit):
                self.assertEqual(count(self.src, unit, 'storage_stress_c', text), n)
                for case in ('storage_stress', 'storage_stress_b', 'file', 'accounting'):
                    self.assertEqual(count(self.src, unit, case, text), 0)
        self.assertEqual(count(self.src, 'process.c', 'storage_stress_b', 'storage_stress_b_progress(p, f, res);'), 1)
        self.assertEqual(count(self.src, 'process.c', 'storage_stress_c', 'storage_stress_b_progress'), 0)
        self.assertEqual(count(self.src, 'kernel.c', 'storage_stress_c', 'selftest_storage_stress_b'), 0)
        self.assertEqual(count(self.src, 'kernel.c', 'storage_stress_b', 'selftest_storage_stress_c'), 0)

    def test_header_identical_apart_from_gate_declarations(self):
        b = [x for x in header(self.src, 'storage_stress_b') if not any(t in x for t in GATE_B_ONLY)]
        c = [x for x in header(self.src, 'storage_stress_c') if not any(t in x for t in GATE_C_ONLY)]
        self.assertEqual(c, b)
        self.assertIn('struct stress_event events[128];', ' '.join(header(self.src, 'storage_stress_c')).replace('  ', ' '))
        self.assertEqual(len([x for x in header(self.src, 'storage_stress_c') if any(t in x for t in GATE_C_ONLY)]), 2)

    def test_gate_c_compiles_gate_a_disk_self_tests(self):
        a = self_tests(self.src, 'storage_stress')
        c = self_tests(self.src, 'storage_stress_c')
        dispatch = 'int selftest_storage_stress_c(const struct boot_info *bi);\n'   # selftest.h's Gate C tail only
        self.assertEqual([x.count(dispatch) for x in c], [1, 1])
        self.assertEqual([x.replace(dispatch, '', 1) for x in c], a)
        self.assertIn('virtio-blk device ready (ext2 image)', a[0])
        self.assertNotIn('test_absent();', a[0])
        self.assertNotIn('no usable disk: no device registered', a[1])


if __name__ == '__main__':
    main()
