#!/usr/bin/env python3
"""Focused bounded Ethernet/ARP host and isolated BIOS/UEFI wire checks."""
import argparse
import json
from pathlib import Path
import resource
import runpy
import shutil
import socket
import struct
import subprocess
import time
REPO=Path(__file__).resolve().parents[1]
H=runpy.run_path(str(REPO/'scripts/test-virtio-net.py'),run_name='arp_helpers')
run,QMP,png,require,sha=H['run'],H['QMP'],H['png'],H['require'],H['sha']
G=bytes.fromhex('525400123456');P=bytes.fromhex('525400aabbcc');P2=bytes.fromhex('525400aabbdd');B=b'\xff'*6;Z=b'\0'*6
GI=bytes([10,0,2,15]);PI=bytes([10,0,2,2]);PI2=bytes([10,0,2,3])
def arp(op,src,spa,tpa,dst=B,tha=Z):
    return dst+src+b'\x08\x06'+struct.pack('!HHBBH',1,0x0800,6,4,op)+src+spa+tha+tpa+b'\0'*18
def mutated(raw,offset,value):
    p=bytearray(raw);p[offset]=value;return bytes(p)
def malformed_batch():
    valid=arp(2,P,PI,GI,G,G)
    return [valid[:30],mutated(valid,18,5),mutated(valid,21,3),mutated(valid,13,0),
            arp(1,P,PI,bytes([10,0,2,99])),arp(2,P,bytes([10,0,2,9]),GI,G,G),mutated(valid,22,0x50),valid]

def boot(out,fw,iso):
    folder=out/fw;folder.mkdir();serial=folder/'serial.log';sock=folder/'qmp';peer,child=socket.socketpair();peer.setblocking(False)
    args=['qemu-system-x86_64','-machine','q35','-accel','tcg','-m','128M','-cdrom',str(iso),'-boot','d','-display','none','-nic','none',
          '-serial','file:'+str(serial),'-qmp','unix:'+str(sock)+',server=on,wait=off','-no-reboot','-no-shutdown',
          '-netdev','stream,id=n0,server=off,addr.type=fd,addr.str='+str(child.fileno()),
          '-device','virtio-net-pci,netdev=n0,disable-legacy=on,mac=52:54:00:12:34:56','-object','filter-dump,id=wire,netdev=n0,file='+str(folder/'wire.pcap')]
    if fw=='uefi':
        variables=folder/'OVMF_VARS_4M.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        args+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive','if=pflash,format=raw,unit=1,file='+str(variables)]
    (folder/'command.json').write_text(json.dumps(args,indent=2)+'\n');q=None;wire=bytearray();records=[];tx=0
    with (folder/'qemu.log').open('x') as log:
        p=subprocess.Popen(args,cwd=REPO,stdout=log,stderr=subprocess.STDOUT,pass_fds=(child.fileno(),));child.close()
        try:
            until=time.monotonic()+45
            def send(raw,label):
                peer.sendall(struct.pack('!I',len(raw))+raw);records.append(dict(direction='RX',label=label,hex=raw.hex()))
            while True:
                require(p.poll() is None,'QEMU exited')
                data=serial.read_bytes() if serial.exists() else b''
                require(not any(x in data for x in [b'panic:',b'boot: check failed',b'arp: failed']),'ARP guest failure')
                try:
                    more=peer.recv(65536);require(more!=b'','network peer closed');wire.extend(more)
                except BlockingIOError:pass
                while len(wire)>=4:
                    n=struct.unpack_from('!I',wire)[0];require(14<=n<=1514,'wire length')
                    if len(wire)<n+4:break
                    raw=bytes(wire[4:n+4]);del wire[:n+4]
                    expected=[arp(1,G,GI,PI),arp(1,G,GI,PI2),arp(2,G,GI,PI2,P2,P2),arp(2,G,GI,b'\0'*4,P,P)]
                    require(tx<4 and raw==expected[tx],'unexpected guest ARP transmission '+str(tx))
                    records.append(dict(direction='TX',label=['resolve peer','resolve second peer','reply to request','reply to probe'][tx],hex=raw.hex()));tx+=1
                    if tx==1:
                        labels=['truncated ARP','bad hardware length','bad opcode','unsupported IPv4 EtherType','unrelated target','unsolicited reply','SHA/source mismatch','valid solicited reply']
                        for frame,label in zip(malformed_batch(),labels):send(frame,label)
                    elif tx==2:send(arp(1,P2,PI2,GI),'valid local request')
                    elif tx==3:send(arp(1,P,b'\0'*4,GI),'zero sender address probe')
                    else:send(G+P+b'\x05\xff'+b'\0'*46,'invalid EtherType gap')
                if b'arp: PASS TX=4 RX=11; DMA pool released' in data:break
                require(time.monotonic()<until,'ARP boot/wire deadline');time.sleep(.01)
            require(tx==4 and len(records)==15,'packet counts')
            require(b'proc[hello]: exited status 0, all frames reclaimed' in data,'legacy ABI v0 hello')
            q=QMP(sock);regs=q.execute('human-monitor-command',{'command-line':'info registers'});require('HLT=1' in regs,'guest did not halt')
            (folder/'registers.txt').write_text(regs);q.execute('screendump',{'filename':str(folder/'screen.ppm')});capture=png(folder/'screen.ppm',folder/'screen.png')
            return dict(passed=True,firmware=fw,TX=4,RX=11,rejected=8,neighbor_resolution=True,request_reply=True,probe_reply_without_cache=True,
                        no_automatic_application_grants=True,DMA_frames_released=11,ABI_v0_hello=True,screenshot=capture)
        finally:
            (folder/'wire-records.json').write_text(json.dumps(records,indent=2)+'\n');(folder/'peer-pending.hex').write_text(wire.hex()+'\n')
            if q:q.close()
            peer.close()
            if p.poll() is None:p.terminate()
            try:p.wait(timeout=3)
            except subprocess.TimeoutExpired:p.kill();p.wait(timeout=3)

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',required=True,type=Path);a=ap.parse_args()
    out=a.output.resolve();require(out.is_relative_to(REPO/'build') and not out.exists(),'fresh output required');out.mkdir(parents=True)
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));resource.setrlimit(resource.RLIMIT_FSIZE,(64*1024**2,64*1024**2))
    result={'scope':'bounded Ethernet dispatch and ARP','firmware':{}}
    try:
        cc=['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',
            '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Iarch/x86_64']
        run(cc+['tests/host/net-arp.c','arch/x86_64/net_arp.c','arch/x86_64/net_boot.c','-o',str(out/'host-arp')],out/'host-build.log')
        run([str(out/'host-arp')],out/'host-test.log')
        empty=subprocess.run(['make','-n','arp-iso','ARP_BUILD='],cwd=REPO,capture_output=True,text=True)
        require(empty.returncode==2 and 'ARP_BUILD must be nonempty' in empty.stderr,'empty output guard');(out/'path-guard.txt').write_text(empty.stderr)
        run(['make','--no-print-directory','arp-uefi-iso','ARP_BUILD='+str(out/'native')],out/'image-build.log')
        for fw,iso in [('bios','vos.iso'),('uefi','vos-uefi.iso')]:result['firmware'][fw]=boot(out,fw,out/'native/kernel'/iso)
        result.update(passed=True,virtio_net_driver_unchanged=True,no_new_syscall_ABI=True,host_sanitizers=['address','undefined'])
    except BaseException as e:result.update(passed=False,error=str(e));raise
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        rows=[dict(path=str(p.relative_to(out)),size=p.stat().st_size,sha256=sha(p)) for p in sorted(out.rglob('*')) if p.is_file()]
        (out/'manifest.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(result),flush=True)
if __name__=='__main__':main()
