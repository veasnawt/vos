#!/usr/bin/env python3
"""Host test: held publication keeps a test's un-notified virtio-blk request out of the device's view; no VOS execution.

N3 final D1 repair. A suppressed notification does not stop QEMU from processing a request (it kicks the queue when
DRIVER_OK attaches its host notifier), so each self-test request that must not reach the device is submitted under a
publication hold: the driver's avail pointer names a private copy of the available ring, the index the device reads
does not move, and nothing is exposed (virtio 1.2, 2.7.13.3) until a reset or vblk_test_publish_held().

Checked here: the hook's own C code in a host harness (hold, publish in ring order with wrap, reset and not-ready
invalidation, misuse panics); every un-notified site of the vblk, scope-orphan-idle, file and ext2 images holds, and
every late delivery publishes before notifying; and the preprocessor confines the hook and its calls to those four
images, so production and every other image (Gates B/C/D among them) compile none of it. Paths are relative to this
script, so mutation controls can run it against a modified copy of the tree.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from unittest import TestCase, main, skipUnless

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / 'arch/x86_64'
HOLD, PUB = 'vblk_test_hold_publish();', 'vblk_test_publish_held();'
NAMES = ('vblk_test_hold_publish', 'vblk_test_publish_held', 'held_ring', 'held_real')
GUARD = ('#if defined(VOS_TEST) && (defined(VOS_TEST_CASE_vblk) || defined(VOS_TEST_CASE_scope_orphan_idle) || '
         'defined(VOS_TEST_CASE_file) || defined(VOS_TEST_CASE_ext2))')
HOOK_IMAGES = ('vblk', 'scope_orphan_idle', 'file', 'ext2')
# Held sites per file; the one exemption completes its request on purpose (only one owned request per ticket).
HOLDS = {'selftest_vblk.c': 19, 'selftest_blk.c': 1, 'selftest_file.c': 4, 'selftest_ext2.c': 1}
PUBLISHES = {'selftest_vblk.c': 4, 'selftest_blk.c': 0, 'selftest_file.c': 0, 'selftest_ext2.c': 1}
EXEMPT_NEXT = {'skip': 'int a = vblk_submit(400, 1, &slot);', 'notify': 'r = a ? a : vblk_wait(slot, buf, sizeof(buf));'}
UNITS = ('virtio_blk.c', 'selftest_vblk.c', 'selftest_blk.c', 'selftest_file.c', 'selftest_ext2.c')
GCC = shutil.which('gcc')


def hook_block():
    text = (SRC / 'virtio_blk.c').read_text()
    i = text.index(GUARD)
    j = text.index('\n#endif', i)
    return text, i, text[i:j + len('\n#endif')]


def preprocess(unit, case):
    defs = [] if case is None else ['-DVOS_TEST'] + ([] if case == '' else ['-DVOS_TEST_CASE_' + case])
    return subprocess.run([GCC, '-std=gnu11', '-ffreestanding', '-E', '-P', *defs, '-I', str(SRC), str(SRC / unit)],
                          check=True, capture_output=True, text=True).stdout


def cases():
    mk = (SRC / 'arch.mk').read_text()
    return sorted({m.replace('-', '_') for m in re.findall(r'VOS_TEST_CASE=([a-z0-9-]+)', mk)} |
                  {'accounting', 'storage_stress', 'storage_stress_b', 'storage_stress_c', 'storage_stress_d'})


class Sites(TestCase):
    def lines(self, name):
        return (SRC / name).read_text().split('\n')

    def test_every_unnotified_request_is_held(self):
        for name, want in HOLDS.items():
            with self.subTest(name):
                ls = self.lines(name)
                held = exempt = 0
                for k, l in enumerate(ls):
                    if 'vblk_test_skip_notify(' not in l:
                        continue
                    if re.search(r'vblk_test_skip_notify\([^;]*\);\s*' + re.escape(HOLD), l):
                        held += 1
                    elif name == 'selftest_vblk.c' and ls[k + 1].strip() == EXEMPT_NEXT['skip']:
                        exempt += 1
                    else:
                        self.fail('%s:%d skips a notification without holding publication' % (name, k + 1))
                self.assertEqual((held, exempt), (want, 1 if name == 'selftest_vblk.c' else 0))
                self.assertEqual(sum(l.count(HOLD) for l in ls), want, 'a hold without a suppressed notification')

    def test_late_delivery_publishes_first(self):
        for name, want in PUBLISHES.items():
            with self.subTest(name):
                ls = self.lines(name)
                pub = 0
                for k, l in enumerate(ls):
                    if 'vblk_test_notify();' not in l:
                        continue
                    if re.search(re.escape(PUB) + r'\s*vblk_test_notify\(\);', l):
                        pub += 1
                    elif not (name == 'selftest_vblk.c' and ls[k + 1].strip() == EXEMPT_NEXT['notify']):
                        self.fail('%s:%d notifies without publishing held requests' % (name, k + 1))
                self.assertEqual(pub, want)
                self.assertEqual(sum(l.count(PUB) for l in ls), want)

    def test_hook_is_one_guarded_block_after_all_driver_code(self):
        text, i, block = hook_block()
        self.assertEqual(text.count(GUARD), 1)
        self.assertEqual(text[i + len(block):], '\n', 'the hook block must end the file')
        before = text[:i]
        self.assertTrue(before.endswith('#endif\n\n'), 'the block follows the last existing line')
        for n in NAMES:
            self.assertNotIn(n, before, n + ' used outside the hook block')
        self.assertEqual(block.count('avail = held_ring;'), 1)
        self.assertIn('if (info.state != VBLK_READY || !avail || avail == held_ring)', block)


@skipUnless(GCC, 'gcc not available')
class Preprocessor(TestCase):
    def test_only_the_four_images_compile_the_hook_or_its_calls(self):
        for case in [None, ''] + cases():
            label = 'production' if case is None else (case or 'test')
            out = {u: preprocess(u, case) for u in UNITS}
            defined = 'void vblk_test_hold_publish(void)\n{' in out['virtio_blk.c']
            calls = any(re.search(r'vblk_test_(hold_publish|publish_held)\(\);', out[u]) for u in UNITS[1:])
            with self.subTest(label):
                self.assertEqual(defined, case in HOOK_IMAGES)
                self.assertEqual(calls, case in HOOK_IMAGES, 'calls without the hook would not link; hook without calls is dead')
                if case not in HOOK_IMAGES:
                    for n in ('held_ring', 'held_real', 'vblk_test_hold_publish();', 'vblk_test_publish_held();'):
                        self.assertFalse(any(n in o for o in out.values()), '%s in %s' % (n, label))

    def test_protected_images_see_no_new_token(self):
        for case in (None, 'storage_stress_b', 'storage_stress_c', 'storage_stress_d'):
            with self.subTest(case or 'production'):
                for u in UNITS:
                    self.assertNotRegex(preprocess(u, case), r'vblk_test_hold_publish|vblk_test_publish_held|held_ring|held_real')


HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VBLK_QUEUE_SIZE 16
enum { VBLK_ABSENT, VBLK_FAILED, VBLK_BROKEN, VBLK_READY };
static struct { int state; } info = { VBLK_READY };
static volatile uint16_t *avail;
static void barrier(void) {}
static void panic(const char *m) { printf("PANIC %s\n", m); fflush(stdout); exit(3); }
#include "hook.inc"
static uint16_t A[2 + VBLK_QUEUE_SIZE], B[2 + VBLK_QUEUE_SIZE];
static void submit(uint16_t head) { avail[2 + avail[1] % VBLK_QUEUE_SIZE] = head; avail[1] = (uint16_t)(avail[1] + 1); }
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", argv[1], __LINE__, #c); return 1; } } while (0)
int main(int argc, char **argv)
{
    (void)argc;
    avail = A;
    const char *t = argv[1];
    if (!strcmp(t, "hold-publish")) {
        A[1] = 5; A[2 + 4] = 9;
        vblk_test_hold_publish();
        CHECK(avail != A);
        submit(3); submit(6);
        CHECK(A[1] == 5 && A[2 + 5] == 0 && A[2 + 6] == 0 && A[2 + 4] == 9);        /* nothing exposed */
        vblk_test_publish_held();
        CHECK(avail == A && A[1] == 7 && A[2 + 5] == 3 && A[2 + 6] == 6 && A[2 + 4] == 9);
        submit(12);                                                              /* back to the real ring */
        CHECK(A[1] == 8 && A[2 + 7] == 12);
    } else if (!strcmp(t, "wrap")) {
        A[1] = 65535;
        vblk_test_hold_publish();
        submit(9); submit(10);
        CHECK(A[1] == 65535);
        vblk_test_publish_held();
        CHECK(A[1] == 1 && A[2 + 15] == 9 && A[2 + 0] == 10 && avail == A);
    } else if (!strcmp(t, "flags")) {
        vblk_test_hold_publish();
        avail[0] = 1;                                                            /* storm fallback while held */
        submit(3);
        vblk_test_publish_held();
        CHECK(A[0] == 1 && A[1] == 1 && avail == A);
    } else if (!strcmp(t, "reset")) {
        vblk_test_hold_publish();
        submit(3);
        avail = B;                                                               /* bring_up: a fresh ring */
        vblk_test_publish_held();
        CHECK(avail == B && A[1] == 0 && A[2] == 0 && B[1] == 0 && B[2] == 0);   /* a reset request is never exposed */
        vblk_test_hold_publish();                                                /* a new hold is allowed */
        submit(6);
        vblk_test_publish_held();
        CHECK(avail == B && B[1] == 1 && B[2] == 6 && A[1] == 0);
    } else if (!strcmp(t, "broken")) {
        vblk_test_hold_publish();
        submit(3);
        info.state = VBLK_BROKEN;                                                /* timed out, not yet reinitialised */
        vblk_test_publish_held();
        CHECK(A[1] == 0 && A[2] == 0);
        avail = A; memset(A, 0, sizeof(A)); info.state = VBLK_READY;             /* bring_up */
        vblk_test_hold_publish();
        CHECK(avail != A);
    } else if (!strcmp(t, "no-hold")) {
        A[1] = 2;
        vblk_test_publish_held();
        CHECK(avail == A && A[1] == 2);
    } else if (!strcmp(t, "double-hold")) {
        vblk_test_hold_publish();
        vblk_test_hold_publish();
    } else if (!strcmp(t, "hold-not-ready")) {
        info.state = VBLK_BROKEN;
        vblk_test_hold_publish();
    } else if (!strcmp(t, "hold-no-queue")) {
        avail = 0;
        vblk_test_hold_publish();
    } else
        return 2;
    printf("OK %s\n", t);
    return 0;
}
'''


@skipUnless(GCC, 'gcc not available')
class Hook(TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        d = Path(cls.tmp.name)
        _, _, block = hook_block()
        body = '\n'.join(l for l in block.split('\n') if not l.startswith(('#if ', '#include', '#endif')))
        (d / 'hook.inc').write_text(body + '\n')
        (d / 'h.c').write_text(HARNESS)
        cls.bin = d / 'h'
        subprocess.run([GCC, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-o', str(cls.bin), str(d / 'h.c')], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_case(self, name):
        return subprocess.run([str(self.bin), name], capture_output=True, text=True)

    def test_scenarios(self):
        for name in ('hold-publish', 'wrap', 'flags', 'reset', 'broken', 'no-hold'):
            with self.subTest(name):
                p = self.run_case(name)
                self.assertEqual((p.returncode, p.stdout.strip()), (0, 'OK ' + name))

    def test_misuse_panics(self):
        for name in ('double-hold', 'hold-not-ready', 'hold-no-queue'):
            with self.subTest(name):
                p = self.run_case(name)
                self.assertEqual((p.returncode, p.stdout.strip()), (3, 'PANIC vblk test: publication hold needs a ready, unheld queue'))


if __name__ == '__main__':
    main()
