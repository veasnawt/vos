#!/usr/bin/env python3
"""Bounded native virtio-net verification with an isolated Unix socket peer."""
import argparse
import json
import os
from pathlib import Path
import resource
import runpy
import shutil
import socket
import struct
import subprocess
import time

REPO=Path(__file__).resolve().parents[1]
H=runpy.run_path(str(REPO/'scripts/test-native-vsh.py'),run_name='net_helpers')
run,QMP,png,require,sha=H['run'],H['QMP'],H['png'],H['require'],H['sha']
GUEST=bytes.fromhex('525400123456');PEER=bytes.fromhex('525400aabbcc')
def frame(n,reply=False):
    return (GUEST+PEER if reply else PEER+GUEST)+b'\x88\xb5'+(b'VOS-N5-RX' if reply else b'VOS-N5-TX')+bytes([n])+bytes((n+i)&255 for i in range(24,60))

def boot(out,fw,iso,mode):
    folder=out/(mode+'-'+fw);folder.mkdir();serial=folder/'serial.log';sock=folder/'qmp';q=None;peer=None;child=None
    args=['qemu-system-x86_64','-machine','q35','-accel','tcg','-m','128M','-cdrom',str(iso),'-boot','d',
          '-display','none','-nic','none','-serial','file:'+str(serial),'-qmp','unix:'+str(sock)+',server=on,wait=off','-no-reboot','-no-shutdown']
    if mode!='absent':
        peer,child=socket.socketpair();peer.setblocking(False)
        args+=['-netdev','stream,id=n0,server=off,addr.type=fd,addr.str='+str(child.fileno()),
               '-device','virtio-net-pci,netdev=n0,disable-legacy=on,mac=52:54:00:12:34:56',
               '-object','filter-dump,id=wire,netdev=n0,file='+str(folder/'wire.pcap')]
    if fw=='uefi':
        variables=folder/'OVMF_VARS_4M.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        args+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive','if=pflash,format=raw,unit=1,file='+str(variables)]
    (folder/'command.json').write_text(json.dumps(args,indent=2)+'\n')
    with (folder/'qemu.log').open('x') as log:
        p=subprocess.Popen(args,cwd=REPO,stdout=log,stderr=subprocess.STDOUT,pass_fds=(child.fileno(),) if child else ())
        if child:child.close()
        try:
            received=bytearray();frames=[];until=time.monotonic()+45;injected=False
            marker=b'net: PASS TX=16 RX=16' if mode=='net' else b'net: absent' if mode=='absent' else ('native: '+mode+' PASS, process reclaimed').encode()
            while True:
                require(p.poll() is None,'QEMU exited')
                data=serial.read_bytes() if serial.exists() else b''
                require(not any(x in data for x in [b'panic:',b'boot: check failed',b'native: vsh FAIL',b'native: rixie FAIL',b'net: initialization failed']),'guest failure')
                if peer:
                    try:
                        chunk=peer.recv(65536);require(chunk!=b'','peer closed');received.extend(chunk)
                    except BlockingIOError:pass
                    while len(received)>=4:
                        length=struct.unpack_from('!I',received)[0];require(14<=length<=1514,'invalid peer frame length')
                        if len(received)<4+length:break
                        raw=bytes(received[4:4+length]);del received[:4+length]
                        require(mode=='net' and len(frames)<16,'unexpected network use outside opted-in demo')
                        n=len(frames);require(raw==frame(n),'guest TX bytes differ')
                        reply=frame(n,True);peer.sendall(struct.pack('!I',len(reply))+reply)
                        frames.append(dict(sequence=n,tx_hex=raw.hex(),rx_hex=reply.hex()))
                if mode in ('vsh','rixie') and not injected:
                    prompt=b'user[vsh]: VOS> \r\n' if mode=='vsh' else b'user[rixie]: Rixie> \r\n'
                    if prompt in data:
                        q=QMP(sock)
                        # Check the real keyboard path remains live with an uninitialized NIC present.
                        for c in ('echo compatible' if mode=='vsh' else 'hello'):
                            q.tap('spc' if c==' ' else c)
                        q.tap('ret');injected=True
                        until_reply=time.monotonic()+8
                        expected=b'user[vsh]: compatible' if mode=='vsh' else b"Hello! I'm Rixie, at home in VOS."
                        while expected not in serial.read_bytes():
                            require(time.monotonic()<until_reply,'application reply timeout');time.sleep(.05)
                        for c in 'exit':q.tap(c)
                        q.tap('ret')
                if marker in data:break
                require(time.monotonic()<until,'boot/frame deadline');time.sleep(.01)
            require(b'proc[hello]: exited status 0, all frames reclaimed' in data,'ABI v0 hello')
            if mode=='net':require(len(frames)==16 and b'short buffer retained; DMA pool released' in data,'frame count/teardown')
            if mode in ('vsh','rixie'):require(b'net: ready' not in data and not frames,'application received automatic network access')
            if not q:q=QMP(sock)
            regs=q.execute('human-monitor-command',{'command-line':'info registers'});require('HLT=1' in regs,'guest did not halt')
            (folder/'registers.txt').write_text(regs);(folder/'frames.json').write_text(json.dumps(frames,indent=2)+'\n')
            q.execute('screendump',{'filename':str(folder/'screen.ppm')});capture=png(folder/'screen.ppm',folder/'screen.png')
            return dict(passed=True,frames=len(frames),mode=mode,firmware=fw,ABI_v0_hello=True,halted=True,screenshot=capture,
                        NIC_present=mode!='absent',isolated_Unix_peer=mode!='absent',no_TAP_NAT_LAN=True)
        finally:
            (folder/'frames-observed.json').write_text(json.dumps(frames,indent=2)+'\n')
            (folder/'peer-pending.hex').write_text(received.hex()+'\n')
            if q:q.close()
            if peer:peer.close()
            if p.poll() is None:p.terminate()
            try:p.wait(timeout=3)
            except subprocess.TimeoutExpired:p.kill();p.wait(timeout=3)

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',required=True,type=Path);a=ap.parse_args()
    out=a.output.resolve();require(out.is_relative_to(REPO/'build') and not out.exists(),'fresh output under build required');out.mkdir(parents=True)
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));resource.setrlimit(resource.RLIMIT_FSIZE,(64*1024**2,64*1024**2))
    result={'scope':'first N5 modern virtio-net adapter increment','boots':[]}
    try:
        cc=['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Iarch/x86_64']
        run(cc+['tests/host/virtio-net.c','arch/x86_64/virtio_blk.c','-o',str(out/'host-net')],out/'host-build.log')
        run([str(out/'host-net')],out/'host-test.log')
        empty=subprocess.run(['make','-n','net-iso','NET_BUILD='],cwd=REPO,capture_output=True,text=True)
        require(empty.returncode==2 and 'NET_BUILD must be nonempty' in empty.stderr,'empty build path guard');(out/'path-guard.txt').write_text(empty.stderr)
        run(['make','--no-print-directory','net-uefi-iso','NET_BUILD='+str(out/'net')],out/'image-build.log')
        for fw,iso in [('bios','vos.iso'),('uefi','vos-uefi.iso')]:result['boots'].append(boot(out,fw,out/'net/kernel'/iso,'net'))
        result['boots'].append(boot(out,'bios',out/'net/kernel/vos.iso','absent'))
        # Only two focused N4 compatibility boots. Rebuild from unchanged sources
        # into fresh outputs and require the accepted application identities.
        kernel=out/'compat'
        rust=os.environ.get('RUSTC','rustc')
        result['rustc']=subprocess.check_output([rust,'--version'],text=True).strip()
        run(['make','--no-print-directory','USER_BUILD='+str(out/'net/user'),'RUSTC='+rust,'RUST_APP=rixie','user-rust','user-vsh'],out/'application-build.log')
        expected=json.loads((REPO/'docs/results/n4-rixie/evidence.json').read_text())['products']
        for mode,fw in [('vsh','bios'),('rixie','uefi')]:
            cfg=out/(mode+'.cfg')
            app=out/'net/user'/(mode+'.elf');require(sha(app)==expected[mode+'/user/'+mode+'.elf'],'accepted application identity')
            cfg.write_text('set timeout=0\nmenuentry "N5 compatibility" {\nmultiboot2 /boot/vos.bin vos.'+mode+'=1\nmodule2 /boot/hello.elf hello\nmodule2 /boot/'+mode+'.elf '+mode+'\nboot\n}\n')
            build=kernel/mode
            run(['make','--no-print-directory','BUILD='+str(build),'USER_BUILD='+str(out/'net/user'),'GRUB_CFG='+str(cfg),'ISO_FILES='+str(app)+':boot/'+mode+'.elf','uefi-iso' if fw=='uefi' else 'iso'],out/(mode+'-build.log'))
            result['boots'].append(boot(out,fw,build/('vos-uefi.iso' if fw=='uefi' else 'vos.iso'),mode))
        result.update(passed=True,userspace_ABI_unchanged=True,automatic_application_network_grants=False)
    except BaseException as e:result.update(passed=False,error=str(e));raise
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        entries=[dict(path=str(p.relative_to(out)),size=p.stat().st_size,sha256=sha(p)) for p in sorted(out.rglob('*')) if p.is_file()]
        (out/'manifest.json').write_text(json.dumps(entries,indent=2)+'\n');print(json.dumps(result),flush=True)
if __name__=='__main__':main()
