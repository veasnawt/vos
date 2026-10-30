#!/usr/bin/env python3
"""USB.2-C4.1 independent reference oracle for the hosted xHCI initialization protocol model.

This is a second implementation of the protocol rules in another language, written from the
rule table (phase groups and ordered checks), not from the Rust control flow. It replays operation
traces exported by the Rust tests (`T <seed> <mode> <policy>` then `S <operation> => <outcome>`),
keeps its own state, predicts every outcome (accepted transition + rule + write event, or the named
refusal) and reports the first disagreement per trace. The Rust model and this oracle share no code.

Limits of its independence: both were written by the same author in one session; the structural
difference (language, declarative groups, separate state) is what is claimed, not organizational
independence. A disagreement means one of the two implementations misreads the rule table.
"""
import json
import sys

RING = 256
PAGE = 4096
TERMINAL = {'quiesced', 'failed', 'quarantined', 'refused'}
PRE_OWNED = {'qualified', 'handoff', 'handed-off', 'owned'}
STOPPING = {'halting', 'halted', 'resetting'}
LIVE = {'contained', 'programming', 'programmed', 'bus-mastering', 'event-ring-armed'}
ALLOC_OPEN = {'qualified', 'handoff', 'handed-off', 'owned', 'contained', 'programming', 'halting', 'halted', 'resetting'}
KINDS = ('command', 'event', 'dcbaa', 'erst')
PROFILE = ('qemu-10.2.1', 'unqualified')
DBC = ('absent', 'disabled', 'enabled', 'unknown')
TRI = ('no', 'yes', 'unknown')
FORBIDDEN = {'interrupt-enable': 'forbidden-interrupt-enable', 'port-write': 'forbidden-port-write',
             'slot-doorbell': 'forbidden-slot-doorbell', 'command': 'forbidden-command'}


class Out(Exception):
    """An operation result produced by a rule."""

    def __init__(self, text):
        self.text = text


def ok(frm, to, rule, ev='-'):
    return f'ok {frm}>{to} {rule} {ev}'


def err(reason):
    return f'err {reason}'


def ev(id_, target, width, value):
    return f'{id_}:{target}:{width}:{value}'


class Oracle:
    def __init__(self, policy):
        self.policy = policy
        self.phase = 'idle'
        self.f = None
        self.now = 0
        self.deadline = None
        self.tail = 'done'
        self.legacy = None  # None = absent, else [bios, os]
        self.bme = self.ver = self.armed = self.rs = self.hch = False
        self.written = 0
        self.pages = {k: ['free', 0] for k in KINDS}
        self.clean = False
        self.enq, self.pcs = 0, True
        self.staged = None
        self.link = False
        self.pending = None
        self.deq, self.ccs = 0, True
        self.unacked = self.step = 0
        self.last_erdp = 0
        self.cause = '-'

    # ---- helpers ----
    def exposed(self):
        return self.armed or any(p[0] == 'published' for p in self.pages.values())

    def go(self, to, rule, event='-'):
        frm = self.phase
        self.phase = to
        return ok(frm, to, rule, event)

    def stay(self, rule):
        return self.go(self.phase, rule)

    def fail(self, rule, reason):
        self.cause = reason
        self.deadline = None
        if self.armed and self.bme:
            self.tail = 'need-halt' if self.rs else ('need-bme-clear' if self.hch else 'await-halt')
        else:
            self.tail = 'done'
        return self.go('quarantined' if self.exposed() else 'failed', rule)

    def arm(self, reason, ticks):
        self.deadline = (self.now, ticks, reason)

    # ---- dispatch ----
    def apply(self, name, a):
        try:
            return self._apply(name, a)
        except Out as o:
            return o.text

    def _apply(self, name, a):
        if self.phase in TERMINAL:
            if self.phase in ('failed', 'quiesced') and name == 'DmaRelease':
                return self.release(a[0])
            if self.phase == 'quarantined':
                return self.tail_op(name, a)
            return err('after-terminal')
        if name == 'Forbidden':
            return err(FORBIDDEN[a[0]])
        if self.phase == 'idle':
            if name == 'Qualify':
                return self.qualify([int(x) for x in a])
            if name == 'Tick':
                return self.tick(int(a[0]))
            return err('not-qualified')
        if name == 'Qualify':
            return err('already-qualified')
        return getattr(self, 'op_' + name)(*a)

    # ---- terminal tail ----
    def tail_op(self, name, a):
        t = self.tail
        if t == 'need-halt' and name == 'ClearRunStop':
            self.rs = self.hch = False
            self.tail = 'await-halt'
            return ok('quarantined', 'quarantined', 'quarantine.halt', ev('N-16', 'usbcmd.run-stop', 32, 0))
        if t == 'await-halt' and name == 'ObserveStatus':
            self.hch = a[0] == '1'
            if self.hch:
                self.tail = 'need-bme-clear'
                return self.stay('quarantine.halted')
            return self.stay('halt.pending')
        if t == 'need-bme-clear' and name == 'ClearBusMaster':
            self.bme = self.ver = False
            self.tail = 'await-verify'
            return ok('quarantined', 'quarantined', 'quarantine.bme-clear', ev('N-17', 'pci.command.bme', 16, 0))
        if t == 'await-verify' and name == 'ObservePci':
            self.tail = 'done'
            if a[0] == '1':
                return self.stay('quarantine.bme-stuck')
            self.ver = True
            return self.stay('quarantine.verified')
        return err('after-terminal')

    # ---- qualification and time ----
    def qualify(self, v):
        (profile, version, page4k, slots, ports, intr, scratch, lp, lb, lo, dbc, iommu, ident, nosnoop, msi, bme,
         running, halted, hse, hce, connected, limit, _pad) = v
        checks = [
            (profile != 0, 'unqualified-platform'),
            (version != 0x100, 'unsupported-version'),
            (not page4k, 'page-size-unsupported'),
            (slots == 0 or ports == 0 or ports > 16 or intr == 0, 'bad-capabilities'),
            (limit < 0x200000 or limit > 1 << 30, 'bad-dma-limit'),
            (hse or hce, 'controller-fault'),
            (halted == running, 'inconsistent-run-state'),
            (iommu == 1, 'iommu-present'),
            (iommu == 2, 'iommu-unresolved'),
            (ident == 0, 'identity-dma-denied'),
            (ident == 2, 'identity-dma-unresolved'),
            (nosnoop == 1, 'no-snoop-enabled'),
            (nosnoop == 2, 'no-snoop-unresolved'),
            (msi, 'msi-enabled'),
            (dbc == 2, 'dbc-enabled'),
            (dbc == 3, 'dbc-unresolved'),
            (scratch > 0, 'scratchpads-required'),
            (lp and lb and lo, 'ownership-conflict'),
            (connected != 0, 'device-attached'),
        ]
        for cond, reason in checks:
            if cond:
                self.cause = reason
                self.phase = 'refused'
                return ok('idle', 'refused', 'fault.qualification-refused')
        self.f = dict(ports=ports, limit=limit)
        self.legacy = [bool(lb), bool(lo)] if lp else None
        self.bme, self.ver, self.rs, self.hch = bool(bme), True, bool(running), bool(halted)
        if self.legacy is None:
            return self.go('owned', 'qualify.no-legacy')
        return self.go('qualified', 'qualify.legacy')

    def tick(self, t):
        if t < self.now:
            return self.fail('fault.clock', 'clock-backwards')
        self.now = t
        if self.deadline and t - self.deadline[0] >= self.deadline[1]:
            return self.fail('fault.timeout', self.deadline[2])
        return self.stay('tick.advance')

    def op_Tick(self, t):
        return self.tick(int(t))

    # ---- ownership ----
    def op_RequestOsOwnership(self):
        if self.legacy is None:
            return err('no-legacy-capability')
        if self.legacy[1]:
            return err('already-os-owned')
        if self.phase != 'qualified':
            return err('wrong-phase')
        self.legacy[1] = True
        self.arm('handoff-timeout', 100)
        return self.go('handoff', 'handoff.request', ev('N-04', 'usblegsup.os-owned', 8, 1))

    def op_DisableLegacySmi(self):
        if self.legacy is None:
            return err('no-legacy-capability')
        bios, os_ = self.legacy
        if self.phase == 'handed-off' or (self.phase == 'qualified' and os_ and not bios):
            return self.go('owned', 'smi.disabled', ev('N-05', 'usblegctlsts.smi-enable', 32, 0))
        if self.phase in ('qualified', 'handoff'):
            return err('smi-before-ownership')
        return err('wrong-phase')

    def op_ObserveHandoff(self, bios, os_):
        bios, os_ = bios == '1', os_ == '1'
        if self.legacy is None:
            return err('no-legacy-capability')
        if self.phase == 'handoff':
            if not os_:
                return self.fail('fault.ownership', 'ownership-lost')
            if bios:
                return self.stay('handoff.pending')
            self.legacy = [False, True]
            self.deadline = None
            return self.go('handed-off', 'handoff.complete')
        if self.phase == 'qualified':
            if bios and os_:
                return self.fail('fault.ownership', 'ownership-conflict')
            self.legacy = [bios, os_]
            return self.stay('handoff.stable')
        if bios or not os_:
            return self.fail('fault.ownership', 'ownership-conflict' if bios else 'ownership-lost')
        return self.stay('handoff.stable')

    # ---- observations ----
    def op_ObserveStatus(self, hch, hse, hce):
        hch, fault = hch == '1', hse == '1' or hce == '1'
        if not fault:
            if self.phase in ('qualified', 'handoff', 'handed-off', 'resetting'):
                return err('observation-not-expected')
            if self.phase == 'owned' and self.rs:
                return err('halt-not-requested')
        self.hch = hch
        if fault:
            return self.fail('fault.status-error', 'controller-fault')
        p = self.phase
        if p == 'owned':
            return self.go('halted', 'halt.idle-observed') if hch else self.fail('fault.inconsistent', 'inconsistent-run-state')
        if p == 'halting':
            if hch:
                self.deadline = None
                return self.go('halted', 'halt.observed')
            return self.stay('halt.pending')
        if p in ('halted', 'contained', 'programming', 'programmed', 'bus-mastering'):
            return self.stay('status.stable') if hch else self.fail('fault.inconsistent', 'inconsistent-run-state')
        if p == 'event-ring-armed':
            if not hch:
                return self.fail('fault.inconsistent', 'inconsistent-run-state')
            self.clean = True
            return self.stay('armed.status-clean')
        if p == 'starting':
            if hch:
                return self.stay('run.pending')
            self.deadline = None
            return self.go('running', 'run.started')
        if p == 'running':
            return self.fail('fault.unexpected-halt', 'unexpected-halt') if hch else self.stay('status.stable')
        return err('wrong-phase')

    def op_ObserveReset(self, cnr, hch, usbcmd_zero, pointers_zero, bme, ports_quiet):
        if self.phase != 'resetting':
            return err('observation-not-expected')
        if cnr == '1':
            return self.stay('reset.pending')
        if not (hch == '1' and usbcmd_zero == '1' and pointers_zero == '1' and bme == '0' and ports_quiet == '1'):
            return self.fail('fault.reset-unverified', 'reset-unverified')
        self.deadline = None
        self.written = 0
        self.rs = False
        self.hch = True
        self.bme = False
        self.ver = True
        if self.exposed():
            self.cause = '-'
        return self.go('quiesced' if self.exposed() else 'contained', 'reset.quiesced' if self.exposed() else 'reset.verified')

    def op_ObservePci(self, bme, msi):
        if msi == '1':
            return self.fail('fault.msi', 'msi-enabled')
        if (bme == '1') != self.bme:
            return self.fail('fault.bme-mismatch', 'bus-master-mismatch')
        self.ver = True
        return self.stay('pci.stable')

    def op_ObservePorts(self, mask):
        mask = int(mask)
        ports = self.f['ports']
        if mask & ~((1 << ports) - 1):
            return err('port-mask-invalid')
        if mask == 0:
            return self.stay('ports.quiet')
        if self.phase == 'running':
            return self.stay('ports.device-noted')
        return self.fail('fault.device-attached', 'device-attached')

    # ---- halt, bus master, reset ----
    def op_ClearRunStop(self):
        if self.phase == 'owned':
            if not self.rs:
                return err('already-halted')
            self.rs, self.hch = False, False
            self.arm('halt-timeout', 10)
            return self.go('halting', 'halt.request', ev('N-01', 'usbcmd.run-stop', 32, 0))
        if self.phase in ('starting', 'running'):
            self.rs, self.hch = False, False
            self.staged = self.pending = None
            self.link = False
            self.arm('halt-timeout', 100)
            return self.go('halting', 'halt.request', ev('N-16', 'usbcmd.run-stop', 32, 0))
        if self.phase in ('qualified', 'handoff', 'handed-off'):
            return err('ownership-unresolved')
        return err('already-halted')

    def op_ClearBusMaster(self):
        if self.phase == 'halted':
            if not self.bme:
                return err('bus-master-already-clear')
            ident = 'N-17' if self.exposed() else 'N-02'
            self.bme = self.ver = False
            return self.go('halted', 'bme.clear', ev(ident, 'pci.command.bme', 16, 0))
        if self.phase in LIVE:
            return err('use-cancel')
        if self.phase == 'resetting':
            return err('wrong-phase')
        return err('bme-clear-before-halt')

    def op_AssertReset(self):
        if self.phase == 'halted':
            if self.bme:
                return err('reset-with-bus-master')
            if not self.ver:
                return err('bus-master-unverified')
            ident = 'N-18' if self.exposed() else 'N-03'
            self.arm('reset-timeout', 100)
            return self.go('resetting', 'reset.assert', ev(ident, 'usbcmd.hcrst', 32, 2))
        if self.phase in LIVE:
            return err('use-cancel')
        if self.phase == 'resetting':
            return err('wrong-phase')
        return err('reset-before-halt')

    def op_Cancel(self):
        if self.phase in LIVE:
            self.deadline = None
            return self.go('halted', 'cancel.halt')
        if self.phase in ('starting', 'running'):
            return err('use-clear-run-stop')
        if self.phase in STOPPING:
            return err('wrong-phase')
        return self.fail('fault.cancelled', 'cancelled')

    # ---- static registers ----
    def program_phase(self):
        p = self.phase
        if p in ('contained', 'programming'):
            return
        if p in PRE_OWNED:
            raise Out(err('program-before-containment'))
        if p in STOPPING:
            raise Out(err('programming-closed' if self.exposed() else 'program-before-containment'))
        raise Out(err('programming-closed'))

    def registered(self, bit, rule, event):
        self.written |= bit
        return self.go('programmed' if self.written == 31 else 'programming', rule, event)

    def page(self, k):
        return self.pages[k]

    def op_SetMaxSlots(self, n):
        self.program_phase()
        if self.written & 1:
            return err('register-repeated')
        if int(n) != 1:
            return err('slots-invalid')
        return self.registered(1, 'program.slots', ev('N-06', 'config', 32, 1))

    def op_SetDcbaap(self, addr):
        addr = int(addr)
        self.program_phase()
        if self.written & 2:
            return err('register-repeated')
        if self.page('dcbaa')[0] != 'prepared':
            return err('page-not-prepared')
        if addr != self.page('dcbaa')[1]:
            return err('pointer-mismatch')
        self.page('dcbaa')[0] = 'published'
        return self.registered(2, 'program.dcbaap', ev('N-07', 'dcbaap', 64, addr))

    def op_SetCommandRing(self, addr, rcs):
        addr = int(addr)
        if self.phase in ('starting', 'running'):
            return err('crcr-while-running')
        self.program_phase()
        if self.written & 4:
            return err('register-repeated')
        if self.page('command')[0] != 'prepared':
            return err('page-not-prepared')
        if rcs != '1':
            return err('cycle-state-invalid')
        if addr != self.page('command')[1]:
            return err('pointer-mismatch')
        self.page('command')[0] = 'published'
        return self.registered(4, 'program.crcr', ev('N-08', 'crcr', 64, addr | 1))

    def op_SetErstSize(self, n):
        self.program_phase()
        if self.written & 8:
            return err('register-repeated')
        if self.page('erst')[0] != 'prepared':
            return err('page-not-prepared')
        if int(n) != 1:
            return err('erst-size-invalid')
        return self.registered(8, 'program.erstsz', ev('N-09', 'ir0.erstsz', 32, 1))

    def op_SetEventDequeue(self, addr, ehb):
        addr, ehb = int(addr), ehb == '1'
        if self.phase == 'running':
            if addr != self.page('event')[1] + 16 * self.deq:
                return err('erdp-mismatch')
            if not ehb:
                return err('ehb-not-set')
            if addr == self.last_erdp:
                return err('erdp-repeat')
            self.last_erdp = addr
            self.unacked = 0
            return self.go('running', 'ack.erdp', ev('N-15', 'ir0.erdp', 64, addr | 8))
        if self.phase == 'starting':
            return err('not-running')
        self.program_phase()
        if self.written & 16:
            return err('register-repeated')
        if self.page('event')[0] != 'prepared':
            return err('page-not-prepared')
        if ehb:
            return err('ehb-not-expected')
        if addr != self.page('event')[1]:
            return err('pointer-mismatch')
        self.page('event')[0] = 'published'
        self.last_erdp = addr
        return self.registered(16, 'program.erdp', ev('N-10', 'ir0.erdp', 64, addr))

    # ---- bus master, event ring, run ----
    def op_SetBusMaster(self):
        p = self.phase
        if p == 'programmed':
            if self.bme:
                return err('bus-master-already-set')
            self.bme, self.armed, self.ver = True, True, False
            return self.go('bus-mastering', 'bme.set', ev('N-11', 'pci.command.bme', 16, 1))
        if p in ('bus-mastering', 'event-ring-armed', 'starting', 'running'):
            return err('bus-master-already-set')
        if p in STOPPING:
            return err('wrong-phase')
        return err('bus-master-before-programming')

    def op_SetErstBase(self, addr):
        addr = int(addr)
        p = self.phase
        if p == 'bus-mastering':
            if not self.ver:
                return err('bus-master-unverified')
            if self.page('erst')[0] != 'prepared':
                return err('page-not-prepared')
            if addr != self.page('erst')[1]:
                return err('pointer-mismatch')
            self.page('erst')[0] = 'published'
            self.clean = False
            return self.go('event-ring-armed', 'erstba.armed', ev('N-12', 'ir0.erstba', 64, addr))
        if p in ('contained', 'programming'):
            return err('erstba-before-static')
        if p == 'programmed':
            return err('erstba-without-bus-master')
        if p in ('event-ring-armed', 'starting', 'running'):
            return err('register-repeated')
        if p in PRE_OWNED:
            return err('program-before-containment')
        return err('wrong-phase')

    def op_SetRunStop(self):
        p = self.phase
        if p == 'event-ring-armed':
            if not self.clean:
                return err('status-not-checked')
            self.rs, self.hch = True, False
            self.arm('start-timeout', 100)
            return self.go('starting', 'run.start', ev('N-13', 'usbcmd.run-stop', 32, 1))
        if p in ('contained', 'programming', 'programmed', 'bus-mastering'):
            return err('run-before-event-ring')
        if p in ('starting', 'running'):
            return err('already-running')
        if p in PRE_OWNED:
            return err('program-before-containment')
        return err('wrong-phase')

    # ---- command and event rings ----
    def running(self):
        if self.phase != 'running':
            raise Out(err('not-running'))

    def op_PublishNoop(self, index, cycle):
        index, cycle = int(index), cycle == '1'
        self.running()
        if self.pending is not None or self.staged is not None:
            return err('command-outstanding')
        if index != self.enq or index > RING - 2:
            return err('command-index-mismatch')
        if cycle != self.pcs:
            return err('command-cycle-mismatch')
        self.staged = index
        if index == RING - 2:
            self.link = True
            self.enq = RING - 1
        else:
            self.enq += 1
        return self.stay('cmd.noop-staged')

    def op_PublishLink(self, index, cycle, toggle, target):
        index, cycle, toggle, target = int(index), cycle == '1', toggle == '1', int(target)
        self.running()
        if not self.link:
            return err('link-not-expected')
        if index != self.enq:
            return err('command-index-mismatch')
        if cycle != self.pcs:
            return err('command-cycle-mismatch')
        if not toggle:
            return err('link-toggle-missing')
        if target != self.page('command')[1]:
            return err('link-target-mismatch')
        self.link = False
        self.enq = 0
        self.pcs = not self.pcs
        return self.stay('cmd.link-published')

    def op_RingCommandDoorbell(self):
        self.running()
        if self.pending is not None:
            return err('command-outstanding')
        if self.staged is None:
            return err('nothing-staged')
        if self.link:
            return err('link-missing')
        if self.unacked > 0:
            return err('ack-missing')
        self.pending, self.staged = self.staged, None
        self.arm('command-timeout', 100)
        return self.go('running', 'doorbell.noop', ev('N-14', 'doorbell0', 32, 0))

    def op_BeginPollStep(self):
        self.running()
        self.step = 0
        return self.stay('poll.step')

    def advance(self):
        if self.deq == RING - 1:
            self.deq, self.ccs = 0, not self.ccs
        else:
            self.deq += 1
        self.unacked += 1
        self.step += 1

    def op_ConsumeEvent(self, *a):
        self.running()
        if a == ('none',):
            return self.stay('event.empty')
        index, cycle, kind, pointer, code, param, slot, vf, port = [int(x) for x in a]
        if self.step >= 32:
            return err('poll-budget-exceeded')
        if self.unacked >= 32:
            return err('ack-missing')
        if index != self.deq:
            return err('event-index-mismatch')
        if bool(cycle) != self.ccs:
            return err('event-cycle-mismatch')
        if kind == 33:
            if self.pending is None:
                return self.fail('fault.event', 'unexpected-completion')
            if pointer != self.page('command')[1] + 16 * self.pending:
                return self.fail('fault.event', 'completion-pointer-mismatch')
            if code != 1:
                return self.fail('fault.event', 'command-failed')
            if slot or vf or param:
                return self.fail('fault.event', 'completion-field-invalid')
            self.pending = None
            self.deadline = None
            self.advance()
            return self.stay('event.completion')
        if kind == 34:
            if port == 0 or port > self.f['ports']:
                return self.fail('fault.event', 'port-out-of-range')
            self.advance()
            return self.stay('event.port-change')
        return self.fail('fault.event', 'unexpected-event-type')

    # ---- DMA page lifecycle ----
    def alloc_open(self):
        if self.phase not in ALLOC_OPEN or self.exposed():
            raise Out(err('allocation-closed'))

    def op_DmaAllocate(self, kind, addr):
        addr = int(addr)
        self.alloc_open()
        if self.page(kind)[0] != 'free':
            return err('page-already-owned')
        if addr % PAGE:
            return err('misaligned')
        if not (addr >= 0x100000 and addr + PAGE <= self.f['limit']):
            return err('address-range')
        if any(p[0] != 'free' and abs(p[1] - addr) < PAGE for p in self.pages.values()):
            return err('page-overlap')
        self.pages[kind] = ['allocated', addr]
        return self.stay('dma.allocate')

    def owned_state(self, kind, want):
        s = self.page(kind)[0]
        if s == 'free':
            raise Out(err('page-not-owned'))
        if s != want:
            raise Out(err('page-state'))

    def op_DmaZero(self, kind):
        self.alloc_open()
        self.owned_state(kind, 'allocated')
        self.page(kind)[0] = 'zeroed'
        return self.stay('dma.zero')

    def prepare(self, kind, extra=None):
        self.alloc_open()
        self.owned_state(kind, 'zeroed')
        if extra:
            extra()
        self.page(kind)[0] = 'prepared'
        return self.stay('dma.prepare')

    def op_PrepareCommandRing(self):
        return self.prepare('command')

    def op_PrepareEventRing(self):
        return self.prepare('event')

    def op_PrepareDcbaa(self, entry0):
        def chk():
            if int(entry0) != 0:
                raise Out(err('scratchpad-pointer-unsupported'))
        return self.prepare('dcbaa', chk)

    def op_PrepareErst(self, segment, size):
        segment, size = int(segment), int(size)

        def chk():
            ev_state, ev_addr = self.page('event')
            if ev_state not in ('prepared', 'published'):
                raise Out(err('dependency-not-ready'))
            if segment != ev_addr:
                raise Out(err('erst-segment-mismatch'))
            if size < 16:
                raise Out(err('erst-segment-too-small'))
            if size != RING:
                raise Out(err('erst-segment-size-unsupported'))
        return self.prepare('erst', chk)

    def op_DmaRelease(self, kind):
        return self.release(kind)

    def release(self, kind):
        s = self.page(kind)[0]
        terminal = self.phase in TERMINAL
        if s == 'free':
            return err('page-not-owned')
        if s == 'published':
            if self.phase != 'quiesced':
                return err('page-still-published')
            if self.policy == 'never':
                return err('release-forbidden-by-policy')
            self.pages[kind] = ['free', 0]
            return self.stay('cleanup.release')
        self.pages[kind] = ['free', 0]
        return self.stay('cleanup.release' if terminal else 'dma.release')


def main(argv):
    if len(argv) < 2:
        sys.exit('usage: xhci_init_oracle.py TRACE [--json]')
    stats = {'traces': 0, 'steps': 0, 'accepted': 0, 'refused': 0, 'mismatches': 0, 'first_mismatches': []}
    rules, reasons = set(), set()
    o = None
    bad_trace = False
    for line in open(argv[1]):
        line = line.rstrip('\n')
        if line.startswith('T '):
            _, seed, mode, policy = line.split()
            o, bad_trace = Oracle(policy), False
            stats['traces'] += 1
            continue
        _, rest = line.split(' ', 1)
        op, expect = rest.split(' => ')
        parts = op.split()
        got = o.apply(parts[0], parts[1:]) + ' @ ' + o.cause
        stats['steps'] += 1
        if got.startswith('ok'):
            stats['accepted'] += 1
            rules.add(got.split()[2])
        else:
            stats['refused'] += 1
            reasons.add(got.split()[1])
        if got != expect and not bad_trace:
            stats['mismatches'] += 1
            bad_trace = True
            if len(stats['first_mismatches']) < 5:
                stats['first_mismatches'].append({'trace': stats['traces'], 'op': op, 'oracle': got, 'model': expect})
    stats['rules_seen'] = len(rules)
    stats['reasons_seen'] = len(reasons)
    print(json.dumps(stats, indent=1))
    sys.exit(0 if stats['mismatches'] == 0 and stats['steps'] > 0 else 1)


if __name__ == '__main__':
    main(sys.argv)
