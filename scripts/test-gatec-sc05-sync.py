#!/usr/bin/env python3
"""Host test: every SC05 inspection sees its own attempt's destination; compiles and runs the real Gate C program.

The kernel controller inspects the program's destination at the debug barrier that follows each attempt (SC05:
attempt 1 at barrier 3, the retry at barrier 4), before releasing the program. Execution 2 showed the program
refilling its sentinel before each barrier: at barrier 3 attempt 1's data had already been overwritten (the
exact-data check failed), and SC05's "destination unchanged" checks passed without testing anything.

user/storage_stress/gatec.c is compiled natively against the real <vos/file.h> and linked with a model of the
controller: each barrier snapshots the destination exactly as the controller's copy_from_user would, and each file
operation either publishes attempt-specific data, publishes a partial prefix then fails (a defect the controller's
sentinel check must be able to see), or fails cleanly (EIO, EINTR). Checked:
  * each attempt's destination is what the next barrier shows, for read, stat and directory operations, one or two
    attempts, and every outcome SC05 produces;
  * a partial publication by attempt 1 is visible at barrier 3, so the failure-path check is meaningful;
  * positive control: execution 2's order (fill before barrier) reproduces the overwrite;
  * the kernel's SC05 inspects attempt 1 after barrier 3 and before releasing the program.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from unittest import TestCase, main, skipUnless

REPO = Path(__file__).resolve().parent.parent
PROGRAM = REPO / 'user/storage_stress/gatec.c'
KERNEL = REPO / 'arch/x86_64/storage_stress_c.c'
FILL = '        for(unsigned j=0;j<4096;j++) bytes[j]=0xa5;\n'
BARRIER = '        if(vos_debug_write(0,0)) return 5;\n'
OUT_LEN = {1: 4096, 4: 4096, 2: 64, 3: 272}           # read r0, read r1, stat, directory entry
OK_RESULT = {1: 4096, 4: 4096, 2: 0, 3: 1}
HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vos/file.h>
int gatec_main(void);
struct config { uint64_t buffer, op, attempts, region, stale; };
static struct config *cfg;
static int op, attempts, phase, done;
static const char *plan;
static unsigned char view[16][4096];        /* the controller's view of the destination at barrier k */
long vos_debug_write(const void *buf, unsigned long len)
{
    if (len) exit(90);
    if (++phase == 1) {                      /* trusted configuration, as the controller fills it */
        cfg = (struct config *)buf;
        cfg->op = op; cfg->attempts = attempts; cfg->region = 1; cfg->stale = 7;
        return 0;
    }
    if (phase >= 16) exit(91);
    memcpy(view[phase], (const void *)(uintptr_t)cfg->buffer, sizeof(view[phase]));
    return 0;
}
static long attempt(void *d, uint64_t n, long ok)
{
    int a = done++;
    unsigned char *b = d;
    if (phase < 2 || !plan[a]) exit(92);
    switch (plan[a]) {
    case 'o': for (uint64_t j = 0; j < n; j++) b[j] = (unsigned char)(0x30 + 7 * a + j); return ok;
    case 'p': if (n) memset(b, 0x11, n < 64 ? n : 64); return -15;
    case 'e': return -15;
    case 'i': return -19;
    }
    exit(93);
}
long vos_file_initial(uint64_t index) { return index == 0 ? 100 : -1; }
long vos_file_open_at(long directory, const void *path, uint64_t length, uint64_t rights)
{
    (void)directory; (void)path; (void)length; (void)rights;
    return phase >= 2 ? attempt(0, 0, 300) : 200;
}
long vos_file_read_at(long file, void *destination, uint64_t length, uint64_t offset)
{
    (void)file; (void)offset;
    return attempt(destination, length, (long)length);
}
long vos_file_stat(long handle, void *destination, uint64_t size)
{
    if (phase < 2) return handle == 7 ? -VOS_EBADHANDLE : -1;   /* the stale-token check */
    return attempt(destination, size, 0);
}
long vos_directory_next(long directory, uint64_t cookie, void *destination, uint64_t size)
{
    (void)directory; (void)cookie;
    return attempt(destination, size, 1);
}
int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    op = atoi(argv[1]); attempts = atoi(argv[2]); plan = argv[3];
    int rc = gatec_main();
    printf("{\"rc\": %d, \"barriers\": %d, \"attempts\": %d, \"views\": {", rc, phase, done);
    for (int k = 2; k <= phase; k++) {
        printf("%s\"%d\": \"", k > 2 ? ", " : "", k);
        for (int j = 0; j < 4096; j++) printf("%02x", view[k][j]);
        printf("\"");
    }
    printf("}}\n");
    return 0;
}
'''


def order(text, fill_first):
    """The program with the sentinel fill before (execution 2) or after (repair) the attempt barrier."""
    if text.count(FILL) != 1 or text.count(BARRIER) != 1:
        raise AssertionError('attempt loop not found in gatec.c')
    body = text.replace(FILL, '')
    return body.replace(BARRIER, FILL + BARRIER if fill_first else BARRIER + FILL)


def build(source, workdir):
    w = Path(workdir)
    (w / 'gatec.c').write_text(source)
    (w / 'harness.c').write_text(HARNESS)
    cc = ['gcc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(REPO / 'user/include')]
    for args in ([*cc, '-Dmain=gatec_main', '-c', str(w / 'gatec.c'), '-o', str(w / 'gatec.o')],
                 [*cc, '-c', str(w / 'harness.c'), '-o', str(w / 'harness.o')],
                 ['gcc', str(w / 'gatec.o'), str(w / 'harness.o'), '-o', str(w / 'model')]):
        p = subprocess.run(args, capture_output=True, text=True, timeout=120)
        if p.returncode:
            raise AssertionError(' '.join(args[:2]) + ': ' + p.stderr[:600])
    return w / 'model'


def run_model(model, op, attempts, plan):
    p = subprocess.run([str(model), str(op), str(attempts), plan], capture_output=True, text=True, timeout=60)
    if p.returncode:
        raise AssertionError('model exited %d' % p.returncode)
    r = json.loads(p.stdout)
    r['views'] = {int(k): bytes.fromhex(v) for k, v in r['views'].items()}
    return r


def expected(op, a, outcome):
    """Attempt a's destination, starting from the sentinel."""
    n = OUT_LEN[op]
    d = bytearray([0xa5]) * 4096
    if outcome == 'o':
        d[:n] = bytes((0x30 + 7 * a + j) & 255 for j in range(n))
    elif outcome == 'p':
        d[:min(n, 64)] = b'\x11' * min(n, 64)
    return bytes(d)


def classify(view, op, attempts):
    if view == b'\xa5' * 4096:
        return 'sentinel'
    if view == bytes(4096):
        return 'untouched'
    for a in range(attempts):
        for outcome in ('o', 'p'):
            if view == expected(op, a, outcome):
                return ('attempt %d data' if outcome == 'o' else 'attempt %d partial publication') % (a + 1)
    return 'other'


def problems(result, op, attempts, plan):
    found = []
    if result['rc'] or result['barriers'] != attempts + 2 or result['attempts'] != attempts:
        found.append('program did not run its %d attempts to completion: %s' % (attempts, {k: result[k] for k in ('rc', 'barriers', 'attempts')}))
    for a in range(attempts):
        b = a + 3                                     # the controller inspects attempt a+1 at barrier a+3
        want = expected(op, a, plan[a])
        if result['views'].get(b) != want:
            found.append('barrier %d shows %s, not attempt %d\'s destination (%s)' % (
                b, classify(result['views'].get(b, b''), op, attempts), a + 1, classify(want, op, attempts)))
    return found


@skipUnless(shutil.which('gcc'), 'gcc required')
class Synchronization(TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        base = Path(cls.tmp.name)
        (base / 'repair').mkdir()
        (base / 'execution2').mkdir()
        text = PROGRAM.read_text()
        cls.model = build(text, base / 'repair')
        cls.control = build(order(text, fill_first=True), base / 'execution2')

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_every_attempt_is_inspected_before_the_next_refill(self):
        for op in (1, 4, 2, 3):
            for plan in ('oo', 'eo', 'io', 'po'):
                with self.subTest(op=op, plan=plan):
                    self.assertEqual(problems(run_model(self.model, op, 2, plan), op, 2, plan), [])

    def test_single_attempt_programs(self):
        for op in (1, 4, 2, 3):
            for plan in ('o', 'e', 'p'):
                with self.subTest(op=op, plan=plan):
                    self.assertEqual(problems(run_model(self.model, op, 1, plan), op, 1, plan), [])
        r = run_model(self.model, 0, 1, 'o')                       # open: no destination
        self.assertEqual((r['rc'], r['barriers'], r['attempts']), (0, 3, 1))

    def test_failure_path_checks_are_meaningful(self):
        # A partial publication by attempt 1 must reach the controller's sentinel check at barrier 3.
        for op in (1, 2, 3):
            with self.subTest(op=op):
                view = run_model(self.model, op, 2, 'po')['views'][3]
                self.assertEqual(classify(view, op, 2), 'attempt 1 partial publication')
                self.assertNotEqual(view, b'\xa5' * 4096)

    def test_execution_2_order_reproduces_the_overwrite(self):
        r = run_model(self.control, 1, 2, 'oo')
        self.assertIn("barrier 3 shows sentinel, not attempt 1's destination (attempt 1 data)", problems(r, 1, 2, 'oo'))
        r = run_model(self.control, 1, 2, 'po')
        self.assertIn("barrier 3 shows sentinel, not attempt 1's destination (attempt 1 partial publication)", problems(r, 1, 2, 'po'))

    def test_controller_inspects_attempt_one_before_release(self):
        text = KERNEL.read_text()
        case = text[text.index('static void sc05_case('):text.index('static void sc05(')]
        rel = case.index('    release(V);wait_barrier(V,4);\n')
        last3 = case.rindex('wait_barrier(V,3)', 0, rel)
        for check in ('        exact(V,OP_READ,region);\n', '        sentinel(V,4096);\n'):
            with self.subTest(check=check.strip()):
                self.assertEqual(case.count(check), 1)
                self.assertTrue(last3 < case.index(check) < rel)
        self.assertGreater(case.index('"same-handle retry succeeds");exact(V,OP_READ,region);'), rel)
        self.assertEqual(re.findall(r'victim\(bi,V,OP_READ,(\d+),region,0\)', case), ['2'])


if __name__ == '__main__':
    main()
