#!/usr/bin/env python3
"""Host test: Gate B SC01/SC02 controller pacing and the CPU release window (storage_stress_b.c); no VOS execution.

The pacing helpers and the three waits are taken verbatim from storage_stress_b.c and compiled against a
model of the kernel: interrupt-driven progress (a device completion, a timer wakeup) happens only when a
tick passes, which a sleeping controller allows and a yielding one does not (IF = 0, interrupts only in the
idle thread); polled progress happens on yields. usage: test-gateb-controller-pacing.py [SOURCE]
"""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from unittest import TestCase, main, skipUnless

REPO = Path(__file__).resolve().parent.parent
SOURCE = Path(sys.argv.pop(1)) if len(sys.argv) > 1 and sys.argv[1].endswith('.c') else REPO / 'arch/x86_64/storage_stress_b.c'
FUNCS = ('pace_deadline', 'pacing', 'pace', 'wait_blocked', 'zero_ack', 'wait_progress')

PRELUDE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VOS_EAGAIN 11
#define THREAD_BLOCKED 3
#define VBLK_MODE_POLL 0
#define VBLK_MODE_IRQ 1
%(defines)s
struct process { uint64_t tid; };
static struct process procs[8];
static unsigned cpu_phase, phase;
static struct { int mode; } vi;
static uint64_t ticks, ready_at;            /* the awaited event happens once ticks (IRQ) or yields (polling) reach ready_at */
static unsigned long yields, sleeps, recvs, steps;
static const char *failed;
__attribute__((unused)) static int ready(void) { return vi.mode == VBLK_MODE_IRQ ? ticks >= ready_at : yields >= ready_at; }
__attribute__((unused)) static void step(void) { if (++steps > 50000000ul) { puts("UNBOUNDED"); exit(3); } if (ready()) { phase = cpu_phase = 2; } }
__attribute__((unused)) static void *vblk_info_ptr(void) { return &vi; }
#define vblk_info() ((typeof(&vi))vblk_info_ptr())
__attribute__((unused)) static uint64_t irq_ticks(void) { return ticks; }
__attribute__((unused)) static void thread_yield(void) { yields++; step(); }
__attribute__((unused)) static void thread_sleep(uint64_t n) { sleeps++; ticks += n ? n : 1; step(); }
__attribute__((unused)) static int thread_state(uint64_t tid) { (void)tid; return ready() ? THREAD_BLOCKED : 1; }
__attribute__((unused)) static int64_t channel_recv(struct process *p, int64_t h, void *b, uint64_t c, void *l) {
    (void)p; (void)h; (void)b; (void)c; (void)l; recvs++; return ready() ? 0 : -VOS_EAGAIN; }
__attribute__((unused)) static void require(int ok, const char *why) { if (!ok && !failed) failed = why; }
'''

MAIN = r'''
int main(int argc, char **argv) {
    vi.mode = strcmp(argv[2], "irq") ? VBLK_MODE_POLL : VBLK_MODE_IRQ;
    ticks = strtoull(argv[4], 0, 0);
    ready_at = (vi.mode == VBLK_MODE_IRQ ? ticks : 0) + strtoull(argv[3], 0, 0);
    if (!strcmp(argv[1], "ack")) zero_ack(0, 1);
    else if (!strcmp(argv[1], "blocked")) wait_blocked(0);
    else wait_progress(2, !strcmp(argv[1], "cpu"));
    printf("yields=%lu sleeps=%lu recvs=%lu ticks=%llu\n%s\n", yields, sleeps, recvs, (unsigned long long)ticks,
           failed ? failed : "OK");
    return 0;
}
'''


def extract(text):
    out = []
    for name in FUNCS:
        m = re.search(r'^static [^\n]*\b%s\([^\n]*\{[^\n]*\}$' % name, text, re.M) or \
            re.search(r'^static [^\n]*\b%s\([^\n]*\{\n.*?^\}$' % name, text, re.M | re.S)
        if not m:
            raise AssertionError('function not found in source: ' + name)
        out.append(m.group(0))
    return '\n'.join(out)


def header_define(path, name):
    m = re.search(r'^#define %s\s+(\S+)' % name, (REPO / path).read_text(), re.M)
    return '#define %s %s' % (name, m.group(1))


@skipUnless(shutil.which('gcc'), 'gcc required')
class Pacing(TestCase):
    @classmethod
    def setUpClass(cls):
        text = SOURCE.read_text()
        cls.text = text
        defines = [header_define('arch/x86_64/virtio_blk.h', 'VBLK_WAIT_TICKS'), header_define('arch/x86_64/pit.h', 'PIT_HZ')]
        defines += re.findall(r'^#define (?:PACE_\w+|CPU_RELEASE_TICKS)\b[^\n]*', text, re.M)
        cls.dir = Path(tempfile.mkdtemp(prefix='pacing-'))
        (cls.dir / 'h.c').write_text(PRELUDE % dict(defines='\n'.join(defines)) + extract(text) + MAIN)
        subprocess.run(['gcc', '-std=gnu11', '-O1', '-Wall', '-Werror', '-o', str(cls.dir / 'h'), str(cls.dir / 'h.c')],
                       check=True, capture_output=True, timeout=120)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir)

    def run_case(self, wait, mode, after, start=0):
        p = subprocess.run([str(self.dir / 'h'), wait, mode, str(after), str(start)], capture_output=True, text=True, timeout=60)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        counts, verdict = p.stdout.rstrip('\n').split('\n')
        return verdict, {k: int(v) for k, v in (f.split('=') for f in counts.split())}

    # Interrupt transport: progress needs ticks, so the controller must sleep.
    def test_irq_ack_arrives_after_ticks(self):
        head, c = self.run_case('ack', 'irq', 3)
        self.assertEqual((head, c['yields']), ('OK', 0))
        self.assertGreaterEqual(c['sleeps'], 3)

    def test_irq_waits_complete_after_ticks(self):
        for wait in ('blocked', 'progress', 'cpu'):
            with self.subTest(wait):
                head, c = self.run_case(wait, 'irq', 5)
                self.assertEqual((head, c['yields'], c['sleeps']), ('OK', 0, 5))

    def test_irq_wait_is_tick_bounded(self):
        for wait, why in (('ack', 'exact paced zero-length acknowledgment'), ('blocked', 'deterministic channel/progress barrier reached'),
                          ('progress', 'exact progress phase reached, finite work bound')):
            with self.subTest(wait):
                head, c = self.run_case(wait, 'irq', 10 ** 9)
                self.assertEqual(head, why)
                self.assertEqual(c['sleeps'], 4 * 200)          # PACE_TICKS = 4 x VBLK_WAIT_TICKS, then fail closed

    def test_irq_large_tick_values(self):
        head, c = self.run_case('ack', 'irq', 7, start=1 << 62)
        self.assertEqual(head, 'OK')

    # Polling transport: exactly the original 4096-round yield loops.
    def test_poll_ack_unchanged(self):
        self.assertEqual(self.run_case('ack', 'poll', 10), ('OK', dict(yields=10, sleeps=0, recvs=11, ticks=0)))
        head, c = self.run_case('ack', 'poll', 10 ** 9)
        self.assertEqual((head, c['yields'], c['recvs'], c['sleeps']), ('exact paced zero-length acknowledgment', 4096, 4096, 0))

    def test_poll_waits_unchanged(self):
        for wait in ('blocked', 'progress'):
            with self.subTest(wait):
                self.assertEqual(self.run_case(wait, 'poll', 7)[1], dict(yields=7, sleeps=0, recvs=0, ticks=0))
                self.assertEqual(self.run_case(wait, 'poll', 10 ** 9)[1]['yields'], 4096)

    # CPU companion release window: one quarter (17 of 68 epochs) of the 240-second group watchdog.
    def test_cpu_release_window(self):
        m = re.search(r'^#define CPU_RELEASE_TICKS \((\d+)u\*PIT_HZ\*(\d+)u/(\d+)u\)', self.text, re.M)
        self.assertIsNotNone(m)
        group, quarter, epochs = map(int, m.groups())
        self.assertEqual((group, quarter, epochs), (240, 17, 68))
        self.assertEqual(group * 100 * quarter // epochs, 6000)
        hook = re.search(r'if\(p==&procs\[2\] && f->rax==SYS_DEBUG_WRITE\) \{.*?\n        \}', self.text, re.S).group(0)
        self.assertIn('thread_block_until(&progress_gate,irq_ticks()+CPU_RELEASE_TICKS)', hook)
        self.assertIn('require(e==THREAD_WOKEN,"CPU deterministic release, no timeout")', hook)

    def test_pre_notify_loops_still_yield(self):
        sc01 = re.search(r'^static void sc01\(.*?^\}', self.text, re.M | re.S).group(0)
        self.assertIn('if(pending) break;\n            thread_yield();', sc01)
        self.assertIn('!file_process_busy(&procs[follow]);i++) thread_yield();', sc01)


if __name__ == '__main__':
    main()
