#!/usr/bin/env python3
"""Focused offline Rixie host tests, BIOS/UEFI interaction and vsh smoke checks."""
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
H=runpy.run_path(str(REPO/'scripts/test-native-vsh.py'),run_name='rixie_helpers')
run,QMP,png,require,sha=H['run'],H['QMP'],H['png'],H['require'],H['sha']

def boot(out,fw,iso,app):
    folder=out/(app+'-'+fw);folder.mkdir();serial=folder/'serial.log';sock=folder/'qmp'
    args=['qemu-system-x86_64','-machine','q35','-accel','tcg','-m','128M','-cdrom',str(iso),'-boot','d',
          '-display','none','-nic','none','-serial','file:'+str(serial),'-qmp','unix:'+str(sock)+',server=on,wait=off',
          '-no-reboot','-no-shutdown']
    if fw=='uefi':
        variables=folder/'OVMF_VARS_4M.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        args+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
               '-drive','if=pflash,format=raw,unit=1,file='+str(variables)]
    (folder/'command.json').write_text(json.dumps(args,indent=2)+'\n')
    prompt=('user['+app+']: '+('Rixie> ' if app=='rixie' else 'VOS> ')).encode()+b'\r\n'
    q=None;cases=[]
    with (folder/'qemu.log').open('x') as log:
        proc=subprocess.Popen(args,cwd=REPO,stdout=log,stderr=subprocess.STDOUT)
        try:
            def data():
                require(proc.poll() is None,'QEMU exited')
                d=serial.read_bytes() if serial.exists() else b''
                require(not any(x in d for x in (b'panic:',b'boot: check failed',b'native: rixie FAIL',b'native: vsh FAIL')),'guest failure')
                return d
            def wait(test,seconds=8):
                until=time.monotonic()+seconds
                while True:
                    d=data()
                    if test(d):return d
                    require(time.monotonic()<until,'guest interaction deadline');time.sleep(.05)
            initial=wait(lambda d:prompt in d,45);q=QMP(sock)
            require(b'proc[hello]: exited status 0, all frames reclaimed' in initial,'ABI v0 hello')
            def text(s):
                codes={' ':'spc','/':'slash','.':'dot','-':'minus'}
                for c in s:
                    if c=='?':q.key('shift',True);q.tap('slash');q.key('shift',False)
                    elif c.isupper():q.key('shift',True);q.tap(c.lower());q.key('shift',False)
                    else:q.tap(codes.get(c,c))
            def command(s,expected,edit=False):
                before=data();count=before.count(prompt)
                if edit:q.tap('backspace')
                text(s)
                if edit:q.tap('backspace')
                q.tap('ret');after=wait(lambda d:d.count(prompt)>count)
                tail=after[len(before):]
                require(expected.encode() in tail,'missing reply for '+repr(s))
                cases.append(dict(input=s,expected=expected,prompt_safe_editing=edit,passed=True))
            if app=='rixie':
                require(b'deterministic offline prototype' in initial and b'not a conversational AI model' in initial,'prototype identity')
                require(b'user[vsh]' not in initial,'Rixie depends on vsh')
                command('','')
                command('hello',"Hello! I'm Rixie, at home in VOS.")
                command('HELP','Topics: hello, help, about, questions, explain <command>, exit')
                command('about','independent Rust ring-3 application in VOS.')
                command('explain cd',"cd: change vsh's working directory; invalid paths leave it unchanged.")
                command('explain cat','cat: display a real read-only file under vsh\'s grant.')
                command('what is VOS?','independently written operating-system project')
                command('are you AI?','No. Rixie Core v0 selects fixed replies; I am not a conversational AI model.')
                q.execute('screendump',{'filename':str(folder/'interactive.ppm')});screen=png(folder/'interactive.ppm',folder/'interactive.png')
                command('questions','Try: what is VOS? / are you AI? / are you online?')
                command('are you online?','entirely offline, with no network or cloud connection.')
                command('what can you do?','answer a small set of predefined questions.')
                command('who made you?','VOS is developed under human direction with AI coding assistance.')
                command('explain unknown','No explanation for that topic.')
                command('cd /','No predefined reply.')
                command('vsh rixie','No predefined reply.')
                command('x'*128,'No predefined reply.')
                command('hellox',"Hello! I'm Rixie, at home in VOS.",edit=True)
            else:
                require(b'user[rixie]' not in initial,'Rixie unexpectedly launched in vsh mode')
                command('help','Available commands: help, clear, echo, pwd, ls, cd, cat')
                command('echo vsh remains independent','vsh remains independent')
                q.execute('screendump',{'filename':str(folder/'interactive.ppm')});screen=png(folder/'interactive.ppm',folder/'interactive.png')
            text('exit');q.tap('ret')
            final=wait(lambda d:('native: '+app+' PASS, process reclaimed').encode() in d)
            if app=='rixie':require(b'Goodbye from Rixie.' in final,'exit reply')
            registers=q.execute('human-monitor-command',{'command-line':'info registers'})
            require('HLT=1' in registers,'kernel did not halt after reclamation')
            return dict(passed=True,application=app,cases=cases,exit_status=0,reclaimed=True,ABI_v0_hello=True,screenshot=screen,
                        network_device=False,data_disk=False,console_grant=True,filesystem_grant=False)
        finally:
            if q:q.close()
            if proc.poll() is None:proc.terminate()
            try:proc.wait(timeout=3)
            except subprocess.TimeoutExpired:proc.kill();proc.wait(timeout=3)

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',required=True,type=Path);a=ap.parse_args()
    out=a.output.resolve();require(out.is_relative_to(REPO/'build') and not out.exists(),'fresh output required');out.mkdir(parents=True)
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));resource.setrlimit(resource.RLIMIT_FSIZE,(64*1024**2,64*1024**2))
    result={'scope':'Rixie Core v0 and existing vsh launch compatibility','rixie':{},'vsh_smoke':{}}
    try:
        rust=os.environ.get('RUSTC','rustc');result['rustc']=subprocess.check_output([rust,'--version'],text=True).strip()
        run([rust,'--edition=2024','--test','user/rixie/dialog.rs','-o',str(out/'dialog-host')],out/'host-build.log')
        run([str(out/'dialog-host'),'--test-threads=1'],out/'host-test.log')
        empty=subprocess.run(['make','--no-print-directory','-n','rixie-iso','RIXIE_BUILD='],cwd=REPO,capture_output=True,text=True)
        require(empty.returncode==2 and 'RIXIE_BUILD must be nonempty' in empty.stderr,'empty Rixie output path accepted')
        (out/'build-path-guard.log').write_text(empty.stderr)
        for app in ['rixie','vsh']:
            run(['make','--no-print-directory',app+'-uefi-iso',app.upper()+'_BUILD='+str(out/app),'RUSTC='+rust],out/(app+'-build.log'))
        sdk=json.loads((REPO/'docs/results/n4-sdk/evidence.json').read_text())
        require(sha(REPO/'user/rust/vos.rs')==sdk['sources']['user/rust/vos.rs'],'accepted SDK changed')
        expected=json.loads((REPO/'docs/results/n4-prompt/evidence.json').read_text())['local_products']['user/vsh.elf']
        require(sha(out/'vsh/user/vsh.elf')==expected,'accepted shell ELF changed')
        cc=['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Iarch/x86_64']
        run(cc+['scripts/elfcheck.c','arch/x86_64/elf.c','-o',str(out/'elfcheck')],out/'loader-build.log')
        for app in ['rixie','vsh']:
            elf=out/app/'user'/(app+'.elf')
            run(['scripts/check-user-elf.sh',str(elf)],out/(app+'-elf.log'))
            run([str(out/'elfcheck'),str(elf)],out/(app+'-loader.log'))
            for fw,iso in [('bios','vos.iso'),('uefi','vos-uefi.iso')]:
                result['rixie' if app=='rixie' else 'vsh_smoke'][fw]=boot(out,fw,out/app/'kernel'/iso,app)
        result.update(passed=True,SDK_unchanged=True,vsh_ELF_unchanged=True,rixie_ELF_sha256=sha(out/'rixie/user/rixie.elf'))
    except BaseException as e:result.update(passed=False,error=str(e));raise
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        entries=[dict(path=str(p.relative_to(out)),size=p.stat().st_size,sha256=sha(p)) for p in sorted(out.rglob('*')) if p.is_file()]
        (out/'manifest.json').write_text(json.dumps(entries,indent=2)+'\n');print(json.dumps(result),flush=True)

if __name__=='__main__':main()
