#!/usr/bin/env python3
"""USB.2-C2 BIOS/UEFI native checks with independent QEMU observations.

One ISO per firmware carries a GRUB choice: n normal, d DMA ownership exercise
(vos.usb2c2-dma=1), b exercise plus C1 qualification. The exercise also runs
with no xHCI attached. Traces (pci_cfg_write, all usb_xhci_* including device
ring fetches, memory_region_ops_write, msix_write_config, serial_write; MMIO
reads from the GRUB prompt on) are attributed after the kernel's first serial
byte. The C1 helpers supply QMP, register snapshots and firmware provenance.
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


C1 = load('usb2c1_native', 'test-usb2c1-native.py')
H, V, T = C1.H, C1.V, C1.T
need, sha, APPS, EVENTS, DEVICES = C1.need, C1.sha, C1.APPS, C1.EVENTS, C1.DEVICES
PAGE = 4096


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

            wait('USB2C2 CHOICE:')
            q = H.QMP(sock)
            functions = C1.xhci_functions(q)
            (folder / 'query-pci-before.json').write_text(json.dumps(q.command('query-pci'), indent=2) + '\n')
            q.command('trace-event-set-state', {'name': 'memory_region_ops_read', 'enable': True})
            q.key({'normal': 'n', 'dma': 'd', 'both': 'b'}[mode])
            q.key('ret')
            s = wait('gui-demo: PRESENTED;', seconds=60)
            need('rust: kernel probe PASS' in s and 'boot: check failed' not in s, 'boot checks')
            if mode == 'normal':
                need('usb2c2:' not in s and 'usb2c1:' not in s, 'C2/C1 absent unless selected')
            else:
                need('usb2c2: PASS' in s, 'DMA ownership exercise passed')
                text = s[s.index('usb2c2:'):]
                result['report'] = dict(re.findall(r'\b(\w+)=(0x[0-9a-f]+)', text))
                result['set_frames'] = [int(x, 16) for x in re.findall(r'set_frame=(0x[0-9a-f]+)', text)]
                need(len(result['set_frames']) == 4, 'four set frames reported')
                r = result['report']
                need(r['retained_now'] == '0x1' and r['active_now'] == '0x0' and r['frames_held'] == '0x1',
                     'one deliberate quarantine retained, nothing else live')
                need(int(r['pmm_before'], 16) - int(r['pmm_after'], 16) == 1, 'exactly one frame retained')
            if mode == 'both':
                need('usb2c1: QUALIFIED: profile qualified' in s, 'C1 qualification alongside C2')
            q.key('a'); wait('gui-demo: KEY a PRESENTED')
            q.key('b'); wait('gui-demo: KEY b PRESENTED')
            if mode == 'dma' and device == 'qemu-xhci':
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
            if mode == 'dma' and device == 'qemu-xhci':
                q.command('screendump', {'filename': str(folder / 'shell.png'), 'format': 'png'})
            q.text('exit\n'); wait('native: vsh PASS, process reclaimed')
            for event in ('memory_region_ops_read', 'usb_xhci_cap_read', 'usb_xhci_oper_read',
                          'usb_xhci_port_read', 'usb_xhci_runtime_read', 'usb_xhci_doorbell_read'):
                q.command('trace-event-set-state', {'name': event, 'enable': False})
            (folder / 'query-pci-after.json').write_text(json.dumps(q.command('query-pci'), indent=2) + '\n')
            need(C1.xhci_functions(q) == functions, 'QMP xHCI identity/BAR regions unchanged')
            result['xhci_functions'] = functions
            if functions:
                base = next(r['address'] for r in functions[0]['regions'] if r.get('bar') == 0)
                result['bar0'] = base
                result['register_snapshot'] = C1.snapshot(q, base)
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
    need(vos['xhci_mmio_reads'] == (18 if mode == 'both' and device == 'qemu-xhci' else 0),
         'register reads only from C1 qualification')
    if 'register_snapshot' in result:
        firmware, by_vos = T.register_writes(trace, result['bar0'], analysis['boundary_event_index'])
        need(not by_vos, 'no VOS register writes (provenance)')
        stale = {k: (v, result['register_snapshot'][k]) for k, v in firmware.items()
                 if k in result['register_snapshot'] and result['register_snapshot'][k] != v}
        need(not stale, f'snapshot equals last firmware write: {stale}')
        snap = result['register_snapshot']
        # Firmware ring/context pages (CRCR, DCBAAP, ERSTBA, ERDP; low DWORDs,
        # high DWORDs are zero in this profile) versus C2's frames.
        fw_pages = {k: (snap[k] & ~(PAGE - 1)) for k in ('0x0058', '0x0070', '0x1030', '0x1038')}
        result['firmware_dma_pages'] = {k: hex(v) for k, v in fw_pages.items()}
        frames = set(result.get('set_frames', [])) | ({int(result['report']['retained_frame'], 16)}
                                                      if 'report' in result else set())
        result['c2_frames_overlapping_firmware_pages'] = sorted(hex(f) for f in frames if f in fw_pages.values())
    result['trace_before_positive_control'] = {k: v for k, v in analysis['before'].items() if k != 'examples'}
    result['passed'] = True
    (folder / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kernel', type=Path, required=True)
    ap.add_argument('--apps', type=Path, required=True)
    ap.add_argument('--efi', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
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
                   'echo "USB2C2 CHOICE: n normal, d DMA ownership exercise, b exercise and qualification"\n'
                   'read selection\nset extra=""\n'
                   'if [ "$selection" = "d" ]; then set extra="vos.usb2c2-dma=1"; fi\n'
                   'if [ "$selection" = "b" ]; then set extra="vos.usb2-qualify=1 vos.usb2c2-dma=1"; fi\n'
                   'set gfxpayload=1024x768x32,auto\n'
                   'multiboot2 /boot/vos.bin vos.apps=1 vos.desktop=1 vos.gui=1 $extra\n' +
                   ''.join(f'module2 /boot/{n}.elf {n}\n' for n in APPS) + 'boot\n')
    report = {'passed': False, 'physical_hardware': False, 'inputs': {k: sha(v) for k, v in inputs.items()},
              'cases': [], 'failures': []}
    cases = (('normal', 'qemu-xhci'), ('dma', 'qemu-xhci'), ('both', 'qemu-xhci'), ('dma', 'none'))
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
            for mode, device in cases:
                try:
                    r = boot(out, fw, iso, variables, mode, device)
                    report['cases'].append(r)
                    print(r['case'], 'PASS', flush=True)
                except Exception as error:
                    report['failures'].append({'case': f'{fw}-{mode}-{device}', 'error': str(error)})
                    print(f'{fw}-{mode}-{device}', 'FAIL', error, flush=True)
        report['passed'] = not report['failures'] and len(report['cases']) == 2 * len(cases)
    finally:
        (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    need(report['passed'], 'native matrix failures: ' + str(report['failures']))


if __name__ == '__main__':
    main()
