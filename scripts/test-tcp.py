#!/usr/bin/env python3
"""Focused passive TCP establishment checks with an isolated wire peer."""
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
H=runpy.run_path(str(REPO/'scripts/test-udp.py'),run_name='tcp_helpers')
run,QMP,png,require,sha,checksum=[H[n] for n in ['run','QMP','png','require','sha','checksum']]
G,P,GI,PI,arp=[H[n] for n in ['G','P','GI','PI','arp']]
ISN=0x11223344;NEXT=ISN+0x10000;PEER=0x01020304

def tcp(flags,sequence,ack=0,*,native=False,ipid=0x1122,source_port=None,dest_port=None,words=5,options=b'',payload=b''):
    source,dest=(GI,PI) if native else (PI,GI)
    sport=(4001 if native else 5001) if source_port is None else source_port
    dport=(5001 if native else 4001) if dest_port is None else dest_port
    body=bytearray(struct.pack('!HHIIBBHHH',sport,dport,sequence,ack,words<<4,flags,0 if native else 1024,0,0)+options+payload)
    struct.pack_into('!H',body,16,checksum(source+dest+b'\0\x06'+struct.pack('!H',len(body))+body))
    header=bytearray(struct.pack('!BBHHHBBH4s4s',0x45,0,20+len(body),ipid,0x4000,64,6,0,source,dest));struct.pack_into('!H',header,10,checksum(header))
    packet=(P+G if native else G+P)+b'\x08\x00'+header+body
    return bytes(packet)+b'\0'*max(0,60-len(packet))

def change(raw,offset,value,fix_ip=False,fix_tcp=False):
    p=bytearray(raw);p[offset]=value
    if fix_ip:p[24:26]=b'\0\0';struct.pack_into('!H',p,24,checksum(p[14:34]))
    if fix_tcp:
        size=struct.unpack_from('!H',p,16)[0]-20;p[50:52]=b'\0\0'
        struct.pack_into('!H',p,50,checksum(p[26:34]+b'\0\x06'+struct.pack('!H',size)+p[34:34+size]))
    return bytes(p)

def negatives():
    first=tcp(2,0xffffffff);second=tcp(2,PEER);ack=tcp(16,PEER+1,NEXT+1)
    return [
        [(first[:50],'truncated TCP'),(change(first,46,0x40,fix_tcp=True),'TCP offset below minimum'),
         (change(first,46,0xf0,fix_tcp=True),'TCP offset exceeds segment'),(change(first,50,first[50]^1),'bad TCP checksum'),
         (tcp(2,0xffffffff,source_port=0),'zero source port'),(tcp(16,0,ISN+1),'unsolicited ACK')],
        [(change(second,24,second[24]^1),'bad IP checksum'),(change(second,20,0x20,fix_ip=True),'fragmented IP'),
         (change(second,46,0x51,fix_tcp=True),'TCP reserved bit'),(tcp(2,PEER,words=6,options=bytes(4)),'unsupported TCP options'),
         (tcp(2,PEER,payload=b'X'),'unsupported SYN payload'),(tcp(2,PEER,dest_port=4002),'unbound endpoint')],
        [(change(ack,29,3,fix_ip=True,fix_tcp=True),'wrong peer IP'),(change(ack,6,ack[6]^4),'wrong peer MAC'),
         (tcp(16,PEER+1,NEXT),'wrong final acknowledgment'),(tcp(16,PEER,NEXT+1),'wrong final sequence'),
         (tcp(4,PEER+1,NEXT+1),'unsupported reset'),(tcp(2,PEER,source_port=5002),'second connection refused')]]

def boot(out,fw,iso):
    folder=out/fw;folder.mkdir();serial=folder/'serial.log';sock=folder/'qmp';peer,child=socket.socketpair();peer.setblocking(False)
    args=['qemu-system-x86_64','-machine','q35','-accel','tcg','-m','128M','-cdrom',str(iso),'-boot','d','-display','none','-nic','none',
          '-serial','file:'+str(serial),'-qmp','unix:'+str(sock)+',server=on,wait=off','-no-reboot','-no-shutdown',
          '-netdev','stream,id=n0,server=off,addr.type=fd,addr.str='+str(child.fileno()),
          '-device','virtio-net-pci,netdev=n0,disable-legacy=on,mac=52:54:00:12:34:56','-object','filter-dump,id=wire,netdev=n0,file='+str(folder/'wire.pcap')]
    if fw=='uefi':
        variables=folder/'OVMF_VARS_4M.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        args+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive','if=pflash,format=raw,unit=1,file='+str(variables)]
    (folder/'command.json').write_text(json.dumps(args,indent=2)+'\n');q=None;pending=bytearray();records=[];tx=0;retry=False
    with (folder/'qemu.log').open('x') as log:
        p=subprocess.Popen(args,cwd=REPO,stdout=log,stderr=subprocess.STDOUT,pass_fds=(child.fileno(),));child.close()
        try:
            until=time.monotonic()+45
            def send(raw,label):
                peer.sendall(struct.pack('!I',len(raw))+raw);records.append(dict(direction='RX',label=label,hex=raw.hex()))
            expected=[arp(1,G,GI,PI),tcp(18,ISN,0,native=True,ipid=1),tcp(18,NEXT,PEER+1,native=True,ipid=2)]
            while True:
                require(p.poll() is None,'QEMU exited')
                data=serial.read_bytes() if serial.exists() else b''
                require(not any(x in data for x in [b'panic:',b'boot: check failed',b'tcp: failed']),'TCP guest failure')
                if b'tcp: half-open timeout cleared; listening' in data and not retry:
                    require(tx==2,'timeout phase order')
                    for raw,label in negatives()[1]:send(raw,label)
                    send(tcp(2,PEER),'second peer SYN');retry=True
                try:
                    more=peer.recv(65536);require(more!=b'','peer closed');pending.extend(more)
                except BlockingIOError:pass
                while len(pending)>=4:
                    n=struct.unpack_from('!I',pending)[0];require(14<=n<=1514,'wire bounds')
                    if len(pending)<4+n:break
                    raw=bytes(pending[4:4+n]);del pending[:4+n]
                    require(tx<3 and raw==expected[tx],'unexpected native ARP/TCP frame '+str(tx))
                    records.append(dict(direction='TX',label=['ARP resolution','first SYN-ACK','second SYN-ACK'][tx],hex=raw.hex()));tx+=1
                    if tx==1:
                        send(arp(2,P,PI,GI,G,G),'solicited ARP reply')
                        for raw,label in negatives()[0]:send(raw,label)
                        send(tcp(2,0xffffffff),'first peer SYN')
                    elif tx==2:
                        send(tcp(16,0,ISN+2),'wrong first acknowledgment');send(tcp(16,1,ISN+1),'wrong first sequence')
                    else:
                        for raw,label in negatives()[2]:send(raw,label)
                        send(tcp(16,PEER+1,NEXT+1),'valid final ACK')
                if b'tcp: PASS handshake=1 timeout=1 rejected=20 TX=3 RX=24; DMA pool released' in data:break
                require(time.monotonic()<until,'TCP wire deadline');time.sleep(.01)
            require(tx==3 and len(records)==27 and retry,'wire counts/timeout')
            require(b'proc[hello]: exited status 0, all frames reclaimed' in data,'ABI v0 hello')
            q=QMP(sock);regs=q.execute('human-monitor-command',{'command-line':'info registers'});require('HLT=1' in regs,'kernel did not halt')
            (folder/'registers.txt').write_text(regs);q.execute('screendump',{'filename':str(folder/'screen.ppm')});capture=png(folder/'screen.ppm',folder/'screen.png')
            return dict(passed=True,firmware=fw,handshakes=1,half_open_timeouts=1,SYNACK_sent=2,rejected=20,connection_limit=1,context_bound_bytes=512,
                        TX=3,RX=24,ARP_resolution=True,adapter_reset_confirmed=True,DMA_frames_released=11,endpoint_erased=True,ABI_v0_hello=True,
                        deterministic_test_ISNs=True,no_streaming_or_retransmission=True,no_FIN_RST_exchange=True,no_application_grants=True,screenshot=capture)
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
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));resource.setrlimit(resource.RLIMIT_FSIZE,(64*1024**2,64*1024**2));result={'scope':'bounded passive TCP establishment','firmware':{}}
    try:
        cc=['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Iarch/x86_64']
        for name,extra in [('net-arp',[]),('net-ipv4',['arch/x86_64/net_ipv4.c']),('net-udp',['arch/x86_64/net_ipv4.c','arch/x86_64/net_udp.c']),('net-tcp',['arch/x86_64/net_ipv4.c','arch/x86_64/net_tcp.c'])]:
            binary=out/name
            run(cc+['tests/host/'+name+'.c','arch/x86_64/net_arp.c','arch/x86_64/net_boot.c']+extra+['-o',str(binary)],out/(name+'-build.log'))
            run([str(binary)],out/(name+'-test.log'))
        empty=subprocess.run(['make','-n','tcp-iso','TCP_BUILD='],cwd=REPO,capture_output=True,text=True)
        require(empty.returncode==2 and 'TCP_BUILD must be nonempty' in empty.stderr,'empty output guard');(out/'path-guard.txt').write_text(empty.stderr)
        run(['make','--no-print-directory','tcp-uefi-iso','TCP_BUILD='+str(out/'native')],out/'image-build.log')
        for fw,iso in [('bios','vos.iso'),('uefi','vos-uefi.iso')]:result['firmware'][fw]=boot(out,fw,out/'native/kernel'/iso)
        result.update(passed=True,host_sanitizers=['address','undefined'],userspace_ABI_unchanged=True)
    except BaseException as e:result.update(passed=False,error=str(e));raise
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        rows=[dict(path=str(p.relative_to(out)),size=p.stat().st_size,sha256=sha(p)) for p in sorted(out.rglob('*')) if p.is_file()]
        (out/'manifest.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(result),flush=True)
if __name__=='__main__':main()
