#!/usr/bin/env python3
"""Fixed USB.1 baseline/migration boot comparison; no automatic retry or builds.

Requires already verified equivalent probe media. Every guest uses a private
OVMF variable copy, no network/host disk, and the unchanged 35-second GUI gate.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]


def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline',type=Path,required=True,help='retained USB.1 producer root')
    p.add_argument('--migration',type=Path,required=True,help='retained migration producer root')
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--present-phases',action='store_true',help='four fixed UEFI present-controller starts with detailed phase timestamps')
    a=p.parse_args();out=a.out.resolve();roots={'C':a.baseline.resolve(),'Rust':a.migration.resolve()}
    if out.exists() or not out.is_relative_to(ROOT/'build'):p.error('fresh output below checkout build/ required')
    for root in roots.values():
        for fw in ['bios','uefi']:
            if not (root/f'probe-{fw}.iso').is_file():p.error('verified probe media missing')
    out.mkdir(parents=True)
    spec=importlib.util.spec_from_file_location('usb_qmp',ROOT/'scripts/test-usb-probe.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    result={'passed':False,'cases':[],'timeout_seconds':35,'retry_policy':'none; all starts predefined',
            'tool_sha256':sha(__file__),
            'phase_resolution':'host serial observation every 20 ms, not guest CPU latency',
            'firmware_sha256':{name:sha('/usr/share/OVMF/'+name) for name in ['OVMF_CODE_4M.fd','OVMF_VARS_4M.fd']},
            'image_sha256':{name:{fw:sha(root/f'probe-{fw}.iso') for fw in ['bios','uefi']} for name,root in roots.items()}}
    # Four paired no-controller UEFI attempts, balanced AB/BA order. Plus
    # one present-controller BIOS/UEFI boot per implementation, not a retry loop.
    plan=[(fw,present,name) for n in range(4) for name in (['C','Rust'] if n%2==0 else ['Rust','C']) for fw,present in [('uefi',False)]]
    plan += [('bios',True,'C'),('bios',True,'Rust'),('uefi',True,'Rust'),('uefi',True,'C')]
    if a.present_phases:plan=[('uefi',True,n) for n in ['C','Rust','Rust','C']]
    result['plan']=[{'firmware':f,'controller_present':c,'implementation':n} for f,c,n in plan]

    def save(): (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')

    try:
        # Inspect actual ISO contents. No reconstructed kernel/application inputs.
        payloads={}
        for name,root in roots.items():
            check=out/(name+'-payloads');check.mkdir()
            for file in ['vos.bin','vsh.elf','hello.elf','rixie.elf','gui-demo.elf','grub/grub.cfg']:
                target=check/file;target.parent.mkdir(exist_ok=True)
                with (check/(file.replace('/','-')+'.txt')).open('xb') as log:
                    r=subprocess.run(['xorriso','-osirrox','on','-indev',str(root/'probe-uefi.iso'),'-extract','/boot/'+file,str(target)],stdout=log,stderr=log,timeout=30)
                if r.returncode:raise RuntimeError('image extraction failed')
            payloads[name]={file:sha(check/file) for file in ['vos.bin','vsh.elf','hello.elf','rixie.elf','gui-demo.elf','grub/grub.cfg']}
        for f in payloads['C']:
            if f!='vos.bin' and payloads['C'][f]!=payloads['Rust'][f]:raise RuntimeError('unequal boot input '+f)
        result['payloads']=payloads;save()
        for number,(fw,present,name) in enumerate(plan):
            folder=out/f'{number+1:02}-{name}-{fw}-{"present" if present else "absent"}'
            folder.mkdir();serial=folder/'serial.txt';socket=folder/'qmp';image=roots[name]/f'probe-{fw}.iso'
            argv=['qemu-system-x86_64','-machine','q35','-accel','tcg','-m','128M','-nic','none','-cdrom',str(image),'-boot','d',
                  '-display','none','-serial',f'file:{serial}','-qmp',f'unix:{socket},server=on,wait=off','-no-reboot','-no-shutdown']
            if present:argv+=['-device','qemu-xhci,id=xhci']
            if fw=='uefi':
                variables=folder/'OVMF_VARS.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
                argv+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                       '-drive',f'if=pflash,format=raw,unit=1,file={variables}']
            (folder/'command.json').write_text(json.dumps(argv,indent=2)+'\n')
            record={'case':folder.name,'firmware':fw,'implementation':name,'controller_present':present,'events':{},'passed':False};q=None
            with (folder/'qemu.txt').open('xb') as log:
                started=time.monotonic();process=subprocess.Popen(argv,cwd=ROOT,stdout=log,stderr=log)
                try:
                    text=''
                    while time.monotonic()-started<35:
                        text=serial.read_text(errors='replace') if serial.exists() else ''
                        if 'panic:' in text or 'boot: check failed' in text:raise RuntimeError('guest failure '+folder.name)
                        markers=[('firmware_handoff','BdsDxe: starting'),('kernel','Hello from VOS!'),('USB_begin','usb: USB.1 read-only'),('USB_end','usb: summary'),
                                 ('boot_checks','boot: checks passed'),('desktop','desktop: scene ready'),('worker','input-diag: GUI input worker startup ok'),
                                 ('ELF','input-diag: GUI ELF load ok'),('keyboard','input-diag: GUI PS/2 keyboard initialization ok'),
                                 ('grant','input-diag: GUI authority grant ok'),('GUI','gui-demo: PRESENTED;')]
                        for label,marker in markers:
                            if marker in text and label not in record['events']:record['events'][label]=round(time.monotonic()-started,4)
                        if 'GUI' in record['events']:break
                        if process.poll() is not None:break
                        time.sleep(.02)
                    if 'GUI' in record['events']:
                        expected='inspected=1 unsupported=0 invalid=0' if present else 'inspected=0 unsupported=0 invalid=0'
                        if expected not in text or 'WARNING PCI' in text:raise RuntimeError('discovery result mismatch')
                        if present and 'usb: PCI command and BAR unchanged' not in text:raise RuntimeError('PCI readback missing')
                        record['passed']=True
                    else:
                        record['failure']='35-second GUI timeout or premature QEMU exit'
                    if (name=='Rust' and present) or not record['passed']:
                        q=module.QMP(socket);q.command('screendump',{'filename':str(folder/'screen.png'),'format':'png'})
                        record['screenshot_sha256']=sha(folder/'screen.png')
                    record['elapsed_seconds']=round(time.monotonic()-started,4)
                    record['pretermination_exit']=process.poll()
                finally:
                    if q:q.close()
                    if process.poll() is None:
                        process.terminate()
                        try:process.wait(timeout=5)
                        except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
                    record['supervised_exit_code']=process.returncode
                    record['serial_sha256']=sha(serial) if serial.exists() else None
                    result['cases'].append(record);save()
            print(json.dumps(record),flush=True)
        result['passed']=all(c['passed'] for c in result['cases']);save()
    except BaseException as error:
        result['error']=str(error);save();raise


if __name__=='__main__':main()
