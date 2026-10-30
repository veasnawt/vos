#!/usr/bin/env python3
"""USB.2-C3a.2 BIOS/UEFI native checks with independent QEMU observations.

GRUB choice per boot (as C3a): n normal, r residue guard, q guard + C1, d guard
+ C2, p USB.1 probe, c probe + guard (refused). Every boot is traced (writes,
PCI configuration, usb_xhci_*; MMIO reads from the GRUB prompt) and observed
through QEMU's gdbstub without gdb (scripts/vos_gdbstub.py):

  - a breakpoint on the first pmm_alloc_frame call snapshots VOS's PMM state
    and both bitmaps immediately BEFORE THE FIRST ALLOCATION, plus the raw
    Multiboot2 information block;
  - a write watchpoint on kernel_pmm.free records every change, giving the
    exact allocation low-water mark of the whole desktop/application run;
  - after the shell exits the bitmaps are read again.

An independent Python oracle (Multiboot2 tags 6/17 parsed here, reservations
from the serial log) predicts the whole avail bitmap frame by frame. With
--expect baseline it must equal the kernel's bitmap without EFI exclusion (the
pre-C3a.2 kernel); with --expect exclusion it must equal it with every EFI
frame outside {LoaderCode, LoaderData, Conventional} with WB, without RUNTIME/
SP, removed from tag-6 available RAM. Firmware xHCI and AHCI DMA pointers are
read through HMP with read tracing off and checked against the PMM bits.
"""
import argparse
import gzip
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import threading
import time
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


C3A = load('usb2c3a_native', 'test-usb2c3a-native.py')
G = load('vos_gdbstub', 'vos_gdbstub.py')
V2 = load('usb2c3b2_trace', 'usb2c3b2_trace.py')
C1, H, V, T = C3A.C1, C3A.H, C3A.V, C3A.T
need, sha, APPS, EVENTS, DEVICES = C3A.need, C3A.sha, C3A.APPS, C3A.EVENTS, C3A.DEVICES
# USB.2-C3b.1: 't' runs the read-only containment dry-run.
EXTRA = {**C3A.EXTRA, 't': 'vos.usb2-contain=1'}
# USB.2-C3b.2 (QEMU test-image kernels only; a production kernel must ignore every one of these):
# 'x' is the single authorized execute option; the rest are the execute option plus one conflicting option.
CONFLICTS = {'y': 'vos.usb2-contain=1', 'z': 'vos.usb2-residue=1', 'w': 'vos.usb_probe=1', 'u': 'vos.usb2-qualify=1',
             'v': 'vos.usb2c2-dma=1'}
EXTRA['x'] = 'vos.usb2-contain-execute=1'
EXTRA.update({k: 'vos.usb2-contain-execute=1 ' + v for k, v in CONFLICTS.items()})
XMODES = 'xyzwuv'
FLAVOUR = 'production'   # --flavour test: the kernel under test is the QEMU-only containment image
MARKER = 'VOS-CONTAIN-TEST-IMAGE: QEMU-ONLY EXPERIMENTAL; NEVER FOR PHYSICAL PCS'
COMPRESS = False   # --compress-traces: verified, hash-recorded gzip of each analysed raw trace


def device_args(device):
    """QEMU arguments for 'base+extra+...': +disk (read-only ext2 on virtio-blk),
    +kbd (USB keyboard on the xHCI), +kbd2 (USB keyboard and tablet)."""
    base, *extras = device.split('+')
    args = list(DEVICES[base])
    for e in extras:
        if e in ('nohv', 'oem'):   # environment variants: handled by machine_args
            continue
        elif e == 'disk':
            args += ['-drive', f'file={DISK},if=none,id=vs0,format=raw,readonly=on',
                     '-device', 'virtio-blk-pci,drive=vs0,disable-legacy=on']
        elif e in ('kbd', 'kbd2'):
            args += ['-device', 'usb-kbd,bus=xhci.0'] + (['-device', 'usb-tablet,bus=xhci.0'] if e == 'kbd2' else [])
        else:
            raise SystemExit('unknown device extra ' + e)
    return args


def usb_conflict_boot(mode):
    """The kernel skips the whole USB.2 block when the probe is combined with an observation option (mode c)."""
    return mode == 'c'


def verdict_execute(s, result, fw, device, mode):
    """The guest's account of a boot with the execute option: authorized, refused, or ignored (production)."""
    # Every observation session that ran alongside reports its own register-read count; the trace must equal their sum.
    result['mmio_reads_reported'] = sum(int(x, 16) for x in re.findall(r'mmio_reads=(0x[0-9a-f]+)', s))
    if FLAVOUR == 'production':
        need('usb2c3b2' not in s and 'CONTAIN-TEST' not in s, 'production image: the execute option is unknown; nothing printed')
        result['verdict'] = 'production image ignores the execute option'
        result['contained'] = False
        return
    m = re.search(r'usb2c3b2: (CONTAINED|REFUSED|STOPPED|FAIL)[^\r\n]*', s)
    need(m and 'usb2c3b2: ' + MARKER in s, 'test-image banner and an execution verdict')
    text = s[s.index('usb2c3b2: ' + MARKER):m.end()]
    result['verdict'] = m[0]
    result['contained'] = m[1] == 'CONTAINED'
    result['execute'] = dict(re.findall(r'\b(\w+)=(0x[0-9a-f]+)', text))
    c = result['execute']
    if mode in CONFLICTS:
        need(m[1] == 'REFUSED' and f'conflicting option {CONFLICTS[mode]}; nothing written' in m[0], f'conflicting option {CONFLICTS[mode]} refused')
        return
    extras = device.split('+')[1:]
    if fw == 'uefi' and device in ('qemu-xhci', 'qemu-xhci+disk'):
        need(m[1] == 'CONTAINED' and all(c.get(k) == v for k, v in (
            ('state', '0xd'), ('refusals', '0x0'), ('environment', '0x0'), ('writes', '0x2'), ('polls', '0x1'),
            ('w1_old', '0x7'), ('w1_new', '0x3'), ('w1_readback', '0x3'), ('w1_status', '0x10'), ('usbsts_w1', '0x1'),
            ('w2_old', '0x0'), ('w2_new', '0x2'), ('usbcmd', '0x0'), ('usbsts', '0x1'), ('dnctrl', '0x0'), ('config', '0x0'),
            ('ports_active', '0x0'), ('irq_regs_clear', '0x1'), ('command', '0x3'), ('status', '0x10'), ('crcr', '0x0'),
            ('dcbaap', '0x0'))), 'eligible idle UEFI QEMU profile: both writes issued and every read-back verified')
        return
    names = set(m[0].split('REFUSED:')[1].split(';')[0].split()) if m[1] == 'REFUSED' else set()
    result['refused_for'] = sorted(names)
    want = ({'unqualified'} if not device.startswith('qemu-xhci') else
            {'environment'} if set(extras) & {'nohv', 'oem'} else
            {'not-uefi', 'running'} if running_firmware(fw, device) else {'not-uefi'} if fw == 'bios' else {'slots', 'usbsts', 'interrupts'})
    need(m[1] == 'REFUSED' and 'nothing written' in m[0] and want <= names, f'refused for {sorted(want)}, nothing written (got {sorted(names)})')
    if fw == 'bios' and not device.startswith('qemu-xhci+kbd'):
        need('not-uefi' in names, 'BIOS controllers are never contained')


def machine_args(device):
    """-machine/-accel/-cpu: q35 under TCG. '+oem' changes the ACPI OEM ID, '+nohv' hides the hypervisor CPUID bit."""
    extras = device.split('+')[1:]
    machine = 'q35' + (',x-oem-id=EVILRS' if 'oem' in extras else '')
    return ['-machine', machine, '-accel', 'tcg'] + (['-cpu', 'qemu64,-hypervisor'] if 'nohv' in extras else [])


def running_firmware(fw, device):
    """SeaBIOS leaves the xHCI running when it drives a USB keyboard (observed)."""
    return fw == 'bios' and '+kbd' in device


def preserve(trace):
    """Evidence-preserving trace compression: gzip deterministically, prove the
    round trip reproduces the original bytes, record both hashes, then (and only
    then) remove the raw file."""
    raw = trace.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    gz = trace.with_name(trace.name + '.gz')
    with gz.open('xb') as f, gzip.GzipFile(filename='', mode='wb', fileobj=f, compresslevel=9, mtime=0) as z:
        z.write(raw)
    back = gzip.decompress(gz.read_bytes())
    need(len(back) == len(raw) and hashlib.sha256(back).hexdigest() == digest, 'compressed trace reproduces the original')
    record = {'raw_sha256': digest, 'raw_bytes': len(raw), 'gz_sha256': sha(gz), 'gz_bytes': gz.stat().st_size,
              'roundtrip_verified': True, 'restore': 'gzip -dk trace.txt.gz'}
    (trace.parent / 'trace.preservation.json').write_text(json.dumps(record, indent=2) + '\n')
    trace.unlink()
    return record
words, u64, KVMA = C3A.words, C3A.u64, C3A.KVMA
LO, HI, PAGE = 1 << 20, 1 << 30, 4096
NFRAMES = HI // PAGE
ALLOWED, WB, SP, RUNTIME = {1, 2, 7}, 1 << 3, 1 << 18, 1 << 63
EFI_RESV = 'firmware memory (EFI)'
DISK = None    # --disk: read-only ext2 fixture for the storage cases


def parse_mb2(blob):
    """Independent Multiboot2 parser: tag headers, tag 6 entries, tag 17 header and descriptors."""
    total = int.from_bytes(blob[0:4], 'little')
    need(8 <= total <= len(blob), 'multiboot2 total size')
    out = {'total': total, 'tags': [], 'mmap': [], 'efi': [], 'efi_header': None, 'raw': {}}
    off = 8
    while off + 8 <= total:
        t, s = int.from_bytes(blob[off:off + 4], 'little'), int.from_bytes(blob[off + 4:off + 8], 'little')
        need(s >= 8 and off + s <= total, 'tag bounds')
        out['tags'].append({'type': t, 'size': s, 'offset': off})
        body = blob[off:off + s]
        if t == 0:
            break
        if t in (6, 17):
            out['raw'][t] = body
        if t == 6:
            es = int.from_bytes(body[8:12], 'little')
            for e in range(16, s - es + 1, es):
                out['mmap'].append({'base': int.from_bytes(body[e:e + 8], 'little'),
                                    'length': int.from_bytes(body[e + 8:e + 16], 'little'),
                                    'type': int.from_bytes(body[e + 16:e + 20], 'little')})
        if t == 17:
            ds, ver = int.from_bytes(body[8:12], 'little'), int.from_bytes(body[12:16], 'little')
            out['efi_header'] = {'size': s, 'descriptor_size': ds, 'descriptor_version': ver,
                                 'descriptors': (s - 16) // ds if ds else 0, 'remainder': (s - 16) % ds if ds else None}
            for e in range(16, s - ds + 1, ds):
                out['efi'].append({'type': int.from_bytes(body[e:e + 4], 'little'),
                                   'base': int.from_bytes(body[e + 8:e + 16], 'little'),
                                   'pages': int.from_bytes(body[e + 24:e + 32], 'little'),
                                   'attribute': int.from_bytes(body[e + 32:e + 40], 'little')})
        off += (s + 7) & ~7
    return out


def efi_allowed(d):
    return d['type'] in ALLOWED and d['attribute'] & WB and not d['attribute'] & (SP | RUNTIME)


def oracle(mb2, resv, exclude):
    """Expected avail bitmap (one byte per frame) and the EFI-excluded frames.

    Exclusions are defined over tag-6 available RAM alone (independently of the
    reservation table), then everything is removed in PMM order.
    """
    ram = bytearray(NFRAMES)
    for e in mb2['mmap']:
        end = e['base'] + e['length']
        if e['type'] != 1 or e['length'] == 0 or end >= 1 << 64:
            continue
        s, t = (max(e['base'], LO) + PAGE - 1) // PAGE, min(end, HI) // PAGE
        if s < t:
            ram[s:t] = b'\1' * (t - s)
    excluded = bytearray(NFRAMES)
    if exclude:
        ok, bad = bytearray(NFRAMES), bytearray(NFRAMES)
        for d in mb2['efi']:
            s, t = d['base'] // PAGE, min(d['base'] // PAGE + d['pages'], NFRAMES)
            if s < t:
                (ok if efi_allowed(d) else bad)[s:t] = b'\1' * (t - s)
        for f in range(LO // PAGE, NFRAMES):
            if ram[f] and (not ok[f] or bad[f]):
                excluded[f] = 1
    avail = bytearray(a and not x for a, x in zip(ram, excluded))
    for e in mb2['mmap']:
        if e['type'] == 1 or e['length'] == 0:
            continue
        s, t = e['base'] // PAGE, min((e['base'] + e['length'] + PAGE - 1) // PAGE, NFRAMES)
        if s < t:
            avail[s:t] = bytes(t - s)
    for s, t in resv:
        s, t = s // PAGE, min((t + PAGE - 1) // PAGE, NFRAMES)
        if s < t:
            avail[s:t] = bytes(t - s)
    avail[:LO // PAGE] = bytes(LO // PAGE)
    return avail, excluded


def runs(flags):
    out, f = [], 0
    while f < len(flags):
        if flags[f]:
            s = f
            while f < len(flags) and flags[f]:
                f += 1
            out.append((s * PAGE, f * PAGE))
        else:
            f += 1
    return out


def bits(raw, n=NFRAMES):
    return bytearray((raw[f >> 3] >> (f & 7)) & 1 for f in range(n))


class Watch(threading.Thread):
    """Services every write-watchpoint stop on kernel_pmm.free; tracks the minimum."""

    def __init__(self, gdb, addr):
        super().__init__(daemon=True)
        self.gdb, self.addr = gdb, addr
        self.lock, self.stop_req, self.running = threading.Lock(), False, False
        self.hits, self.minimum, self.maximum, self.error = 0, None, None, None

    def run(self):
        try:
            while True:
                with self.lock:
                    if self.stop_req:
                        return
                    self.gdb._send('c')
                    self.running = True
                reply = self.gdb._packet()
                with self.lock:
                    self.running = False
                if self.stop_req:
                    return
                need(reply.startswith('T05') or reply.startswith('S05'), 'watchpoint stop ' + reply[:16])
                v = self.gdb.u64(self.addr)
                self.hits += 1
                self.minimum = v if self.minimum is None else min(self.minimum, v)
                self.maximum = v if self.maximum is None else max(self.maximum, v)
        except Exception as error:      # reported by the caller
            self.error = str(error)

    def halt(self):
        with self.lock:
            self.stop_req = True
            if self.running:
                self.gdb.s.sendall(b'\x03')
        self.join(timeout=30)
        need(not self.is_alive() and self.error is None, f'watchpoint service stopped cleanly ({self.error})')


def snapshot(gdb, syms):
    st = gdb.read(syms['kernel_pmm'], 64)
    f = [int.from_bytes(st[i:i + 8], 'little') for i in range(0, 56, 8)]
    state = int.from_bytes(st[56:60], 'little')
    frames = f[2]
    avail = gdb.read(f[0], frames // 8)
    alloc = gdb.read(f[1], frames // 8)
    return {'avail_ptr': f[0], 'alloc_ptr': f[1], 'frames': frames, 'usable': f[3], 'free': f[4],
            'hint': f[5], 'ignored': f[6], 'state': state}, avail, alloc


READ_EVENTS = ('memory_region_ops_read', 'usb_xhci_cap_read', 'usb_xhci_oper_read',
               'usb_xhci_port_read', 'usb_xhci_runtime_read', 'usb_xhci_doorbell_read')


def quiet_pages(q, base):
    """Firmware xHCI ring/context pages via HMP with read tracing paused (so the
    observer's own reads are never attributed to VOS), then resumed."""
    for event in READ_EVENTS:
        q.command('trace-event-set-state', {'name': event, 'enable': False})
    try:
        return C3A.firmware_pages(q, base)
    finally:
        for event in READ_EVENTS:
            q.command('trace-event-set-state', {'name': event, 'enable': True})


def mappings(gdb, root, frames):
    """Every leaf of the live kernel page tables (read from guest memory, never
    written) that maps one of `frames`: [(frame, va, writable, nx, cache_bits)]."""
    found, seen = [], set()

    def walk(table, level, va):
        need(table not in seen and len(seen) < 128, 'bounded page-table walk')
        seen.add(table)
        raw = gdb.read(KVMA + table, 4096)
        for i in range(512):
            e = int.from_bytes(raw[i * 8:i * 8 + 8], 'little')
            if not e & 1:
                continue
            v = va | i << (12 + 9 * level)
            if level == 3 and i >= 256:
                v |= 0xffff000000000000
            pa = e & 0x000ffffffffff000
            if level == 0 or (level in (1, 2) and e & 0x80):
                size = 1 << (12 + 9 * level)
                pa &= ~(size - 1)
                pat = (e >> 12 if level else e >> 7) & 1          # PAT bit position by level
                for f in frames:
                    if pa <= f < pa + size:
                        found.append((f, v + (f - pa), bool(e & 2), bool(e >> 63), (e >> 3) & 3 | pat << 2))
            else:
                walk(pa, level - 1, v)
    walk(root, 3, 0)
    return found


def ahci_pointers(q, regions):
    abar = next(g['address'] for g in regions if g.get('bar') == 5)
    pi = words(q, abar + 0x0c, 1)[0]
    ports = {}
    for n in range(32):
        if pi >> n & 1:
            w = words(q, abar + 0x100 + 0x80 * n, 8)
            cmd = words(q, abar + 0x100 + 0x80 * n + 0x18, 1)[0]
            ports[n] = {'clb': w[0] | w[1] << 32, 'fb': w[2] | w[3] << 32, 'cmd': cmd,
                        'st': cmd & 1, 'fre': cmd >> 4 & 1, 'cr': cmd >> 15 & 1, 'fr': cmd >> 14 & 1}
    return {'abar': abar, 'pi': pi, 'ports': ports}


def boot(out, fw, iso, variables, mode, device, syms, expect):
    name = f'{fw}-{mode}-{device}'
    folder = out / name
    folder.mkdir()
    serial, sock, gsock, trace = folder / 'serial.txt', folder / 'qmp', folder / 'gdb', folder / 'trace.txt'
    events = folder / 'events.txt'
    events.write_text('\n'.join(EVENTS) + '\n')
    argv = ['qemu-system-x86_64', *machine_args(device), '-m', '128M', '-cdrom', str(iso),
            '-boot', 'd', '-display', 'none', '-nic', 'none', *device_args(device),
            '-serial', 'file:' + str(serial), '-qmp', 'unix:' + str(sock) + ',server=on,wait=off',
            '-gdb', 'unix:' + str(gsock) + ',server=on,wait=off',
            '-no-reboot', '-no-shutdown', '-trace', f'events={events},file={trace}']
    if fw == 'uefi':
        argv += ['-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                 '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
    (folder / 'command.json').write_text(json.dumps(argv, indent=2) + '\n')
    result = {'case': name, 'firmware': fw, 'mode': mode, 'device': device, 'expect': expect, 'passed': False}
    q, gdb, watch, functions = None, None, None, []
    with (folder / 'qemu.txt').open('x') as log:
        process = subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=log)
        try:
            def data():
                need(process.poll() is None, 'owned QEMU exited')
                s = serial.read_text(errors='replace') if serial.exists() else ''
                need('gui: FAIL' not in s, 'GUI failure')
                return s

            def wait(marker, start=0, seconds=60):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    s = data()
                    need('panic:' not in s, 'guest panic: ' + s[s.find('panic:'):][:200])
                    if marker in s[start:]:
                        return s
                    time.sleep(.025)
                raise RuntimeError(f'bounded marker timeout {marker!r} in {name}')

            wait('USB2C3A CHOICE:')
            q = H.QMP(sock)
            functions = C1.xhci_functions(q)
            gdb = G.GdbStub(gsock)                   # connecting stops the VM
            gdb.insert(0, syms['pmm_alloc_frame'], 1)
            gdb._send('c')
            q.command('trace-event-set-state', {'name': 'memory_region_ops_read', 'enable': True})
            q.key(mode)
            q.key('ret')
            stop = gdb._packet()
            need(stop.startswith('T05') or stop.startswith('S05'), 'breakpoint at the first pmm_alloc_frame ' + stop[:16])
            gdb.remove(0, syms['pmm_alloc_frame'], 1)
            s = data()
            m = re.search(r'boot: multiboot2 info at (0x[0-9a-f]+)-(0x[0-9a-f]+)', s)
            need(m, 'multiboot2 info location')
            info = gdb.read(KVMA + int(m[1], 16), int(m[2], 16) - int(m[1], 16))
            (folder / 'mb2-info.bin').write_bytes(info)
            first, avail0, alloc0 = snapshot(gdb, syms)
            need(first['state'] == 1 and first['free'] == first['usable'] and not any(alloc0),
                 'PMM ready with zero allocations at the first pmm_alloc_frame')
            need('devmap:' not in s, 'first allocation precedes the device window')
            result['first_allocation'] = first
            mb2 = parse_mb2(info)
            result['mb2'] = {'tags': [(t['type'], t['size']) for t in mb2['tags']], 'efi_header': mb2['efi_header'],
                             'mmap_entries': len(mb2['mmap']), 'efi_descriptors': len(mb2['efi'])}
            for t, raw in mb2['raw'].items():
                (folder / f'tag{t}.bin').write_bytes(raw)
            resv_all = [(int(a, 16), int(b, 16), w.strip()) for a, b, w in
                        re.findall(r'boot: reserved (0x[0-9a-f]+)-(0x[0-9a-f]+) ([^\r\n]*)', s)]
            resv = [(a, b) for a, b, w in resv_all if w != EFI_RESV]
            printed = [(a, b) for a, b, w in resv_all if w == EFI_RESV]
            exclude = expect != 'baseline' and fw == 'uefi'
            want, excluded = oracle(mb2, resv, exclude)
            got = bits(avail0)
            diff = [f for f in range(NFRAMES) if got[f] != want[f]]
            need(not diff, f'avail bitmap equals the independent oracle ({len(diff)} differ, first {diff[:4]})')
            need(sum(want) == first['usable'], 'usable count equals the oracle')
            bs = bytearray(NFRAMES)
            for d in mb2['efi']:
                if d['type'] in (3, 4):
                    s0, t0 = d['base'] // PAGE, min(d['base'] // PAGE + d['pages'], NFRAMES)
                    if s0 < t0:
                        bs[s0:t0] = b'\1' * (t0 - s0)
            bs_window = [f for f in range(LO // PAGE, NFRAMES) if bs[f]]
            bs_free = [f for f in bs_window if got[f]]
            result['efi'] = {'boot_services_frames_in_window': len(bs_window),
                             'boot_services_frames_allocatable': len(bs_free),
                             'excluded_frames_oracle': sum(excluded), 'excluded_ranges_oracle':
                             [(hex(a), hex(b)) for a, b in runs(excluded)],
                             'excluded_ranges_kernel': [(hex(a), hex(b)) for a, b in printed]}
            if exclude:
                need(mb2['efi_header'] and len(bs_window) > 0 and not bs_free,
                     'every BootServices frame excluded before the first allocation')
                need(printed == runs(excluded), 'kernel exclusion ranges equal the oracle')
                rem = re.search(r'efi: (\d+) frames excluded in (\d+) ranges', s)
                need(rem and int(rem[1]) == sum(excluded) and int(rem[2]) == len(printed), 'efi summary line')
            elif fw == 'uefi':
                need(not printed and len(bs_free) == len(bs_window) > 0,
                     'baseline: BootServices frames allocatable (the hazard)')
            else:
                need(not mb2['efi_header'] and not printed, 'BIOS: no EFI map, no EFI exclusion')
            if expect in ('inspection', 'containment', 'execution') and mode != 'c' and functions and device.startswith('qemu-xhci'):
                # USB.2-C3a.3: observe the guard window from outside: stop at the
                # guard's entry and at screen_init (its successor), compare PMM
                # bitmaps, firmware page bytes and every page-table mapping of them.
                bar = next(r['address'] for r in functions[0]['regions'] if r.get('bar') == 0)
                window = {}
                for label, symbol in (('guard_entry', 'xhci_residue_boot'), ('after_guard', 'screen_init')):
                    gdb.insert(0, syms[symbol], 1)
                    stop = gdb.cont()
                    need(stop.startswith('T05') or stop.startswith('S05'), f'breakpoint at {symbol}')
                    gdb.remove(0, syms[symbol], 1)
                    state, av, al = snapshot(gdb, syms)
                    pointers, pages = quiet_pages(q, bar)
                    if label == 'after_guard' and mode == 'x' and not pages and window.get('guard_entry', {}).get('pages'):
                        # The authorized reset cleared the controller's pointers. The firmware's pages are the ones it
                        # had at the guard's entry: hash and map those, and keep the cleared pointers as evidence.
                        result['pointers_at_guard_exit'] = pointers
                        pages = window['guard_entry']['pages']
                    window[label] = {'state': state, 'avail': av, 'alloc': al, 'pages': pages,
                                     'sha256': {hex(p): hashlib.sha256(gdb.read(KVMA + p, PAGE)).hexdigest() for p in pages},
                                     'mappings': mappings(gdb, gdb.u64(syms['kernel_root']), pages)}
                g0, g1 = window['guard_entry'], window['after_guard']
                need(g0['pages'] == g1['pages'] and g0['pages'], 'firmware pointers stable across the guard')
                need(g0['avail'] == avail0 == g1['avail'], 'avail bitmap unchanged across the guard')
                need(g0['alloc'] == g1['alloc'], 'no frame allocated, reserved or freed by the inspection')
                # A controller the firmware left running may change its own rings (firmware-originated DMA).
                need(running_firmware(fw, device) or g0['sha256'] == g1['sha256'], 'firmware page bytes unchanged by the inspection')
                result['firmware_dma_changed_pages'] = sorted(k for k in g1['sha256'] if g0['sha256'].get(k) != g1['sha256'][k])
                for label in window:
                    maps = window[label]['mappings']
                    need(sorted(m[0] for m in maps) == sorted(window[label]['pages']), f'{label}: one mapping per page')
                    need(all(va == KVMA + f and w and nx and cache == 0 for f, va, w, nx, cache in maps),
                         f'{label}: only the write-back kernel alias (no new alias)')
                result['guard_window'] = {k: {'free': v['state']['free'], 'pages': [hex(p) for p in v['pages']],
                                              'sha256': v['sha256'],
                                              'mappings': [(hex(f), hex(va), w, nx, c) for f, va, w, nx, c in v['mappings']]}
                                          for k, v in window.items()}
            watch = Watch(gdb, syms['kernel_pmm'] + 32)
            gdb.insert(2, syms['kernel_pmm'] + 32, 8)
            watch.start()
            if mode == 'c':
                s = wait('boot: FAILED')
                need('vos.usb_probe=1 is exclusive with' in s and 'usb2c3a:' not in s and 'usb: ' not in s,
                     'conflict refused before either path ran')
            else:
                s = wait('gui-demo: PRESENTED;', seconds=120)
                need('rust: kernel probe PASS' in s and 'boot: check failed' not in s, 'boot checks')
                if mode in 'rqd':
                    m = re.search(r'usb2c3a: (PLAN VALID|REFUSED)[^\r\n]*', s)
                    need(m, 'guard verdict')
                    result['verdict'] = m[0]
                    text = s[s.index('usb2c3a:'):m.end()]
                    result['guard'] = dict(re.findall(r'\b(\w+)=(0x[0-9a-f]+)', text))
                    result['reserved'] = [int(x, 16) for x in re.findall(r'reserved frame=(0x[0-9a-f]+)', text)]
                    g = result['guard']
                    if device.startswith('qemu-xhci') and fw == 'uefi' and expect == 'baseline':
                        need('PLAN VALID' in m[0] and result['reserved'], 'baseline UEFI guard reserves (C3a)')
                    elif device.startswith('qemu-xhci') and fw == 'uefi' and expect in ('inspection', 'containment', 'execution'):
                        need('PLAN VALID' in m[0] and not result['reserved'] and g.get('findings') == '0x0' and
                             g.get('excluded') == '0x7' and g.get('inspection') == '0x3' and g.get('hazards') == '0x0' and
                             'reuse_protected=1 inspection_complete=1; firmware memory is never released' in text + s,
                             'excluded firmware residue inspected in place: complete, nothing reserved')
                    elif device.startswith('qemu-xhci') and expect in ('inspection', 'containment', 'execution'):
                        need('REFUSED: residue incomplete' in m[0] and not result['reserved'] and g.get('hazards') == '0x0'
                             and g.get('excluded') == '0x0' and g.get('inspection') == '0x1',
                             'BIOS: map-protected residue, reuse protected, inspection incomplete, refused')
                    elif device.startswith('qemu-xhci'):
                        need('REFUSED: residue incomplete' in m[0] and not result['reserved'] and
                             result['guard'].get('hazards') == '0x0',
                             'firmware residue already protected (map/EFI): uninspected, refused, nothing reserved')
                    else:
                        need('REFUSED' in m[0] and not result['reserved'], 'unsupported controller refused')
                elif mode == 't':
                    need('usb2c3a:' not in s, 'no guard output in a dry-run-only boot')
                    m = re.search(r'usb2c3b1: (DRY-RUN COMPLETE|REFUSED)[^\r\n]*', s)
                    need(m, 'containment dry-run verdict')
                    text = s[s.index('usb2c3b1:'):m.end()]
                    result['verdict'] = m[0]
                    result['contain'] = dict(re.findall(r'\b(\w+)=(0x[0-9a-f]+)', text))
                    result['contain_trace'] = re.findall(r'usb2c3b1: trace=[^\r\n]*', text)
                    c = result['contain']
                    if fw == 'uefi' and device in ('qemu-xhci', 'qemu-xhci+disk'):
                        need('DRY-RUN COMPLETE' in m[0] and all(c.get(k) == v for k, v in (
                            ('state', '0x6'), ('refusals', '0x0'), ('command', '0x7'), ('status', '0x10'), ('usbcmd', '0x0'),
                            ('usbsts', '0x1'), ('findings', '0x0'), ('irq_off', '0x1'), ('plan_len', '0x5'), ('trace_len', '0x6'))),
                             'eligible idle UEFI profile: dry-run complete')
                        joined = '\n'.join(result['contain_trace'])
                        need('trace=2 pci-write NOT-ISSUED width=16 offset=0x4 old=0x7 new=0x3' in joined and
                             'trace=4 mmio-write NOT-ISSUED width=32 offset=0x40 old=0x0 new=0x2' in joined and
                             joined.count('revalidate ok') == 2, 'exact proposed writes after two clean revalidations, none issued')
                    else:
                        names = set(m[0].split('REFUSED:')[1].split(';')[0].split())
                        result['refused_for'] = sorted(names)
                        required = ({'unqualified'} if not device.startswith('qemu-xhci') else
                                {'not-uefi', 'running'} if running_firmware(fw, device) else
                                {'not-uefi'} if fw == 'bios' else {'slots', 'usbsts', 'interrupts'})
                        need('nothing written' in m[0] and 'NOT-ISSUED' not in text and required <= names,
                             f'refused for {sorted(required)} before proposing anything (got {sorted(names)})')
                elif mode in XMODES:
                    verdict_execute(s, result, fw, device, mode)
                else:
                    need('usb2c3a:' not in s, 'guard absent unless selected')
                if mode not in ('t', 'y'):   # 'y' selects the dry-run together with the execute option
                    need('usb2c3b1:' not in s, 'containment dry-run absent unless selected')
                if mode not in XMODES:
                    need(not re.search(r'usb2c3b2: (CONTAINED|REFUSED|STOPPED|FAIL)', s), 'the execute path is silent unless selected')
                need((MARKER in s) == (FLAVOUR == 'test' and 'usb2c3b2:' in s and not usb_conflict_boot(mode)),
                     'only the QEMU test image carries (and prints) the experimental marker')
                if mode == 'q':
                    need('usb2c1: QUALIFIED: profile qualified' in s, 'C1 qualification alongside the guard')
                if mode == 'd':
                    need('usb2c2: PASS' in s, 'C2 exercise alongside the guard')
                if mode == 'p':
                    need('usb: summary inspected=1' in s and 'usb: PCI command and BAR unchanged' in s, 'USB.1 probe')
                light = '+kbd' in device   # QMP key events may go to the USB keyboard, not VOS's PS/2 driver
                if not light:
                    q.key('a'); wait('gui-demo: KEY a PRESENTED')
                    q.key('b'); wait('gui-demo: KEY b PRESENTED')
                    shots = (mode == 'n' or result.get('contained')) and device == 'qemu-xhci'
                    if shots:
                        q.command('screendump', {'filename': str(folder / 'gui.png'), 'format': 'png'})
                    q.key('q'); wait('user[vsh]: VOS>')
                    need('gui: PASS; kernel-CR3 reclamation, all resources restored' in data(), 'GUI cleanup')

                    def command(line, expected, prompt='user[vsh]: VOS>'):
                        start = len(data())
                        for c in line + '\n':      # QMP qcodes for every character typed here
                            q.key({' ': 'spc', '\n': 'ret', '-': 'minus', '.': 'dot', '/': 'slash'}.get(c, c))
                            time.sleep(.04)
                        s = wait(prompt, start)
                        need(expected in s[start:], 'application ' + line)
                    command('apps', 'rixie'); command('hello', 'Hello from VOS!')
                    command('rixie', 'deterministic', 'user[rixie]: Rixie>')
                    command('help', 'Topics:', 'user[rixie]: Rixie>')
                    command('exit', 'Goodbye from Rixie.')
                    command('echo restored', 'user[vsh]: restored')
                    if device.endswith('+disk'):
                        need('vblk: no virtio-blk' not in s and 'vblk:' in s, 'virtio-blk device registered')
                        command('ls', 'docs'); command('cd docs', '', 'user[vsh]: VOS>docs>')
                        command('pwd', '/docs', 'user[vsh]: VOS>docs>')
                        command('cat welcome.txt', 'Unified apps: real read-only filesystem.', 'user[vsh]: VOS>docs>')
                        command('cd /', '')
                    if shots:
                        q.command('screendump', {'filename': str(folder / 'shell.png'), 'format': 'png'})
                    q.text('exit\n'); wait('native: vsh PASS, process reclaimed')
            watch.halt()
            gdb.remove(2, syms['kernel_pmm'] + 32, 8)
            last, avail1, alloc1 = snapshot(gdb, syms)
            need(avail1 == avail0 and last['usable'] == first['usable'], 'avail bitmap unchanged since initialization')
            if 'guard_window' in result:
                after = {hex(p): hashlib.sha256(gdb.read(KVMA + p, PAGE)).hexdigest() for p in window['after_guard']['pages']}
                need(running_firmware(fw, device) or after == window['after_guard']['sha256'],
                     'firmware page bytes unchanged through the GUI run')
                maps = mappings(gdb, gdb.u64(syms['kernel_root']), window['after_guard']['pages'])
                need(len(maps) == len(after) and all(va == KVMA + f and w and nx and c == 0 for f, va, w, nx, c in maps),
                     'after the GUI run: still only the kernel alias')
                result['guard_window']['end'] = {'sha256': after, 'mappings': [(hex(f), hex(va), w, nx, c) for f, va, w, nx, c in maps]}
            a1 = bits(alloc1)
            need(not any(a1[f] for f in range(NFRAMES) if not want[f]), 'no unavailable frame ever allocated')
            need(not any(a1[f] for f in range(NFRAMES) if excluded[f]), 'no excluded frame allocated')
            low = min(first['free'], watch.minimum if watch.minimum is not None else first['free'])
            result['after'] = {k: last[k] for k in ('usable', 'free')}
            result['allocated_after'] = sum(a1)
            result['low_water'] = {'watch_hits': watch.hits, 'min_free': low,
                                   'peak_in_use_frames': first['usable'] - low,
                                   'peak_in_use_bytes': (first['usable'] - low) * PAGE}
            for event in ('memory_region_ops_read', 'usb_xhci_cap_read', 'usb_xhci_oper_read',
                          'usb_xhci_port_read', 'usb_xhci_runtime_read', 'usb_xhci_doorbell_read'):
                q.command('trace-event-set-state', {'name': event, 'enable': False})
            need(C1.xhci_functions(q) == functions, 'QMP xHCI identity/BAR regions unchanged')
            pci = [d for b in q.command('query-pci') for d in b['devices']]
            if functions and device.startswith('qemu-xhci'):
                base = next(r['address'] for r in functions[0]['regions'] if r.get('bar') == 0)
                result['bar0'] = base
                result['register_snapshot'] = C1.snapshot(q, base)
                result['firmware_pointers'], pages = C3A.firmware_pages(q, base)
                result['contained'] = mode == 'x' and 'usb2c3b2: CONTAINED' in data()
                if result['contained']:
                    # The reset cleared the controller's pointers; the pages themselves are the firmware's, as seen
                    # (and hashed) at the guard's entry, and they must still be unallocatable.
                    fp = result['firmware_pointers']
                    need(not pages and fp['crcr'] == fp['dcbaap'] == fp['erstba'] == fp['erdp'] == '0x0',
                         'the reset cleared the command ring, DCBAA and event ring pointers')
                    pages = window['guard_entry']['pages']
                result['xhci_pages'] = {hex(p): got[p // PAGE] for p in pages}
                need(pages, 'firmware xHCI ring/context pointers present')
                if fw == 'uefi' and expect == 'baseline':
                    need(all(got[p // PAGE] for p in pages), 'baseline UEFI: xHCI pages allocatable (the hazard)')
                else:
                    need(not any(got[p // PAGE] for p in pages), 'firmware xHCI ring/context pages unallocatable')
                if exclude:
                    need(all(excluded[p // PAGE] for p in pages), 'xHCI pages excluded via the EFI map')
            sata = next((d for d in pci if d['class_info']['class'] == 0x0106), None)
            if sata:
                a = ahci_pointers(q, sata['regions'])
                live = {p // PAGE * PAGE for n in a['ports'].values() if n['fre'] or n['st']
                        for p in (n['clb'], n['fb']) if p}
                result['ahci'] = {'abar': hex(a['abar']), 'ports': {k: {**v, 'clb': hex(v['clb']), 'fb': hex(v['fb'])}
                                                                     for k, v in a['ports'].items()},
                                  'live_pages': {hex(p): got[p // PAGE] for p in sorted(live)}}
                need(live, 'firmware left at least one AHCI engine armed')
                if expect != 'baseline' or fw == 'bios':
                    need(not any(got[p // PAGE] for p in live), 'armed AHCI command/FIS pages unallocatable')
                else:
                    need(all(got[p // PAGE] for p in live), 'baseline UEFI: armed AHCI pages allocatable (the hazard)')
        finally:
            if gdb:
                gdb.close()
            if q:
                try:
                    q.close()
                except OSError:
                    pass
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill(); process.wait(timeout=5)
            result['serial_sha256'] = sha(serial) if serial.exists() else None
            (folder / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    bdf = (functions[0]['bus'], functions[0]['slot'], functions[0]['function']) if functions else (255, 255, 255)
    result['xhci_bdf'] = list(bdf)
    analysis = T.analyze(trace, bdf)
    (folder / 'trace-analysis.json').write_text(json.dumps(analysis, indent=2) + '\n')
    result['trace_sha256'], result['trace_bytes'] = sha(trace), trace.stat().st_size
    trace_checks(result, trace, analysis, mode, device, fw)
    if COMPRESS:
        result['trace_preservation'] = preserve(trace)
    result['passed'] = True
    (folder / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def cfg_writes_by_device(trace, first):
    """PCI configuration writes after the VOS boundary, per QEMU device name."""
    out = {}
    lines = Path(trace).read_text(errors='replace').splitlines()
    for e in lines[first:]:
        m = re.match(r'(?:\d+@[\d.]+:)?pci_cfg_write (\S+) ', e)
        if m:
            out[m[1]] = out.get(m[1], 0) + 1
    return out


def trace_checks(result, trace, analysis, mode, device, fw=''):
    """Zero device-write predicates on one boot's trace. Storage boots run the
    accepted N3 virtio-blk driver, whose configuration writes must target only
    that function; xHCI writes are never allowed."""
    need(analysis.get('boundary_found'), 'VOS serial boundary found in trace')
    vos = analysis['vos']
    result['trace_vos'] = {k: v for k, v in vos.items() if k != 'examples'}
    if mode in XMODES:
        return execute_trace_checks(result, trace, analysis, mode, device, fw)
    need(vos['xhci_mmio_writes'] == 0, 'no xHCI MMIO writes after VOS start')
    need(vos['xhci_cfg_writes'] == 0, 'no xHCI PCI configuration writes after VOS start')
    by = cfg_writes_by_device(trace, analysis['boundary_event_index'])
    result['vos_cfg_writes_by_device'] = by
    if device.endswith('+disk'):
        need(set(by) <= {'virtio-blk-pci'} and sum(by.values()) == vos['other_cfg_writes'],
             'configuration writes only to the virtio-blk function (accepted N3 storage driver)')
    else:
        need(vos['other_cfg_writes'] == 0 and not by, 'no PCI configuration writes after VOS start')
    if running_firmware(fw, device):
        # The firmware left this controller running: its own ring activity is
        # firmware-originated. VOS-originated writes are still excluded above
        # and by register provenance below.
        result['firmware_originated_state_events'] = vos['state_events']
        result['firmware_state_event_examples'] = analysis['vos'].get('examples', {}).get('state_events', [])[:6]
    else:
        need(vos['state_events'] == 0, 'no reset/run/doorbell/ring fetch/event/slot/transfer after VOS start')
    need(vos['xhci_read_events'] == vos['xhci_mmio_reads'], 'usb_xhci read events agree with MMIO reads')
    guard_reads = int(result.get('guard', {}).get('mmio_reads', '0x0'), 16)
    contain_reads = int(result.get('contain', {}).get('mmio_reads', '0x0'), 16)
    expected = {'n': 0, 'c': 0, 'p': 7, 'r': guard_reads, 'd': guard_reads, 'q': guard_reads + 18, 't': contain_reads}[mode]
    need(vos['xhci_mmio_reads'] == expected, f'register reads {vos["xhci_mmio_reads"]} == expected {expected}')
    if 'register_snapshot' in result:
        firmware, by_vos = T.register_writes(trace, result['bar0'], analysis['boundary_event_index'])
        need(not by_vos, 'no VOS register writes (provenance)')
        stale = {k: (v, result['register_snapshot'][k]) for k, v in firmware.items()
                 if k in result['register_snapshot'] and result['register_snapshot'][k] != v}
        # With a USB device attached the hardware itself updates status registers.
        need(not stale or '+kbd' in device, f'snapshot equals last firmware write: {stale}')


def execute_trace_checks(result, trace, analysis, mode, device, fw):
    """Boots with the execute option, judged by the independent verifier (usb2c3b2_trace.py): a contained boot has exactly
    W1 then W2 and the documented reset consequences; every other boot has no xHCI write and no VOS-caused state event."""
    contained = bool(result.get('contained'))
    bdf = tuple(result.get('xhci_bdf', (255, 255, 255)))
    verdict = V2.verify(V2.load(trace), bdf, result.get('bar0', 0), slots=0x40, ports=8, contained=contained,
                        firmware_driven=running_firmware(fw, device),
                        other_cfg_allowed=('virtio-blk-pci',) if device.endswith('+disk') else ())
    result['write_trace'] = {k: v for k, v in verdict.items() if k != 'problems'}
    result['write_trace_problems'] = verdict['problems']
    need(verdict['ok'], f'independent write-trace verdict: {verdict["problems"]}')
    vos = analysis['vos']
    if not device.endswith('+disk'):
        need(vos['other_cfg_writes'] == 0, 'no other PCI configuration writes after VOS start')
    need(vos['xhci_read_events'] == vos['xhci_mmio_reads'], 'usb_xhci read events agree with MMIO reads')
    # Reported reads of every session that ran, the USB.1 probe's fixed 7 (mode w), and the W2 adapter's own two
    # re-reads of CAPLENGTH and USBCMD when W2 was issued.
    expected = result.get('mmio_reads_reported', 0) + (7 if mode == 'w' else 0) + (2 if contained else 0)
    need(vos['xhci_mmio_reads'] == expected, f'register reads {vos["xhci_mmio_reads"]} == expected {expected}')
    if 'register_snapshot' in result:
        firmware, by_vos = T.register_writes(trace, result['bar0'], analysis['boundary_event_index'])
        snap = result['register_snapshot']
        if contained:
            need(by_vos == {'0x0040': [2]}, f'the only register VOS wrote is USBCMD, once, with HCRST: {by_vos}')
            need(all(snap[k] == 0 for k in ('0x0058', '0x005c', '0x0070', '0x0074', '0x0078', '0x1030', '0x1038')) and
                 snap['0x0040'] == 0 and snap['0x0044'] == 1, 'registers read at the end are the reset defaults')
        else:
            need(not by_vos, 'no VOS register writes (provenance)')


def reanalyze(folder):
    """Re-check a retained boot whose guest-side checks all passed, from its raw
    trace and recorded analysis; the original result.json is left untouched."""
    result = json.loads((folder / 'result.json').read_text())
    analysis = json.loads((folder / 'trace-analysis.json').read_text())
    trace = folder / 'trace.txt'
    need(sha(trace) == result.get('trace_sha256', sha(trace)), 'raw trace unchanged')
    for k in ('first_allocation', 'efi', 'low_water', 'after'):
        need(k in result, f'guest-side checks completed ({k})')
    out = {'case': result['case'], 'original_passed': result['passed'], 'trace_sha256': sha(trace),
           **{k: result[k] for k in ('guard', 'register_snapshot', 'bar0') if k in result}}
    try:
        trace_checks(out, trace, analysis, result['mode'], result['device'], result['firmware'])
        out['passed'] = True
    except Exception as error:
        out['passed'], out['error'] = False, str(error)
    (folder / 'reanalysis.json').write_text(json.dumps(out, indent=2) + '\n')
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kernel', type=Path, required=True)
    ap.add_argument('--apps', type=Path, required=True)
    ap.add_argument('--efi', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--expect', choices=('baseline', 'exclusion', 'inspection', 'containment', 'execution'), required=True)
    ap.add_argument('--compress-traces', action='store_true', help='verified gzip of analysed traces (raw hash kept)')
    ap.add_argument('--flavour', choices=('production', 'test'), default='production', help='kernel flavour under test')
    ap.add_argument('--cases', help='comma-separated fw-mode-device subset')
    ap.add_argument('--disk', type=Path, required=True, help='read-only ext2 fixture (accepted N6.3 disk)')
    ap.add_argument('--iso-dir', type=Path, help='reuse validated images of an earlier run of the same inputs')
    ap.add_argument('--reanalyze', type=Path, help='re-check the traces of retained boots (with --cases) and exit')
    args = ap.parse_args()
    if args.reanalyze:
        rows = [reanalyze(args.reanalyze / c) for c in args.cases.split(',')]
        print(json.dumps(rows, indent=2))
        need(all(r['passed'] for r in rows), 'reanalysis failures')
        return
    out = args.out.resolve()
    need(not out.exists() and out.is_relative_to(ROOT / 'build'), 'fresh isolated output')
    out.mkdir(parents=True)
    global DISK
    DISK = args.disk.resolve()
    need(DISK.is_file() and not os.access(DISK, os.W_OK), 'read-only disk fixture')
    disk_sha = sha(DISK)
    kernel = args.kernel.resolve()
    table = {l.split()[-1]: int(l.split()[0], 16) for l in subprocess.check_output(['nm', str(kernel)], text=True).splitlines()
             if len(l.split()) == 3}
    syms = {k: table[k] for k in ('kernel_pmm', 'pmm_alloc_frame', 'xhci_residue_boot', 'screen_init', 'kernel_root')
            if k in table}
    need(len(syms) == 5 or args.expect not in ('inspection', 'containment', 'execution'), 'symbols for the guard window')
    inputs = {'/boot/vos.bin': kernel}
    for n in APPS:
        inputs[f'/boot/{n}.elf'] = (args.apps / f'{n}.elf').resolve()
    need(all(p.is_file() and not p.is_symlink() for p in inputs.values()), 'explicit regular inputs')
    cfg = out / 'grub.cfg'
    cfg.write_text('serial --unit=0 --speed=115200\nterminal_output console serial\n'
                   'echo "USB2C3A CHOICE: n r q d p c t x y z w u v"\nread selection\nset extra=""\n' +
                   ''.join(f'if [ "$selection" = "{k}" ]; then set extra="{v}"; fi\n' for k, v in EXTRA.items() if v) +
                   'set gfxpayload=1024x768x32,auto\n'
                   'multiboot2 /boot/vos.bin vos.apps=1 vos.desktop=1 vos.gui=1 $extra\n' +
                   ''.join(f'module2 /boot/{n}.elf {n}\n' for n in APPS) + 'boot\n')
    report = {'passed': False, 'physical_hardware': False, 'expect': args.expect,
              'inputs': {k: sha(v) for k, v in inputs.items()}, 'cases': [], 'failures': []}
    cases = {'bios': [('n', 'qemu-xhci'), ('r', 'qemu-xhci'), ('q', 'qemu-xhci'), ('d', 'qemu-xhci'),
                      ('p', 'qemu-xhci'), ('c', 'qemu-xhci'), ('r', 'none'), ('r', 'nec-xhci'), ('r', 'two-xhci'),
                      ('n', 'qemu-xhci+disk')],
             'uefi': [('n', 'qemu-xhci'), ('r', 'qemu-xhci'), ('q', 'qemu-xhci'), ('d', 'qemu-xhci'),
                      ('p', 'qemu-xhci'), ('c', 'qemu-xhci'), ('n', 'qemu-xhci+disk')]}
    if args.expect == 'containment':
        cases['uefi'] += [('t', 'qemu-xhci'), ('t', 'qemu-xhci+disk'), ('t', 'qemu-xhci+kbd'), ('t', 'qemu-xhci+kbd2'),
                          ('n', 'qemu-xhci+kbd'), ('t', 'none'), ('t', 'nec-xhci'), ('t', 'two-xhci')]
        cases['bios'] += [('t', 'qemu-xhci'), ('t', 'qemu-xhci+disk'), ('t', 'qemu-xhci+kbd'), ('n', 'qemu-xhci+kbd'),
                          ('t', 'none'), ('t', 'nec-xhci'), ('t', 'two-xhci')]
    if args.expect == 'execution':
        need(args.flavour == 'test', '--expect execution runs the QEMU test-image kernel (--flavour test)')
        cases = {'uefi': [('n', 'qemu-xhci'), ('r', 'qemu-xhci'), ('t', 'qemu-xhci'), ('x', 'qemu-xhci'), ('x', 'qemu-xhci+disk'),
                          ('x', 'qemu-xhci+kbd'), ('x', 'qemu-xhci+kbd2'), ('x', 'none'), ('x', 'nec-xhci'), ('x', 'two-xhci'),
                          ('x', 'qemu-xhci+nohv'), ('x', 'qemu-xhci+oem')] +
                         [(k, 'qemu-xhci') for k in CONFLICTS],
                 'bios': [('n', 'qemu-xhci'), ('x', 'qemu-xhci'), ('x', 'qemu-xhci+disk'), ('x', 'qemu-xhci+kbd'), ('x', 'none')]}
    elif args.expect == 'containment' and args.flavour == 'production':
        # The production kernel must ignore the execute option completely.
        cases['uefi'] += [('x', 'qemu-xhci')]
        cases['bios'] += [('x', 'qemu-xhci')]
    global COMPRESS, FLAVOUR
    COMPRESS = args.compress_traces
    FLAVOUR = args.flavour
    try:
        tmp = out / 'producer-tmp'; tmp.mkdir()
        for fw in ('bios', 'uefi'):
            todo = [(m, d) for m, d in cases[fw] if not args.cases or f'{fw}-{m}-{d}' in args.cases.split(',')]
            if not todo:
                continue
            expected = dict(inputs)
            fw_cfg = cfg
            if fw == 'uefi':
                fw_cfg = out / 'grub-uefi.cfg'
                fw_cfg.write_text((ROOT / 'arch/x86_64/grub-uefi-head.cfg').read_text() + cfg.read_text())
            expected['/boot/grub/grub.cfg'] = fw_cfg
            if args.iso_dir:
                earlier = json.loads((args.iso_dir / 'result.json').read_text())
                iso = (args.iso_dir / f'{fw}.iso').resolve()
                need(earlier['inputs'] == report['inputs'] and earlier.get(f'{fw}_iso_sha256') == sha(iso),
                     'reused image built from identical inputs')
                V.validate(iso, expected, fw, out / f'{fw}-image-validation.txt')
                report[f'{fw}_iso_reused_from'] = str(iso.relative_to(ROOT))
            else:
                iso = out / f'{fw}.iso'
                modules = Path('/usr/lib/grub/i386-pc') if fw == 'bios' else args.efi
                V.run(['grub-mkrescue', '-d', modules, '-o', iso, '-graft-points',
                       *[f'{k}={v}' for k, v in expected.items()]], out / f'{fw}-producer.txt',
                      dict(os.environ, TMPDIR=str(tmp), PYTHONDONTWRITEBYTECODE='1'))
                V.validate(iso, expected, fw, out / f'{fw}-image-validation.txt')
            report[f'{fw}_iso_sha256'] = sha(iso)
            variables = out / f'{fw}-OVMF_VARS_4M.fd'
            if fw == 'uefi':
                shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd', variables)
            for mode, device in todo:
                try:
                    r = boot(out, fw, iso, variables, mode, device, syms, args.expect)
                    report['cases'].append(r)
                    print(r['case'], 'PASS', flush=True)
                except Exception as error:
                    report['failures'].append({'case': f'{fw}-{mode}-{device}', 'error': str(error),
                                               'where': traceback.format_exc(limit=-3)})
                    print(f'{fw}-{mode}-{device}', 'FAIL', error, flush=True)
        wanted = len(args.cases.split(',')) if args.cases else sum(map(len, cases.values()))
        report['passed'] = not report['failures'] and len(report['cases']) == wanted
        report['subset'] = args.cases
        report['disk_sha256'] = disk_sha
        need(sha(DISK) == disk_sha, 'disk fixture unchanged')
        report['peak_in_use_frames'] = max((c['low_water']['peak_in_use_frames'] for c in report['cases']
                                            if 'low_water' in c), default=None)
    finally:
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    need(report['passed'], 'native matrix failures: ' + str(report['failures']))


if __name__ == '__main__':
    main()
