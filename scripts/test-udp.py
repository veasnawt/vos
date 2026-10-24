#!/usr/bin/env python3
"""Focused native UDP host and isolated BIOS/UEFI wire checks."""
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
H=runpy.run_path(str(REPO/'scripts/test-ipv4.py'),run_name='udp_helpers')
run,QMP,png,require,sha,checksum=[H[n] for n in ['run','QMP','png','require','sha','checksum']]
G,P,GI,PI,arp=[H[n] for n in ['G','P','GI','PI','arp']]

def udp(data,*,native=False,source_port=None,dest_port=None,checked=True,ipid=0x1122):
    source,dest=(GI,PI) if native else (PI,GI)
    sport=(4000 if native else 5000) if source_port is None else source_port
    dport=(5000 if native else 4000) if dest_port is None else dest_port
    body=bytearray(struct.pack('!HHHH',sport,dport,8+len(data),0)+data)
    if checked:struct.pack_into('!H',body,6,checksum(source+dest+b'\0\x11'+struct.pack('!H',len(body))+body) or 65535)
    header=bytearray(struct.pack('!BBHHHBBH4s4s',0x45,0,20+len(body),ipid,0x4000,64,17,0,source,dest))
    struct.pack_into('!H',header,10,checksum(header))
    packet=(P+G if native else G+P)+b'\x08\x00'+header+body
    return bytes(packet)+b'\0'*max(0,60-len(packet))

def negative_batches():
    good=udp(b'peer UDP reply!')
    def changed(offset,value,ip=False):
        p=bytearray(good);p[offset]=value
        if ip:p[24:26]=b'\0\0';struct.pack_into('!H',p,24,checksum(p[14:34]))
        return bytes(p)
    first=[(good[:40],'truncated UDP'),(changed(39,7),'UDP length below header'),(changed(39,24),'UDP length exceeds IP'),
           (changed(39,22),'UDP length shorter than IP'),(changed(40,good[40]^1),'bad UDP checksum'),(udp(b'peer UDP reply!',dest_port=4001),'unbound port')]
    second=[(udp(b'no port',dest_port=0),'zero destination port'),(udp(bytes(513)),'payload over configured bound'),
            (changed(29,3,True),'pseudoheader mismatch'),(changed(20,0x20,True),'fragmented IP'),(changed(23,6,True),'unsupported TCP'),(changed(24,good[24]^1),'bad IP checksum')]
    return first,second

def boot(out,fw,iso):
    folder=out/fw;folder.mkdir();serial=folder/'serial.log';sock=folder/'qmp';peer,child=socket.socketpair();peer.setblocking(False)
    args=['qemu-system-x86_64','-machine','q35','-accel','tcg','-m','128M','-cdrom',str(iso),'-boot','d','-display','none','-nic','none',
          '-serial','file:'+str(serial),'-qmp','unix:'+str(sock)+',server=on,wait=off','-no-reboot','-no-shutdown',
          '-netdev','stream,id=n0,server=off,addr.type=fd,addr.str='+str(child.fileno()),
          '-device','virtio-net-pci,netdev=n0,disable-legacy=on,mac=52:54:00:12:34:56','-object','filter-dump,id=wire,netdev=n0,file='+str(folder/'wire.pcap')]
    if fw=='uefi':
        variables=folder/'OVMF_VARS_4M.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        args+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive','if=pflash,format=raw,unit=1,file='+str(variables)]
    (folder/'command.json').write_text(json.dumps(args,indent=2)+'\n');q=None;pending=bytearray();records=[];tx=0
    with (folder/'qemu.log').open('x') as log:
        p=subprocess.Popen(args,cwd=REPO,stdout=log,stderr=subprocess.STDOUT,pass_fds=(child.fileno(),));child.close()
        try:
            until=time.monotonic()+45
            def send(raw,label):
                peer.sendall(struct.pack('!I',len(raw))+raw);records.append(dict(direction='RX',label=label,hex=raw.hex()))
            expected=[arp(1,G,GI,PI),udp(b'VOS UDP hello',native=True,ipid=1),udp(b'queue-ready',native=True,ipid=2),
                      udp(b'\xc4\xa1',native=True,ipid=3),udp(b'UDP complete',native=True,ipid=4)]
            while True:
                require(p.poll() is None,'QEMU exited')
                data=serial.read_bytes() if serial.exists() else b''
                require(not any(x in data for x in [b'panic:',b'boot: check failed',b'udp: failed']),'UDP guest failure')
                try:
                    more=peer.recv(65536);require(more!=b'','peer closed');pending.extend(more)
                except BlockingIOError:pass
                while len(pending)>=4:
                    n=struct.unpack_from('!I',pending)[0];require(14<=n<=1514,'wire bounds')
                    if len(pending)<4+n:break
                    raw=bytes(pending[4:4+n]);del pending[:4+n]
                    require(tx<5 and raw==expected[tx],'unexpected native ARP/UDP frame '+str(tx))
                    records.append(dict(direction='TX',label=['ARP resolution','native UDP hello','queue ready','computed-zero checksum','complete'][tx],hex=raw.hex()));tx+=1
                    if tx==1:send(arp(2,P,PI,GI,G,G),'solicited ARP reply')
                    elif tx==2:
                        for raw,label in negative_batches()[0]:send(raw,label)
                        send(udp(b'peer UDP reply!'),'valid UDP reply')
                    elif tx==3:
                        for raw,label in [(udp(b'A'),'FIFO first'),(udp(b'zero-checksum',checked=False),'no UDP checksum'),(udp(b'',source_port=0),'optional source port'),
                                          (udp(bytes(range(256))*2),'512-byte payload'),(udp(b'overflow'),'queue overflow')]:send(raw,label)
                    elif tx==4:
                        for raw,label in negative_batches()[1]:send(raw,label)
                        send(udp(b'after-full'),'queue recovery');send(udp(b'\xc4\xa1'),'ffff checksum')
                if b'udp: PASS TX=5 RX=21 accepted=7 rejected=12 full=1; DMA pool released' in data:break
                require(time.monotonic()<until,'UDP wire deadline');time.sleep(.01)
            require(tx==5 and len(records)==26,'wire counts')
            require(b'proc[hello]: exited status 0, all frames reclaimed' in data,'ABI v0 hello')
            q=QMP(sock);regs=q.execute('human-monitor-command',{'command-line':'info registers'});require('HLT=1' in regs,'kernel did not halt')
            (folder/'registers.txt').write_text(regs);q.execute('screendump',{'filename':str(folder/'screen.ppm')});capture=png(folder/'screen.ppm',folder/'screen.png')
            return dict(passed=True,firmware=fw,UDP_sent=4,UDP_received=7,invalid_unsupported_or_unbound=12,queue_full_rejected=1,queue_slots=4,payload_bound=512,
                        TX=5,RX=21,ARP_resolution=True,IPv4_zero_checksum_accepted=True,computed_zero_transmitted_as_ffff=True,adapter_reset_confirmed=True,
                        DMA_frames_released=11,endpoint_erased=True,ABI_v0_hello=True,no_application_grants=True,screenshot=capture)
        finally:
            (folder/'wire-records.json').write_text(json.dumps(records,indent=2)+'\n');(folder/'peer-pending.hex').write_text(pending.hex()+'\n')
            if q:q.close()
            peer.close()
            if p.poll() is None:p.terminate()
            try:p.wait(timeout=3)
            except subprocess.TimeoutExpired:p.kill();p.wait(timeout=3)

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',required=True,type=Path);a=ap.parse_args()
    out=a.output.resolve();require(out.is_relative_to(REPO/'build') and not out.exists(),'fresh output required');out.mkdir(parents=True)
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));resource.setrlimit(resource.RLIMIT_FSIZE,(64*1024**2,64*1024**2));result={'scope':'bounded native UDP','firmware':{}}
    try:
        cc=['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Iarch/x86_64']
        for name,extra in [('net-arp',[]),('net-ipv4',['arch/x86_64/net_ipv4.c']),('net-udp',['arch/x86_64/net_ipv4.c','arch/x86_64/net_udp.c'])]:
            binary=out/name
            run(cc+['tests/host/'+name+'.c','arch/x86_64/net_arp.c','arch/x86_64/net_boot.c']+extra+['-o',str(binary)],out/(name+'-build.log'))
            run([str(binary)],out/(name+'-test.log'))
        empty=subprocess.run(['make','-n','udp-iso','UDP_BUILD='],cwd=REPO,capture_output=True,text=True)
        require(empty.returncode==2 and 'UDP_BUILD must be nonempty' in empty.stderr,'empty output guard');(out/'path-guard.txt').write_text(empty.stderr)
        run(['make','--no-print-directory','udp-uefi-iso','UDP_BUILD='+str(out/'native')],out/'image-build.log')
        for fw,iso in [('bios','vos.iso'),('uefi','vos-uefi.iso')]:result['firmware'][fw]=boot(out,fw,out/'native/kernel'/iso)
        result.update(passed=True,host_sanitizers=['address','undefined'],userspace_ABI_unchanged=True)
    except BaseException as e:result.update(passed=False,error=str(e));raise
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        rows=[dict(path=str(p.relative_to(out)),size=p.stat().st_size,sha256=sha(p)) for p in sorted(out.rglob('*')) if p.is_file()]
        (out/'manifest.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(result),flush=True)
if __name__=='__main__':main()
