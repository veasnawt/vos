#!/usr/bin/env python3
"""Bounded TCP closure correctness, controlled FIN loss and separate release metrics."""
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
BASELINE=REPO/'build/n5-tcp-retry-20261024T193224Z/results'

def expected():
    a=S+0x10000;b=R+0x1000;c=S+0x20000;d=R+0x2000;e=S+0x30000;f=R+0x3000
    rows=[(18,S-1,R,64,b''),(24,S,R,64,b'retain'),(16,S+6,R+1,0,b''),(24,S+2,R+1,0,b'tain'),
          (17,S+6,R+1,0,b''),(17,S+6,R+1,0,b''),(16,S+7,R+1,0,b''),
          (18,a-1,b,64,b''),(17,a,b,64,b''),(16,a+1,b,64,b''),(16,a+1,b,64,b''),
          (16,a+1,b+1,0,b''),(16,a+1,b+1,0,b''),(16,a+1,b+1,0,b''),
          (18,c-1,d,64,b''),(16,c,d+2,62,b''),(24,c,d+2,62,b'tx'),
          (18,e-1,f,64,b'')]+[(17,e,f,64,b'')]*4
    return [arp(1,G,GI,PI)]+[packet(flags,seq,ack,win,data,native=True,ipid=i+1) for i,(flags,seq,ack,win,data) in enumerate(rows)]

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
            exp=expected();A=S+0x10000;B=R+0x1000;C=S+0x20000;D=R+0x2000;E=S+0x30000;F=R+0x3000
            while True:
                require(p.poll() is None,'QEMU exited')
                if time.monotonic()-last_sample>=.1:sample(p.pid)
                data=serial.read_bytes() if serial.exists() else b''
                require(not any(x in data for x in [b'panic:',b'boot: check failed',b'tcp-close: failed']),'TCP closure guest failure')
                try:
                    more=peer.recv(65536);require(more!=b'','peer closed');pending.extend(more)
                except BlockingIOError:pass
                while len(pending)>=4:
                    n=struct.unpack_from('!I',pending)[0];require(14<=n<=1514,'wire bounds')
                    if len(pending)<4+n:break
                    raw=bytes(pending[4:4+n]);del pending[:4+n]
                    require(tx<23 and raw==exp[tx],'unexpected closure frame '+str(tx))
                    records.append(dict(direction='TX',label='native '+str(tx+1),hex=raw.hex()));tx+=1
                    if tx==1:send(arp(2,P,PI,GI,G,G),'solicited ARP reply');send(packet(2,R-1,0,64),'passive peer SYN')
                    elif tx==2:send(packet(16,R,S,64),'passive final ACK')
                    elif tx==3:
                        delivered.extend(b're');loss.append(dict(native=tx,action='accept two-byte prefix, withhold four-byte tail and half-close peer send side',bytes=4))
                        send(packet(17,R,S+2,64),'peer FIN and partial data ACK')
                    elif tx==5:delivered.extend(b'tain');send(packet(16,R+1,S+6,64),'ACK retained tail after peer half-close')
                    elif tx==6:loss.append(dict(native=tx,action='drop FIN ACK',dropped_ACK_hex=packet(16,R+1,S+7,64).hex()))
                    elif tx==7:send(packet(17,R,S+6,64),'duplicate peer FIN after local FIN retry')
                    elif tx==8:send(packet(16,R+1,S+7,64),'ACK local FIN; LAST-ACK completes');send(packet(2,B-1,0,64),'active-close peer SYN')
                    elif tx==9:send(packet(16,B,A,64),'active-close final ACK')
                    elif tx==10:
                        send(packet(16,B,A+2,64),'future ACK cannot retire unsent byte')
                        send(packet(17,B+2,A,64),'invalid FIN sequence')
                        send(packet(4,B+1,0,64),'in-window inaccurate RST')
                        send(packet(4,B-1,0,64),'out-of-window RST')
                    elif tx==12:
                        send(packet(17,B,A,64),'simultaneous peer FIN')
                        send(packet(17,B,A+1,64),'duplicate FIN with ACK of local FIN')
                        send(packet(4,B+1,0,64),'RST cannot assassinate TIME-WAIT')
                        send(packet(17,B,A+1,64),'duplicate FIN during TIME-WAIT')
                        send(packet(2,B-1,0,64),'SYN reuse blocked during TIME-WAIT')
                    elif tx==15:send(packet(2,D-1,0,64),'reset-test peer SYN after explicit local abort')
                    elif tx==16:send(packet(16,D,C,64),'reset-test final ACK');send(packet(24,D,C,64,b'rx'),'receive data retained for reset test')
                    elif tx==18:
                        bad=bytearray(packet(4,D+2,0,64));bad[6]^=4
                        send(bytes(bad),'wrong-MAC spoofed reset');send(packet(4,D+1,0,64),'old-sequence reset rejected')
                        send(packet(4,D+2,0,64),'exact-sequence bound-peer reset')
                        send(packet(2,F-1,0,64),'FIN-timeout peer SYN')
                    elif tx==19:send(packet(16,F,E,64),'FIN-timeout final ACK')
                    elif tx>=20:loss.append(dict(native=tx,action='blackhole FIN to test retry exhaustion'))
                if b'tcp-close: PASS TX=23 RX=26; FIN/reset/timeout cleanup; DMA frames released=11' in data:break
                require(time.monotonic()<until,'closure wire deadline');time.sleep(.01)
            require(tx==23 and len(records)==49 and delivered==b'retain','wire/delivery counts')
            require(b'proc[hello]: exited status 0, all frames reclaimed' in data and b'TCP context bytes: 504;' in data,'ABI/context measurement');sample(p.pid)
            metric=dict(kind='optimized_unsanitized_QEMU_closure_scenario',firmware=fw,kernel_optimization='-O2 with debug symbols; no sanitizers',QEMU='q35/TCG, 128 MiB, modern virtio-net, isolated stream peer',
                        elapsed_to_PASS_seconds=time.monotonic()-start,QEMU_CPU_seconds=cpu/os.sysconf('SC_CLK_TCK'),QEMU_observed_VmHWM_KiB=hwm,guest_RAM_MiB=128,
                        context_bytes=504,protocol_RX_TX_bytes=128,private_DMA_bytes=45056,unique_delivered_bytes=6,
                        caveat='Includes firmware and deliberate FIN loss/timeout. Live TIME-WAIT is explicitly aborted after validation, never shortened. Not matched throughput comparison.',host=host_conditions())
            (folder/'performance.json').write_text(json.dumps(metric,indent=2)+'\n')
            q=QMP(sock);regs=q.execute('human-monitor-command',{'command-line':'info registers'});require('HLT=1' in regs,'kernel did not halt')
            (folder/'registers.txt').write_text(regs);q.execute('screendump',{'filename':str(folder/'screen.ppm')});capture=png(folder/'screen.ppm',folder/'screen.png')
            return dict(passed=True,firmware=fw,passive_close_completed=True,active_and_simultaneous_TIME_WAIT_verified=True,TIME_WAIT_live_explicit_abort=True,
                        full_TIME_WAIT_expiry_host_tested=True,retained_tail_after_peer_FIN=True,lost_FIN_ACK_recovered=True,invalid_controls_rejected=True,
                        exact_sequence_reset_erased_RX_TX=True,exhaustion_retries=3,TX=23,RX=26,context_bytes=504,adapter_reset_confirmed=True,DMA_frames_released=11,
                        no_retry_after_destruction=True,endpoint_erased=True,ABI_v0_hello=True,no_application_grants=True,screenshot=capture)
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
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));resource.setrlimit(resource.RLIMIT_FSIZE,(64*1024**2,64*1024**2));result={'scope':'bounded TCP connection closure','firmware':{}}
    try:
        cc=['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Iarch/x86_64']
        common=['arch/x86_64/net_arp.c','arch/x86_64/net_ipv4.c','arch/x86_64/net_tcp.c','arch/x86_64/net_boot.c']
        for name in ['net-tcp-close','net-tcp-retry','net-tcp-data','net-tcp']:
            binary=out/name
            run(cc+['tests/host/'+name+'.c']+common+['-o',str(binary)],out/(name+'-build.log'));run([str(binary)],out/(name+'-test.log'))
        require(sha(BASELINE/'manifest.json')=='7b5c643c517b8be33ad1222f2a0610c4848aba9e2104ec6cd957d33798d02f35','accepted retry baseline manifest')
        rows=json.loads((BASELINE/'manifest.json').read_text())
        for name in ['release-model','retry-model']:
            require(sha(BASELINE/name)==next(x['sha256'] for x in rows if x['path']==name),'accepted model binary '+name)
        release=['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-Iarch/x86_64','-Itests/host']
        run(release+['-DVOS_TCP_PAYLOAD_BENCH','tests/host/net-tcp-bench.c']+common+['-o',str(out/'release-model')],out/'release-model-build.log')
        run(release+['tests/host/net-tcp-retry-bench.c']+common+['-o',str(out/'retry-model')],out/'retry-model-build.log')
        run(release+['tests/host/net-tcp-close-bench.c']+common+['-o',str(out/'close-model')],out/'close-model-build.log')
        for name in ['release-model','retry-model']:
            run([str(BASELINE/name)],out/(name+'-baseline.json'));run([str(out/name)],out/(name+'.json'))
        run([str(out/'close-model')],out/'close-model.json')
        old=json.loads((out/'release-model-baseline.json').read_text());new=json.loads((out/'release-model.json').read_text())
        rold=json.loads((out/'retry-model-baseline.json').read_text());rnew=json.loads((out/'retry-model.json').read_text())
        changes={k:(statistics.median(new[k])/statistics.median(old[k])-1)*100 for k in ['legacy_ns_per_handshake','payload_ns_per_two_byte_bidirectional_transaction_with_setup']}
        changes['ns_per_retry']=(statistics.median(rnew['ns_per_retry'])/statistics.median(rold['ns_per_retry'])-1)*100
        (out/'performance-pair.json').write_text(json.dumps(dict(kind='paired optimized host protocol models, not QEMU throughput',accepted_retry_commit='d512db7cdf40b5b3dbcaf9c18f21041943d1c22c',
            accepted_manifest_sha256=sha(BASELINE/'manifest.json'),canonical_benchmark_sha256=sha(REPO/'tests/host/net-tcp-bench.c'),retry_benchmark_sha256=sha(REPO/'tests/host/net-tcp-retry-bench.c'),
            release_command=release,baseline=old,current=new,retry_baseline=rold,retry_current=rnew,closure=json.loads((out/'close-model.json').read_text()),
            median_change_percent=changes,potential_regression_over_25_percent=any(x>25 for x in changes.values()),host=host_conditions(),sanitizer_results_separate=True),indent=2)+'\n')
        empty=subprocess.run(['make','-n','tcp-close-iso','TCP_CLOSE_BUILD='],cwd=REPO,capture_output=True,text=True)
        require(empty.returncode==2 and 'TCP_CLOSE_BUILD must be nonempty' in empty.stderr,'empty output guard');(out/'path-guard.txt').write_text(empty.stderr)
        run(['make','--no-print-directory','tcp-close-uefi-iso','TCP_CLOSE_BUILD='+str(out/'native')],out/'image-build.log')
        for fw,iso in [('bios','vos.iso'),('uefi','vos-uefi.iso')]:result['firmware'][fw]=boot(out,fw,out/'native/kernel'/iso)
        result.update(passed=True,host_sanitizers=['address','undefined'],userspace_ABI_unchanged=True)
    except BaseException as e:result.update(passed=False,error=str(e));raise
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');rows=[dict(path=str(p.relative_to(out)),size=p.stat().st_size,sha256=sha(p)) for p in sorted(out.rglob('*')) if p.is_file()]
        (out/'manifest.json').write_text(json.dumps(rows,indent=2)+'\n');print(json.dumps(result),flush=True)
if __name__=='__main__':main()
