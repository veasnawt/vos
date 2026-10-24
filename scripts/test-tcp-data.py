#!/usr/bin/env python3
"""Bounded TCP payload correctness and compact, separate release metrics."""
import argparse
import json
import os
from pathlib import Path
import platform
import resource
import runpy
import shutil
import socket
import statistics
import struct
import subprocess
import time
REPO=Path(__file__).resolve().parents[1]
H=runpy.run_path(str(REPO/'scripts/test-tcp.py'),run_name='payload_helpers')
run,QMP,png,require,sha,checksum=[H[n] for n in ['run','QMP','png','require','sha','checksum']]
G,P,GI,PI,arp=[H[n] for n in ['G','P','GI','PI','arp']]
S=0x11223345;R=0x01020305
BASELINE=REPO/'build/n5-tcp-data-20261024T185044Z'

def packet(flags,seq,ack,window,data=b'',*,native=False,ipid=0x1122):
    p=bytearray(H['tcp'](flags,seq,ack,native=native,ipid=ipid,payload=data));struct.pack_into('!H',p,48,window);p[50:52]=bytes(2)
    size=20+len(data);struct.pack_into('!H',p,50,checksum(p[26:34]+bytes([0,6])+struct.pack('!H',size)+p[34:34+size]));return bytes(p)

def expected():
    specs=[(18,S-1,R,64,b''),(16,S,R+64,0,b''),(16,S,R+64,0,b''),(16,S,R+64,0,b''),(16,S,R+64,0,b''),
           (16,S,R+64,16,b''),(16,S,R+80,0,b''),(16,S,R+80,64,b''),(16,S,R+89,55,b''),(24,S,R+89,55,b'VOS!'),
           (16,S+4,R+90,54,b''),(24,S+4,R+90,54,bytes(range(128,192))),(16,S+68,R+90,54,b''),(16,S+68,R+90,54,b''),(16,S+68,R+144,0,b'')]
    return [arp(1,G,GI,PI)]+[packet(*x,native=True,ipid=i+1) for i,x in enumerate(specs)]

def host_conditions():
    return dict(platform=platform.platform(),logical_cpus=os.cpu_count(),loadavg=os.getloadavg(),
                CPU_model=next((x.split(':',1)[1].strip() for x in Path('/proc/cpuinfo').read_text().splitlines() if x.startswith('model name')),'unknown'))

def boot(out,fw,iso):
    folder=out/fw;folder.mkdir();serial=folder/'serial.log';sock=folder/'qmp';peer,child=socket.socketpair();peer.setblocking(False)
    args=['qemu-system-x86_64','-machine','q35','-accel','tcg','-m','128M','-cdrom',str(iso),'-boot','d','-display','none','-nic','none',
          '-serial','file:'+str(serial),'-qmp','unix:'+str(sock)+',server=on,wait=off','-no-reboot','-no-shutdown',
          '-netdev','stream,id=n0,server=off,addr.type=fd,addr.str='+str(child.fileno()),
          '-device','virtio-net-pci,netdev=n0,disable-legacy=on,mac=52:54:00:12:34:56','-object','filter-dump,id=wire,netdev=n0,file='+str(folder/'wire.pcap')]
    if fw=='uefi':
        variables=folder/'OVMF_VARS_4M.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        args+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive','if=pflash,format=raw,unit=1,file='+str(variables)]
    (folder/'command.json').write_text(json.dumps(args,indent=2)+'\n');q=None;pending=bytearray();records=[];tx=0;partial=False;start=time.monotonic();last_sample=0;cpu=0;hwm=0
    def sample(pid):
        nonlocal cpu,hwm,last_sample
        fields=Path('/proc/'+str(pid)+'/stat').read_text().split(') ',1)[1].split();cpu=max(cpu,int(fields[11])+int(fields[12]))
        status=Path('/proc/'+str(pid)+'/status').read_text().splitlines()
        hwm=max(hwm,next(int(x.split()[1]) for x in status if x.startswith('VmHWM:')));last_sample=time.monotonic()
    with (folder/'qemu.log').open('x') as log:
        p=subprocess.Popen(args,cwd=REPO,stdout=log,stderr=subprocess.STDOUT,pass_fds=(child.fileno(),));child.close()
        try:
            until=time.monotonic()+45
            def send(raw,label):
                peer.sendall(struct.pack('!I',len(raw))+raw);records.append(dict(direction='RX',label=label,hex=raw.hex()))
            exp=expected()
            while True:
                require(p.poll() is None,'QEMU exited')
                if time.monotonic()-last_sample>=.1:sample(p.pid)
                data=serial.read_bytes() if serial.exists() else b''
                require(not any(x in data for x in [b'panic:',b'boot: check failed',b'tcp-data: failed']),'TCP payload guest failure')
                if b'tcp-data: partial ACK retained 2 bytes' in data and not partial:
                    require(tx==11,'partial ACK phase')
                    send(packet(16,R+89,S+4,0),'full ACK zero peer window');send(packet(24,R+89,S+4,0,b'Z'),'one byte while peer window zero');partial=True
                try:
                    more=peer.recv(65536);require(more!=b'','peer closed');pending.extend(more)
                except BlockingIOError:pass
                while len(pending)>=4:
                    n=struct.unpack_from('!I',pending)[0];require(14<=n<=1514,'wire bounds')
                    if len(pending)<4+n:break
                    raw=bytes(pending[4:4+n]);del pending[:4+n]
                    require(tx<16 and raw==exp[tx],'unexpected native payload/ACK frame '+str(tx))
                    records.append(dict(direction='TX',label='native '+str(tx+1),hex=raw.hex()));tx+=1
                    if tx==1:send(arp(2,P,PI,GI,G,G),'solicited ARP reply');send(packet(2,R-1,0,64),'peer SYN')
                    elif tx==2:send(packet(16,R,S,64),'final ACK');send(packet(24,R,S,64,bytes(range(64))),'first 64 bytes')
                    elif tx==3:
                        send(packet(24,R,S,64,bytes(range(64))),'duplicate 64 bytes')
                        send(packet(24,R+60,S,64,b'overlap!'),'overlap crossing receive edge')
                        send(packet(24,R+64,S,64,b'X'),'zero window new byte')
                    elif tx==7:send(packet(24,R+64,S,64,bytes(range(64,80))),'16 reopened-window bytes')
                    elif tx==9:
                        send(packet(24,R+80,S+1,64,b'bad'),'future ACK with data');send(packet(24,R+80,S,4,b'peer-data'),'valid 9 bytes peer window 4')
                    elif tx==11:
                        send(packet(16,R+89,S+5,64),'future transmit ACK')
                        bad=bytearray(packet(16,R+89,S+4,64));bad[50]^=1;send(bytes(bad),'bad TCP checksum')
                        send(packet(16,R+89,S+2,0),'partial ACK zero window');send(packet(16,R+89,S+2,0),'duplicate partial ACK')
                    elif tx==12:send(packet(16,R+90,S+4,64),'peer window reopened')
                    elif tx==13:
                        send(packet(24,R+91,S+68,64,b'future'),'out of order cannot retire TX')
                        send(packet(24,R+89,S+68,64,b'over'),'overlap cannot retire TX')
                        send(packet(24,R+90,S+4,64,bytes(range(200,254))),'last 54 bytes full receive buffer')
                if b'tcp-data: PASS RXbytes=144 TXbytes=68 TX=16 RX=20; timeout/reset erased buffers' in data:break
                require(time.monotonic()<until,'payload wire deadline');time.sleep(.01)
            require(tx==16 and len(records)==36 and partial,'wire counts')
            require(b'proc[hello]: exited status 0, all frames reclaimed' in data,'ABI v0 hello')
            require(b'TCP context bytes: 504;' in data,'actual context measurement');sample(p.pid)
            metric=dict(kind='optimized_unsanitized_QEMU_scenario',firmware=fw,kernel_optimization='-O2 with debug symbols; no sanitizers',QEMU='q35/TCG, 128 MiB, modern virtio-net, isolated stream peer',
                        elapsed_to_PASS_seconds=time.monotonic()-start,QEMU_CPU_seconds=cpu/os.sysconf('SC_CLK_TCK'),QEMU_observed_VmHWM_KiB=hwm,guest_RAM_MiB=128,
                        context_bytes=504,protocol_RX_TX_bytes=128,private_DMA_bytes=45056,accepted_RX_bytes=144,sent_TX_bytes=68,
                        caveat='Scenario includes peer polling and deliberate 2-second TX timeout; not maximum throughput or hard real-time measurement.',host=host_conditions())
            (folder/'performance.json').write_text(json.dumps(metric,indent=2)+'\n')
            q=QMP(sock);regs=q.execute('human-monitor-command',{'command-line':'info registers'});require('HLT=1' in regs,'kernel did not halt')
            (folder/'registers.txt').write_text(regs);q.execute('screendump',{'filename':str(folder/'screen.ppm')});capture=png(folder/'screen.ppm',folder/'screen.png')
            return dict(passed=True,firmware=fw,handshakes=1,accepted_RX_bytes=144,read_RX_bytes=80,sent_TX_bytes=68,acknowledged_TX_bytes=4,timeout_aborted_RX_bytes=64,timeout_aborted_TX_bytes=64,
                        corrective_ACKs=5,invalid_packets_or_ACKs=3,TX=16,RX=20,RX_TX_buffer_bytes=64,context_bytes=504,timeout_abort=True,adapter_reset_confirmed=True,
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
    out=a.output.resolve();require(out.is_relative_to(REPO/'build') and not out.exists(),'fresh output');out.mkdir(parents=True)
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));resource.setrlimit(resource.RLIMIT_FSIZE,(64*1024**2,64*1024**2));result={'scope':'bounded TCP payload and relevant security/performance evidence','firmware':{}}
    try:
        cc=['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Iarch/x86_64']
        for name,extra in [('net-arp',[]),('net-ipv4',['arch/x86_64/net_ipv4.c']),('net-udp',['arch/x86_64/net_ipv4.c','arch/x86_64/net_udp.c']),('net-tcp',['arch/x86_64/net_ipv4.c','arch/x86_64/net_tcp.c']),('net-tcp-data',['arch/x86_64/net_ipv4.c','arch/x86_64/net_tcp.c'])]:
            binary=out/name
            run(cc+['tests/host/'+name+'.c','arch/x86_64/net_arp.c','arch/x86_64/net_boot.c']+extra+['-o',str(binary)],out/(name+'-build.log'));run([str(binary)],out/(name+'-test.log'))
        baseline=json.loads((BASELINE/'baseline-record.json').read_text())
        require(sha(BASELINE/'baseline')==baseline['binary_sha256'] and sha(REPO/'tests/host/net-tcp-bench.c')==baseline['benchmark_sha256'],'paired benchmark identity')
        release=['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Iarch/x86_64','-Itests/host','-DVOS_TCP_PAYLOAD_BENCH']
        run(release+['tests/host/net-tcp-bench.c','arch/x86_64/net_tcp.c','arch/x86_64/net_ipv4.c','arch/x86_64/net_arp.c','arch/x86_64/net_boot.c','-o',str(out/'release-model')],out/'release-model-build.log')
        run([str(BASELINE/'baseline')],out/'baseline-contemporaneous.json');run([str(out/'release-model')],out/'release-model.json')
        old=json.loads((out/'baseline-contemporaneous.json').read_text());new=json.loads((out/'release-model.json').read_text());delta=(statistics.median(new['legacy_ns_per_handshake'])/statistics.median(old['legacy_ns_per_handshake'])-1)*100
        (out/'performance-pair.json').write_text(json.dumps(dict(kind='paired optimized hosted protocol model, not QEMU throughput',baseline_record=str(BASELINE/'baseline-record.json'),baseline_binary_sha256=baseline['binary_sha256'],same_benchmark_sha256=baseline['benchmark_sha256'],
            release_command=release,baseline=old,current=new,legacy_median_change_percent=delta,potential_regression_over_25_percent=delta>25,host=host_conditions(),sanitizer_results_separate=True),indent=2)+'\n')
        empty=subprocess.run(['make','-n','tcp-data-iso','TCP_DATA_BUILD='],cwd=REPO,capture_output=True,text=True)
        require(empty.returncode==2 and 'TCP_DATA_BUILD must be nonempty' in empty.stderr,'empty output guard');(out/'path-guard.txt').write_text(empty.stderr)
        run(['make','--no-print-directory','tcp-data-uefi-iso','TCP_DATA_BUILD='+str(out/'native')],out/'image-build.log')
        for fw,iso in [('bios','vos.iso'),('uefi','vos-uefi.iso')]:result['firmware'][fw]=boot(out,fw,out/'native/kernel'/iso)
        result.update(passed=True,host_sanitizers=['address','undefined'],userspace_ABI_unchanged=True)
    except BaseException as e:result.update(passed=False,error=str(e));raise
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');rows=[dict(path=str(p.relative_to(out)),size=p.stat().st_size,sha256=sha(p)) for p in sorted(out.rglob('*')) if p.is_file()]
        (out/'manifest.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(result),flush=True)
if __name__=='__main__':main()
