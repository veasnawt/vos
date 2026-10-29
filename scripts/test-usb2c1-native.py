#!/usr/bin/env python3
"""USB.2-C1 BIOS/UEFI native checks with independent QEMU observations.

One ISO per firmware carries a GRUB choice: n normal, q qualify, c conflict
(USB.1 probe plus qualification; both must be refused), p USB.1 probe only.
Device negative controls boot q with no xHCI, two xHCIs or the QEMU NEC xHCI.
Every boot records a QEMU trace (pci_cfg_write, all usb_xhci_* events,
memory_region_ops_write, msix_write_config, serial_write; MMIO reads from the
GRUB prompt on) and attributes events after the kernel's first serial byte to
VOS. QMP query-pci and HMP register snapshots are taken with read tracing off.
No host disks or network; owned QEMU children only; fresh output.
"""
import argparse
import hashlib
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


H = load('usb_probe_helpers', 'test-usb-probe.py')
V = load('usb_resources_native', 'test-usb-resources-native.py')
T = load('usb2c1_trace', 'usb2c1_trace.py')
APPS = ('vsh', 'hello', 'rixie', 'gui-demo')
EVENTS = ('pci_cfg_write', 'usb_xhci_*', 'memory_region_ops_write', 'msix_write_config', 'serial_write')
# Registers compared between normal and qualification boots (HMP xp, offsets
# from BAR0): capability, operational, interrupter 0 and the MSI-X table/PBA.
SNAPSHOT = ((0x00, 8), (0x40, 4), (0x54, 4), (0x70, 3), (0x1020, 8), (0x3000, 64), (0x3800, 2))
DEVICES = {
    'qemu-xhci': ['-device', 'qemu-xhci,id=xhci'],
    'none': [],
    'two-xhci': ['-device', 'qemu-xhci,id=xhci0', '-device', 'qemu-xhci,id=xhci1'],
    'nec-xhci': ['-device', 'nec-usb-xhci,id=xhci'],
}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def need(value, message):
    if not value:
        raise RuntimeError(message)


def xhci_functions(q):
    rows = []
    for bus in q.command('query-pci'):
        for d in bus['devices']:
            if d['class_info']['class'] == 0x0c03:  # USB controllers (only xHCI attached here)
                rows.append({'bus': d['bus'], 'slot': d['slot'], 'function': d['function'],
                             'vendor': d['id']['vendor'], 'device': d['id']['device'],
                             'regions': d.get('regions', [])})
    return rows


def snapshot(q, base):
    values = {}
    for offset, count in SNAPSHOT:
        text = q.command('human-monitor-command', {'command-line': f'xp /{count}wx {base + offset:#x}'})
        words = [int(x, 16) for x in re.findall(r'\b0x[0-9a-f]{8}\b', text)]
        need(len(words) == count, f'HMP snapshot {offset:#x}: {text!r}')
        for i, w in enumerate(words):
            values[f'{offset + 4 * i:#06x}'] = w
    return values


def boot(out, fw, iso, variables, mode, device):
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
    q = None
    functions = []
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

            wait('USB2C1 CHOICE:')
            q = H.QMP(sock)
            functions = xhci_functions(q)
            (folder / 'query-pci-before.json').write_text(json.dumps(q.command('query-pci'), indent=2) + '\n')
            # Firmware is done; record VOS MMIO reads from here on.
            q.command('trace-event-set-state', {'name': 'memory_region_ops_read', 'enable': True})
            q.key({'normal': 'n', 'qualify': 'q', 'conflict': 'c', 'probe': 'p'}[mode])
            q.key('ret')
            if mode == 'conflict':
                s = wait('boot: FAILED')
                need('boot: check failed: vos.usb_probe=1 and vos.usb2-qualify=1 are mutually exclusive' in s,
                     'conflict refused')
                need('usb2c1:' not in s and 'usb: ' not in s, 'neither USB path ran')
            else:
                s = wait('gui-demo: PRESENTED;', seconds=60)
                need('rust: kernel probe PASS' in s, 'R0 native call')
                if mode == 'qualify':
                    verdict = re.search(r'usb2c1: (QUALIFIED|REJECTED|FAIL): ([^\r\n]*)', s)
                    need(verdict, 'qualification verdict')
                    result['verdict'] = [verdict[1], verdict[2]]
                    result['report'] = dict(re.findall(r'\b(\w+)=(0x[0-9a-f]+)', s[s.index('usb2c1:'):]))
                    expected = {'qemu-xhci': ('QUALIFIED', 'profile qualified'),
                                'none': ('REJECTED', 'no xHCI on bus 0'),
                                'two-xhci': ('REJECTED', 'ambiguous xHCI candidates'),
                                'nec-xhci': ('REJECTED', 'unsupported identity')}[device]
                    need(tuple(result['verdict']) == expected, f'verdict {result["verdict"]} != {expected}')
                    need('usb2c1: resources restored' in s, 'resources restored')
                    need('boot: check failed' not in s, 'no boot check failure')
                else:
                    need('usb2c1:' not in s, 'qualification absent unless selected')
                if mode == 'probe':
                    need('usb: summary inspected=1' in s and 'usb: PCI command and BAR unchanged' in s, 'USB.1 probe')
                if mode == 'normal':
                    need('usb: ' not in s, 'USB.1 absent unless selected')
                full = device == 'qemu-xhci'
                q.key('a'); wait('gui-demo: KEY a PRESENTED')
                q.key('b'); wait('gui-demo: KEY b PRESENTED')
                if full and mode in ('normal', 'qualify'):
                    q.command('screendump', {'filename': str(folder / 'gui.png'), 'format': 'png'})
                q.key('q'); wait('user[vsh]: VOS>')
                need('gui: PASS; kernel-CR3 reclamation, all resources restored' in data(), 'GUI cleanup')
                if full:
                    def command(line, expected, prompt='user[vsh]: VOS>'):
                        start = len(data()); q.text(line + '\n'); s = wait(prompt, start)
                        need(expected in s[start:], 'application ' + line)
                    command('apps', 'rixie'); command('hello', 'Hello from VOS!')
                    command('rixie', 'deterministic', 'user[rixie]: Rixie>')
                    command('help', 'Topics:', 'user[rixie]: Rixie>')
                    command('exit', 'Goodbye from Rixie.')
                    command('echo restored', 'user[vsh]: restored')
                    if mode in ('normal', 'qualify'):
                        q.command('screendump', {'filename': str(folder / 'shell.png'), 'format': 'png'})
                    q.text('exit\n'); wait('native: vsh PASS, process reclaimed')
            # Independent observations, with read tracing off so HMP reads are
            # never attributed to VOS.
            for event in ('memory_region_ops_read', 'usb_xhci_cap_read', 'usb_xhci_oper_read',
                          'usb_xhci_port_read', 'usb_xhci_runtime_read', 'usb_xhci_doorbell_read'):
                q.command('trace-event-set-state', {'name': event, 'enable': False})
            (folder / 'query-pci-after.json').write_text(json.dumps(q.command('query-pci'), indent=2) + '\n')
            after = xhci_functions(q)
            need(after == functions, 'QMP xHCI identity/BAR regions unchanged')
            result['xhci_functions'] = functions
            if functions and device == 'qemu-xhci':
                base = next(r['address'] for r in functions[0]['regions'] if r.get('bar') == 0)
                result['bar0'] = base
                result['register_snapshot'] = snapshot(q, base)
            result['passed_guest'] = True
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
    # Trace attribution after QEMU has exited and flushed its log.
    bdf = (functions[0]['bus'], functions[0]['slot'], functions[0]['function']) if functions else (255, 255, 255)
    analysis = T.analyze(trace, bdf)
    (folder / 'trace-analysis.json').write_text(json.dumps(analysis, indent=2) + '\n')
    result['trace_sha256'] = sha(trace)
    result['trace_bytes'] = trace.stat().st_size
    need(analysis.get('boundary_found'), 'VOS serial boundary found in trace')
    vos = analysis['vos']
    result['trace_vos'] = {k: v for k, v in vos.items() if k != 'examples'}
    need(vos['xhci_mmio_writes'] == 0, 'no xHCI MMIO writes after VOS start')
    need(vos['xhci_cfg_writes'] == 0 and vos['other_cfg_writes'] == 0, 'no PCI configuration writes after VOS start')
    need(vos['state_events'] == 0, 'no xHCI reset/run/doorbell/slot/endpoint/transfer events after VOS start')
    reads = int(result.get('report', {}).get('mmio_reads', '0x0'), 16)
    need(vos['xhci_read_events'] == vos['xhci_mmio_reads'], 'usb_xhci read events agree with MMIO reads')
    if mode == 'qualify':
        need(vos['xhci_mmio_reads'] == reads, f'trace reads {vos["xhci_mmio_reads"]} == guest reads {reads}')
    elif mode == 'probe':
        need(vos['xhci_mmio_reads'] == 7, 'USB.1 copies seven capability DWORDs')
    else:
        need(vos['xhci_mmio_reads'] == 0, 'no xHCI register access unless selected')
    before = analysis['before']
    result['trace_before_positive_control'] = {k: v for k, v in before.items() if k != 'examples'}
    if 'register_snapshot' in result:
        firmware, by_vos = T.register_writes(trace, result['bar0'], analysis['boundary_event_index'])
        need(not by_vos, 'no VOS register writes (provenance)')
        # Registers firmware programmed: the snapshot must equal its last write.
        provenance = {k: v for k, v in firmware.items() if k in result['register_snapshot']}
        stale = {k: (v, result['register_snapshot'][k]) for k, v in provenance.items()
                 if result['register_snapshot'][k] != v}
        need(not stale, f'snapshot equals last firmware write: {stale}')
        result['firmware_written_registers'] = provenance
    result['passed'] = True
    (folder / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kernel', type=Path, required=True)
    ap.add_argument('--apps', type=Path, required=True)
    ap.add_argument('--efi', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--firmware', choices=('both', 'bios', 'uefi'), default='both')
    args = ap.parse_args()
    out = args.out.resolve()
    need(not out.exists() and out.is_relative_to(ROOT / 'build'), 'fresh isolated output')
    out.mkdir(parents=True)
    inputs = {'/boot/vos.bin': args.kernel.resolve()}
    for n in APPS:
        inputs[f'/boot/{n}.elf'] = (args.apps / f'{n}.elf').resolve()
    need(all(p.is_file() and not p.is_symlink() for p in inputs.values()), 'explicit regular inputs')
    cfg = out / 'grub.cfg'
    cfg.write_text('serial --unit=0 --speed=115200\nterminal_output console serial\n'
                   'echo "USB2C1 CHOICE: n normal, q qualify, c conflict, p USB.1 probe"\n'
                   'read selection\nset extra=""\n'
                   'if [ "$selection" = "q" ]; then set extra="vos.usb2-qualify=1"; fi\n'
                   'if [ "$selection" = "c" ]; then set extra="vos.usb_probe=1 vos.usb2-qualify=1"; fi\n'
                   'if [ "$selection" = "p" ]; then set extra="vos.usb_probe=1"; fi\n'
                   'set gfxpayload=1024x768x32,auto\n'
                   'multiboot2 /boot/vos.bin vos.apps=1 vos.desktop=1 vos.gui=1 $extra\n' +
                   ''.join(f'module2 /boot/{n}.elf {n}\n' for n in APPS) + 'boot\n')
    report = {'passed': False, 'physical_hardware': False, 'hardware_writes_by_vos': None,
              'inputs': {k: sha(v) for k, v in inputs.items()}, 'cases': [], 'failures': []}
    firmwares = ('bios', 'uefi') if args.firmware == 'both' else (args.firmware,)
    try:
        tmp = out / 'producer-tmp'; tmp.mkdir()
        for fw in firmwares:
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
            for mode, device in (('normal', 'qemu-xhci'), ('qualify', 'qemu-xhci'), ('conflict', 'qemu-xhci'),
                                 ('probe', 'qemu-xhci'), ('qualify', 'none'), ('qualify', 'two-xhci'),
                                 ('qualify', 'nec-xhci')):
                try:
                    r = boot(out, fw, iso, variables, mode, device)
                    report['cases'].append(r)
                    print(r['case'], 'PASS', flush=True)
                except Exception as error:
                    report['failures'].append({'case': f'{fw}-{mode}-{device}', 'error': str(error)})
                    print(f'{fw}-{mode}-{device}', 'FAIL', error, flush=True)
            snaps = {r['mode']: r.get('register_snapshot') for r in report['cases']
                     if r['firmware'] == fw and r['device'] == 'qemu-xhci' and r['mode'] in ('normal', 'qualify')}
            if snaps.get('normal') and snaps.get('qualify'):
                diff = {k: (snaps['normal'][k], snaps['qualify'][k]) for k in snaps['normal']
                        if snaps['normal'][k] != snaps['qualify'][k]}
                report[f'{fw}_register_snapshot_normal_vs_qualify_differences'] = diff
                # Firmware (OVMF) may place its own rings at different addresses
                # per boot. A difference is accepted only where, in both boots,
                # the value is exactly what firmware last wrote before VOS ran
                # (checked per boot above); every other register must match.
                written = [set(r.get('firmware_written_registers', {})) for r in report['cases']
                           if r['firmware'] == fw and r['device'] == 'qemu-xhci' and r['mode'] in ('normal', 'qualify')]
                unexplained = {k: v for k, v in diff.items() if not all(k in w for w in written)}
                report[f'{fw}_register_differences_unexplained_by_firmware'] = unexplained
                if unexplained:
                    report['failures'].append({'case': f'{fw}-snapshot', 'error': 'register differences ' + str(unexplained)})
        report['hardware_writes_by_vos'] = 0 if not report['failures'] else 'see failures'
        report['passed'] = not report['failures'] and len(report['cases']) == 7 * len(firmwares)
    finally:
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    need(report['passed'], 'native matrix failures: ' + str(report['failures']))


if __name__ == '__main__':
    main()
