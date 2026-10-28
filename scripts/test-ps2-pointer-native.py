#!/usr/bin/env python3
"""R2-B ordinary native regression runner; fresh outputs and explicit binaries.

Packet tracking and firmware assertions retain the accepted N6.3 test semantics.
Historical controller entry points are neither imported nor executed.
"""
import argparse,hashlib,importlib.util,json,os,re,runpy,shutil,socket,struct,subprocess,time,zlib
from pathlib import Path
REPO=Path(__file__).resolve().parents[1]
def load(name):
    spec=importlib.util.spec_from_file_location(name.replace('-','_'),REPO/'scripts'/name)
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
H=load('test-usb-probe.py')
V=load('test-usb-resources-native.py')
G=runpy.run_path(str(REPO/'scripts/test-gui.py'),run_name='pointer_gui_helpers')
sha,need=H.sha,H.need
# Accepted N6.3 read-only ext2 disk: vsh pwd needs a filesystem grant.
DISK_SHA256='5ef98c6e0b4dd983dfb7db1a8d73d0175cbb86c358e61f2957c689d164890dcc'
class QMP(H.QMP):
    execute=H.QMP.command
    tap=H.QMP.key
def capture(q,folder,name):
    path=folder/(name+'.png')
    need(not path.exists(),'fresh authentic screenshot')
    q.execute('screendump',{'filename':str(path),'format':'png'})
    return {'path':str(path),'sha256':sha(path)}

def rgb(path):
    blob=path.read_bytes();need(blob[:8]==b'\x89PNG\r\n\x1a\n','authentic PNG')
    at=8;stream=b''
    while at<len(blob):
        n=struct.unpack_from('>I',blob,at)[0];tag=blob[at+4:at+8];v=blob[at+8:at+8+n];at+=n+12
        if tag==b'IHDR':
            w,h,depth,kind,c,f,i=struct.unpack('>IIBBBBB',v)
            need(depth==8 and kind in (2,6) and c==f==i==0,'supported PNG')
        if tag==b'IDAT':stream+=v
    bpp=3 if kind==2 else 4;stride=w*bpp;raw=zlib.decompress(stream)
    need(len(raw)==(stride+1)*h,'PNG extent')
    rows=[];previous=bytearray(stride);at=0
    for _ in range(h):
        kind=raw[at];at+=1;row=bytearray(raw[at:at+stride]);at+=stride;need(kind<=4,'PNG filter')
        for x in range(stride):
            a=row[x-bpp] if x>=bpp else 0;b=previous[x];c=previous[x-bpp] if x>=bpp else 0
            p=a+b-c;dist=(abs(p-a),abs(p-b),abs(p-c));paeth=(a,b,c)[dist.index(min(dist))]
            add=(0,a,b,(a+b)//2,paeth)[kind];row[x]=(row[x]+add)&255
        rows.append(row);previous=row
    return w,h,lambda x,y:int.from_bytes(rows[y][x*bpp:x*bpp+3],'big')

def pointer_pixels(path,x,y,color):
    w,h,pixel=rgb(path);x0=(w-256)//2;y0=84+(h-156-160)//2
    need(pixel(x,y)==0x101a2a,'visible native cursor outline at expected position')
    if x+1<w and y+4<h:need(pixel(x+1,y+4)==0xffffff,'visible cursor interior')
    need(pixel(x0+60,y0+50)==color,'independent Rust panel click/palette pixel')
    need(pixel(x0,y0)==0x80baff,'native surface border')
    return dict(passed=True,cursor=[x,y],panel_rgb=hex(color),width=w,height=h)

def ps2_trace(path):
    path=Path(path)
    blob=path.read_text() if path.exists() else ''
    need(len(blob.encode())<=262144,'bounded PS/2 trace <=256 KiB')
    packets=[];reads=[];last=None;mouse=None
    for line in blob.splitlines():
        m=re.search(r'ps2_mouse_send_packet\s+(0x[0-9a-fA-F]+) x (-?\d+) y (-?\d+) z (-?\d+) bs 0x([0-9a-fA-F]+)',line)
        if m:
            p=dict(device=m[1],dx=int(m[2]),dy=int(m[3]),dz=int(m[4]),header=int(m[5],16))
            if mouse is None:mouse=p['device']
            need(mouse==p['device'],'single traced PS/2 mouse');packets.append(p);continue
        m=re.search(r'ps2_read_data\s+(0x[0-9a-fA-F]+)',line)
        if m:last=m[1];continue
        m=re.search(r'pckbd_kbd_read_data\s+0x([0-9a-fA-F]+)',line)
        if m:
            if last==mouse and mouse is not None:reads.append(int(m[1],16))
            last=None
    decoded=[]
    for i in range(len(reads)//3):
        h,x,y=reads[3*i:3*i+3]
        need(h&8 and not h&0xc0,'traced standard packet header/overflow')
        d=dict(dx=x-(256 if h&16 else 0),dy=y-(256 if h&32 else 0),buttons=h&1,bytes=[h,x,y])
        need(i<len(packets) and d['dx']==packets[i]['dx'] and d['dy']==packets[i]['dy'] and h==packets[i]['header'],'every guest-read packet matches emitted packet')
        decoded.append(d)
    return dict(emitted=packets,read_bytes=reads,decoded=decoded,dx=sum(p['dx'] for p in packets),dy=sum(p['dy'] for p in packets),drained=len(reads)==3*len(packets) and len(decoded)==len(packets))

def ps2_enable(q,path):
    for name in ('ps2_mouse_send_packet','ps2_read_data','pckbd_kbd_read_data'):
        q.execute('trace-event-set-state',dict(name=name,enable=True))

def ps2_wait_read(q,path,start_count,deadline):
    while True:
        state=ps2_trace(path)
        if len(state['emitted'])>start_count and state['drained']:return state
        need(time.monotonic()<deadline,'bounded PS/2 packet/read completion deadline')
        time.sleep(.002)

class PS2Motion:
    """Complete each injected vector before accepting another; no guest ABI."""
    def __init__(self,q,path,events,start):
        self.q=q;self.path=Path(path);self.events=events;self.start=start
        ps2_enable(q,path)
    def send(self,events,dx,dy):
        before=ps2_trace(self.path)
        need(before['drained'],'previous generated PS/2 bytes consumed')
        target=(before['dx']+dx,before['dy']-dy)
        deadline=time.monotonic()+12
        count=len(before['emitted']);flushes=0
        self.q.execute('input-send-event',dict(events=events))
        self.events.append(dict(elapsed=time.monotonic()-self.start,kind='requested',events=events,target_emitted_delta=list(target)))
        while True:
            state=ps2_wait_read(self.q,self.path,count,deadline)
            if (state['dx'],state['dy'])==target:break
            need(flushes<32 and time.monotonic()<deadline,'bounded PS/2 accumulated motion completion')
            count=len(state['emitted']);flushes+=1
            zero=[dict(type='rel',data=dict(axis='x',value=0))]
            self.q.execute('input-send-event',dict(events=zero))
            self.events.append(dict(elapsed=time.monotonic()-self.start,kind='zero-relative-sync',events=zero))
        self.events.append(dict(elapsed=time.monotonic()-self.start,kind='guest-read-complete',emitted_packets=len(state['emitted']),read_bytes=len(state['read_bytes']),emitted_delta=[state['dx'],state['dy']],flushes=flushes))
        return state

class GDB:
    """Minimal owned-QEMU RSP client: two one-shot production breakpoints."""
    def __init__(self,path):
        self.s=socket.socket(socket.AF_UNIX);self.s.settimeout(10);self.s.connect(str(path))
    def send(self,text):
        b=text.encode();self.s.sendall(b'$'+b+b'#'+('%02x'%(sum(b)&255)).encode())
        need(self.s.recv(1)==b'+','GDB request ACK')
    def receive(self):
        need(self.s.recv(1)==b'$','GDB packet start');b=b''
        while True:
            c=self.s.recv(1);need(c,'GDB connection live')
            if c==b'#':break
            b+=c
        checksum=self.s.recv(2);need(int(checksum,16)==sum(b)&255,'GDB checksum')
        self.s.sendall(b'+');return b.decode()
    def command(self,text):
        self.send(text);return self.receive()

def execution_proof(q,folder,symbols,trace):
    q.execute('stop');g=GDB(folder/'gdb')
    rows=[]
    try:
        need(g.command('?').startswith(('T','S')),'stopped owned target')
        for name in ('pointer_idle','pointer_decode'):
            addr=symbols[name]
            need(g.command('Z0,%x,1'%addr)=='OK','install one-shot breakpoint')
            g.send('c')
            if name=='pointer_decode':
                q.execute('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':0}}]})
            stop=g.receive();need(stop.startswith('T05') or stop.startswith('S05'),'actual breakpoint trap')
            regs=q.execute('human-monitor-command',{'command-line':'info registers'})
            match=re.search(r'RIP=([0-9a-fA-F]+)',regs)
            need(match and int(match[1],16)==addr,'native RIP equals Rust symbol entry')
            need(g.command('z0,%x,1'%addr)=='OK','remove one-shot breakpoint')
            rows.append({'symbol':name,'RIP':hex(addr),'stop':stop})
        g.send('c')
        # Do not await a stop: normal guest execution must remain running.
        end=time.monotonic()+10
        while not ps2_trace(trace)['drained']:
            need(time.monotonic()<end,'proof packet drained');time.sleep(.005)
    finally:g.s.close()
    (folder/'rust-execution.json').write_text(json.dumps(rows,indent=2)+'\n')
    return rows

def boot(out,fw,mode,iso,symbols,disk):
    folder=out/(fw+'-'+mode);folder.mkdir();serial=folder/'serial.log';sock=folder/'qmp'
    events=[];shots={};oracles={};started=time.monotonic()
    argv=['qemu-system-x86_64','-machine','q35','-accel','tcg','-m','128M','-cdrom',str(iso),
          '-boot','d','-display','none','-nic','none','-vga','none',
          '-device','VGA,edid=on,xres=1024,yres=768','-serial','file:'+str(serial),
          '-qmp','unix:'+str(sock)+',server=on,wait=off','-no-reboot','-no-shutdown',
          '-drive','file='+str(disk)+',if=none,id=vs0,format=raw,readonly=on',
          '-device','virtio-blk-pci,drive=vs0,disable-legacy=on',
          '-trace','file='+str(folder/'ps2.trace'),'-gdb','unix:'+str(folder/'gdb')+',server=on,wait=off']
    if fw=='uefi':
        variables=folder/'OVMF_VARS_4M.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        argv+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
               '-drive','if=pflash,format=raw,unit=1,file='+str(variables)]
    (folder/'command.json').write_text(json.dumps(argv,indent=2)+'\n')
    result={'passed':False,'firmware':fw,'mode':mode};q=None
    with (folder/'qemu.log').open('x') as log:
        process=subprocess.Popen(argv,cwd=REPO,stdout=log,stderr=log)
        try:
            def data():
                need(process.poll() is None,'owned QEMU exited')
                b=serial.read_bytes() if serial.exists() else b''
                need(not any(x in b for x in (b'panic:',b'boot: check failed',b'boot: FAILED',
                     b'pointer-probe: FAIL',b'gui-probe: FAIL',b'native: vsh FAIL',b'gui: FAIL')),'native failure')
                return b
            def wait(predicate,seconds=20):
                end=time.monotonic()+seconds
                while time.monotonic()<end:
                    b=data()
                    if predicate(b):return b
                    time.sleep(.01)
                raise RuntimeError('bounded native marker deadline '+str(folder))
            if mode=='verify':
                wait(lambda b:b'gui-probe: OVERFLOW INPUT WINDOW' in b,45);q=QMP(sock)
                for _ in range(40):q.tap('a')
                wait(lambda b:b'pointer-probe: OVERFLOW WINDOW' in b)
                for i in range(48):
                    q.execute('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':1 if i%2==0 else -1}}]})
                    time.sleep(.025)
                wait(lambda b:b'gui-demo: PRESENTED;' in b)
                s=capture(q,folder,'checkerboard-regression-initial');shots['checkerboard-initial']=s
                G['verify_pixels'](Path(s['path']),0)
                q.tap('a');wait(lambda b:b'gui-demo: KEY a PRESENTED' in b)
                s=capture(q,folder,'checkerboard-regression-a');shots['checkerboard-a']=s
                G['verify_pixels'](Path(s['path']),1)
                q.tap('b');wait(lambda b:b'gui-demo: KEY b PRESENTED' in b);q.tap('q')
            wait(lambda b:b'pointer-demo: READY;' in b,45)
            if q is None:q=QMP(sock)
            tracker=PS2Motion(q,folder/'ps2.trace',events,started)
            if mode=='normal':result['native_Rust_calls']=execution_proof(q,folder,symbols,folder/'ps2.trace')
            def shot(name,x,y,color=None):
                s=capture(q,folder,name);shots[name]=s
                w,h,pixel=rgb(Path(s['path']));need((w,h)==(1024,768),'exact native framebuffer')
                need(pixel(x,y)==0x101a2a,'exact tip '+name)
                if color is not None:oracles[name]=pointer_pixels(Path(s['path']),x,y,color)
            def mouse(dx=0,dy=0,down=None):
                e=[]
                if dx:e.append({'type':'rel','data':{'axis':'x','value':dx}})
                if dy:e.append({'type':'rel','data':{'axis':'y','value':dy}})
                if down is not None:e.append({'type':'btn','data':{'button':'left','down':down}})
                tracker.send(e,dx,dy)
            shot('initial',512,390,0x409ddd)
            before=len(data());mouse(12,-8)
            wait(lambda b:b'x=140 y=72 down=0 inside=1' in b[before:]);shot('movement',524,382,0x409ddd)
            mouse(down=True);wait(lambda b:b'pointer-demo: LEFT DOWN' in b);shot('press',524,382,0xe8b34e)
            mouse(down=False);wait(lambda b:b'pointer-demo: LEFT UP' in b);shot('click',524,382,0x42c497)
            for name,dx,dy,x,y in [('top-left',-3000,-3000,0,0),('top-right',3000,0,1023,0),
                                  ('bottom-right',0,3000,1023,767),('bottom-left',-3000,0,0,767)]:
                mouse(dx,dy);shot(name,x,y)
            # Reposition from the bottom-left to the original surface center.
            mouse(512,-377);before=len(data());mouse(1,0);wait(lambda b:b'inside=1' in b[before:])
            for _ in range(4):mouse(down=True);mouse(down=False)
            q.tap('a');wait(lambda b:b'pointer-demo: KEY a' in b)
            q.tap('b');wait(lambda b:b'pointer-demo: KEY b' in b)
            mouse(down=True);shot('final',513,390)
            q.tap('q');first=wait(lambda b:b'user[vsh]: VOS> ' in b)
            need(b'pointer-demo: RELEASED' in first and b'gui: PASS; kernel-CR3 reclamation, all resources restored' in first,'GUI reclamation')
            q.execute('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':False}}]})
            def prompts(b):return len(re.findall(rb'(?m)^user\[vsh\]: (VOS>[^\r\n]* )\r?$',b))
            def text(s):
                for c in s:q.tap({' ':'spc','/':'slash','.':'dot','-':'minus'}.get(c,c));time.sleep(.01)
            def command(s,expected):
                before=data();n=prompts(before);text(s);q.tap('ret');b=wait(lambda b:prompts(b)>n)
                need(expected.encode() in b[len(before):],'shell response '+s)
            command('apps','hello');command('hello','Hello from VOS!')
            before=data();n=prompts(before);text('rixie');q.tap('ret')
            wait(lambda b:b.count(b'user[rixie]: Rixie> ')>before.count(b'user[rixie]: Rixie> '))
            text('about');q.tap('ret');wait(lambda b:b'deterministic prototype' in b[len(before):])
            text('exit');q.tap('ret');wait(lambda b:prompts(b)>n)
            command('echo input restored','input restored')
            need(b'user[vsh]: q\r\n' not in data(),'pointer-demo key did not leak into shell')
            command('pwd','/')
            text('exit');q.tap('ret');final=wait(lambda b:b'native: vsh PASS, process reclaimed' in b)
            if mode=='verify':
                need(all(('gui-verify: case %d PASS'%i).encode() in final for i in range(16)),'sixteen native cases')
                need(b'gui-bootstrap: 18 repeated load failures PASS' in final,'bootstrap worker reclamation')
                need(b'pointer-probe: overflow snapshot PASS' in final,'real native pointer overflow recovery')
            result.update(passed=True,screenshots=shots,pixels=oracles,exact_corners=True,
                          keyboard_mouse=True,held_exit_cleanup=True,shell_hello_rixie=True,
                          native_cases=16 if mode=='verify' else 0,elapsed_seconds=time.monotonic()-started)
        except Exception as error:result['error']=str(error);raise
        finally:
            if q:q.close()
            process.terminate()
            try:process.wait(timeout=5)
            except subprocess.TimeoutExpired:process.kill();process.wait()
            (folder/'events.json').write_text(json.dumps(events,indent=2)+'\n')
            result['serial_sha256']=sha(serial) if serial.exists() else None
            (folder/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    return result

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--normal-kernel',type=Path,required=True)
    ap.add_argument('--verify-kernel',type=Path,required=True)
    ap.add_argument('--apps-manifest',type=Path,required=True,help='application name: {path, sha256}')
    ap.add_argument('--efi',type=Path,required=True)
    ap.add_argument('--disk',type=Path,required=True,help='accepted read-only ext2 disk image')
    ap.add_argument('--out',type=Path,required=True)
    ap.add_argument('--mode',choices=['normal','verify','both'],default='both')
    args=ap.parse_args();out=args.out.resolve();disk=args.disk.resolve()
    need(not out.exists() and out.is_relative_to(REPO/'build'),'fresh isolated output')
    need(disk.is_file() and sha(disk)==DISK_SHA256,'accepted read-only disk')
    out.mkdir(parents=True)
    apps=json.loads(args.apps_manifest.read_text());all_apps={}
    for name,row in apps.items():
        need(name in ('vsh','hello','rixie','gui-demo','gui-probe','pointer-demo','pointer-probe'),'known bounded application')
        source=Path(row['path']);need(source.is_file() and sha(source)==row['sha256'],'verified retained ELF '+name)
        all_apps[name]=source
    report={'passed':False,'physical_hardware':False,'cases':[]}
    try:
        tmp=out/'producer-tmp';tmp.mkdir()
        modes=['normal','verify'] if args.mode=='both' else [args.mode]
        for mode in modes:
            kernel=(args.normal_kernel if mode=='normal' else args.verify_kernel).resolve()
            syms=subprocess.check_output(['nm','-S','--defined-only',str(kernel)],text=True)
            symbols={n:int(next(l.split()[0] for l in syms.splitlines() if l.endswith(' '+n)),16) for n in ('pointer_decode','pointer_idle')}
            # GUI verification cases exist only in a -DVOS_GUI_VERIFY kernel.
            verify_only=any(l.endswith(' gui_test_alloc_budget') for l in syms.splitlines())
            need(verify_only==(mode=='verify'),'kernel built with matching VOS_GUI_VERIFY for '+mode)
            for fw,modules in [('bios',Path('/usr/lib/grub/i386-pc')),('uefi',args.efi)]:
                names=['vsh','hello','rixie','gui-demo','pointer-demo']+(['gui-probe','pointer-probe'] if mode=='verify' else [])
                cfg=out/(mode+'-'+fw+'.cfg')
                cfg.write_text(((REPO/'arch/x86_64/grub-uefi-head.cfg').read_text() if fw=='uefi' else '')+
                    'set timeout=0\nset default=0\nmenuentry "VOS R2-B pointer" {\nset gfxpayload=1024x768x32,auto\n'+
                    'multiboot2 /boot/vos.bin vos.apps=1 vos.desktop=1 vos.gui=1 vos.pointer=1'+
                    (' vos.gui-verify=1' if mode=='verify' else '')+'\n'+
                    ''.join('module2 /boot/'+n+'.elf '+n+'\n' for n in names)+'boot\n}\n')
                expected={'/boot/vos.bin':kernel,'/boot/grub/grub.cfg':cfg,
                          **{'/boot/'+n+'.elf':all_apps[n] for n in names}}
                iso=out/(mode+'-'+fw+'.iso')
                V.run(['grub-mkrescue','-d',modules,'-o',iso,'-graft-points',
                       *[n+'='+str(p) for n,p in expected.items()]],out/(mode+'-'+fw+'-producer.log'),
                      dict(os.environ,TMPDIR=str(tmp),PYTHONDONTWRITEBYTECODE='1'))
                V.validate(iso,expected,fw,out/(mode+'-'+fw+'-image-validation.log'))
                report['cases'].append(boot(out,fw,mode,iso,symbols,disk))
                print(mode,fw,'PASS',flush=True)
        report['passed']=True
    finally:(out/'result.json').write_text(json.dumps(report,indent=2)+'\n')

if __name__=='__main__':main()
