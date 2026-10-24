#!/usr/bin/env python3
"""Focused native console/ABI probes and Rust vsh on BIOS and UEFI. Fresh outputs only."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import resource
import shutil
import struct
import subprocess
import time

REPO=Path(__file__).resolve().parents[1]
H=runpy.run_path(str(REPO/'scripts/test-keyboard-console.py'),run_name='n4_helpers')
run,QMP,png,require=H['run'],H['QMP'],H['png_from_ppm'],H['require']
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()

def boot(out,fw,iso,disk):
    folder=out/fw;folder.mkdir();serial=folder/'serial.log';sock=folder/'qmp'
    args=['qemu-system-x86_64','-machine','q35','-accel','tcg','-m','128M','-cdrom',str(iso),'-boot','d',
          '-display','none','-nic','none','-serial','file:'+str(serial),'-qmp','unix:'+str(sock)+',server=on,wait=off',
          '-no-reboot','-no-shutdown','-drive','file='+str(disk)+',if=none,id=vs0,format=raw,readonly=on',
          '-device','virtio-blk-pci,drive=vs0,disable-legacy=on']
    if fw=='uefi':
        variables=folder/'OVMF_VARS_4M.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        args+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
               '-drive','if=pflash,format=raw,unit=1,file='+str(variables)]
    (folder/'command.json').write_text(json.dumps(args,indent=2)+'\n')
    q=None
    with (folder/'qemu.log').open('x') as log:
        p=subprocess.Popen(args,cwd=REPO,stdout=log,stderr=subprocess.STDOUT)
        try:
            def wait(marker,seconds=45):
                until=time.monotonic()+seconds
                while True:
                    require(p.poll() is None,'QEMU exited')
                    data=serial.read_bytes() if serial.exists() else b''
                    require(not any(x in data for x in (b'panic:',b'boot: check failed',b'native: console-owner FAIL',b'native: vsh FAIL')),'guest failure')
                    if marker in data:return data
                    require(time.monotonic()<until,'guest marker deadline: '+repr(marker));time.sleep(.05)
            wait(b'user[console-owner]: probe: ready')
            q=QMP(sock)
            def text(s):
                codes={' ':'spc','/':'slash','.':'dot','-':'minus'}
                for c in s:
                    if c.isupper():q.key('shift',True);q.tap(c.lower());q.key('shift',False)
                    else:q.tap(codes.get(c,c))
            text('abc');q.tap('ret');wait(b'probe: empty',5);q.tap('ret')
            data=wait(b'user[vsh]: VOS> ')
            for name in ('console-v0','console-v1','console-v2','console-denied','console-owner'):
                require(('native: '+name+' PASS, process reclaimed').encode() in data,'probe not passed '+name)
            def command(s):
                before=serial.read_bytes().count(b'user[vsh]: VOS> ')
                text(s);q.tap('ret');until=time.monotonic()+8
                while True:
                    data=serial.read_bytes()
                    require(b'panic:' not in data and b'boot: check failed' not in data,'guest failure')
                    if data.count(b'user[vsh]: VOS> ')>before:return data
                    require(time.monotonic()<until,'command deadline '+s);time.sleep(.05)
            command('help')
            data=command('echo Hello VOS')
            require(b'user[vsh]: Available commands: help, clear, echo, pwd, ls, cd, cat' in data,'help')
            require(b'user[vsh]: Hello VOS' in data,'echo')
            q.execute('screendump',{'filename':str(folder/'shell.ppm')});screen=png(folder/'shell.ppm',folder/'shell.png')
            command('clear');command('pwd');command('ls');command('cd docs');command('pwd');data=command('cat welcome.txt')
            require(b'user[vsh]: /docs' in data and b'user[vsh]: Real ext2 file read by native Rust vsh.' in data,'filesystem/cwd')
            data=command('cd ../../');require(b'vsh: operation failed' not in data,'root clamp')
            command('pwd');data=command('cat missing.txt');require(b'user[vsh]: vsh: operation failed' in data,'missing file not rejected')
            command('echo filesystem checks complete')
            q.execute('screendump',{'filename':str(folder/'files.ppm')});files=png(folder/'files.ppm',folder/'files.png')
            text('exit');q.tap('ret');data=wait(b'native: vsh PASS, process reclaimed',8)
            regs=q.execute('human-monitor-command',{'command-line':'info registers'})
            require('HLT=1' in regs,'kernel did not halt after shell exit')
            require(b'proc[hello]: exited status 0, all frames reclaimed' in data,'ABI v0 hello regression')
            return dict(passed=True,ring3_probes=5,commands=['help','clear','echo','pwd','ls','cd','cat','exit'],shell_screenshot=screen,files_screenshot=files,exit_status=0,reclaimed=True)
        finally:
            if q:q.close()
            if p.poll() is None:p.terminate()
            try:p.wait(timeout=3)
            except subprocess.TimeoutExpired:p.kill();p.wait(timeout=3)

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',required=True,type=Path);a=ap.parse_args()
    out=a.output.resolve();require(out.is_relative_to(REPO/'build') and not out.exists(),'fresh output required');out.mkdir(parents=True)
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));resource.setrlimit(resource.RLIMIT_FSIZE,(64*1024**2,64*1024**2))
    result={'scope':'native Rust vsh and ABI v3 console','firmware':{}}
    try:
        cc=['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Iarch/x86_64']
        run(cc+['tests/host/user-console.c','arch/x86_64/syscall.c','-o',str(out/'host-console')],out/'host-build.log')
        run([str(out/'host-console')],out/'host-test.log',10)
        run(cc+['tests/host/buffered-input.c','arch/x86_64/input_console.c','-o',str(out/'host-reader')],out/'reader-build.log')
        run([str(out/'host-reader')],out/'reader-test.log',10)
        rust=os.environ.get('RUSTC','rustc');result['rustc']=subprocess.check_output([rust,'--version'],text=True).strip()
        user=out/'user';build=out/'kernel'
        run(['make','--no-print-directory','USER_BUILD='+str(user),'RUSTC='+rust,'user-vsh']+
            [str(user/('console-'+s+'.elf')) for s in ['v0','v1','v2','denied','owner']],out/'user-build.log')
        for name in ['vsh','console-v0','console-v1','console-v2','console-denied','console-owner']:
            run(['scripts/check-user-elf.sh',str(user/(name+'.elf'))],out/(name+'-elf.log'),30)
        # Same loader validator must still reject future ABI notes and W+X.
        run(cc+['scripts/elfcheck.c','arch/x86_64/elf.c','-o',str(out/'elfcheck')],out/'elfcheck-build.log')
        run([str(out/'elfcheck'),str(user/'vsh.elf')],out/'elfcheck.log')
        original=(user/'vsh.elf').read_bytes()
        for kind in ['future-abi','writable-code']:
            data=bytearray(original);phoff=struct.unpack_from('<Q',data,32)[0];count=struct.unpack_from('<H',data,56)[0]
            for i in range(count):
                p=phoff+56*i;typ,flags=struct.unpack_from('<II',data,p)
                if kind=='future-abi' and typ==4:
                    off=struct.unpack_from('<Q',data,p+8)[0];struct.pack_into('<I',data,off+16,4);break
                if kind=='writable-code' and typ==1 and flags&1:
                    struct.pack_into('<I',data,p+4,7);break
            bad=out/(kind+'.elf');bad.write_bytes(data)
            for tool in ['scripts/check-user-elf.sh',str(out/'elfcheck')]:
                rejection=subprocess.run([tool,str(bad)],cwd=REPO,capture_output=True,text=True)
                require(rejection.returncode==1,'invalid ELF was accepted: '+kind)
                with (out/'elf-negative.log').open('a') as f:f.write(tool+' '+kind+'\n'+rejection.stdout+rejection.stderr)
        source=out/'fixture';(source/'shell/docs').mkdir(parents=True)
        (source/'shell/docs/welcome.txt').write_text('Real ext2 file read by native Rust vsh.\n')
        (source/'shell/readme.txt').write_text('VOS native shell filesystem fixture.\n')
        (source/'outside.txt').write_text('Outside the shell grant.\n')
        disk=out/'disk.img'
        with disk.open('xb') as f:f.truncate(8*1024**2)
        run(['mke2fs','-q','-F','-t','ext2','-b','1024','-I','128','-O','none','-d',str(source),str(disk)],out/'fixture-build.log')
        run(['e2fsck','-fn',str(disk)],out/'fixture-check.log');os.chmod(disk,0o444);digest=sha(disk)
        cfg=out/'grub.cfg';lines=['set timeout=0','set default=0','menuentry "VOS native vsh verification" {','multiboot2 /boot/vos.bin vos.vsh=1','module2 /boot/hello.elf hello','module2 /boot/vsh.elf vsh']
        inputs=[str(user/'vsh.elf')+':boot/vsh.elf']
        for n in ['v0','v1','v2','denied','owner']:
            lines.append('module2 /boot/console-'+n+'.elf console-'+n);inputs.append(str(user/('console-'+n+'.elf'))+':boot/console-'+n+'.elf')
        lines+=['boot','}'];cfg.write_text('\n'.join(lines)+'\n')
        run(['make','--no-print-directory','BUILD='+str(build),'USER_BUILD='+str(user),'GRUB_CFG='+str(cfg),'ISO_FILES='+' '.join(inputs),'uefi-iso'],out/'image-build.log')
        for fw,iso in [('bios','vos.iso'),('uefi','vos-uefi.iso')]:result['firmware'][fw]=boot(out,fw,build/iso,disk)
        require(sha(disk)==digest,'read-only fixture changed');result.update(passed=True,disk_sha256=digest)
    except BaseException as e:result.update(passed=False,error=str(e));raise
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        rows=[dict(path=str(p.relative_to(out)),size=p.stat().st_size,sha256=sha(p)) for p in sorted(out.rglob('*')) if p.is_file()]
        (out/'manifest.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(result),flush=True)

if __name__=='__main__':main()
