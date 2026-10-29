#!/usr/bin/env python3
"""USB.2-C1 QEMU trace attribution (library and CLI; read-only analysis).

QEMU's log backend records events in execution order. The guest's serial
bytes (serial_write, THR/offset 0) are reassembled from the same trace, so the
first byte of the kernel's first boot line is an exact ordering boundary:
events after it are VOS's. memory_region_ops_write covers every MMIO/port
write dispatched to an ops-backed region (all xHCI register blocks and the
MSI-X table/PBA); pci_cfg_write covers CF8/CFC and MMCONFIG writes.
"""
import json
import re
import sys
from pathlib import Path

BOUNDARY = b'boot: x86_64 long mode'
XHCI_REGIONS = {'capabilities', 'operational', 'runtime', 'doorbell', 'msix-table', 'msix-pba'}
PORT = re.compile(r'^usb[23] port #\d+$')
OPS = re.compile(r"^memory_region_ops_(read|write) cpu (-?\d+) mr (0x[0-9a-f]+) addr (0x[0-9a-f]+) "
                 r"value (0x[0-9a-f]+) size (\d+) name '([^']*)'")
CFG = re.compile(r'^pci_cfg_write (\S+) ([0-9a-f]{2}):([0-9a-f]{2})\.(\d) @(0x[0-9a-f]+) <- (0x[0-9a-f]+)')
SERIAL = re.compile(r'^serial_write write addr (0x[0-9a-f]+) val (0x[0-9a-f]+)')
# Exact event names (a prefix such as usb_xhci_run would also match the
# harmless usb_xhci_runtime_read). Families matched by explicit prefix.
STATE_EVENTS = {'usb_xhci_reset', 'usb_xhci_run', 'usb_xhci_stop', 'usb_xhci_oper_write',
                'usb_xhci_runtime_write', 'usb_xhci_doorbell_write', 'usb_xhci_port_write',
                'usb_xhci_fetch_trb', 'usb_xhci_queue_event', 'usb_xhci_port_reset',
                'usb_xhci_port_link', 'usb_xhci_irq_intx', 'usb_xhci_irq_msi', 'usb_xhci_irq_msix',
                'msix_write_config'}
STATE_FAMILIES = ('usb_xhci_slot_', 'usb_xhci_ep_', 'usb_xhci_xfer_')
READ_EVENTS = {'usb_xhci_cap_read', 'usb_xhci_oper_read', 'usb_xhci_port_read', 'usb_xhci_runtime_read',
               'usb_xhci_doorbell_read'}


def xhci_region(name):
    return name in XHCI_REGIONS or bool(PORT.match(name))


def analyze(trace, xhci_bdf):
    """xhci_bdf: (bus, dev, fn) of the qualified function, from the guest report."""
    lines = Path(trace).read_text(errors='replace').splitlines()
    # Drop the optional "pid@time:" prefix of the log backend.
    events = [re.sub(r'^\d+@[\d.]+:', '', l) for l in lines]
    serial = bytearray()
    boundary = None
    for i, e in enumerate(events):
        m = SERIAL.match(e)
        if m and int(m[1], 16) == 0:
            serial.append(int(m[2], 16))
            if boundary is None:
                at = serial.find(BOUNDARY)
                if at >= 0:
                    boundary = i  # index of the event completing the marker
    result = {'events': len(events), 'boundary_found': boundary is not None,
              'serial_bytes_reconstructed': len(serial)}
    if boundary is None:
        return result
    # Walk back to the event that wrote the marker's first byte.
    need = len(BOUNDARY) - 1
    first = boundary
    while need:
        first -= 1
        m = SERIAL.match(events[first])
        if m and int(m[1], 16) == 0:
            need -= 1
    result['boundary_event_index'] = first
    bus, dev, fn = xhci_bdf
    split = {'before': events[:first], 'vos': events[first:]}
    for phase, chunk in split.items():
        s = {'xhci_mmio_writes': [], 'xhci_mmio_reads': 0, 'xhci_cfg_writes': [], 'state_events': [],
             'other_cfg_writes': 0, 'xhci_read_events': 0}
        for e in chunk:
            m = OPS.match(e)
            if m and xhci_region(m[7]):
                if m[1] == 'write':
                    s['xhci_mmio_writes'].append(e)
                else:
                    s['xhci_mmio_reads'] += 1
                continue
            m = CFG.match(e)
            if m:
                if (int(m[2], 16), int(m[3], 16), int(m[4])) == (bus, dev, fn):
                    s['xhci_cfg_writes'].append(e)
                else:
                    s['other_cfg_writes'] += 1
                continue
            event = e.split(' ', 1)[0]
            if event in STATE_EVENTS or event.startswith(STATE_FAMILIES):
                s['state_events'].append(e)
            elif event in READ_EVENTS:
                s['xhci_read_events'] += 1
        summary = {k: (len(v) if isinstance(v, list) else v) for k, v in s.items()}
        summary['examples'] = {k: v[:20] for k, v in s.items() if isinstance(v, list) and v}
        result[phase] = summary
    return result


def register_writes(trace, base, boundary):
    """Last DWORD written to each xHCI register offset before the boundary
    (firmware) and every DWORD written after it (VOS). Absolute trace
    addresses are converted to BAR0 offsets; 8-byte writes are split."""
    before, vos = {}, {}
    for i, line in enumerate(Path(trace).read_text(errors='replace').splitlines()):
        m = OPS.match(re.sub(r'^\d+@[\d.]+:', '', line))
        if not (m and m[1] == 'write' and xhci_region(m[7])):
            continue
        addr, value, size = int(m[4], 16), int(m[5], 16), int(m[6])
        for k in range(max(size // 4, 1)):
            key = f'{addr + 4 * k - base:#06x}'
            word = (value >> (32 * k)) & 0xffffffff
            if i < boundary:
                before[key] = word
            else:
                vos.setdefault(key, []).append(word)
    return before, vos


if __name__ == '__main__':
    bdf = tuple(int(x) for x in sys.argv[2].split(':'))
    print(json.dumps(analyze(sys.argv[1], bdf), indent=2))
