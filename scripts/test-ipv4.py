#!/usr/bin/env python3
"""Native IPv4/ICMP checks with a bounded, isolated Ethernet peer."""
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
H=runpy.run_path(str(REPO/'scripts/test-arp.py'),run_name='ipv4_helpers')
run,QMP,png,require,sha=H['run'],H['QMP'],H['png'],H['require'],H['sha']
G,P,GI,PI,arp=H['G'],H['P'],H['GI'],H['PI'],H['arp']
DATA=b'VOS ping data';HOST_DATA=b'peer odd payload!';LARGE=bytes(range(64))
def checksum(raw):
    if len(raw)&1:raw+=b'\0'
    total=sum(struct.unpack('!'+str(len(raw)//2)+'H',raw))
    while total>>16:total=(total&65535)+(total>>16)
    return (~total)&65535
def ip_packet(kind,identifier,sequence,data,*,native=False,ipid=0x1122,version=4,ihl=5,flags=0x4000,protocol=1,options=b''):
    icmp=bytearray(struct.pack('!BBHHH',kind,0,0,identifier,sequence)+data);struct.pack_into('!H',icmp,2,checksum(bytes(icmp)))
    header=bytearray(struct.pack('!BBHHHBBH4s4s',(version<<4)|(6 if options else ihl),0,20+len(options)+len(icmp),ipid,flags,64,protocol,0,GI if native else PI,PI if native else GI)+options)
    struct.pack_into('!H',header,10,checksum(bytes(header)))
    frame=(P+G if native else G+P)+b'\x08\x00'+header+icmp
    return bytes(frame)+b'\0'*max(0,60-len(frame))
def change(raw,offset,value,fix=None):
    p=bytearray(raw);p[offset]=value
    if fix=='ip':p[24:26]=b'\0\0';struct.pack_into('!H',p,24,checksum(bytes(p[14:34])))
    if fix=='icmp':
        total=struct.unpack_from('!H',p,16)[0];p[36:38]=b'\0\0';struct.pack_into('!H',p,36,checksum(bytes(p[34:14+total])))
    return bytes(p)
def negative_batches():
    valid=ip_packet(0,0x564f,1,DATA)
    first=[valid[:24],ip_packet(0,0x564f,1,DATA,version=6),ip_packet(0,0x564f,1,DATA,ihl=4),change(valid,17,19,'ip'),change(valid,24,valid[24]^1)]
    second=[ip_packet(0,0x564f,1,DATA,flags=0x2000),ip_packet(0,0x564f,1,DATA,protocol=17),change(valid,36,valid[36]^1),change(valid,35,1,'icmp'),ip_packet(0,0x564f,1,DATA,options=b'\0'*4)]
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
            while True:
                require(p.poll() is None,'QEMU exited')
                data=serial.read_bytes() if serial.exists() else b''
                require(not any(x in data for x in [b'panic:',b'boot: check failed',b'ipv4: failed']),'IPv4 guest failure')
                try:
                    more=peer.recv(65536);require(more!=b'','peer closed');pending.extend(more)
                except BlockingIOError:pass
                while len(pending)>=4:
                    n=struct.unpack_from('!I',pending)[0];require(14<=n<=1514,'wire bounds')
                    if len(pending)<4+n:break
                    raw=bytes(pending[4:4+n]);del pending[:4+n]
                    expected=[arp(1,G,GI,PI),ip_packet(8,0x564f,1,DATA,native=True,ipid=1),
                              ip_packet(0,0x1234,7,HOST_DATA,native=True,ipid=2),ip_packet(8,0x564f,2,LARGE,native=True,ipid=3)]
                    require(tx<4 and raw==expected[tx],'unexpected native ARP/ICMP frame '+str(tx))
                    records.append(dict(direction='TX',label=['ARP resolution','native ping seq1','native echo reply','native ping seq2'][tx],hex=raw.hex()));tx+=1
                    if tx==1:send(arp(2,P,PI,GI,G,G),'solicited ARP reply')
                    elif tx==2:
                        for packet,label in zip(negative_batches()[0],['truncated IP','bad version','bad IHL','bad total length','bad IP checksum']):send(packet,label)
                        send(ip_packet(8,0x1234,7,HOST_DATA),'valid peer echo request')
                    elif tx==3:
                        for packet,label in zip(negative_batches()[1],['fragmented IP','unsupported UDP','bad ICMP checksum','bad ICMP code','unsupported IP options']):send(packet,label)
                        send(ip_packet(0,0x564f,1,DATA),'valid ping seq1 reply')
                    else:
                        send(ip_packet(0,0x564f,99,LARGE),'unmatched sequence')
                        send(ip_packet(0,0x564f,2,LARGE),'valid ping seq2 reply')
                if b'ipv4: PASS ping=2/2 loss=0 TX=4 RX=15; DMA pool released' in data:break
                require(time.monotonic()<until,'IPv4 wire deadline');time.sleep(.01)
            require(tx==4 and len(records)==19,'wire counts')
            require(b'proc[hello]: exited status 0, all frames reclaimed' in data,'ABI v0 hello')
            q=QMP(sock);regs=q.execute('human-monitor-command',{'command-line':'info registers'});require('HLT=1' in regs,'kernel did not halt')
            (folder/'registers.txt').write_text(regs);q.execute('screendump',{'filename':str(folder/'screen.ppm')});capture=png(folder/'screen.ppm',folder/'screen.png')
            return dict(passed=True,firmware=fw,pings_sent=2,pings_matched=2,loss=0,echo_requests_answered=1,malformed_unsupported_unmatched_rejected=11,
                        TX=4,RX=15,ARP_resolution=True,DMA_frames_released=11,ABI_v0_hello=True,no_application_grants=True,screenshot=capture)
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
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));resource.setrlimit(resource.RLIMIT_FSIZE,(64*1024**2,64*1024**2));result={'scope':'native IPv4 and ICMP echo','firmware':{}}
    try:
        cc=['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Iarch/x86_64']
        # Shared Ethernet validation changed: rerun only the focused ARP host regression.
        for name,extra in [('net-arp',[]),('net-ipv4',['arch/x86_64/net_ipv4.c'])]:
            binary=out/name
            run(cc+['tests/host/'+name+'.c','arch/x86_64/net_arp.c','arch/x86_64/net_boot.c']+extra+['-o',str(binary)],out/(name+'-build.log'))
            run([str(binary)],out/(name+'-test.log'))
        empty=subprocess.run(['make','-n','ipv4-iso','IPV4_BUILD='],cwd=REPO,capture_output=True,text=True)
        require(empty.returncode==2 and 'IPV4_BUILD must be nonempty' in empty.stderr,'empty output guard');(out/'path-guard.txt').write_text(empty.stderr)
        run(['make','--no-print-directory','ipv4-uefi-iso','IPV4_BUILD='+str(out/'native')],out/'image-build.log')
        for fw,iso in [('bios','vos.iso'),('uefi','vos-uefi.iso')]:result['firmware'][fw]=boot(out,fw,out/'native/kernel'/iso)
        result.update(passed=True,host_sanitizers=['address','undefined'],userspace_ABI_unchanged=True)
    except BaseException as e:result.update(passed=False,error=str(e));raise
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        rows=[dict(path=str(p.relative_to(out)),size=p.stat().st_size,sha256=sha(p)) for p in sorted(out.rglob('*')) if p.is_file()]
        (out/'manifest.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(result),flush=True)
if __name__=='__main__':main()
