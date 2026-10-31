#!/usr/bin/env python3
"""Independent reference for the USB.3-A EHCI qualification and handoff model.

Replays the traces exported by tests/rust/ehci_init_model.rs (seeded operation streams, each step with the Rust model's
result) through a second implementation written from the EHCI register definitions, not from the Rust source, and reports
the first step at which the two disagree. Hosted only: no hardware, no QEMU.

Trace format (one item per line): `T <seed>`, then pairs of an operation line and the model's result line.
  operations: Q <16 facts>, O CAP|SUP|CTL|CMD|STS|CFG|FAIL <args>, A <ticks>, R <step>
  results:    ok <from> <to> <rule> <event> <failure>   or   err <reason>
A fact is U (unknown), C (contradictory), O:<hex> (observed) or I:<hex> (inferred); the legacy offset is O:none for
"looked for, absent".
"""
import sys

# EHCI 1.0 USBCMD / USBSTS bits and the EHCI extended-capability USBLEGCTLSTS enables.
RS, HCRESET, PSE, ASE = 0x1, 0x2, 0x10, 0x20
HSE, HCH, PSS, ASS = 0x10, 0x1000, 0x4000, 0x8000
ENABLES = 0x3f | 0xe000
U64 = (1 << 64) - 1
TICKS = {'handoff': 1000, 'smi': 100, 'sched': 20, 'halt': 20, 'reset': 250}
FIELDS = ['vendor', 'device', 'class-code', 'hci-version', 'caplength', 'ports', 'companions', 'ports-per-companion',
          'legacy-capability', 'legacy-bios-owned', 'legacy-os-owned', 'legacy-control', 'usbcmd', 'usbsts', 'configflag', 'firmware-active']
FACT_KEYS = ['vendor', 'device', 'class', 'hci', 'cap', 'ports', 'cc', 'pcc', 'legacy', 'bios', 'os', 'ctl', 'cmd', 'sts', 'cfg', 'fw']
NAME = dict(zip(FACT_KEYS, FIELDS))
STALE = {'sup': 'stale.legacy-owner', 'bios': 'stale.legacy-owner', 'os': 'stale.legacy-owner', 'ctl': 'stale.legacy-control', 'cmd': 'stale.usbcmd', 'sts': 'stale.usbsts'}


def parse_fact(tok):
    if tok == 'U':
        return ('U',)
    if tok == 'C':
        return ('C',)
    kind, v = tok.split(':')
    return (kind, 'none' if v == 'none' else int(v, 16))


def reconcile(a, b):
    if a[0] == 'C' or b[0] == 'C':
        return ('C',)
    if a[0] == 'U':
        return b
    if b[0] == 'U':
        return a
    if a[0] == 'O' and b[0] == 'O':
        return a if a[1] == b[1] else ('C',)
    if a[0] == 'O':
        return a
    if b[0] == 'O':
        return b
    return a if a[1] == b[1] else ('C',)


class Refuse(Exception):
    pass


def need(m, key):
    f = m.facts[key]
    if f[0] == 'O':
        return f[1]
    raise Refuse({'U': 'missing.', 'I': 'inferred.', 'C': 'contradictory.'}[f[0]] + NAME[key])


class Model:
    def __init__(self):
        self.phase = 'idle'
        self.facts = {k: ('U',) for k in FACT_KEYS}
        self.now = 0
        self.deadline = None
        self.fresh = set()
        self.stable = None
        self.reset_seen = 0
        self.failure = None

    # ---- helpers -------------------------------------------------------------------------------------------------
    def out(self, frm, to, why, ev='none', fail='none'):
        return f'ok {frm} {to} {why} {ev} {fail}'

    def fail(self, reason, rule):
        frm = self.phase
        self.phase, self.deadline, self.failure = 'failed', None, reason
        return self.out(frm, 'failed', rule, fail=reason)

    def go(self, to, why, ev='none'):
        frm = self.phase
        self.phase = to
        return self.out(frm, to, why, ev)

    def stay(self, why):
        return self.out(self.phase, self.phase, why)

    def arm(self, kind, reason):
        self.deadline = (min(self.now + TICKS[kind], U64), reason)

    def legacy_cap(self):
        v = need(self, 'legacy')
        if v == 'none':
            return None
        if 0x40 <= v <= 0xf8 and v % 4 == 0:
            return v
        raise Refuse('legacy-offset-invalid')

    def base(self):
        c = need(self, 'cap')
        if not (0x10 <= c <= 0x40 and c % 4 == 0):
            raise Refuse('caplength-invalid')

    def readiness(self, step):
        """Returns 'ready', 'notneeded' or raises Refuse."""
        if step in ('handoff', 'mask-smi'):
            if self.legacy_cap() is None:
                return 'notneeded'
            if step == 'handoff':
                need(self, 'bios')
                need(self, 'os')
            else:
                need(self, 'ctl')
            return 'ready'
        self.base()
        need(self, 'cmd')
        need(self, 'sts')
        if step == 'reset' and need(self, 'cc') != 0:
            raise Refuse('companions-present')
        return 'ready'

    def need_fresh(self, *keys):
        for k in keys:
            if k not in self.fresh:
                raise Refuse(STALE[k])

    # ---- operations ----------------------------------------------------------------------------------------------
    def apply(self, line):
        parts = line.split()
        try:
            if self.phase in ('reset', 'failed', 'refused'):
                return 'err terminal'
            if parts[0] == 'Q':
                return self.qualify([parse_fact(t) for t in parts[1:]])
            if self.phase == 'idle':
                return 'err wrong-phase'
            if parts[0] == 'O':
                return self.observe(parts[1:])
            if parts[0] == 'A':
                return self.advance(int(parts[1]))
            return self.request(parts[1])
        except Refuse as r:
            return f'err {r.args[0]}'

    def qualify(self, facts):
        f = dict(zip(FACT_KEYS, facts))
        self.facts = f
        try:
            for k in ('vendor', 'device', 'class', 'hci', 'ports'):
                if f[k][0] == 'C':
                    raise Refuse('contradictory.' + NAME[k])
            if need(self, 'vendor') != 0x8086:
                raise Refuse('not-intel')
            need(self, 'device')
            if need(self, 'class') != 0x0c0320:
                raise Refuse('not-ehci-class')
            if need(self, 'hci') != 0x0100:
                raise Refuse('hci-version-unsupported')
            ports = need(self, 'ports')
            if ports == 0 or ports > 15:
                raise Refuse('ports-invalid')
            if f['cc'][0] == 'O' and f['pcc'][0] == 'O' and f['cc'][1] > 0 and ports > f['cc'][1] * f['pcc'][1]:
                raise Refuse('contradictory.ports')
        except Refuse as r:
            self.facts = {k: ('U',) for k in FACT_KEYS}
            self.phase, self.failure = 'refused', r.args[0]
            raise
        return self.go('qualified', 'qualify.accepted')

    def advance(self, n):
        if n == 0 or self.now + n > U64:
            raise Refuse('bad-ticks')
        self.now += n
        if self.deadline and self.now >= self.deadline[0]:
            return self.fail(self.deadline[1], 'fault.timeout')
        return self.stay('tick.advance')

    def observe(self, a):
        kind = a[0]
        if kind == 'FAIL':
            return self.fail('read-failed', 'fault.read-failed')
        if kind == 'CAP':
            old = self.facts['cap']
            new = reconcile(old, ('O', int(a[1], 16)))
            self.facts['cap'] = new
            return self.stay('observe.contradiction' if new[0] == 'C' and old[0] != 'C' else 'observe.recorded')
        if kind == 'SUP':
            bios, os_ = a[1] == '1', a[2] == '1'
            self.facts['bios'], self.facts['os'] = ('O', bios), ('O', os_)
            self.fresh |= {'bios', 'os'}
            self.fresh.add('sup')
            if self.phase != 'handoff':
                return self.stay('observe.recorded')
            if not os_:
                return self.fail('firmware-revoked', 'fault.firmware-revoked')
            if bios:
                self.stable = None
                return self.stay('handoff.pending')
            if self.stable is not None and self.now > self.stable:
                self.deadline = None
                return self.go('handed-off', 'handoff.stable')
            if self.stable is None:
                self.stable = self.now
            return self.stay('handoff.complete')
        if kind == 'CTL':
            v = int(a[1], 16)
            self.facts['ctl'] = ('O', v)
            self.fresh.add('ctl')
            if self.phase != 'masking-smi':
                return self.stay('observe.recorded')
            if v & ENABLES == 0:
                self.deadline = None
                return self.go('smi-masked', 'smi.masked')
            return self.stay('smi.pending')
        if kind == 'CMD':
            v = int(a[1], 16)
            self.facts['cmd'] = ('O', v)
            self.fresh.add('cmd')
            if self.phase != 'resetting':
                return self.stay('observe.recorded')
            self.reset_seen = (self.reset_seen | 1) if v & HCRESET == 0 else (self.reset_seen & ~1)
            return self.reset_progress()
        if kind == 'STS':
            v = int(a[1], 16)
            if v & HSE:
                return self.fail('host-system-error', 'fault.host-system-error')
            self.facts['sts'] = ('O', v)
            self.fresh.add('sts')
            if self.phase == 'stopping-schedules':
                if v & (PSS | ASS) == 0:
                    self.deadline = None
                    return self.go('quiet', 'schedules.stopped')
                return self.stay('schedules.pending')
            if self.phase == 'halting':
                if v & HCH:
                    self.deadline = None
                    return self.go('halted', 'halt.observed')
                return self.stay('halt.pending')
            if self.phase == 'resetting':
                self.reset_seen = (self.reset_seen | 2) if v & HCH else (self.reset_seen & ~2)
                return self.reset_progress()
            return self.stay('observe.recorded')
        if kind == 'CFG':
            b = a[1] == '1'
            self.facts['cfg'] = ('O', b)
            if self.phase != 'resetting':
                return self.stay('observe.recorded')
            if b:
                self.reset_seen &= ~4
                if self.reset_seen & 1:
                    return self.fail('reset-unverified', 'fault.reset-unverified')
            else:
                self.reset_seen |= 4
            return self.reset_progress()
        raise ValueError(a)

    def reset_progress(self):
        if self.reset_seen == 7:
            self.deadline = None
            return self.go('reset', 'reset.verified')
        return self.stay('reset.pending')

    def request(self, step):
        pred = {'handoff': 'qualified', 'mask-smi': 'handed-off', 'stop-schedules': 'smi-masked', 'halt': 'quiet', 'reset': 'halted'}[step]
        if self.phase != pred:
            raise Refuse('wrong-phase')
        ready = self.readiness(step)
        cmd = self.facts['cmd'][1] if self.facts['cmd'][0] == 'O' else 0
        sts = self.facts['sts'][1] if self.facts['sts'][0] == 'O' else 0
        if ready == 'notneeded':
            return self.go('handed-off', 'handoff.not-needed') if step == 'handoff' else self.go('smi-masked', 'smi.not-needed')
        if step == 'handoff':
            self.need_fresh('sup')
            cap = self.legacy_cap()
            bios = self.facts['bios'][1] if self.facts['bios'][0] == 'O' else True
            os_ = self.facts['os'][1] if self.facts['os'][0] == 'O' else False
            if not bios and os_:
                return self.go('handed-off', 'handoff.already-owned')
            self.stable = None
            self.arm('handoff', 'firmware-no-release')
            if os_:
                return self.go('handoff', 'handoff.wait')
            self.fresh.discard('sup')
            return self.go('handoff', 'handoff.request', f'os:{cap:x}')
        if step == 'mask-smi':
            self.need_fresh('ctl')
            cap = self.legacy_cap()
            ctl = self.facts['ctl'][1] if self.facts['ctl'][0] == 'O' else ENABLES
            if ctl & ENABLES == 0:
                return self.go('smi-masked', 'smi.already-masked')
            self.fresh.discard('ctl')
            self.arm('smi', 'smi-not-disabled')
            return self.go('masking-smi', 'smi.request', f'ctl:{cap:x}:{ctl & 0xffff & ~ENABLES:x}')
        self.need_fresh('cmd', 'sts')
        if step == 'stop-schedules':
            enabled, active = cmd & (PSE | ASE) != 0, sts & (PSS | ASS) != 0
            if not enabled and not active:
                return self.go('quiet', 'schedules.already-off')
            self.arm('sched', 'schedule-stop-timeout')
            if not enabled:
                return self.go('stopping-schedules', 'schedules.wait')
            self.fresh -= {'cmd', 'sts'}
            return self.go('stopping-schedules', 'schedules.request', f'cmd:{cmd & ~(PSE | ASE):x}')
        if step == 'halt':
            if cmd & (PSE | ASE) or sts & (PSS | ASS):
                raise Refuse('schedules-still-active')
            if cmd & RS == 0 and sts & HCH:
                return self.go('halted', 'halt.already-halted')
            self.arm('halt', 'halt-timeout')
            if cmd & RS == 0:
                return self.go('halting', 'halt.wait')
            self.fresh -= {'cmd', 'sts'}
            return self.go('halting', 'halt.request', f'cmd:{cmd & ~RS:x}')
        if cmd & RS or not sts & HCH:
            raise Refuse('not-halted')
        self.fresh -= {'cmd', 'sts'}
        self.reset_seen = 0
        self.facts['cfg'] = ('U',)
        self.arm('reset', 'reset-timeout')
        return self.go('resetting', 'reset.request', f'cmd:{cmd | HCRESET:x}')


def replay(path):
    lines = [l.rstrip('\n') for l in open(path)]
    i = steps = traces = 0
    m = None
    while i < len(lines):
        if lines[i].startswith('T '):
            m, traces, seed = Model(), traces + 1, lines[i][2:]
            i += 1
            continue
        want = lines[i + 1]
        got = m.apply(lines[i])
        steps += 1
        if got != want:
            print(f'MISMATCH seed {seed} step: {lines[i]}\n  rust:   {want}\n  oracle: {got}')
            return False, steps, traces
        i += 2
    return True, steps, traces


if __name__ == '__main__':
    ok, steps, traces = replay(sys.argv[1])
    print(f'oracle: {traces} traces, {steps} steps, {"all agree" if ok else "MISMATCH"}')
    sys.exit(0 if ok else 1)
