#!/usr/bin/env python3
"""Focused Rust SDK, independent application and shared-vsh verification."""
import argparse
import json
import os
from pathlib import Path
import resource
import runpy
import shutil
import subprocess
import time

REPO=Path(__file__).resolve().parents[1]
V=runpy.run_path(str(REPO/'scripts/test-native-vsh.py'),run_name='sdk_helpers')
run,QMP,png,require,sha=V['run'],V['QMP'],V['png'],V['require'],V['sha']

def demo(out,fw,iso):
    folder=out/('example-'+fw);folder.mkdir();serial=folder/'serial.log';sock=folder/'qmp'
    args=['qemu-system-x86_64','-machine','q35','-accel','tcg','-m','128M','-cdrom',str(iso),'-boot','d',
          '-display','none','-nic','none','-serial','file:'+str(serial),'-qmp','unix:'+str(sock)+',server=on,wait=off','-no-reboot','-no-shutdown']
    if fw=='uefi':
        variables=folder/'OVMF_VARS_4M.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        args+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
               '-drive','if=pflash,format=raw,unit=1,file='+str(variables)]
    (folder/'command.json').write_text(json.dumps(args,indent=2)+'\n');q=None
    with (folder/'qemu.log').open('x') as log:
        proc=subprocess.Popen(args,cwd=REPO,stdout=log,stderr=subprocess.STDOUT)
        try:
            until=time.monotonic()+45
            while True:
                require(proc.poll() is None,'example QEMU exited')
                data=serial.read_bytes() if serial.exists() else b''
                require(b'panic:' not in data and b'boot: check failed' not in data and b'native: rust-demo FAIL' not in data,'example guest failure')
                if b'native: rust-demo PASS, process reclaimed' in data:break
                require(time.monotonic()<until,'example boot deadline');time.sleep(.05)
            require(b'user[rust-demo]: Rust SDK example: independent ring-3 application' in data,'independent app output missing')
            require(b'SDK: console/files denied without grants; C ABI reused' in data,'capability demonstration missing')
            require(b'must not appear' not in data and b'user[vsh]' not in data,'unexpected authority or shell dependency')
            require(b'proc[hello]: exited status 0, all frames reclaimed' in data,'ABI v0 hello regression')
            q=QMP(sock);registers=q.execute('human-monitor-command',{'command-line':'info registers'});require('HLT=1' in registers,'example did not halt after cleanup')
            q.execute('screendump',{'filename':str(folder/'example.ppm')})
            return dict(passed=True,status=0,console_grant=False,filesystem_grant=False,shell_loaded=False,screenshot=png(folder/'example.ppm',folder/'example.png'))
        finally:
            if q:q.close()
            if proc.poll() is None:proc.terminate()
            try:proc.wait(timeout=3)
            except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=3)

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',required=True,type=Path);a=ap.parse_args()
    out=a.output.resolve();require(out.is_relative_to(REPO/'build') and not out.exists(),'fresh output required');out.mkdir(parents=True)
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));resource.setrlimit(resource.RLIMIT_FSIZE,(64*1024**2,64*1024**2))
    result={'scope':'minimal Rust SDK and independent example','example':{}}
    try:
        rust=os.environ.get('RUSTC','rustc')
        run([rust,'--edition=2024','--test','user/rust/vos.rs','-o',str(out/'sdk-host')],out/'sdk-host-build.log')
        run([str(out/'sdk-host'),'--test-threads=1','--nocapture'],out/'sdk-host-test.log',20)
        for goal,option in [('vsh-iso','VSH_BUILD='),('rust-app','RUST_BUILD=')]:
            p=subprocess.run(['make','--no-print-directory','-n',goal,option],cwd=REPO,capture_output=True,text=True)
            require(p.returncode==2 and 'must be nonempty' in p.stderr,'empty build path accepted')
            with (out/'build-path-guards.log').open('a') as f:f.write(goal+'\n'+p.stderr)
        run(['python3','-B','scripts/test-native-vsh.py','--output',str(out/'shell')],out/'shell-check.log',180)
        result['shared_vsh']=json.loads((out/'shell/result.json').read_text())
        user=out/'shell/user';build=out/'demo-kernel'
        run(['make','--no-print-directory','USER_BUILD='+str(user),'RUSTC='+rust,'RUST_APP=rust-demo','user-rust'],out/'example-build.log')
        run(['scripts/check-user-elf.sh',str(user/'rust-demo.elf')],out/'example-elf.log',30)
        run([str(out/'shell/elfcheck'),str(user/'rust-demo.elf')],out/'example-loader.log')
        run(['make','--no-print-directory','BUILD='+str(build),'USER_BUILD='+str(user),'GRUB_CFG=arch/x86_64/grub-rust-demo.cfg',
             'ISO_FILES='+str(user/'rust-demo.elf')+':boot/rust-demo.elf','uefi-iso'],out/'example-iso.log')
        for fw,iso in [('bios','vos.iso'),('uefi','vos-uefi.iso')]:result['example'][fw]=demo(out,fw,build/iso)
        result.update(passed=True,example_ELF_sha256=sha(user/'rust-demo.elf'),sdk_source_sha256=sha(REPO/'user/rust/vos.rs'))
    except BaseException as e:result.update(passed=False,error=str(e));raise
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        entries=[dict(path=str(p.relative_to(out)),size=p.stat().st_size,sha256=sha(p)) for p in sorted(out.rglob('*')) if p.is_file()]
        (out/'manifest.json').write_text(json.dumps(entries,indent=2)+'\n');print(json.dumps(result),flush=True)

if __name__=='__main__':main()
