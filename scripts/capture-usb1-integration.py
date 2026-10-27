#!/usr/bin/env python3
"""Actual QEMU GUI capture after USB.1 discovery; PS/2 input, no USB HID claims."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import resource
import shutil
import struct
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--usb-result',type=Path,required=True)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--trace-discovery',action='store_true',
                        help='Bounded execution trace at the linked Rust xhci_probe')
    parser.add_argument('--kernel',type=Path,
                        help='Exact image kernel for tracing a separately preserved continuation')
    a=parser.parse_args();out=a.out.resolve();result_path=a.usb_result.resolve()
    if out.exists() or not out.is_relative_to(ROOT/'build'):parser.error('fresh output below this checkout build/ required')
    verified=json.loads(result_path.read_text())
    if not verified.get('passed'):parser.error('USB.1 verification must have passed')
    probe_range=None
    if a.trace_discovery:
        kernel=a.kernel.resolve() if a.kernel else result_path.parent/'kernel/vos.bin'
        symbols=subprocess.check_output(['nm','-S',str(kernel)],text=True)
        match=re.search(r'^([0-9a-f]+) ([0-9a-f]+) T xhci_probe$',symbols,re.M)
        if not match:parser.error('linked Rust xhci_probe symbol missing')
        probe_range=tuple(int(x,16) for x in match.groups())
    spec=importlib.util.spec_from_file_location('usb_qmp',ROOT/'scripts/test-usb-probe.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    out.mkdir(parents=True);results=[]
    for fw in ['bios','uefi']:
        folder=out/fw;folder.mkdir();serial=folder/'serial.txt';sock=folder/'qmp'
        iso=Path(verified['media']['probe'][fw]);image=folder/'gui.png'
        args=['qemu-system-x86_64','-machine','q35','-accel','tcg','-m','128M',
              '-cdrom',str(iso),'-boot','d','-display','none','-nic','none',
              '-device','qemu-xhci,id=xhci','-serial',f'file:{serial}',
              '-qmp',f'unix:{sock},server=on,wait=off','-no-reboot']
        trace=folder/'rust-probe-execution.txt'
        if probe_range:
            address,size=probe_range
            args+=['-d','exec,nochain','-dfilter',f'{address:#x}..{address+size-1:#x}','-D',str(trace)]
        if fw=='uefi':
            variables=folder/'OVMF_VARS.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
            args+=['-drive','if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                   '-drive',f'if=pflash,format=raw,file={variables}']
        (folder/'command.json').write_text(json.dumps(args,indent=2)+'\n');q=None
        with (folder/'qemu.txt').open('xb') as stream:
            def trace_bound():resource.setrlimit(resource.RLIMIT_FSIZE,(4*1024**2,4*1024**2))
            process=subprocess.Popen(args,cwd=ROOT,stdout=stream,stderr=stream,
                                     preexec_fn=trace_bound if probe_range else None)
            try:
                def text():
                    if process.poll() is not None:raise RuntimeError('owned QEMU exited')
                    data=serial.read_text(errors='replace') if serial.exists() else ''
                    if 'panic:' in data or 'boot: check failed' in data:raise RuntimeError('guest failure')
                    return data
                def wait(marker,start=0):
                    deadline=time.monotonic()+60
                    while time.monotonic()<deadline:
                        data=text()
                        if marker in data[start:]:return data
                        time.sleep(.025)
                    raise RuntimeError('missing marker: '+marker)
                data=wait('gui-demo: PRESENTED;')
                assert 'usb: summary inspected=1 unsupported=0 invalid=0' in data
                assert 'usb: PCI command and BAR unchanged' in data and 'WARNING PCI' not in data
                m=re.search(r'fn=(0x[0-9a-f]+) seed=(0x[0-9a-f]+) salt=(0x[0-9a-f]+) result=(0x[0-9a-f]+)',data);assert m
                _,seed,salt,value=[int(x,16) for x in m.groups()]
                assert value==(((((seed<<17)|(seed>>47))+((salt>>9)|(salt<<55)))&((1<<64)-1))^0x564f535255535430)
                q=module.QMP(sock);before=q.command('query-pci')
                q.key('a');wait('gui-demo: KEY a PRESENTED')
                q.command('screendump',{'filename':str(image),'format':'png'})
                raw=image.read_bytes();assert raw[:8]==b'\x89PNG\r\n\x1a\n'
                width,height=struct.unpack('>II',raw[16:24]);assert width and height
                q.key('q');wait('user[vsh]: VOS>')
                assert 'gui: PASS; kernel-CR3 reclamation, all resources restored' in text()
                def command(line,expected,prompt='user[vsh]: VOS>'):
                    start=len(text());q.text(line+'\n');data=wait(prompt,start);assert expected in data[start:]
                command('apps','rixie');command('hello','Hello from VOS!')
                command('rixie','deterministic','user[rixie]: Rixie>')
                command('help','Topics:','user[rixie]: Rixie>');command('exit','Goodbye from Rixie.')
                command('echo restored','user[vsh]: restored')
                after=q.command('query-pci');assert before==after
                (folder/'pci-before.json').write_text(json.dumps(before,indent=2)+'\n')
                (folder/'pci-after.json').write_text(json.dumps(after,indent=2)+'\n')
                q.text('exit\n');wait('native: vsh PASS, process reclaimed')
                executed=trace.read_text().count(f'/{probe_range[0]:x}/') if probe_range else None
                if probe_range:assert executed>0,'actual Rust probe was not executed'
                results.append({'firmware':fw,'passed':True,'screenshot':str(image.relative_to(out)),
                                'width':width,'height':height,'screenshot_sha256':hashlib.sha256(raw).hexdigest(),
                                'serial_sha256':hashlib.sha256(serial.read_bytes()).hexdigest(),
                                'USB_keyboard_attached':False,'input':'existing PS/2','pci_metadata_unchanged':True,
                                'Rust_xhci_probe_execution_records':executed})
            finally:
                if q:q.close()
                if process.poll() is None:
                    process.terminate()
                    try:process.wait(timeout=5)
                    except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
    (out/'results.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results,indent=2))


if __name__=='__main__':main()
