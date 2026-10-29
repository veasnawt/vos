#!/usr/bin/env python3
"""USB.2-C3a BIOS/UEFI native checks with independent QEMU observations.

GRUB choice per boot: n normal, r residue guard, q guard + C1 qualification,
d guard + C2 DMA exercise, p USB.1 probe, c USB.1 probe + guard (refused).
Unsupported-controller refusals (none, NEC, two xHCIs) run on BIOS. Every boot
is traced (writes, PCI configuration, all usb_xhci_* incl. ring fetches; MMIO
reads from the GRUB prompt) and attributed after the kernel's first serial
byte. After the shell exits, QEMU HMP reads (with read tracing off) the
firmware ring pointers, the ERST segment and VOS's own PMM bitmaps for every
firmware page, independently of the guard's report.
"""
import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / file)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


C1 = load('usb2c1_native', 'test-usb2c1-native.py')
H, V, T = C1.H, C1.V, C1.T
need, sha, APPS, EVENTS, DEVICES = C1.need, C1.sha, C1.APPS, C1.EVENTS, C1.DEVICES
KVMA = 0xFFFFFFFF80000000
EXTRA = {'n': '', 'r': 'vos.usb2-residue=1', 'q': 'vos.usb2-residue=1 vos.usb2-qualify=1',
         'd': 'vos.usb2-residue=1 vos.usb2c2-dma=1', 'p': 'vos.usb_probe=1', 'c': 'vos.usb_probe=1 vos.usb2-residue=1'}


def words(q, addr, n):
    t = q.command('human-monitor-command', {'command-line': f'xp /{n}wx {addr:#x}'})
    w = [int(x, 16) for x in re.findall(r'\b0x[0-9a-f]{8}\b', t)]
    need(len(w) == n, f'HMP read {addr:#x}')
    return w


def u64(w):
    return w[0] | (w[1] << 32)


def firmware_pages(q, base):
    """Firmware ring/context pages from the controller and the ERST entry."""
    crcr = u64(words(q, base + 0x58, 2)) & ~0x3f
    dcbaap = u64(words(q, base + 0x70, 2)) & ~0x3f
    erstba = u64(words(q, base + 0x1030, 2)) & ~0x3f
    erdp = u64(words(q, base + 0x1038, 2)) & ~0xf
    pages = set()
    for a, n in ((dcbaap, 65 * 8), (crcr, 256 * 16), (erstba, 16)):
        if a:
            pages.update(range(a & ~0xfff, (a + n + 0xfff) & ~0xfff, 0x1000))
    seg = trbs = 0
    if erstba and erstba < (1 << 30):
        e = words(q, erstba, 4)
        seg, trbs = u64(e[:2]) & ~0x3f, e[2] & 0xffff
        if seg:
            pages.update(range(seg & ~0xfff, (seg + trbs * 16 + 0xfff) & ~0xfff, 0x1000))
    return {'crcr': hex(crcr), 'dcbaap': hex(dcbaap), 'erstba': hex(erstba), 'erdp': hex(erdp),
            'segment': hex(seg), 'segment_trbs': trbs}, sorted(pages)


def pmm_bits(q, pmm_sym, frames):
    pw = words(q, pmm_sym - KVMA, 12)
    avail, alloc = u64(pw[0:2]) - KVMA, u64(pw[2:4]) - KVMA
    out = {}
    for f in frames:
        n = f >> 12
        av = (u64(words(q, avail + (n // 64) * 8, 2)) >> (n % 64)) & 1
        al = (u64(words(q, alloc + (n // 64) * 8, 2)) >> (n % 64)) & 1
        out[hex(f)] = {'avail': av, 'alloc': al}
    return out


def boot(out, fw, iso, variables, mode, device, pmm_sym):
    name = f'{fw}-{mode}-{device}'
    folder = out / name
    folder.mkdir()
    serial, sock, trace = folder / 'serial.txt', folder / 'qmp', folder / 'trace.txt'
    events = folder / 'events.txt'
    events.write_text('\n'.join(EVENTS) + '\n')
    argv = ['qemu-system-x86_64', '-machine', 'q35', '-accel', 'tcg', '-m', '128M', '-cdrom', str(iso),
            '-boot', 'd', '-display', 'none', '-nic', 'none', *DEVICES[device],
            '-serial', 'file:' + str(serial), '-qmp', 'unix:' + str(sock) + ',server=on,wait=off',
            '-no-reboot', '-no-shutdown', '-trace', f'events={events},file={trace}']
    if fw == 'uefi':
        argv += ['-drive', 'if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                 '-drive', f'if=pflash,format=raw,unit=1,file={variables}']
    (folder / 'command.json').write_text(json.dumps(argv, indent=2) + '\n')
    result = {'case': name, 'firmware': fw, 'mode': mode, 'device': device, 'passed': False}
    q, functions = None, []
    with (folder / 'qemu.txt').open('x') as log:
        process = subprocess.Popen(argv, cwd=ROOT, stdout=log, stderr=log)
        try:
            def data():
                need(process.poll() is None, 'owned QEMU exited')
                s = serial.read_text(errors='replace') if serial.exists() else ''
                need('panic:' not in s and 'gui: FAIL' not in s, 'guest panic or GUI failure')
                return s

            def wait(marker, start=0, seconds=40):
                deadline = time.monotonic() + seconds
                while time.monotonic() < deadline:
                    s = data()
                    if marker in s[start:]:
                        return s
                    time.sleep(.025)
                raise RuntimeError(f'bounded marker timeout {marker!r} in {name}')

            wait('USB2C3A CHOICE:')
            q = H.QMP(sock)
            functions = C1.xhci_functions(q)
            q.command('trace-event-set-state', {'name': 'memory_region_ops_read', 'enable': True})
            q.key(mode)
            q.key('ret')
            if mode == 'c':
                s = wait('boot: FAILED')
                need('vos.usb_probe=1 is exclusive with' in s and 'usb2c3a:' not in s and 'usb: ' not in s,
                     'conflict refused before either path ran')
            else:
                s = wait('gui-demo: PRESENTED;', seconds=60)
                need('rust: kernel probe PASS' in s and 'boot: check failed' not in s, 'boot checks')
                if mode in 'rqd':
                    m = re.search(r'usb2c3a: (PLAN VALID|REFUSED)[^\r\n]*', s)
                    need(m, 'guard verdict')
                    result['verdict'] = m[0]
                    # Only the guard's own block: later C1/C2 output has its own counters.
                    text = s[s.index('usb2c3a:'):m.end()]
                    result['report'] = dict(re.findall(r'\b(\w+)=(0x[0-9a-f]+)', text))
                    result['reserved'] = [int(x, 16) for x in re.findall(r'reserved frame=(0x[0-9a-f]+)', text)]
                    result['plan'] = re.search(r'plan=([^\r\n]*)', text)[1].split()
                    result['pmm_allocated_before'] = int(re.search(r'pmm_allocated_before=(\d+)', text)[1])
                    need(s.index('usb2c3a:') < s.index('desktop:') if 'desktop:' in s else True,
                         'guard ran before screen/desktop setup')
                    if device == 'qemu-xhci' and fw == 'uefi':
                        need('PLAN VALID' in m[0] and len(result['reserved']) >= 1 and result['report']['hazards'] == '0x0',
                             'UEFI residue complete, protected, plan valid')
                    elif device == 'qemu-xhci':
                        need('REFUSED: residue incomplete' in m[0] and not result['reserved'],
                             'BIOS residue map-protected but uninspected: refused, nothing reserved')
                    else:
                        need('REFUSED' in m[0] and not result['reserved'], 'unsupported controller refused')
                else:
                    need('usb2c3a:' not in s, 'guard absent unless selected')
                if mode == 'q':
                    need('usb2c1: QUALIFIED: profile qualified' in s, 'C1 qualification alongside the guard')
                if mode == 'd':
                    need('usb2c2: PASS' in s, 'C2 exercise alongside the guard')
                    c2 = [int(x, 16) for x in re.findall(r'set_frame=(0x[0-9a-f]+)', s)]
                    need(not set(c2) & set(result['reserved']), 'C2 frames never include residue frames')
                if mode == 'p':
                    need('usb: summary inspected=1' in s and 'usb: PCI command and BAR unchanged' in s, 'USB.1 probe')
                q.key('a'); wait('gui-demo: KEY a PRESENTED')
                q.key('b'); wait('gui-demo: KEY b PRESENTED')
                shots = mode == 'r' and device == 'qemu-xhci'
                if shots:
                    q.command('screendump', {'filename': str(folder / 'gui.png'), 'format': 'png'})
                q.key('q'); wait('user[vsh]: VOS>')
                need('gui: PASS; kernel-CR3 reclamation, all resources restored' in data(), 'GUI cleanup')

                def command(line, expected, prompt='user[vsh]: VOS>'):
                    start = len(data()); q.text(line + '\n'); s = wait(prompt, start)
                    need(expected in s[start:], 'application ' + line)
                command('apps', 'rixie'); command('hello', 'Hello from VOS!')
                command('rixie', 'deterministic', 'user[rixie]: Rixie>')
                command('help', 'Topics:', 'user[rixie]: Rixie>')
                command('exit', 'Goodbye from Rixie.')
                command('echo restored', 'user[vsh]: restored')
                if shots:
                    q.command('screendump', {'filename': str(folder / 'shell.png'), 'format': 'png'})
                q.text('exit\n'); wait('native: vsh PASS, process reclaimed')
            for event in ('memory_region_ops_read', 'usb_xhci_cap_read', 'usb_xhci_oper_read',
                          'usb_xhci_port_read', 'usb_xhci_runtime_read', 'usb_xhci_doorbell_read'):
                q.command('trace-event-set-state', {'name': event, 'enable': False})
            need(C1.xhci_functions(q) == functions, 'QMP xHCI identity/BAR regions unchanged')
            if functions and device == 'qemu-xhci':
                base = next(r['address'] for r in functions[0]['regions'] if r.get('bar') == 0)
                result['bar0'] = base
                result['register_snapshot'] = C1.snapshot(q, base)
                result['firmware_pointers'], pages = firmware_pages(q, base)
                result['firmware_pages_pmm'] = pmm_bits(q, pmm_sym, pages)
        finally:
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
    # Independent PMM evidence: firmware pages under OVMF are protected only by
    # the guard (alloc set, and listed as reserved); without it they are free.
    for page, bits in result.get('firmware_pages_pmm', {}).items():
        if fw == 'bios':
            need(bits['avail'] == 0, f'BIOS firmware page {page} map-protected')
        elif mode in 'rqd':
            need(bits == {'avail': 1, 'alloc': 1} and int(page, 16) in result['reserved'],
                 f'OVMF page {page} reserved by the guard')
        else:
            need(bits == {'avail': 1, 'alloc': 0}, f'OVMF page {page} free (unprotected) without the guard')
    bdf = (functions[0]['bus'], functions[0]['slot'], functions[0]['function']) if functions else (255, 255, 255)
    analysis = T.analyze(trace, bdf)
    (folder / 'trace-analysis.json').write_text(json.dumps(analysis, indent=2) + '\n')
    result['trace_sha256'], result['trace_bytes'] = sha(trace), trace.stat().st_size
    need(analysis.get('boundary_found'), 'VOS serial boundary found in trace')
    vos = analysis['vos']
    result['trace_vos'] = {k: v for k, v in vos.items() if k != 'examples'}
    need(vos['xhci_mmio_writes'] == 0, 'no xHCI MMIO writes after VOS start')
    need(vos['xhci_cfg_writes'] == 0 and vos['other_cfg_writes'] == 0, 'no PCI configuration writes after VOS start')
    need(vos['state_events'] == 0, 'no reset/run/doorbell/ring fetch/event/slot/transfer after VOS start')
    need(vos['xhci_read_events'] == vos['xhci_mmio_reads'], 'usb_xhci read events agree with MMIO reads')
    guard_reads = int(result.get('report', {}).get('mmio_reads', '0x0'), 16)
    expected = {'n': 0, 'c': 0, 'p': 7, 'r': guard_reads, 'd': guard_reads, 'q': guard_reads + 18}[mode]
    need(vos['xhci_mmio_reads'] == expected, f'register reads {vos["xhci_mmio_reads"]} == expected {expected}')
    if 'register_snapshot' in result:
        firmware, by_vos = T.register_writes(trace, result['bar0'], analysis['boundary_event_index'])
        need(not by_vos, 'no VOS register writes (provenance)')
        stale = {k: (v, result['register_snapshot'][k]) for k, v in firmware.items()
                 if k in result['register_snapshot'] and result['register_snapshot'][k] != v}
        need(not stale, f'snapshot equals last firmware write: {stale}')
    result['passed'] = True
    (folder / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kernel', type=Path, required=True)
    ap.add_argument('--apps', type=Path, required=True)
    ap.add_argument('--efi', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--cases', help='comma-separated fw-mode-device subset (reruns only)')
    args = ap.parse_args()
    out = args.out.resolve()
    need(not out.exists() and out.is_relative_to(ROOT / 'build'), 'fresh isolated output')
    out.mkdir(parents=True)
    kernel = args.kernel.resolve()
    pmm_sym = int(next(l.split()[0] for l in subprocess.check_output(['nm', str(kernel)], text=True).splitlines()
                       if l.endswith(' kernel_pmm')), 16)
    inputs = {'/boot/vos.bin': kernel}
    for n in APPS:
        inputs[f'/boot/{n}.elf'] = (args.apps / f'{n}.elf').resolve()
    need(all(p.is_file() and not p.is_symlink() for p in inputs.values()), 'explicit regular inputs')
    cfg = out / 'grub.cfg'
    cfg.write_text('serial --unit=0 --speed=115200\nterminal_output console serial\n'
                   'echo "USB2C3A CHOICE: n r q d p c"\nread selection\nset extra=""\n' +
                   ''.join(f'if [ "$selection" = "{k}" ]; then set extra="{v}"; fi\n' for k, v in EXTRA.items() if v) +
                   'set gfxpayload=1024x768x32,auto\n'
                   'multiboot2 /boot/vos.bin vos.apps=1 vos.desktop=1 vos.gui=1 $extra\n' +
                   ''.join(f'module2 /boot/{n}.elf {n}\n' for n in APPS) + 'boot\n')
    report = {'passed': False, 'physical_hardware': False, 'inputs': {k: sha(v) for k, v in inputs.items()},
              'cases': [], 'failures': []}
    cases = {'bios': [('n', 'qemu-xhci'), ('r', 'qemu-xhci'), ('q', 'qemu-xhci'), ('d', 'qemu-xhci'),
                      ('p', 'qemu-xhci'), ('c', 'qemu-xhci'), ('r', 'none'), ('r', 'nec-xhci'), ('r', 'two-xhci')],
             'uefi': [('n', 'qemu-xhci'), ('r', 'qemu-xhci'), ('q', 'qemu-xhci'), ('d', 'qemu-xhci'),
                      ('p', 'qemu-xhci'), ('c', 'qemu-xhci')]}
    try:
        tmp = out / 'producer-tmp'; tmp.mkdir()
        for fw in ('bios', 'uefi'):
            expected = dict(inputs)
            fw_cfg = cfg
            if fw == 'uefi':
                fw_cfg = out / 'grub-uefi.cfg'
                fw_cfg.write_text((ROOT / 'arch/x86_64/grub-uefi-head.cfg').read_text() + cfg.read_text())
            expected['/boot/grub/grub.cfg'] = fw_cfg
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
            for mode, device in cases[fw]:
                if args.cases and f'{fw}-{mode}-{device}' not in args.cases.split(','):
                    continue
                try:
                    r = boot(out, fw, iso, variables, mode, device, pmm_sym)
                    report['cases'].append(r)
                    print(r['case'], 'PASS', flush=True)
                except Exception as error:
                    report['failures'].append({'case': f'{fw}-{mode}-{device}', 'error': str(error)})
                    print(f'{fw}-{mode}-{device}', 'FAIL', error, flush=True)
        wanted = len(args.cases.split(',')) if args.cases else sum(map(len, cases.values()))
        report['passed'] = not report['failures'] and len(report['cases']) == wanted
        report['subset'] = args.cases
    finally:
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    need(report['passed'], 'native matrix failures: ' + str(report['failures']))


if __name__ == '__main__':
    main()
