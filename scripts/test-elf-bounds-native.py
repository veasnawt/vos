#!/usr/bin/env python3
"""Focused R1 BIOS/UEFI loader and existing application checks; fresh outputs only."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import resource
import shutil
import subprocess

ROOT=Path(__file__).resolve().parents[1]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--rustc',type=Path,required=True)
    p.add_argument('--grub-efi-dir',type=Path,required=True)
    a=p.parse_args();out=a.out.resolve()
    if out.exists():p.error('output exists; preserve it and select a fresh path')
    out.mkdir(parents=True)
    spec=importlib.util.spec_from_file_location('r0_qmp',ROOT/'scripts/test-rust-kernel.py')
    qmp=importlib.util.module_from_spec(spec);spec.loader.exec_module(qmp)
    results={'native':[]}

    def save(): (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    def make(build,name,*targets,extra=()):
        with (out/(name+'.txt')).open('xb') as stream:
            r=subprocess.run(['make','--no-print-directory','-j2','ARCH=x86_64',
                              f'BUILD={build}',f'USER_BUILD={out/"user"}',
                              f'RUSTC={a.rustc}',f'GRUB_EFI_DIR={a.grub_efi_dir}',
                              *extra,*targets],cwd=ROOT,stdout=stream,stderr=subprocess.STDOUT,
                             timeout=180,env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1'))
        if r.returncode:raise RuntimeError(f'{name}: make failed, exit {r.returncode}')

    try:
        normal=out/'normal';test=out/'selftest'
        make(normal,'apps','user-vsh','user-rust','user-gui','user-pointer',extra=('RUST_APP=rixie',))
        files=' '.join(f'{out/"user"/(n+".elf")}:boot/{n}.elf'
                       for n in ['vsh','hello','rixie','gui-demo','pointer-demo'])
        make(normal,'normal-images','uefi-iso',extra=('GRUB_CFG=arch/x86_64/grub-pointer.cfg','ISO_FILES='+files))
        # Existing default selftest exercises real malformed ELF rejection and
        # load/run/destroy/rollback. No storage disk, stress case or new test gate.
        make(test,'selftest-images','uefi-iso',extra=('VOS_TEST=1',))
        symbols=subprocess.check_output(['nm','-S',str(test/'vos.bin')],text=True)
        match=re.search(r'^([0-9a-f]+) ([0-9a-f]+) T vos_rust_elf_inside$',symbols,re.M)
        assert match
        address,size=[int(x,16) for x in match.groups()]
        for firmware in ['bios','uefi']:
            prefix=out/(firmware+'-elf');prefix.mkdir()
            serial=prefix/'serial.txt';trace=prefix/'rust-execution.txt'
            image=test/('vos.iso' if firmware=='bios' else 'vos-uefi.iso')
            args=['qemu-system-x86_64','-machine','q35','-accel','tcg','-cpu','qemu64',
                  '-m','128M','-nic','none','-no-reboot','-display','none',
                  '-device','isa-debug-exit,iobase=0xf4,iosize=0x04',
                  '-serial',f'file:{serial}','-cdrom',str(image),
                  '-d','exec,nochain','-dfilter',f'{address:#x}..{address+size-1:#x}','-D',str(trace)]
            if firmware=='uefi':
                variables=prefix/'OVMF_VARS.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
                args+=['-drive','if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
                       '-drive',f'if=pflash,format=raw,file={variables}']
            (prefix/'command.json').write_text(json.dumps(args,indent=2)+'\n')
            def trace_bound():resource.setrlimit(resource.RLIMIT_FSIZE,(4*1024**2,4*1024**2))
            with (prefix/'qemu.txt').open('xb') as stream:
                process=subprocess.Popen(args,stdout=stream,stderr=stream,preexec_fn=trace_bound)
                try:code=process.wait(timeout=120)
                finally:
                    if process.poll() is None:
                        process.terminate()
                        try:process.wait(timeout=5)
                        except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
            text=serial.read_text();assert code==33,(firmware,code)
            assert 'VOS-TEST: PASS' in text and 'selftest: FAIL:' not in text and 'panic:' not in text
            m=re.search(r'selftest: elf (\d+) cases, 0 failed',text);assert m
            corruption=re.search(r'(\d+) corrupted executables rejected for the intended reason',text);assert corruption
            executed=trace.read_text().count(f'/{address:x}/');assert executed>0
            results['native'].append({'firmware':firmware,'case':'existing ELF selftest',
                                      'result':'PASS','elf_cases':int(m[1]),'corrupted_images':int(corruption[1]),
                                      'qemu_exit':code,'rust_address':hex(address),'rust_size':size,
                                      'rust_execution_records':executed,'trace_sha256':hashlib.sha256(trace.read_bytes()).hexdigest()})
            save()
            image=normal/('vos.iso' if firmware=='bios' else 'vos-uefi.iso')
            g=qmp.Guest(out/(firmware+'-interactive'),image,firmware,'qemu64')
            try:
                g.wait('rust: kernel probe PASS');g.wait('pointer-demo: READY')
                g.key('a');g.wait('pointer-demo: KEY a');g.key('b');g.wait('pointer-demo: KEY b')
                g.key('q');g.wait('pointer-demo: RELEASED');g.wait('VOS>')
                g.line('apps');g.wait('rixie');start=len(g.text());g.line('hello');g.wait('Hello from VOS!',start)
                g.line('rixie');g.wait('Rixie>');start=len(g.text());g.line('help');g.wait('Rixie>',start)
                start=len(g.text());g.line('exit');g.wait('VOS>',start)
                start=len(g.text());g.line('echo restored');g.wait('restored',start)
                g.line('exit');g.wait('native: vsh PASS, process reclaimed')
                assert g.text().count('console: restored parent')==2 and 'panic:' not in g.text()
                results['native'].append({'firmware':firmware,'case':'normal GUI/apps/hello/Rixie/shell',
                                          'result':'PASS','console_restorations':2,
                                          'serial_sha256':hashlib.sha256(g.log.read_bytes()).hexdigest()})
                save()
            finally:g.stop()
        results['status']='PASS';save();print(json.dumps(results,indent=2))
    except BaseException as error:
        results['status']='FAIL';results['error']=str(error);save();raise


if __name__=='__main__':main()
