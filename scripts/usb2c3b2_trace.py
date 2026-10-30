#!/usr/bin/env python3
"""USB.2-C3b.2 independent QEMU write-trace verifier (library and CLI; read-only analysis).

Judges a raw QEMU trace without trusting anything the guest printed. Everything
after the first serial byte of VOS's boot banner is VOS's. For a boot in which
containment is authorized and eligible, VOS must have made exactly these two
device writes, in this order, and nothing else:

  W1  one PCI configuration write to the xHCI function, offset 0x04, value 0x0003,
      a 16-bit access (the access that carries it has size 2 and value 0x0003);
  W2  one 32-bit write of 0x00000002 to the operational register at BAR0 + 0x40.

The only events QEMU may then emit are the documented consequences of that one
reset, contiguous after W2: the operational-write event, one reset, one
slot-disable per slot and one link/notify pair per port. Any other xHCI
configuration or register write, any run/stop/doorbell/ring/slot/transfer/event
activity, a missing, duplicate, reordered or mis-sized write, or a stray
consequence fails. For every other boot (refusals, option absent) VOS must have
made no xHCI write of any kind and caused no controller state event.
Firmware-originated activity (before the boundary, or by a running controller
the firmware drives) is reported separately and never attributed to VOS.
"""
import gzip
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import usb2c1_trace as T  # noqa: E402

W1 = {'offset': 0x4, 'value': 0x3, 'size': 2}
W2 = {'operational_offset': 0x40, 'value': 0x2, 'size': 4}
CONFIG_PORTS = ('pci-conf-data', 'pcie-mmcfg-mmio')
SLOT = re.compile(r'^usb_xhci_slot_disable slotid (\d+)$')
LINK = re.compile(r'^usb_xhci_port_link port (\d+), pls \d+$')
NOTIFY = re.compile(r'^usb_xhci_port_notify port (\d+), bits (0x[0-9a-f]+)$')
RESET = 'usb_xhci_reset === RESET ==='


def load(path):
    path = Path(path)
    data = gzip.decompress(path.read_bytes()) if path.suffix == '.gz' else path.read_bytes()
    return [re.sub(r'^\d+@[\d.]+:', '', l) for l in data.decode(errors='replace').splitlines()]


def boundary(events):
    """Index of the event that wrote the first byte of VOS's boot banner, or None."""
    serial, found = bytearray(), None
    for i, e in enumerate(events):
        m = T.SERIAL.match(e)
        if m and int(m[1], 16) == 0:
            serial.append(int(m[2], 16))
            if found is None and serial.find(T.BOUNDARY) >= 0:
                found = i
    if found is None:
        return None
    need, first = len(T.BOUNDARY) - 1, found
    while need:
        first -= 1
        m = T.SERIAL.match(events[first])
        if m and int(m[1], 16) == 0:
            need -= 1
    return first


# A read of an unimplemented register (usb_xhci_unimplemented, e.g. the runner's own end-of-boot snapshot) cannot
# change the controller and is not a state event.
STATE_EVENTS = T.STATE_EVENTS | {'usb_xhci_port_notify', 'usb_xhci_port_reset'}


def is_state(e):
    """Controller state activity: the shared analyzer's set plus link-change notifications."""
    name = e.split(' ', 1)[0]
    return name in STATE_EVENTS or name.startswith(T.STATE_FAMILIES)


def consequences(slots, ports):
    """The exact events one QEMU xHCI reset emits inside the USBCMD write."""
    seq = ['usb_xhci_oper_write off 0x0000, val 0x00000002', RESET]
    seq += [f'usb_xhci_slot_disable slotid {s}' for s in range(1, slots + 1)]
    for p in range(1, ports + 1):
        seq += [None, None]   # link and notify of port p, checked by pattern below
    return seq


def verify(events, bdf, bar, slots=64, ports=8, contained=True, firmware_driven=False, other_cfg_allowed=()):
    """-> {'ok': bool, 'problems': [...], counts...}. `bdf` = (bus, dev, fn); `bar` = BAR0 base address."""
    problems, b = [], boundary(events)
    result = {'boundary_found': b is not None, 'problems': problems}
    if b is None:
        problems.append('VOS serial boundary not found')
        result['ok'] = False
        return result
    vos = events[b:]
    base = b
    cfg, ops, state = [], [], []
    for i, e in enumerate(vos):
        m = T.CFG.match(e)
        if m:
            where = (int(m[2], 16), int(m[3], 16), int(m[4]))
            if where == tuple(bdf):
                cfg.append((i, int(m[5], 16), int(m[6], 16)))
            elif m[1] not in other_cfg_allowed:
                problems.append(f'configuration write to {m[1]} {m[2]}:{m[3]}.{m[4]} not authorized')
            continue
        m = T.OPS.match(e)
        if m and T.xhci_region(m[7]) and m[1] == 'write':
            ops.append((i, int(m[4], 16), int(m[5], 16), int(m[6]), m[7]))
            continue
        if is_state(e):
            state.append((i, e))
    result.update({'xhci_cfg_writes': len(cfg), 'xhci_register_writes': len(ops), 'state_events': len(state)})
    if not contained:
        if cfg:
            problems.append(f'{len(cfg)} xHCI configuration write(s) in a boot that must make none')
        if ops:
            problems.append(f'{len(ops)} xHCI register write(s) in a boot that must make none')
        if state and not firmware_driven:
            problems.append(f'{len(state)} controller state event(s) in a boot that must cause none')
        result['firmware_originated_state_events'] = len(state) if firmware_driven else 0
        result['ok'] = not problems
        return result
    # ---- W1 ---------------------------------------------------------------------------------------------
    if len(cfg) != 1:
        problems.append(f'expected exactly 1 xHCI configuration write, saw {len(cfg)}')
    if len(ops) != 1:
        problems.append(f'expected exactly 1 xHCI register write, saw {len(ops)}')
    if len(cfg) == 1:
        i, off, val = cfg[0]
        if off != W1['offset']:
            problems.append(f'W1 offset {off:#x}, expected {W1["offset"]:#x}')
        if val != W1['value']:
            problems.append(f'W1 value {val:#x}, expected {W1["value"]:#x}')
        prev = T.OPS.match(vos[i - 1]) if i else None
        if not prev or prev[1] != 'write' or prev[7] not in CONFIG_PORTS:
            problems.append('W1 is not preceded by a configuration-space access event')
        elif int(prev[6]) != W1['size'] or int(prev[5], 16) != W1['value']:
            problems.append(f'W1 access is size {prev[6]} value {prev[5]}, expected a {W1["size"] * 8}-bit write of {W1["value"]:#x}')
        elif prev[7] == 'pci-conf-data':
            # Port access: the index write just before it must address this function at offset 0x04.
            idx = T.OPS.match(vos[i - 2]) if i > 1 else None
            want = 0x8000_0000 | bdf[0] << 16 | bdf[1] << 11 | bdf[2] << 8 | (W1['offset'] & ~3)
            if not idx or idx[7] != 'pci-conf-idx' or int(idx[5], 16) != want or int(idx[6]) != 4:
                problems.append(f'W1 is not preceded by the configuration index write for {bdf} offset {W1["offset"]:#x}')
    # ---- W2 ---------------------------------------------------------------------------------------------
    if len(ops) == 1:
        i, addr, val, size, name = ops[0]
        if name != 'operational':
            problems.append(f'W2 targets region {name!r}, expected the operational registers')
        if addr != bar + W2['operational_offset']:
            problems.append(f'W2 address {addr:#x}, expected {bar + W2["operational_offset"]:#x}')
        if val != W2['value']:
            problems.append(f'W2 value {val:#x}, expected {W2["value"]:#x}')
        if size != W2['size']:
            problems.append(f'W2 size {size}, expected {W2["size"]}')
    # ---- order and consequences --------------------------------------------------------------------------
    if len(cfg) == 1 and len(ops) == 1:
        i1, i2 = cfg[0][0], ops[0][0]
        if i1 > i2:
            problems.append('W2 precedes W1')
        else:
            between = [e for k, e in state if i1 < k < i2]
            if between:
                problems.append(f'controller state events between W1 and W2: {between[:2]}')
            expected = consequences(slots, ports)
            got = vos[i2 + 1:i2 + 1 + len(expected)]
            fixed = expected[:2 + slots]
            if got[:len(fixed)] != fixed:
                problems.append('the events right after W2 are not the reset consequences')
            else:
                tail, links = got[len(fixed):], []
                for p in range(1, ports + 1):
                    pair = tail[2 * (p - 1):2 * p]
                    ok = len(pair) == 2 and LINK.match(pair[0]) and int(LINK.match(pair[0])[1]) == p and \
                        NOTIFY.match(pair[1]) and int(NOTIFY.match(pair[1])[1]) == p and NOTIFY.match(pair[1])[2] == '0x20000'
                    links.append(bool(ok))
                if not all(links) or len(tail) != 2 * ports:
                    problems.append('the per-port reset consequences are not the expected link/notify pairs')
            allowed = {i2 + 1 + k for k in range(len(expected))}
            stray = [e for k, e in state if k not in allowed]
            if stray:
                problems.append(f'{len(stray)} controller state event(s) outside the single reset: {stray[:2]}')
    elif not ops and not cfg:
        problems.append('no VOS write at all')
    result['ok'] = not problems
    return result


def main(argv):
    if len(argv) < 5:
        sys.exit('usage: usb2c3b2_trace.py TRACE[.gz] BUS:DEV:FN BAR0 contained|refused [slots ports]')
    bdf = tuple(int(x) for x in argv[2].split(':'))
    slots, ports = (int(argv[5]), int(argv[6])) if len(argv) > 6 else (64, 8)
    r = verify(load(argv[1]), bdf, int(argv[3], 0), slots, ports, argv[4] == 'contained')
    print(json.dumps(r, indent=2))
    sys.exit(0 if r['ok'] else 1)


if __name__ == '__main__':
    main(sys.argv)
