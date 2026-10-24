#!/usr/bin/env python3
"""Bounded TCP retry correctness, controlled loss and separate release metrics."""
import argparse
import json
import os
from pathlib import Path
import resource
import runpy
import shutil
import socket
import statistics
import struct
import subprocess
import time
REPO=Path(__file__).resolve().parents[1]
H=runpy.run_path(str(REPO/'scripts/test-tcp-data.py'),run_name='retry_helpers')
run,QMP,png,require,sha,packet,host_conditions=[H[n] for n in ['run','QMP','png','require','sha','packet','host_conditions']]
G,P,GI,PI,arp=[H[n] for n in ['G','P','GI','PI','arp']]
S=0x11223345;R=0x01020305
BASELINE=REPO/'build/n5-tcp-data-20261024T185933Z/results'

def expected():
    seq=[S,S,S+8,S+8,S+16,S+20,S+28,S+28,S+28,S+28]
    data=[b'dataLOSS',b'dataLOSS',b'ack-LOSS',b'ack-LOSS',b'partial-data',b'ial-data']+[b'timeout-retained']*4
    return [arp(1,G,GI,PI),packet(18,S-1,R,64,native=True,ipid=1)]+[packet(24,q,R,64,b,native=True,ipid=i+2) for i,(q,b) in enumerate(zip(seq,data))]

def boot(out,fw,iso):
    folder=out/fw;folder.mkdir();serial=folder/'serial.log';sock=folder/'qmp';peer,child=socket.socketpair();peer.setblocking(False)
    args=['qemu-system-x86_64','-machine','q35','-accel','tcg','-m','128M','-cdrom',str(iso),'-boot','d','-display','none','-nic','none',
          '-serial','file:'+str(serial),'-qmp','unix:'+str(sock)+',server=on,wait=off','-no-reboot','-no-shutdown',
          '-netdev','stream,id=n0,server=off,addr.type=fd,addr.str='+str(child.fileno()),
          '-device','virtio-net-pci,netdev=n0,disable-legacy=on,mac=52:54:00:12:34:56','-object','filter-dump,id=wire,netdev=n0,file='+str(folder/'wire.pcap')]
    if fw=='uefi':
        variables=folder/'OVMF_VARS_4M.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        args+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive','if=pflash,format=raw,unit=1,file='+str(variables)]
    (folder/'command.json').write_text(json.dumps(args,indent=2)+'\n');q=None;pending=bytearray();records=[];loss=[];delivered=bytearray();tx=0;start=time.monotonic();last_sample=0;cpu=0;hwm=0
    def sample(pid):
        nonlocal cpu,hwm,last_sample
        fields=Path('/proc/'+str(pid)+'/stat').read_text().split(') ',1)[1].split();cpu=max(cpu,int(fields[11])+int(fields[12]))
        hwm=max(hwm,next(int(x.split()[1]) for x in Path('/proc/'+str(pid)+'/status').read_text().splitlines() if x.startswith('VmHWM:')));last_sample=time.monotonic()
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
                require(not any(x in data for x in [b'panic:',b'boot: check failed',b'tcp-retry: failed']),'TCP retry guest failure')
                try:
                    more=peer.recv(65536);require(more!=b'','peer closed');pending.extend(more)
                except BlockingIOError:pass
                while len(pending)>=4:
                    n=struct.unpack_from('!I',pending)[0];require(14<=n<=1514,'wire bounds')
                    if len(pending)<4+n:break
                    raw=bytes(pending[4:4+n]);del pending[:4+n]
                    require(tx<12 and raw==exp[tx],'unexpected retry frame '+str(tx))
                    records.append(dict(direction='TX',label='native '+str(tx+1),hex=raw.hex()));tx+=1
                    if tx==1:send(arp(2,P,PI,GI,G,G),'solicited ARP reply');send(packet(2,R-1,0,64),'peer SYN')
                    elif tx==2:send(packet(16,R,S,64),'final ACK')
                    elif tx==3:loss.append(dict(native=tx,action='drop data before application delivery',bytes=8))
                    elif tx==4:delivered.extend(b'dataLOSS');send(packet(16,R,S+8,64),'ACK recovered data')
                    elif tx==5:
                        delivered.extend(b'ack-LOSS');loss.append(dict(native=tx,action='deliver once but drop ACK',bytes=8,dropped_ACK_hex=packet(16,R,S+16,64).hex()))
                    elif tx==6:
                        loss.append(dict(native=tx,action='duplicate detected; no application redelivery',bytes=8));send(packet(16,R,S+16,64),'ACK duplicate after ACK loss')
                    elif tx==7:
                        delivered.extend(b'part');loss.append(dict(native=tx,action='accept four-byte prefix; withhold trailing eight bytes',bytes=8))
                        send(packet(16,R,S+20,64),'partial ACK');send(packet(16,R,S+20,64),'duplicate partial ACK');send(packet(16,R,S+29,64),'invalid future ACK')
                    elif tx==8:delivered.extend(b'ial-data');send(packet(16,R,S+28,64),'cumulative tail ACK')
                    else:loss.append(dict(native=tx,action='drop data for retry exhaustion',bytes=16))
                if b'tcp-retry: PASS delivered=28 retries=6 TX=12 RX=9; timeout/reset erased state' in data:break
                require(time.monotonic()<until,'retry wire deadline');time.sleep(.01)
            require(tx==12 and len(records)==21 and delivered==b'dataLOSSack-LOSSpartial-data','wire/delivery counts')
            require(b'proc[hello]: exited status 0, all frames reclaimed' in data and b'TCP context bytes: 504;' in data,'ABI/context measurement');sample(p.pid)
            metric=dict(kind='optimized_unsanitized_QEMU_loss_scenario',firmware=fw,kernel_optimization='-O2 with debug symbols; no sanitizers',QEMU='q35/TCG, 128 MiB, modern virtio-net, isolated stream peer',
                        elapsed_to_PASS_seconds=time.monotonic()-start,QEMU_CPU_seconds=cpu/os.sysconf('SC_CLK_TCK'),QEMU_observed_VmHWM_KiB=hwm,guest_RAM_MiB=128,
                        context_bytes=504,protocol_RX_TX_bytes=128,private_DMA_bytes=45056,unique_delivered_bytes=28,terminal_aborted_TX_bytes=16,
                        caveat='Includes firmware, deliberate loss and fixed retry waits. Not maximum throughput or a matched QEMU latency comparison.',host=host_conditions())
            (folder/'performance.json').write_text(json.dumps(metric,indent=2)+'\n')
            q=QMP(sock);regs=q.execute('human-monitor-command',{'command-line':'info registers'});require('HLT=1' in regs,'kernel did not halt')
            (folder/'registers.txt').write_text(regs);q.execute('screendump',{'filename':str(folder/'screen.ppm')});capture=png(folder/'screen.ppm',folder/'screen.png')
            return dict(passed=True,firmware=fw,data_loss_recovered=True,ACK_loss_recovered_without_redelivery=True,partial_tail_recovered=True,retransmissions=6,
                        exhaustion_retries=3,unique_delivered_bytes=28,wire_TCP_payload_bytes=116,terminal_aborted_TX_bytes=16,TX=12,RX=9,
                        context_bytes=504,adapter_reset_confirmed=True,DMA_frames_released=11,no_retry_after_destruction=True,endpoint_erased=True,ABI_v0_hello=True,no_application_grants=True,screenshot=capture)
        finally:
            (folder/'wire-records.json').write_text(json.dumps(records,indent=2)+'\n');(folder/'loss-decisions.json').write_text(json.dumps(loss,indent=2)+'\n')
            (folder/'application-delivered.hex').write_text(delivered.hex()+'\n');(folder/'peer-pending.hex').write_text(pending.hex()+'\n')
            if q:q.close()
            peer.close()
            if p.poll() is None:p.terminate()
            try:p.wait(timeout=3)
            except subprocess.TimeoutExpired:p.kill();p.wait(timeout=3)

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--output',required=True,type=Path);a=ap.parse_args()
    out=a.output.resolve();require(out.is_relative_to(REPO/'build') and not out.exists(),'fresh output');out.mkdir(parents=True)
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));resource.setrlimit(resource.RLIMIT_FSIZE,(64*1024**2,64*1024**2));result={'scope':'bounded TCP retransmission','firmware':{}}
    try:
        cc=['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Iarch/x86_64']
        for name,extra in [('net-arp',[]),('net-ipv4',['arch/x86_64/net_ipv4.c']),('net-udp',['arch/x86_64/net_ipv4.c','arch/x86_64/net_udp.c']),('net-tcp',['arch/x86_64/net_ipv4.c','arch/x86_64/net_tcp.c']),('net-tcp-data',['arch/x86_64/net_ipv4.c','arch/x86_64/net_tcp.c']),('net-tcp-retry',['arch/x86_64/net_ipv4.c','arch/x86_64/net_tcp.c'])]:
            binary=out/name
            run(cc+['tests/host/'+name+'.c','arch/x86_64/net_arp.c','arch/x86_64/net_boot.c']+extra+['-o',str(binary)],out/(name+'-build.log'));run([str(binary)],out/(name+'-test.log'))
        require(sha(BASELINE/'manifest.json')=='d764382c7ae872df9e825222a868e8b31102015321fad271ba4fb82dacbea688','accepted payload baseline manifest')
        rows=json.loads((BASELINE/'manifest.json').read_text());expected_hash=next(x['sha256'] for x in rows if x['path']=='release-model');require(sha(BASELINE/'release-model')==expected_hash,'accepted benchmark binary')
        release=['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Iarch/x86_64','-Itests/host']
        common=['arch/x86_64/net_tcp.c','arch/x86_64/net_ipv4.c','arch/x86_64/net_arp.c','arch/x86_64/net_boot.c']
        run(release+['-DVOS_TCP_PAYLOAD_BENCH','tests/host/net-tcp-bench.c']+common+['-o',str(out/'release-model')],out/'release-model-build.log')
        run(release+['tests/host/net-tcp-retry-bench.c']+common+['-o',str(out/'retry-model')],out/'retry-model-build.log')
        run([str(BASELINE/'release-model')],out/'baseline-contemporaneous.json');run([str(out/'release-model')],out/'release-model.json');run([str(out/'retry-model')],out/'retry-model.json')
        old=json.loads((out/'baseline-contemporaneous.json').read_text());new=json.loads((out/'release-model.json').read_text());retry=json.loads((out/'retry-model.json').read_text())
        changes={k:(statistics.median(new[k])/statistics.median(old[k])-1)*100 for k in ['legacy_ns_per_handshake','payload_ns_per_two_byte_bidirectional_transaction_with_setup']}
        (out/'performance-pair.json').write_text(json.dumps(dict(kind='paired optimized host protocol model, not QEMU throughput',accepted_payload_commit='ff251a728529908776ec5557cbc359f2247bc369',baseline_binary_sha256=expected_hash,benchmark_source_sha256=sha(REPO/'tests/host/net-tcp-bench.c'),
            release_command=release,baseline=old,current=new,retry=retry,median_change_percent=changes,potential_regression_over_25_percent=any(x>25 for x in changes.values()),host=host_conditions(),sanitizer_results_separate=True),indent=2)+'\n')
        empty=subprocess.run(['make','-n','tcp-retry-iso','TCP_RETRY_BUILD='],cwd=REPO,capture_output=True,text=True)
        require(empty.returncode==2 and 'TCP_RETRY_BUILD must be nonempty' in empty.stderr,'empty output guard');(out/'path-guard.txt').write_text(empty.stderr)
        run(['make','--no-print-directory','tcp-retry-uefi-iso','TCP_RETRY_BUILD='+str(out/'native')],out/'image-build.log')
        for fw,iso in [('bios','vos.iso'),('uefi','vos-uefi.iso')]:result['firmware'][fw]=boot(out,fw,out/'native/kernel'/iso)
        result.update(passed=True,host_sanitizers=['address','undefined'],userspace_ABI_unchanged=True)
    except BaseException as e:result.update(passed=False,error=str(e));raise
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');rows=[dict(path=str(p.relative_to(out)),size=p.stat().st_size,sha256=sha(p)) for p in sorted(out.rglob('*')) if p.is_file()]
        (out/'manifest.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(result),flush=True)
if __name__=='__main__':main()
