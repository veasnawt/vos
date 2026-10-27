#!/usr/bin/env python3
"""USB.2-B opt-in/default firmware checks. No host disks, network or USB I/O.

Two fresh ISOs share a GRUB choice prompt, avoiding four payload copies. Type
r + Enter for resources/read-only discovery; Enter for the normal GUI/shell.
All executable inputs are explicit; no application build or package download.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def need(value, message):
    if not value:
        raise RuntimeError(message)


def run(argv, log, env=None):
    with log.open('x') as f:
        p = subprocess.run(list(map(str, argv)), cwd=ROOT, stdout=f, stderr=subprocess.STDOUT,
                           timeout=120, env=env)
    need(p.returncode == 0, 'producer/validator failed: '+str(log))
    return log.read_text()


def validate(iso, expected, firmware, log):
    catalog = run(['xorriso','-indev',iso,'-report_el_torito','plain','-report_system_area','plain'],log)
    need(('UEFI' if firmware == 'uefi' else 'BIOS') in catalog, 'firmware entry')
    # Same bounded plain ISO extent inspection as test-n6-desktop.py; no payload copies.
    p = subprocess.run(['xorriso','-indev',str(iso),'-find','/','-type','f','-exec','report_lba','--'],
                       cwd=ROOT, capture_output=True, text=True, check=True)
    rows = {}
    for line in p.stdout.splitlines():
        m = re.match(r"File data lba:\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*'([^']+)'",line)
        if m:
            need(int(m[1]) == 0 and m[5] not in rows, 'single ISO extent')
            rows[m[5]] = (int(m[2])*2048, int(m[4]), int(m[3])*2048)
    with iso.open('rb') as f:
        for name, source in expected.items():
            need(name in rows, 'missing image payload '+name)
            offset, size, extent = rows[name]
            need(size <= extent and offset+extent <= iso.stat().st_size and size <= 32*1024**2,
                 'bounded ISO payload')
            f.seek(offset)
            need(hashlib.sha256(f.read(size)).hexdigest() == sha(source), 'exact payload '+name)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--kernel', type=Path, required=True)
    ap.add_argument('--apps', type=Path, required=True)
    ap.add_argument('--efi', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--media', type=Path, help='reuse previously validated images, recheck all payloads')
    ap.add_argument('--firmware', choices=('both','bios','uefi'), default='both')
    args = ap.parse_args()
    firmwares = ('bios','uefi') if args.firmware=='both' else (args.firmware,)
    out = args.out.resolve()
    need(not out.exists() and out.is_relative_to(ROOT/'build'), 'fresh isolated output')
    out.mkdir(parents=True)
    spec = importlib.util.spec_from_file_location('usb_qmp', ROOT/'scripts/test-usb-probe.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    inputs = {'/boot/vos.bin': args.kernel.resolve()}
    for name in ('vsh','hello','rixie','gui-demo'):
        inputs['/boot/'+name+'.elf'] = (args.apps/(name+'.elf')).resolve()
    need(all(p.is_file() and not p.is_symlink() for p in inputs.values()), 'explicit regular inputs')
    identities = {name: {'bytes':p.stat().st_size,'sha256':sha(p)} for name,p in inputs.items()}
    cfg = out/'grub.cfg'
    cfg.write_text('serial --unit=0 --speed=115200\nterminal_output console serial\n'
                   'echo "USB2B CHOICE: r then Enter for resource tests; Enter for normal GUI"\n'
                   'read selection\nset extra=""\n'
                   'if [ "$selection" = "r" ]; then\n'
                   ' set extra="vos.usb-resources=1 vos.usb_probe=1"\nfi\n'
                   'set gfxpayload=1024x768x32,auto\n'
                   'multiboot2 /boot/vos.bin vos.apps=1 vos.desktop=1 vos.gui=1 $extra\n'+
                   ''.join('module2 /boot/'+n+'.elf '+n+'\n' for n in ('vsh','hello','rixie','gui-demo'))+
                   'boot\n')
    inputs['/boot/grub/grub.cfg'] = cfg
    report = {'passed':False,'hardware_activation':False,'payloads':identities,'cases':[]}
    try:
        tmp = out/'producer-tmp'; tmp.mkdir()
        media = args.media.resolve() if args.media else out
        for fw, directory in [('bios',Path('/usr/lib/grub/i386-pc')),('uefi',args.efi)]:
            if fw not in firmwares:
                continue
            expected = dict(inputs)
            if fw == 'uefi':
                # Preserve the established UEFI GOP/input producer contract.
                # Without this header the kernel correctly refuses GUI startup.
                efi_cfg = out/'grub-uefi.cfg'
                efi_cfg.write_text((ROOT/'arch/x86_64/grub-uefi-head.cfg').read_text()+cfg.read_text())
                expected['/boot/grub/grub.cfg'] = efi_cfg
            iso = media/(fw+'.iso')
            if not args.media:
                run(['grub-mkrescue','-d',directory,'-o',iso,'-graft-points',
                     *[name+'='+str(p) for name,p in expected.items()]],out/(fw+'-producer.log'),
                    dict(os.environ,TMPDIR=str(tmp),PYTHONDONTWRITEBYTECODE='1'))
            validate(iso, expected, fw, out/(fw+'-image-validation.log'))
        report['media'] = {fw:{'path':str(media/(fw+'.iso')),'sha256':sha(media/(fw+'.iso'))}
                           for fw in firmwares}
        # One private OVMF variable image for sequential owned guests. It is
        # retained, never shared with an external QEMU process.
        variables = out/'OVMF_VARS_4M.fd'
        shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        for fw in firmwares:
            for mode in ('normal','resources'):
                folder = out/(fw+'-'+mode);folder.mkdir()
                serial=folder/'serial.log';sock=folder/'qmp'
                argv=['qemu-system-x86_64','-machine','q35','-accel','tcg','-m','128M',
                      '-cdrom',str(media/(fw+'.iso')),'-boot','d','-display','none','-nic','none',
                      '-device','qemu-xhci,id=xhci','-serial','file:'+str(serial),
                      '-qmp','unix:'+str(sock)+',server=on,wait=off','-no-reboot','-no-shutdown']
                if fw=='uefi':
                    argv += ['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                             '-drive','if=pflash,format=raw,unit=1,file='+str(variables)]
                (folder/'command.json').write_text(json.dumps(argv,indent=2)+'\n')
                q=None
                with (folder/'qemu.log').open('x') as log:
                    process=subprocess.Popen(argv,cwd=ROOT,stdout=log,stderr=log)
                    try:
                        def data():
                            need(process.poll() is None,'owned QEMU exited')
                            s=serial.read_text(errors='replace') if serial.exists() else ''
                            need(not any(m in s for m in ('panic:','boot: check failed','boot: FAILED','gui: FAIL')),
                                 'guest failure '+str(folder))
                            return s
                        def wait(marker,start=0,seconds=35):
                            deadline=time.monotonic()+seconds
                            while time.monotonic()<deadline:
                                s=data()
                                if marker in s[start:]:return s
                                time.sleep(.025)
                            raise RuntimeError('bounded marker timeout '+marker+' in '+str(folder))
                        wait('USB2B CHOICE:')
                        q=helper.QMP(sock)
                        if mode=='resources':q.key('r')
                        q.key('ret')
                        s=wait('gui-demo: PRESENTED;')
                        need('rust: kernel probe PASS' in s, 'R0 native call')
                        if mode=='resources':
                            for marker in ('usb2b: native Rust resource stage 0 PASS',
                                           'usb2b: native Rust resource stage 1 PASS',
                                           'usb2b: native Rust resource stage 2 PASS',
                                           'usb2b: native Rust resource stage 3 PASS',
                                           '18 created/18 reaped','Rust Drop retains real page identity',
                                           'usb2b: PASS','usb: PCI command and BAR unchanged',
                                           'usb: summary inspected=1 unsupported=0 invalid=0'):
                                need(marker in s,'native evidence '+marker)
                        else:need('usb2b:' not in s and 'usb: ' not in s,'default paths unchanged')
                        before=q.command('query-pci')
                        q.key('a');wait('gui-demo: KEY a PRESENTED')
                        q.key('b');wait('gui-demo: KEY b PRESENTED')
                        if mode=='resources':
                            q.command('screendump',{'filename':str(folder/'gui.png'),'format':'png'})
                        q.key('q');wait('user[vsh]: VOS>')
                        need('gui: PASS; kernel-CR3 reclamation, all resources restored' in data(),'GUI cleanup')
                        def command(line,expected,prompt='user[vsh]: VOS>'):
                            start=len(data());q.text(line+'\n');s=wait(prompt,start)
                            need(expected in s[start:],'application '+line)
                        command('apps','rixie');command('hello','Hello from VOS!')
                        command('rixie','deterministic','user[rixie]: Rixie>')
                        command('help','Topics:','user[rixie]: Rixie>')
                        command('exit','Goodbye from Rixie.')
                        command('echo restored','user[vsh]: restored')
                        need(before==q.command('query-pci'),'PCI metadata unchanged')
                        q.text('exit\n');wait('native: vsh PASS, process reclaimed')
                        (folder/'pci.json').write_text(json.dumps(before,indent=2)+'\n')
                        report['cases'].append({'firmware':fw,'mode':mode,'passed':True,
                                                'serial_sha256':sha(serial),'pci_unchanged':True})
                    finally:
                        try:
                            if process.poll() is None and not any(c['firmware']==fw and c['mode']==mode for c in report['cases']):
                                try:
                                    if q is None:q=helper.QMP(sock)
                                    q.command('screendump',{'filename':str(folder/'failed-boot.png'),'format':'png'})
                                except (OSError,RuntimeError) as error:
                                    report.setdefault('diagnostic_errors',[]).append(str(error))
                        finally:
                            try:
                                if q:q.close()
                            finally:
                                if process.poll() is None:
                                    process.terminate()
                                    try:process.wait(timeout=5)
                                    except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
        for name,p in inputs.items():
            if name in identities:need(sha(p)==identities[name]['sha256'],'read-only input preserved')
        report['passed']=True
    except Exception as error:
        report['error']=str(error)
        raise
    finally:
        (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':
    main()
