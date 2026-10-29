#!/usr/bin/env python3
"""USB.2-C3b.2: tests of the independent write-trace verifier (usb2c3b2_trace.py).

A synthetic trace in QEMU's exact line formats is accepted; each corruption of it
(a missing, duplicated, reordered, mis-sized, mis-valued, mis-addressed or extra
write, a stray or missing consequence) must be rejected for the right reason; and
a refusal trace with any write must be rejected. Optionally, real preserved
traces (--real DIR from earlier milestones) are judged as refusals/no-write boots.
"""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import usb2c3b2_trace as V  # noqa: E402

BDF, BAR, SLOTS, PORTS = (0, 2, 0), 0xc000000000, 64, 8
MR = 'mr 0x5d24e44525a0'


def serial(text):
    return [f'serial_write write addr 0x0 val {ord(c):#x}' for c in text]


def ops(addr, value, size, name, cpu=0):
    return f"memory_region_ops_write cpu {cpu} {MR} addr {addr:#x} value {value:#x} size {size} name '{name}'"


def cfg_write(off, value, size, bdf=BDF, device='qemu-xhci'):
    idx = 0x8000_0000 | bdf[0] << 16 | bdf[1] << 11 | bdf[2] << 8 | (off & ~3)
    return [ops(0xcf8, idx, 4, 'pci-conf-idx'), ops(0xcfc + (off & 3), value, size, 'pci-conf-data'),
            f'pci_cfg_write {device} {bdf[0]:02x}:{bdf[1]:02x}.{bdf[2]} @{off:#x} <- {value:#x}']


def reads(n=3):
    return [f"memory_region_ops_read cpu 0 {MR} addr {BAR + 0x44:#x} value 0x1 size 4 name 'operational'",
            'usb_xhci_oper_read off 0x0044, ret 0x00000001'][:n]


def w2(addr=BAR + 0x40, value=2, size=4, name='operational'):
    return [ops(addr, value, size, name), 'usb_xhci_oper_write off 0x0000, val 0x00000002']


def reset_events(slots=SLOTS, ports=PORTS, bits='0x20000'):
    e = ['usb_xhci_reset === RESET ===']
    e += [f'usb_xhci_slot_disable slotid {s}' for s in range(1, slots + 1)]
    for p in range(1, ports + 1):
        e += [f'usb_xhci_port_link port {p}, pls 5', f'usb_xhci_port_notify port {p}, bits {bits}']
    return e


def good():
    firmware = (cfg_write(0x4, 0x7, 2) + [ops(BAR + 0x40, 2, 4, 'operational'), 'usb_xhci_oper_write off 0x0000, val 0x00000002'] + reset_events())
    return firmware + serial('boot: x86_64 long mode\r\n') + reads() + cfg_write(0x4, 0x3, 2) + reads() + w2()[:1] + \
        w2()[1:] + reset_events() + reads()


def trace_good():
    t = good()
    # `good()` placed W2's oper_write after the ops write; keep QEMU's order: ops write, oper_write, reset...
    return t


def edit(f):
    t = trace_good()
    return f(t) or t


def after_boundary(t):
    return V.boundary(t) or 0


def cases():
    c = {}
    def case(name, problem, fn, **kw):
        c[name] = (problem, fn, kw)
    def drop_w1(t):
        i = next(i for i, e in enumerate(t) if e.startswith('pci_cfg_write') and '@0x4 <- 0x3' in e)
        del t[i - 2:i + 1]
    def drop_w2(t):
        i = next(i for i, e in enumerate(t) if "value 0x2 size 4 name 'operational'" in e and i > after_boundary(t))
        del t[i:i + 2 + 1 + SLOTS + 2 * PORTS]
    def dup(t, text, n):
        i = max(k for k, e in enumerate(t) if text in e)
        t[i + 1:i + 1] = [t[i - j] for j in range(n - 1, -1, -1)]
    case('missing W1', 'exactly 1 xHCI configuration write', drop_w1)
    case('missing W2', 'exactly 1 xHCI register write', drop_w2)
    case('duplicate W1', 'exactly 1 xHCI configuration write', lambda t: dup(t, '@0x4 <- 0x3', 3))
    case('duplicate W2', 'exactly 1 xHCI register write', lambda t: t.__setitem__(slice(len(t) - 3, len(t) - 3), w2()))
    def reorder(t):
        i = max(k for k, e in enumerate(t) if e.startswith('pci_cfg_write') and '@0x4 <- 0x3' in e)
        w = t[i - 2:i + 1]
        del t[i - 2:i + 1]
        j = next(k for k, e in enumerate(t) if "value 0x2 size 4 name 'operational'" in e and k > after_boundary(t))
        t[j + 2 + 1 + SLOTS + 2 * PORTS:j + 2 + 1 + SLOTS + 2 * PORTS] = w
    case('W2 before W1', 'W2 precedes W1', reorder)
    def edit_line(t, old, new, last=True):
        i = (max if last else min)(k for k, e in enumerate(t) if old in e and k > after_boundary(t))
        t[i] = t[i].replace(old, new)
    case('W1 32-bit', 'W1 access is size 4', lambda t: edit_line(t, "addr 0xcfc value 0x3 size 2", "addr 0xcfc value 0x3 size 4"))
    case('W1 8-bit', 'W1 access is size 1', lambda t: edit_line(t, "addr 0xcfc value 0x3 size 2", "addr 0xcfc value 0x3 size 1"))
    case('W1 value 0x1', 'W1 value 0x1', lambda t: (edit_line(t, "addr 0xcfc value 0x3 size 2", "addr 0xcfc value 0x1 size 2"), edit_line(t, "@0x4 <- 0x3", "@0x4 <- 0x1")))
    case('W1 sets bus master', 'W1 value 0x7', lambda t: (edit_line(t, "addr 0xcfc value 0x3 size 2", "addr 0xcfc value 0x7 size 2"), edit_line(t, "@0x4 <- 0x3", "@0x4 <- 0x7")))
    case('W1 wrong offset', 'W1 offset 0x6', lambda t: edit_line(t, "@0x4 <- 0x3", "@0x6 <- 0x3"))
    case('W1 wrong index write', 'configuration index write', lambda t: edit_line(t, "value 0x80001004 size 4 name 'pci-conf-idx'", "value 0x80001104 size 4 name 'pci-conf-idx'"))
    case('W2 wrong value', 'W2 value 0x3', lambda t: edit_line(t, "value 0x2 size 4 name 'operational'", "value 0x3 size 4 name 'operational'"))
    case('W2 wrong size', 'W2 size 1', lambda t: edit_line(t, "value 0x2 size 4 name 'operational'", "value 0x2 size 1 name 'operational'"))
    case('W2 wrong address', 'W2 address', lambda t: edit_line(t, f"addr {BAR + 0x40:#x} value 0x2 size 4", f"addr {BAR + 0x44:#x} value 0x2 size 4"))
    case('W2 wrong region', "W2 targets region 'runtime'", lambda t: edit_line(t, "value 0x2 size 4 name 'operational'", "value 0x2 size 4 name 'runtime'"))
    case('extra register write', 'exactly 1 xHCI register write', lambda t: t.insert(len(t) - 2, ops(BAR + 0x2000, 0, 4, 'doorbell')))
    case('extra config write to xHCI', 'exactly 1 xHCI configuration write', lambda t: t.extend(cfg_write(0x10, 0xffffffff, 4)))
    case('extra config write elsewhere', 'not authorized', lambda t: t.extend(cfg_write(0x4, 0x0, 2, bdf=(0, 3, 0), device='virtio-blk-pci')[2:]))
    case('second reset', 'outside the single reset', lambda t: t.extend(reset_events(slots=0, ports=0)))
    case('controller started', 'outside the single reset', lambda t: t.extend(['usb_xhci_run', 'usb_xhci_fetch_trb']))
    case('stray link notification', 'outside the single reset', lambda t: t.append('usb_xhci_port_notify port 2, bits 0x20000'))
    case('slot activity', 'outside the single reset', lambda t: t.append('usb_xhci_slot_enable slotid 1'))
    case('state between writes', 'between W1 and W2', lambda t: t.insert(next(k for k, e in enumerate(t) if e.startswith('pci_cfg_write') and '@0x4 <- 0x3' in e and k > after_boundary(t)) + 1, 'usb_xhci_doorbell_write off 0x0, val 0x0'))
    case('consequence missing a port', 'link/notify pairs', lambda t: t.__delitem__(next(k for k, e in enumerate(t) if 'port_notify port 8' in e and k > after_boundary(t))))
    case('consequence wrong bits', 'link/notify pairs', lambda t: edit_line(t, 'port_notify port 3, bits 0x20000', 'port_notify port 3, bits 0x40000'))
    case('consequence missing slot', 'reset consequences', lambda t: t.__delitem__(next(k for k, e in enumerate(t) if 'slotid 17' in e and k > after_boundary(t))))
    case('no write at all', 'exactly 1 xHCI configuration write', lambda t: (drop_w1(t), drop_w2(t)))
    case('no boundary', 'boundary not found', lambda t: [t.__delitem__(k) for k in reversed([k for k, e in enumerate(t) if e.startswith('serial_write')])])
    return c


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--real', type=Path, help='directory of preserved real native cases (refusal/no-write boots) to cross-check')
    ap.add_argument('--out', type=Path, help='write the report here (fresh file)')
    args = ap.parse_args()
    report = {'accepted': None, 'rejected': {}, 'real': {}}
    ok = V.verify(trace_good(), BDF, BAR)
    assert ok['ok'], ok
    report['accepted'] = {k: ok[k] for k in ('xhci_cfg_writes', 'xhci_register_writes', 'state_events')}
    assert report['accepted'] == {'xhci_cfg_writes': 1, 'xhci_register_writes': 1, 'state_events': 2 + SLOTS + 2 * PORTS}, report
    # A refusal trace: no VOS write of any kind passes, and the good trace fails as a refusal.
    refusal = good()
    start = next(k for k, e in enumerate(refusal) if e.startswith('serial_write') and '0x62' in e)
    refusal = refusal[:start + 25] + reads()
    assert V.verify(refusal + [], BDF, BAR, contained=False)['ok']
    bad = V.verify(trace_good(), BDF, BAR, contained=False)
    assert not bad['ok'] and any('must make none' in p for p in bad['problems']), bad
    for name, (problem, fn, kw) in cases().items():
        t = trace_good()
        fn(t)
        r = V.verify(t, BDF, BAR, **kw)
        assert not r['ok'], f'{name}: corrupted trace accepted'
        assert any(problem in p for p in r['problems']), f'{name}: rejected for the wrong reason {r["problems"]}, wanted {problem!r}'
        report['rejected'][name] = r['problems'][0]
    if args.real:
        for d in sorted(p for p in args.real.iterdir() if p.is_dir()):
            for t in (d / 'trace.txt.gz', d / 'trace.txt'):
                if t.exists():
                    res = json.loads((d / 'result.json').read_text())
                    bdf, bar = res.get('xhci_bdf'), res.get('bar0')
                    if not bar:
                        break
                    r = V.verify(V.load(t), tuple(bdf or (0, 2, 0)), bar, contained=False, firmware_driven='+kbd' in d.name and d.name.startswith('bios'),
                                 other_cfg_allowed=('virtio-blk-pci',))
                    assert r['ok'], f'{d.name}: {r["problems"]}'
                    wrong = V.verify(V.load(t), tuple(bdf or (0, 2, 0)), bar, contained=True)
                    assert not wrong['ok'], f'{d.name}: a no-write boot was accepted as a contained one'
                    report['real'][d.name] = {'refusal_ok': True, 'rejected_as_contained': True}
                    break
    report['status'] = 'PASS'
    report['variants_rejected'] = len(report['rejected'])
    text = json.dumps(report, indent=2) + '\n'
    if args.out:
        args.out.write_text(text)
    print(f'PASS: 1 synthetic contained trace accepted; {len(report["rejected"])} corruptions rejected for the right reason; '
          f'{len(report["real"])} real no-write traces judged')


if __name__ == '__main__':
    main()
