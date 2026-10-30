#!/usr/bin/env python3
"""USB.2-C4.2a independent oracle for the hosted backend session.

Replays traces exported by the Rust tests (`T`, then per request `Q <request>`, the recorded backend interactions,
and `=> <outcome>`). Written from the rule tables, in another language, with its own state. It reuses the already
independent C4.1 Python machine oracle (scripts/xhci_init_oracle.py) as the machine model, so it checks the *session*
layer on top of it. For every request it predicts (1) exactly which backend calls the session makes, in order, and
verifies them against the recording, and (2) the outcome: the machine transition or named refusal, and whether the
session closed and why. Limits of the independence claim: same author, same session; the difference is structural.
"""
import copy
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xhci_init_oracle as C41  # noqa: E402

WIDTH = {'pci.command': 16, 'pci.msi-control': 16, 'pci.msix-control': 16, 'pcie.device-control': 16, 'hciversion': 16,
         'hcsparams1': 32, 'hcsparams2': 32, 'hccparams1': 32, 'pagesize': 32, 'usbcmd': 32, 'usbsts': 32, 'config': 32,
         'crcr': 64, 'dcbaap': 64, 'erstsz': 32, 'erstba': 64, 'erdp': 64, 'usblegsup': 32, 'dbc.dcctrl': 32, 'portsc': 32}
ONES = {16: 0xFFFF, 32: 0xFFFFFFFF, 64: 0xFFFFFFFFFFFFFFFF}
FAULT_CAUSE = {'all-ones': 'read-all-ones', 'width': 'read-width', 'misaligned': 'read-misaligned', 'out-of-extent': 'read-out-of-extent',
               'torn': 'read-torn', 'partial': 'read-partial', 'budget': 'read-budget', 'lease-lost': 'lease-lost', 'other': 'read-other',
               'not-present': 'read-other'}
WRITE_CAUSE = {'refused': 'write-refused', 'bad-ticket': 'write-bad-ticket', 'unverified': 'write-unverified', 'fault': 'write-failed'}
RSVD_STS = 0xFFFFE0E2
ST, PCI, HND, PRT, EVT = range(5)
TERMINAL = {'quiesced', 'failed', 'quarantined', 'refused'}

# write kinds: (requires, invalidates, preconditions[(reg, mask, expect)], reads-back (reg, mask, expect-or-'ev'), reduces exposure)
HCH, HSE, CNR, HCE = 1, 1 << 2, 1 << 11, 1 << 12
KINDS = {
    'reqos':    ([HND], [HND], [('usblegsup', 1 << 24, 0)], ('usblegsup', 1 << 24, 1 << 24), False),
    'smi':      ([HND], [HND], [('usblegsup', (1 << 24) | (1 << 16), 1 << 24)], None, False),
    'clearrs':  ([ST], [ST], [('usbcmd', 1, 1)], ('usbcmd', 1, 0), True),
    'clearbme': ([ST, PCI], [PCI], [('usbsts', HCH, HCH), ('usbcmd', 1, 0), ('pci.command', 4, 4)], ('pci.command', 4, 0), True),
    'reset':    ([ST, PCI], [ST, PRT, EVT], [('pci.command', 4, 0), ('usbsts', HCH, HCH)], None, True),
    'slots':    ([ST], [], [('usbsts', CNR | HCH, HCH)], ('config', 0xff, 1), False),
    'dcbaap':   ([ST], [], [('usbsts', CNR | HCH, HCH)], ('dcbaap', ~0x3f & (2**64 - 1), 'ev'), False),
    'crcr':     ([ST], [], [('usbsts', CNR | HCH, HCH)], None, False),
    'erstsz':   ([ST], [], [('usbsts', CNR | HCH, HCH)], ('erstsz', 0xffff, 1), False),
    'erdpinit': ([ST], [], [('usbsts', CNR | HCH, HCH)], ('erdp', ~0xf & (2**64 - 1), 'ev'), False),
    'setbme':   ([ST, PCI], [PCI], [('usbsts', HCH, HCH), ('pci.command', 4, 0)], ('pci.command', 4, 4), False),
    'erstba':   ([ST, PCI], [ST], [('pci.command', 4, 4), ('usbsts', HCH | HCE, HCH)], ('erstba', ~0x3f & (2**64 - 1), 'ev'), False),
    'setrs':    ([ST, PCI], [ST], [('usbsts', HCE | HSE, 0), ('pci.command', 4, 4)], ('usbcmd', 1, 1), False),
    'doorbell': ([ST], [ST, EVT], [('usbsts', HCH | HCE | HSE, 0)], None, False),
    'ack':      ([EVT], [EVT], [('usbsts', HCE, 0)], None, False),
}
OPKIND = {'RequestOsOwnership': 'reqos', 'DisableLegacySmi': 'smi', 'ClearRunStop': 'clearrs', 'ClearBusMaster': 'clearbme', 'AssertReset': 'reset',
          'SetMaxSlots': 'slots', 'SetDcbaap': 'dcbaap', 'SetCommandRing': 'crcr', 'SetErstSize': 'erstsz', 'SetBusMaster': 'setbme',
          'SetErstBase': 'erstba', 'SetRunStop': 'setrs', 'RingCommandDoorbell': 'doorbell'}
OBSERVE_OPS = {'Qualify', 'Tick', 'ObserveHandoff', 'ObserveStatus', 'ObserveReset', 'ObservePci', 'ObservePorts', 'ConsumeEvent'}


class Mismatch(Exception):
    pass


class Out(Exception):
    def __init__(self, text):
        self.text = text


def bit(v, n):
    return bool(v >> n & 1)


DSTATES = ['free', 'allocated', 'zeroed', 'prepared', 'published', 'device-owned', 'quiesced', 'quarantined', 'retained']
CORE = ['command', 'event', 'dcbaa', 'erst']
ARRAY, BUF0, BUDGET, SPEC_MAX, PAGE_BYTES, MIN_ADDR, CAP = 4, 5, 64, 1023, 4096, 0x100000, 80
MAX_RECORDS = BUF0 + BUDGET


class Refused(Exception):
    def __init__(self, reason):
        super().__init__(reason)
        self.reason = reason


class LedgerOracle:
    """Written from the ownership rules (software ownership of DMA pages), not from the Rust ledger."""

    def __init__(self, limit, ac64, scratch, policy):
        if scratch > SPEC_MAX:
            raise Refused('scratchpads-invalid')
        if scratch > BUDGET:
            raise Refused('scratchpads-exceed-budget')
        if limit < 2 * MIN_ADDR or limit > 1 << 48:
            raise Refused('bad-limit')
        self.limit, self.ac64, self.scratch, self.policy = limit, ac64, scratch, policy
        self.st = ['free'] * MAX_RECORDS
        self.ad = [0] * MAX_RECORDS
        self.quarantined = 0
        self.closed = False

    def slot(self, item):
        if item[0] == 'core':
            return CORE.index(item[1])
        if item[0] == 'array' and self.scratch > 0:
            return ARRAY
        if item[0] == 'buf' and item[1] < self.scratch:
            return BUF0 + item[1]
        raise Refused('no-such-item')

    def text(self):
        out = [self.st[i] for i in range(4)] + [self.st[ARRAY] if self.scratch else '-']
        out += [self.st[BUF0 + b] for b in range(self.scratch)]
        return ''.join('-' if s == '-' else format(DSTATES.index(s), 'x') for s in out)

    def allocate(self, item, addr):
        i = self.slot(item)
        if self.closed:
            raise Refused('pool-closed')
        if self.st[i] != 'free':
            raise Refused('already-owned')
        if addr % PAGE_BYTES:
            raise Refused('misaligned')
        limit = self.limit if self.ac64 else min(self.limit, 1 << 32)
        if not (addr >= MIN_ADDR and addr + PAGE_BYTES < 1 << 64 and addr + PAGE_BYTES <= limit):
            raise Refused('address-range')
        if any(self.st[j] != 'free' and abs(self.ad[j] - addr) < PAGE_BYTES for j in range(MAX_RECORDS)):
            raise Refused('overlap')
        self.st[i], self.ad[i] = 'allocated', addr

    def expect(self, item, want):
        i = self.slot(item)
        s = self.st[i]
        if s == 'free':
            raise Refused('not-owned')
        if s in ('quarantined', 'retained'):
            raise Refused('quarantined')
        if s != want:
            raise Refused('bad-state')
        return i

    def zero(self, item):
        self.st[self.expect(item, 'allocated')] = 'zeroed'

    def buffers(self, st):
        return all(self.st[BUF0 + b] == st for b in range(self.scratch))

    def prepare(self, item):
        i = self.expect(item, 'zeroed')
        if item == ('core', 'erst'):
            ready = self.st[1] in ('prepared', 'published')
        elif item[0] == 'array':
            ready = self.buffers('prepared')
        elif item == ('core', 'dcbaa') and self.scratch > 0:
            ready = self.st[ARRAY] == 'prepared'
        else:
            ready = True
        if not ready:
            raise Refused('dependency-not-ready')
        self.st[i] = 'prepared'

    def publish(self, kind):
        if kind == 'dcbaa':
            raise Refused('bad-state')
        self.st[self.expect(('core', kind), 'prepared')] = 'published'

    def publish_dcbaa(self):
        d = self.expect(('core', 'dcbaa'), 'prepared')
        if self.scratch > 0:
            if self.st[ARRAY] != 'prepared' or not self.buffers('prepared'):
                raise Refused('scratchpad-incomplete')
            self.st[ARRAY] = 'device-owned'
            for b in range(self.scratch):
                self.st[BUF0 + b] = 'device-owned'
        self.st[d] = 'published'

    def quiesce(self):
        any_ = False
        for i in range(MAX_RECORDS):
            if self.st[i] in ('published', 'device-owned'):
                self.st[i] = 'quiesced'
                any_ = True
        if not any_:
            raise Refused('nothing-to-quiesce')

    def release(self, item):
        i = self.slot(item)
        s = self.st[i]
        if s == 'free':
            raise Refused('not-owned')
        if s in ('quarantined', 'retained'):
            raise Refused('quarantined')
        if s in ('published', 'device-owned'):
            raise Refused('still-published')
        if s == 'quiesced' and self.policy == 'never':
            raise Refused('release-forbidden-by-policy')
        self.st[i], self.ad[i] = 'free', 0

    def quarantine_all(self):
        for i in range(MAX_RECORDS):
            if self.st[i] not in ('free', 'quarantined', 'retained'):
                if self.quarantined < CAP:
                    self.st[i] = 'quarantined'
                    self.quarantined += 1
                else:
                    self.st[i] = 'retained'
                    self.closed = True

    def scratch_overlaps(self, addr):
        return any(self.st[j] != 'free' and abs(self.ad[j] - addr) < PAGE_BYTES for j in range(ARRAY, MAX_RECORDS))

    def array_ready(self, entry0):
        return self.scratch == 0 or (self.st[ARRAY] == 'prepared' and self.ad[ARRAY] == entry0)


def parse_item(text):
    if text == 'array':
        return ('array',)
    kind, _, v = text.partition(':')
    return ('core', v) if kind == 'core' else ('buf', int(v))


class SessionOracle:
    def __init__(self, policy, crcr_reads_zero=False):
        self.m = C41.Oracle(policy)
        self.crcr_reads_zero = crcr_reads_zero
        self.bound = None  # (ident, topo, session)
        self.ports = 0
        self.last_seq = None
        self.last_tick = 0
        self.last_now = 0
        self.last_write_seq = 0
        self.fresh = [False] * 5
        self.know_bme = self.know_rs = self.know_hch = None  # None = unknown
        self.last_cnr = None
        self.nonce = 1
        self.closed = None
        self.latched = False
        self.tail_dead = False
        self.led = None
        self.policy = policy
        self.mirrored = {k: ('free', 0) for k in CORE}
        self.quiesced = False
        self.io = []
        self.pos = 0

    # ---- recorded interactions ----
    def take(self, *kinds):
        if self.pos >= len(self.io):
            raise Mismatch(f'the oracle expected a backend call ({"/".join(kinds)}) the session did not make')
        line = self.io[self.pos].split()
        if line[0] not in kinds:
            raise Mismatch(f'expected {kinds}, recorded {self.io[self.pos]!r}')
        self.pos += 1
        return line

    def done(self):
        if self.pos != len(self.io):
            raise Mismatch(f'the session made unexpected backend calls: {self.io[self.pos:]}')

    # ---- helpers ----
    def sealed(self):
        return self.closed is not None or self.m.phase in TERMINAL

    def stamp(self, seq, tick, session):
        if self.bound is not None and self.bound[2] != session:
            return 'session-mismatch'
        if self.last_seq is not None:
            if seq == self.last_seq:
                return 'sequence-duplicate'
            if seq < self.last_seq:
                return 'sequence-reordered'
            if seq > self.last_seq + 1:
                return 'sequence-gap'
        if tick < self.last_tick:
            return 'clock-backwards'
        self.last_seq, self.last_tick = seq, tick
        return None

    def rd(self, reg, index):
        """-> (value or None for an absent optional register); raises Out-less Cause via return tuple."""
        line = self.take('R', 'RF')
        if line[0] == 'RF':
            _, r, i, kind, seq, tick, sess = line
            c = self.stamp(int(seq), int(tick), int(sess))
            if c:
                raise Cause(c)
            if kind == 'not-present':
                return None
            raise Cause(FAULT_CAUSE[kind])
        _, r, i, width, value, seq, tick, sess = line
        c = self.stamp(int(seq), int(tick), int(sess))
        if c:
            raise Cause(c)
        if r != reg or int(i) != index:
            raise Cause('wrong-register')
        if width != f'w{WIDTH[reg]}':
            raise Cause('width-mismatch')
        if int(value) == ONES[WIDTH[reg]]:
            raise Cause('read-all-ones')
        return int(value)

    def rd_need(self, reg, index=0):
        v = self.rd(reg, index)
        if v is None:
            raise Cause('read-other')
        return v

    def rd_stamped(self, ok_kind, fail_kind):
        line = self.take(ok_kind, fail_kind)
        if line[0] == fail_kind:
            _, kind, seq, tick, sess = line
            c = self.stamp(int(seq), int(tick), int(sess))
            if c:
                raise Cause(c)
            raise Cause(FAULT_CAUSE[kind])
        *fields, seq, tick, sess = line[1:]
        c = self.stamp(int(seq), int(tick), int(sess))
        if c:
            raise Cause(c)
        return fields

    def text(self, a):
        return 'done ' + (a or '-') + ' closed=-'

    def settle(self, a):
        # a is the machine's text for an accepted transition
        if a.startswith('ok ') and a.split()[1].split('>')[1] in TERMINAL and not self.latched:
            self.latched = True
            self.take('L')

    def close(self, cause):
        if self.sealed():
            self.tail_dead = True
            if self.led is not None:
                self.led.quarantine_all()
            return f'done - closed={cause}'
        self.closed = cause
        acc = None
        if self.m.phase != 'idle':
            r = self.m.apply('BackendFault', [])
            acc = r if r.startswith('ok') else None
        if not self.latched:
            self.latched = True
            self.take('L')
        return f'done {acc or "-"} closed={cause}'

    # ---- requests ----
    def request(self, req, io):
        self.io, self.pos = io, 0
        try:
            out = self._request(req)
        except Cause as c:  # an uncaught cause means the oracle forgot a close(); treat as that close
            out = self.close(c.cause)
        c = self.sync()
        if c is not None:
            closed = self.close(c)
            if out.startswith('done') and 'closed=-' in out:
                out = out.split(' closed=')[0] + ' closed=' + c
            elif not out.startswith('done'):
                out = closed
        self.done()
        return out

    def ledger_text(self):
        return '-' if self.led is None else self.led.text()

    def sync(self):
        """Replay what the machine accepted onto the ledger; a ledger that cannot follow is a model inconsistency."""
        if self.led is None:
            return None
        try:
            for k in CORE:
                a, b = self.mirrored[k], tuple(self.m.pages[k])
                if a == b:
                    continue
                if a[0] == 'free' and b[0] == 'allocated':
                    self.led.allocate(('core', k), b[1])
                elif a[0] == 'allocated' and b[0] == 'zeroed':
                    self.led.zero(('core', k))
                elif a[0] == 'zeroed' and b[0] == 'prepared':
                    self.led.prepare(('core', k))
                elif a[0] == 'prepared' and b[0] == 'published':
                    self.led.publish_dcbaa() if k == 'dcbaa' else self.led.publish(k)
                elif b[0] == 'free':
                    self.led.release(('core', k))
                else:
                    raise Refused('bad-state')
                self.mirrored[k] = b
            if self.m.phase == 'quiesced' and not self.quiesced:
                self.quiesced = True
                try:
                    self.led.quiesce()
                except Refused as r:
                    if r.reason != 'nothing-to-quiesce':
                        raise
        except Refused:
            return 'ledger-disagrees'
        return None

    def scratch(self, parts):
        verb, item = parts[0], parse_item(parts[1])
        if item[0] == 'core':
            return 'err s:core-page-via-operation'
        if self.led is None:
            return 'err s:not-qualified'
        if verb != 'release' and not (self.m.phase in C41.ALLOC_OPEN and not self.m.exposed()):
            return 'err m:allocation-closed'
        try:
            if verb == 'alloc':
                self.led.allocate(item, int(parts[2]))
            elif verb == 'zero':
                self.led.zero(item)
            elif verb == 'prepare':
                self.led.prepare(item)
            else:
                self.led.release(item)
        except Refused as r:
            return 'err l:' + r.reason
        return 'done - closed=-'

    def _request(self, req):
        parts = req.split()
        head = parts[0]
        if head == 'bind':
            return self.bind()
        if head == 'op' and parts[1] in OBSERVE_OPS:
            return 'err s:use-backend-observation'
        if self.bound is None:
            return 'err s:' + ('closed' if self.closed else 'not-bound')
        tail_ok = (head in ('status', 'pci') or (head == 'op' and parts[1] in ('ClearRunStop', 'ClearBusMaster', 'DmaRelease'))
                   or (head == 'scratch' and parts[1] == 'release'))
        if self.sealed() and not (tail_ok and self.m.phase != 'idle' and not self.tail_dead):
            return 'err s:closed'
        if head == 'qualify':
            return self.qualify()
        if head == 'tick':
            return self.tick()
        if head in ('status', 'pci', 'reset', 'handoff', 'ports', 'event'):
            if self.m.phase == 'idle':
                return 'err s:not-qualified'
            return self.run(getattr(self, 'obs_' + head))
        if head == 'scratch':
            return self.scratch(parts[1:])
        name, args = parts[1], parts[2:]
        if name == 'PrepareDcbaa' and self.led is not None and not self.led.array_ready(int(args[0])):
            return 'err s:scratchpad-array-not-ready'
        kind = OPKIND.get(name)
        if name == 'SetEventDequeue':
            kind = 'ack' if self.m.phase == 'running' else 'erdpinit'
        if kind and self.m.phase != 'idle':
            return self.write(name, args, kind)
        trial = copy.deepcopy(self.m)
        r = trial.apply(name, args)
        if r.startswith('err'):
            return 'err m:' + r.split()[1]
        if name == 'DmaAllocate' and self.led is not None and self.led.scratch_overlaps(int(args[1])):
            return 'err l:overlap'
        self.m = trial
        self.settle(r)
        return self.text(r)

    def run(self, f):
        try:
            r = f()
        except Cause as c:
            return self.close(c.cause)
        if r.startswith('err'):
            return 'err m:' + r.split()[1]
        self.settle(r)
        return self.text(r)

    def bind(self):
        if self.bound is not None:
            return 'err s:already-bound'
        if self.sealed():
            return 'err s:closed'
        first = self.take('I', 'IF')
        sess = int(first[-1])
        self.bound = ((), (), sess)
        try:
            self.io_pos_back = self.pos - 1
            self.pos -= 1
            fields = self.rd_stamped('I', 'IF')
            ident = tuple(int(x) for x in fields)
            topo = tuple(int(x) for x in self.rd_stamped('T', 'TF'))
            if topo[0] != 1 or not topo[1]:
                raise Cause('ambiguous-topology')
            self.bound = (ident, topo, sess)
            return 'done - closed=-'
        except Cause as c:
            self.bound = None
            self.closed = c.cause
            self.latched = True
            self.take('L')
            return f'done - closed={c.cause}'

    def qualify(self):
        if self.m.phase != 'idle':
            return 'err m:already-qualified'
        try:
            plat = [int(x) for x in self.rd_stamped('P', 'PF')]  # profile iommu ident limit
            ver = self.rd_need('hciversion')
            hcs1 = self.rd_need('hcsparams1')
            hcs2 = self.rd_need('hcsparams2')
            hcc1 = self.rd_need('hccparams1')
            pagesize = self.rd_need('pagesize')
            cmd = self.rd_need('pci.command')
            devctl = self.rd('pcie.device-control', 0)
            msi = self.rd('pci.msi-control', 0)
            msix = self.rd('pci.msix-control', 0)
            usbcmd = self.rd_need('usbcmd')
            usbsts = self.rd_need('usbsts')
            legacy = self.rd('usblegsup', 0)
            dbc = self.rd('dbc.dcctrl', 0)
            ports = hcs1 >> 24
            connected = 0
            if ports <= 16:
                for p in range(ports):
                    if bit(self.rd_need('portsc', p), 0):
                        connected |= 1 << p
            scratch = (((hcs2 >> 21) & 0x1f) << 5) | ((hcs2 >> 27) & 0x1f)
            facts = [plat[0], ver, int(pagesize & 1 != 0), hcs1 & 0xff, ports, (hcs1 >> 8) & 0x7ff, scratch,
                     0 if legacy is None else 1, 0 if legacy is None else int(bit(legacy, 16)), 0 if legacy is None else int(bit(legacy, 24)),
                     0 if dbc is None else (2 if bit(dbc, 31) else 1), plat[1], plat[2], 2 if devctl is None else (1 if bit(devctl, 11) else 0),
                     int((msi is not None and bit(msi, 0)) or (msix is not None and bit(msix, 15))), int(bit(cmd, 2)), int(bit(usbcmd, 0)),
                     int(bit(usbsts, 0)), int(bool(usbsts & HSE)), int(bool(usbsts & HCE)), connected, plat[3], 0]
            if usbsts & RSVD_STS:
                raise Cause('reserved-bits-set')
        except Cause as c:
            return self.close(c.cause)
        r = self.m.apply('Qualify', [str(x) for x in facts])
        if r.startswith('ok'):
            self.ports = min(ports, 16)
            self.know_bme, self.know_rs, self.know_hch = bool(facts[15]), bool(facts[16]), bool(facts[17])
            self.last_cnr = bool(usbsts & CNR)
            self.fresh = [True, True, facts[7] == 1, True, False]
            if self.m.phase not in TERMINAL:
                try:
                    self.led = LedgerOracle(plat[3], bit(hcc1, 0), scratch, self.policy)
                except Refused:
                    return self.close('ledger-disagrees')
            self.settle(r)
            return self.text(r)
        return 'err m:' + r.split()[1]

    def tick(self):
        line = self.take('N')
        now = int(line[1])
        if now < self.last_now:
            return self.close('clock-backwards')
        trial = copy.deepcopy(self.m)
        r = trial.apply('Tick', [str(now)])
        if r.startswith('err'):
            return 'err m:' + r.split()[1]
        self.last_now = now
        self.m = trial
        self.settle(r)
        return self.text(r)

    # ---- observations ----
    def read_usbsts(self, corroborate):
        v = self.rd_need('usbsts')
        if v & RSVD_STS:
            raise Cause('reserved-bits-set')
        halted = bool(v & HCH)
        if self.know_hch is True and not halted and self.know_rs is False:
            raise Cause('impossible-transition')
        if self.last_cnr is False and v & CNR:
            raise Cause('impossible-transition')
        if corroborate and halted and self.know_rs is not True:
            cmd = self.rd_need('usbcmd')
            again = self.rd_need('usbsts')
            if bit(cmd, 0) or not again & HCH or again & RSVD_STS:
                raise Cause('uncorroborated-halt')
        self.last_cnr = bool(v & CNR)
        self.know_hch = halted
        return v

    def obs_status(self):
        v = self.read_usbsts(True)
        self.fresh[ST] = True
        return self.m.apply('ObserveStatus', [str(int(bool(v & HCH))), str(int(bool(v & HSE))), str(int(bool(v & HCE)))])

    def read_pci_bme(self):
        bme = bit(self.rd_need('pci.command'), 2)
        if self.know_bme is not None and self.know_bme != bme:
            raise Cause('unexpected-change')
        self.know_bme = bme
        return bme

    def obs_pci(self):
        bme = self.read_pci_bme()
        msi = self.rd('pci.msi-control', 0)
        msix = self.rd('pci.msix-control', 0)
        flag = (msi is not None and bit(msi, 0)) or (msix is not None and bit(msix, 15))
        self.fresh[PCI] = True
        return self.m.apply('ObservePci', [str(int(bme)), str(int(flag))])

    def obs_reset(self):
        cmd = self.rd_need('usbcmd')
        sts = self.read_usbsts(False)
        bme = self.read_pci_bme()
        config, crcr, dcbaap = self.rd_need('config'), self.rd_need('crcr'), self.rd_need('dcbaap')
        erstsz, erstba, erdp = self.rd_need('erstsz'), self.rd_need('erstba'), self.rd_need('erdp')
        quiet = True
        for p in range(self.ports):
            value = self.rd_need('portsc', p)  # every port is read even after one is not quiet
            quiet = quiet and (value & 0b11011 == 0)
        crcr_ok = (crcr == 0) if self.crcr_reads_zero else True
        zero = config == 0 and dcbaap == 0 and erstsz == 0 and erstba == 0 and erdp == 0 and crcr_ok
        r = self.m.apply('ObserveReset', [str(int(bool(sts & CNR))), str(int(bool(sts & HCH))), str(int(cmd == 0)), str(int(zero)),
                                          str(int(bme)), str(int(quiet))])
        if r.startswith('ok'):
            self.fresh[ST] = self.fresh[PCI] = self.fresh[PRT] = True
        return r

    def obs_handoff(self):
        v = self.rd('usblegsup', 0)
        if v is None:
            return 'err no-legacy-capability'
        r = self.m.apply('ObserveHandoff', [str(int(bit(v, 16))), str(int(bit(v, 24)))])
        if r.startswith('ok'):
            self.fresh[HND] = True
        return r

    def obs_ports(self):
        mask = 0
        for p in range(self.ports):
            if bit(self.rd_need('portsc', p), 0):
                mask |= 1 << p
        r = self.m.apply('ObservePorts', [str(mask)])
        if r.startswith('ok'):
            self.fresh[PRT] = True
        return r

    def obs_event(self):
        index, ccs = self.m.deq, self.m.ccs
        f = self.rd_stamped('E', 'EF')
        if f[0] == 'none':
            args = ['none']
        else:
            args = f[:9]
            if int(args[0]) != index:
                raise Cause('event-mismatch')
        r = self.m.apply('ConsumeEvent', args)
        if r.startswith('ok'):
            self.fresh[EVT] = True
        return r

    # ---- writes ----
    def write(self, name, args, kind):
        requires, invalidates, pre, rb, reduces = KINDS[kind]
        sealed = self.sealed()
        reqs = ([ST] if kind == 'clearbme' else []) if sealed else requires
        if any(not self.fresh[k] for k in reqs):
            return 'err s:stale-observation'
        trial = copy.deepcopy(self.m)
        r = trial.apply(name, args)
        if r.startswith('err'):
            return 'err m:' + r.split()[1]
        ev = r.split(' ', 3)[3]
        if ev == '-':
            self.m = trial
            return self.text(r)
        try:
            if not sealed:
                ident = tuple(int(x) for x in self.rd_stamped('I', 'IF'))
                topo = tuple(int(x) for x in self.rd_stamped('T', 'TF'))
                if ident != self.bound[0]:
                    raise Cause('identity-changed')
                if topo != self.bound[1]:
                    raise Cause('topology-changed')
                for reg, mask, expect in pre:
                    if self.rd_need(reg) & mask != expect:
                        raise Cause('precondition-changed')
        except Cause as c:
            return self.close(c.cause)
        eid, target, width, value = ev.split(':')
        nonce = self.nonce
        self.nonce += 1
        w = self.take('W')
        if w[1:] != [eid, target, width, value, str(nonce)]:
            raise Mismatch(f'write recorded {w} but the accepted event was {ev} with ticket {nonce}')
        res = self.take('WR', 'WF')
        cause = None
        if res[0] == 'WF':
            _, k, seq, tick, sess = res
            cause = self.stamp(int(seq), int(tick), int(sess))
            if cause is None:
                self.last_write_seq = int(seq)
                cause = WRITE_CAUSE[k]
        else:
            _, seq, tick, sess, *rbf = res
            cause = self.stamp(int(seq), int(tick), int(sess))
            if cause is None:
                self.last_write_seq = int(seq)
                if rb is not None:
                    reg, mask, expect = rb
                    expect = int(value) if expect == 'ev' else expect
                    if rbf[0] == 'none':
                        cause = 'write-unverified'
                    else:
                        rreg, ridx, rwidth, rvalue = rbf
                        if rreg != reg or rwidth != f'w{WIDTH[reg]}' or int(rvalue) & mask != expect:
                            cause = 'write-mismatch'
        for k in invalidates:
            self.fresh[k] = False
        verified = cause is None
        self.after_write(kind, verified)
        if verified:
            self.m = trial
            self.settle(r)
            return self.text(r)
        if not reduces:
            self.m = trial
        return self.close(cause)

    def after_write(self, kind, verified):
        known = (lambda v: v) if verified else (lambda v: None)
        if kind == 'clearbme':
            self.know_bme = known(False)
        elif kind == 'setbme':
            self.know_bme = known(True)
        elif kind in ('clearrs', 'reset'):
            self.know_rs, self.know_hch = known(False), None
        elif kind == 'setrs':
            self.know_rs, self.know_hch = known(True), None
        if kind == 'reset':
            self.last_cnr = None


class Cause(Exception):
    def __init__(self, cause):
        super().__init__(cause)
        self.cause = cause


def main(argv):
    if len(argv) < 2:
        sys.exit('usage: xhci_init_oracle_session.py TRACE')
    st = {'traces': 0, 'requests': 0, 'refused': 0, 'closed': 0, 'accepted': 0, 'mismatches': 0, 'first_mismatches': []}
    o, bad, req, io, got, expect = None, False, None, [], None, None
    for line in open(argv[1]):
        line = line.rstrip('\n')
        if line.startswith('T '):
            _, seed, mode, policy, legacy, ports, _pad = line.split()
            o, bad = SessionOracle(policy), False
            st['traces'] += 1
        elif line.startswith('Q '):
            req, io = line[2:], []
        elif line.startswith('  '):
            io.append(line[2:])
        elif line.startswith('=> '):
            expect = line[3:]
            st['requests'] += 1
            got = None
            if bad:
                continue
            try:
                got = o.request(req, io)
            except Mismatch as e:
                got = f'MISMATCH {e}'
            if got.startswith('err'):
                st['refused'] += 1
            elif 'closed=-' not in got:
                st['closed'] += 1
            else:
                st['accepted'] += 1
        elif line.startswith('L '):
            if bad or got is None:
                continue
            if got != expect or o.ledger_text() != line[2:]:
                st['mismatches'] += 1
                bad = True
                if len(st['first_mismatches']) < 5:
                    st['first_mismatches'].append({'trace': st['traces'], 'request': req, 'oracle': got + ' L ' + o.ledger_text(), 'session': expect + ' L ' + line[2:]})
    print(json.dumps(st, indent=1))
    sys.exit(0 if st['mismatches'] == 0 and st['requests'] else 1)


if __name__ == '__main__':
    main(sys.argv)
