#!/usr/bin/env python3
"""One guarded N6.3 attempt. Native PS/2 input; normal media has no test deadline."""
import argparse,hashlib,json,os,re,resource,runpy,shlex,shutil,struct,subprocess,time,zlib
from pathlib import Path
REPO=Path(__file__).resolve().parents[1]
G=runpy.run_path(str(REPO/'scripts/test-gui.py'),run_name='pointer_gui_helpers')
run,QMP,capture,sha,need,validate_image=(G[n] for n in ('run','QMP','capture','sha','need','validate_image'))
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
def boot(out,fw,iso,disk,normal=False,full_pointer=False):
    folder=out/(('interactive-' if normal else '')+fw);folder.mkdir()
    serial=folder/'serial.log';socket=folder/'qmp';events=[]
    argv=['qemu-system-x86_64','-machine','q35','-accel','tcg','-m','128M','-cdrom',str(iso),
          '-boot','d','-display','none','-nic','none','-vga','none','-device','VGA,edid=on,xres=1024,yres=768',
          '-serial','file:'+str(serial),'-qmp','unix:'+str(socket)+',server=on,wait=off','-no-reboot','-no-shutdown',
          '-drive','file='+str(disk)+',if=none,id=vs0,format=raw,readonly=on',
          '-device','virtio-blk-pci,drive=vs0,disable-legacy=on']
    if fw=='uefi':
        variables=folder/'OVMF_VARS_4M.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',variables)
        argv+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
               '-drive','if=pflash,format=raw,unit=1,file='+str(variables)]
    (folder/'command.json').write_text(json.dumps(argv,indent=2)+'\n')
    argv+=['-trace','file='+str(folder/'ps2.trace')]
    proc=q=tracker=None;start=time.monotonic();shots={};oracles={}
    with (folder/'qemu.log').open('x') as log:
        proc=subprocess.Popen(argv,cwd=REPO,stdout=log,stderr=subprocess.STDOUT)
        try:
            def data():
                need(proc.poll() is None,'QEMU exited')
                b=serial.read_bytes() if serial.exists() else b''
                need(not any(x in b for x in (b'panic:',b'boot: check failed',b'boot: FAILED',b'pointer-probe: FAIL',b'gui-probe: FAIL',b'native: vsh FAIL',b'gui: FAIL')),'native failure')
                return b
            def wait(predicate,seconds=12):
                end=time.monotonic()+seconds
                while True:
                    b=data()
                    if predicate(b):return b
                    need(time.monotonic()<end,'bounded native marker deadline');time.sleep(.02)
            def mouse(dx=0,dy=0,down=None,expect_packet=True):
                event=[]
                if dx:event.append(dict(type='rel',data=dict(axis='x',value=dx)))
                if dy:event.append(dict(type='rel',data=dict(axis='y',value=dy)))
                if down is not None:event.append(dict(type='btn',data=dict(button='left',down=down)))
                if not event:return
                if tracker is not None and expect_packet:tracker.send(event,dx,dy)
                else:
                    q.execute('input-send-event',dict(events=event));events.append(dict(elapsed=time.monotonic()-start,events=event))
                    if tracker is None:time.sleep(.025)
            if not normal:
                wait(lambda d:b'gui-probe: OVERFLOW INPUT WINDOW' in d,40);q=QMP(socket)
                for _ in range(40):q.tap('a')
                wait(lambda d:b'pointer-probe: OVERFLOW WINDOW' in d,15)
                for i in range(48):mouse(1 if i%2==0 else -1)
                wait(lambda d:b'gui-demo: PRESENTED;' in d,15)
                regression=capture(q,folder,'checkerboard-regression-initial')
                G['verify_pixels'](Path(regression['path']),0)
                q.tap('a');wait(lambda d:b'gui-demo: KEY a PRESENTED' in d)
                regression=capture(q,folder,'checkerboard-regression-a');G['verify_pixels'](Path(regression['path']),1)
                q.tap('b');wait(lambda d:b'gui-demo: KEY b PRESENTED' in d)
                q.tap('q')
            wait(lambda d:b'pointer-demo: READY;' in d,40 if normal else 15)
            if q is None:q=QMP(socket)
            tracker=PS2Motion(q,folder/'ps2.trace',events,start)
            # Preview artifacts remain separate from final evidence selections.
            shots['first-cursor']=capture(q,folder,'preview-first-cursor')
            oracles['first-cursor']=pointer_pixels(Path(shots['first-cursor']['path']),512,390,0x409ddd)
            print('N63_VISUAL '+json.dumps(dict(firmware=fw,checkpoint='first-cursor',path=shots['first-cursor']['path'],works='cursor rendered',remaining='movement/click/cleanup')),flush=True)
            before=data();mouse(12,-8)
            wait(lambda d:b'x=140 y=72 down=0 inside=1' in d[len(before):])
            shots['movement']=capture(q,folder,'preview-movement')
            oracles['movement']=pointer_pixels(Path(shots['movement']['path']),524,382,0x409ddd)
            print('N63_VISUAL '+json.dumps(dict(firmware=fw,checkpoint='movement',path=shots['movement']['path'],works='actual PS/2 movement and pixel location',remaining='click/cleanup')),flush=True)
            mouse(down=True);wait(lambda d:b'pointer-demo: LEFT DOWN' in d)
            shots['press']=capture(q,folder,'preview-left-press')
            oracles['press']=pointer_pixels(Path(shots['press']['path']),524,382,0xe8b34e)
            mouse(down=False);wait(lambda d:b'pointer-demo: LEFT UP' in d)
            shots['click']=capture(q,folder,'preview-click-response')
            oracles['click']=pointer_pixels(Path(shots['click']['path']),524,382,0x42c497)
            print('N63_VISUAL '+json.dumps(dict(firmware=fw,checkpoint='click',path=shots['click']['path'],works='actual left press/release changes Rust panel',remaining='bounds/cleanup/remaining firmware')),flush=True)
            if not normal or full_pointer:
                # Large relative events are split by QEMU's PS/2 device. Allow
                # the bounded hardware polling stream to drain before each shot.
                for name,dx,dy,x,y in [('top-left',-3000,-3000,0,0),('top-right',3000,0,1023,0),
                                      ('bottom-right',0,3000,1023,767),('bottom-left',-3000,0,0,767)]:
                    mouse(dx,dy)
                    shot=capture(q,folder,'edge-'+name);w,h,pixel=rgb(Path(shot['path']))
                    need(pixel(x,y)==0x101a2a,'clipped cursor at '+name);shots[name]=shot
                mouse(512,-377)
                # Reposition from the bottom-left to the original surface center.
                before=data();mouse(1,0);wait(lambda d:b'inside=1' in d[len(before):])
                for _ in range(4):mouse(down=True);mouse(down=False)
            q.tap('a');wait(lambda d:b'pointer-demo: KEY a' in d)
            q.tap('b');wait(lambda d:b'pointer-demo: KEY b' in d)
            # State is observed via actual decoded packets, not keyboard synthesis.
            # Exit while held checks authority revocation and stale-button cleanup.
            mouse(down=True)
            shots['final']=capture(q,folder,'final-pointer')
            q.tap('q')
            first=wait(lambda d:b'user[vsh]: VOS> ' in d,15)
            need(b'pointer-demo: RELEASED' in first and b'gui: PASS; kernel-CR3 reclamation, all resources restored' in first,'native GUI teardown')
            mouse(down=False,expect_packet=False);time.sleep(.05)
            def prompts(b):return re.findall(rb'(?m)^user\[vsh\]: (VOS>[^\r\n]* )\r?$',b)
            def text(s):
                for c in s:q.tap({' ':'spc','/':'slash','.':'dot','-':'minus'}.get(c,c))
            def command(s,expected=''):
                before=data();n=len(prompts(before));text(s);q.tap('ret')
                after=wait(lambda d:len(prompts(d))>n)
                need(expected.encode() in after[len(before):],'shell '+s)
            command('apps','hello');command('hello','Hello from VOS!')
            before=data();n=len(prompts(before));text('rixie');q.tap('ret')
            wait(lambda d:d.count(b'user[rixie]: Rixie> ')>before.count(b'user[rixie]: Rixie> '))
            text('about');q.tap('ret');wait(lambda d:b'deterministic prototype' in d[len(before):])
            text('exit');q.tap('ret');wait(lambda d:len(prompts(d))>n)
            command('echo input restored','input restored')
            need(b'user[vsh]: q\r\n' not in data(),'pointer-demo key did not leak into shell')
            command('pwd','/')
            text('exit');q.tap('ret');final=wait(lambda d:b'native: vsh PASS, process reclaimed' in d)
            if not normal:
                need(all(('gui-verify: case %d PASS'%i).encode() in final for i in range(16)),'sixteen actual GUI/native pointer cases')
                need(b'gui-bootstrap: 18 repeated load failures PASS' in final,'preserved bootstrap reclamation')
                need(b'pointer-probe: overflow snapshot PASS' in final,'native pointer overflow recovery')
            return dict(passed=True,normal_media=normal,firmware=fw,screenshots=shots,pixel_oracles=oracles,
                        real_PS2_motion_and_click=True,checkerboard_regression=not normal,initial_ELF_failures=18 if not normal else 0,
                        native_cases=16 if not normal else 0,shell_hello_rixie=True,held_exit_cleanup=True,elapsed_seconds=time.monotonic()-start)
        finally:
            (folder/'injected-events.json').write_text(json.dumps(events,indent=2)+'\n')
            if q:q.close()
            if proc and proc.poll() is None:proc.terminate()
            if proc:
                try:proc.wait(timeout=3)
                except subprocess.TimeoutExpired:proc.kill();proc.wait()

def archive_members(path):
    rows={};names=b''
    with Path(path).open('rb') as f:
        need(f.read(8)==b'!<arch>\n','regular Rust/native archive')
        while True:
            head=f.read(60)
            if not head:break
            need(len(head)==60 and head[58:]==b'\x60\n','archive member header')
            name=head[:16].strip();length=int(head[48:58]);data=f.read(length)
            need(len(data)==length,'archive member extent')
            if length%2:f.read(1)
            if name==b'//':names=data;continue
            if name.startswith(b'/') and name[1:].isdigit():
                off=int(name[1:]);name=names[off:names.index(b'/\n',off)]
            else:name=name.rstrip(b'/')
            if not name:continue
            n=name.decode();need(n not in rows,'unique archive member')
            rows[n]=dict(size=length,sha256=hashlib.sha256(data).hexdigest(),
                         ELF64_x86_64=data[:6]==b'\x7fELF\x02\x01' and len(data)>=20 and struct.unpack_from('<H',data,18)[0]==62)
    return rows

def compact_recipe(out,phase):
    """Admitted <=8 MiB proof, preceding admission of the full workload."""
    rustc=os.environ.get('RUSTC','/home/veasnawt/.rustup/toolchains/nightly-2026-09-03-x86_64-unknown-linux-gnu/bin/rustc')
    os.environ['RUSTC']=rustc  # Process-scoped, hash-checked accepted toolchain.
    need(sha(Path(rustc))=='369640757edac99bfa1358946cfc2861be80ec39785416a46c06a990840facfb','selected Rust compiler identity')
    libdir=Path(subprocess.check_output([rustc,'--print','target-libdir','--target','x86_64-unknown-none'],text=True).strip())
    pinned={'libcore-41cea3d940a8abf0.rlib':'39a4793ea97ae36360733c197c00d55044da7e3c9404e1d55edd95f44f07cf9c',
            'libcompiler_builtins-1670e0eec149624a.rlib':'a4c78cd4cd9b9ee46aacdbce544a32b20d9b0187fd5af7c96f7cbfa0ce025fc8'}
    system={}
    for n,h in pinned.items():
        p=libdir/n;need(sha(p)==h,'immutable matching target runtime '+n);system.update(archive_members(p))
    olduser=REPO/'build/n6-gui-20261025T083036Z/image-recipe-recovery-1/image/user'
    frozen=json.loads((phase/'N62-FROZEN.json').read_text())
    old={}
    for n in ('gui-demo','gui-probe'):
        p=olduser/n/'main.a';need(sha(p)==frozen[str(p)],'accepted static archive identity')
        old[n]=archive_members(p)
    shared={n:v for n,v in old['gui-demo'].items() if n.startswith(('core-','compiler_builtins-'))}
    need(len(shared)==390 and sum(v['size'] for v in shared.values())==10760272,'accepted common runtime extent')
    need(all(old['gui-probe'].get(n)==v and system.get(n)==v and v['ELF64_x86_64'] for n,v in shared.items()),'byte-identical runtime objects/configurations')
    (out/'runtime-members.json').write_text(json.dumps(shared,indent=2)+'\n')
    user=out/'user';targets=[str(user/(n+'.elf')) for n in ('pointer-demo','pointer-probe')]
    command=['make','--no-print-directory','ARCH=x86_64','USER_BUILD='+str(user),'RUSTC='+rustc,*targets]
    dry=subprocess.run(command[:2]+['-n']+command[2:],cwd=REPO,capture_output=True,text=True)
    (out/'compact-dry-run.log').write_text(dry.stdout+dry.stderr);need(dry.returncode==0,'compact Make dry-run')
    commands=[]
    for line in dry.stdout.splitlines():
        try:words=shlex.split(line)
        except ValueError:continue
        if not words:continue
        if '-o' in words:
            p=Path(words[words.index('-o')+1])
            need(p.is_absolute() and p.resolve().is_relative_to(out),'admitted compiler output')
        if words[0]==rustc:
            need('--crate-type=staticlib' not in words,'no per-app upstream archive replication')
            commands.append(words)
    need(sum('--crate-type=bin' in a for a in commands)==2,'two compiler-driven native ELF links')
    run(command,out/'compact-build.log',90)
    native=user/'rust/libvos_gui_native.a';native_hash=sha(native)
    need(len(archive_members(native))==9 and native.stat().st_size<65536,'small shared native CRT archive')
    run(['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Iarch/x86_64',
         'scripts/elfcheck.c','arch/x86_64/elf.c','-o',str(out/'compact-elfcheck')],out/'compact-elfcheck-build.log')
    apps={};repeat=out/'repro';repeat.mkdir()
    for n in ('pointer-demo','pointer-probe'):
        original=user/(n+'.elf')
        run(['scripts/check-user-elf.sh',str(original)],out/(n+'-elf.log'))
        need('VOS ABI 5' in (out/(n+'-elf.log')).read_text(),'ABI5 application '+n)
        run([str(out/'compact-elfcheck'),str(original)],out/(n+'-kernel-validator.log'))
        compile_=[a for a in commands if 'user/'+n+'/main.rs' in a]
        need(len(compile_)==1,'unique native compiler command '+n)
        argv=compile_[0][:]
        for i,v in enumerate(argv):
            if v.startswith('--emit=link,dep-info='):argv[i]='--emit=link,dep-info='+str(repeat/(n+'.d'))
            if v.startswith('link-arg=-Map='):argv[i]='link-arg=-Map='+str(repeat/(n+'.map'))
        argv[argv.index('-o')+1]=str(repeat/(n+'.elf'))
        run(argv,out/(n+'-repro-build.log'),60)
        need(original.read_bytes()==(repeat/(n+'.elf')).read_bytes(),'bit-identical complete ELF rebuild '+n)
        need(sha(native)==native_hash,'shared native CRT remained immutable')
        maptext=(user/(n+'.map')).read_text()
        need('libvos_gui_native.a(crt0.o)' in maptext and 'abi_note_v5.o' in maptext,'explicit VOS entry and ABI note retained')
        selected=set()
        for lib in pinned:
            selected.update(re.findall(re.escape(lib)+r'\(([^)\n]+)\)',maptext))
        need(all(member in shared for member in selected),'only verified runtime object content selected')
        apps[n]=dict(path=str(original),sha256=sha(original),ELF_bytes=original.stat().st_size,
                     bit_identical_rebuild=True,independent_shell_and_kernel_validators=True,selected_verified_runtime_members=sorted(selected))
    # Negative ABI control uses a new fixture; neither validator nor the
    # accepted original future-ABI rejection evidence is edited.
    blob=bytearray((user/'pointer-demo.elf').read_bytes());phoff=struct.unpack_from('<Q',blob,32)[0];num=struct.unpack_from('<H',blob,56)[0]
    notes=[]
    for i in range(num):
        at=phoff+i*56
        if struct.unpack_from('<I',blob,at)[0]==4:notes.append(struct.unpack_from('<Q',blob,at+8)[0])
    need(len(notes)==1 and blob[notes[0]+12:notes[0]+16]==b'VOS\0' and struct.unpack_from('<I',blob,notes[0]+16)[0]==5,'exact ABI5 note')
    struct.pack_into('<I',blob,notes[0]+16,6);negative=out/'compact-future-abi.elf';negative.write_bytes(blob)
    rejection=[]
    for label,argv in [('shell',['scripts/check-user-elf.sh',str(negative)]),('kernel',[str(out/'compact-elfcheck'),str(negative)])]:
        proc=subprocess.run(argv,cwd=REPO,capture_output=True,text=True)
        (out/('compact-negative-'+label+'.log')).write_text(proc.stdout+proc.stderr)
        need(proc.returncode==1,'strict future-ABI rejection '+label);rejection.append(label)
    need(all(sha(libdir/n)==h for n,h in pinned.items()),'read-only runtime identities after all links')
    allocation=sum(p.stat().st_blocks*512 for p in [out,*out.rglob('*')] if p.is_file() or p.is_dir())
    need(allocation<=8*1024**2,'bounded compact-recipe proof')
    before=sum((olduser/n/'main.a').stat().st_size for n in ('gui-demo','gui-probe'))
    after=native.stat().st_size+sum(p.stat().st_size for p in (user/'rust').glob('*.rlib'))
    answer=dict(passed=True,linking_driver='rustc, existing GNU ld and VOS linker script',
        shared_runtime_inputs={str(libdir/n):h for n,h in pinned.items()},
        common_objects_proven_identical=390,old_two_static_archives_bytes=before,
        new_intermediate_archive_bytes=after,upstream_runtime_copied_bytes=0,
        proof_allocated_bytes=allocation,applications=apps,negative_validators=rejection,
        shared_native_CRT_archive_sha256=native_hash,ABI_or_kernel_behavior_changed=False,
        existing_application_rules_unchanged=True)
    (out/'compact-recipe-result.json').write_text(json.dumps(answer,indent=2)+'\n')
    return answer

def original_attempt_main():
    ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);args=ap.parse_args()
    out=args.output.resolve();phase=Path(os.environ['N63_ROOT']).resolve();budget=json.loads((phase/'N63-BUDGET.json').read_text())
    need(out==phase/'attempt-1' and not out.exists() and budget['attempt_limit_bytes']==72*1024**2,'one fresh admitted pointer execution')
    out.mkdir();(out/'tmp').mkdir();os.environ['TMPDIR']=str(out/'tmp')
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));resource.setrlimit(resource.RLIMIT_FSIZE,(8*1024**2,32*1024**2))
    result=dict(passed=False,scope='N6.3 PS/2 pointer foundation',physical_hardware_verified=False)
    try:
        names=[n for n in budget['allowed_source_files'] if not n.startswith('docs/') and (REPO/n).is_file()]
        names+=['arch/x86_64/desktop.c','arch/x86_64/font8x16.c','arch/x86_64/exec.c','arch/x86_64/process.c','arch/x86_64/input_console.c',
                'user/rust/vos.rs','user/gui-demo/main.rs','user/vsh/main.rs','user/rixie/main.rs','scripts/test-gui.py','scripts/test-app-handoff.py',
                'scripts/elfcheck.c','arch/x86_64/elf.c','user/lib/user.ld','user/rust/runtime.c','user/lib/crt0.S','user/lib/abi_note_v5.S',
                'user/lib/syscall.S','user/lib/syscall_v1.S','user/lib/syscall_v2.S','user/lib/syscall_v3.S','user/lib/syscall_v4.S']
        result['source_sha256']={n:sha(REPO/n) for n in sorted(set(names))}
        result['compact_recipe']=compact_recipe(out,phase)
        # Both fresh EFI producers choose distinct temporary names. Account
        # their high-water scratch separately; never credit producer deletion.
        provisions=dict(verification_images=20,normal_images=20,producer_scratch=28,
                        payload_staging=5,kernel_and_app_outputs=6,host_evidence=4,contingency=4)
        peak=sum(provisions.values())*1024**2
        result['peak_admission']=dict(passed=peak<=budget['attempt_limit_bytes'],
            conservative_MiB=provisions,required_execution_bytes=peak,
            execution_limit_bytes=budget['attempt_limit_bytes'],
            shortfall_bytes=max(0,peak-budget['attempt_limit_bytes']),
            existing_proof_bytes_included_in_kernel_app_and_host_provisions=True,
            source_forecast_and_finalization_reservations_unchanged=True)
        (out/'full-peak-admission.json').write_text(json.dumps(result['peak_admission'],indent=2)+'\n')
        need(result['peak_admission']['passed'],'compact recipe passed but full conservative execution peak is 87 MiB > unchanged 72 MiB; stop before sanitizer/native execution')
        resource.setrlimit(resource.RLIMIT_FSIZE,(32*1024**2,32*1024**2))

        cc=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Iarch/x86_64','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
        src=(REPO/'arch/x86_64/syscall.c').read_text();debug=src[src.index('void debug_emit('):src.index('/* debug_write')]
        (out/'debug.c').write_text('#include <stdint.h>\n'+debug)
        service=['-DVOS_GUI_HOST','-DVOS_CONSOLE_HOST','tests/host/pointer.c','arch/x86_64/gui.c','arch/x86_64/user_console.c','arch/x86_64/keyboard.c','arch/x86_64/pointer.c','arch/x86_64/input_console.c',str(out/'debug.c')]
        render=['-DPOINTER_RENDER','tests/host/pointer.c','arch/x86_64/desktop.c','arch/x86_64/font8x16.c']
        console=['-DVOS_CONSOLE_HOST','tests/host/user-console-handoff.c','arch/x86_64/user_console.c','arch/x86_64/keyboard.c','arch/x86_64/pointer.c','arch/x86_64/input_console.c',str(out/'debug.c')]
        result['host']={}
        for name,files in [('pointer-service',service),('pointer-renderer',render),('console-regression',console)]:
            run(cc+['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']+files+['-o',str(out/name)],out/(name+'-build.log'),60)
            run([str(out/name)],out/(name+'.log'),60)
            result['host'][name]=(out/(name+'.log')).read_text();need('PASS' in result['host'][name],'actual host '+name)
        rustc=os.environ['RUSTC']
        run([rustc,'--cfg','test','--crate-name','vos','--crate-type=rlib','--edition=2024','user/rust/vos.rs','-o',str(out/'libvos.rlib')],out/'rust-vos.log')
        run([rustc,'--crate-name','vos_gui','--crate-type=rlib','--edition=2024','-L','dependency='+str(out),'--extern','vos='+str(out/'libvos.rlib'),'user/rust/gui.rs','-o',str(out/'libvos_gui.rlib')],out/'rust-gui.log')
        run([rustc,'--test','-C','strip=symbols','--edition=2024','-L','dependency='+str(out),'--extern','vos_gui='+str(out/'libvos_gui.rlib'),'tests/rust/pointer.rs','-o',str(out/'rust-tests')],out/'rust-test-build.log')
        run([str(out/'rust-tests')],out/'rust-tests.log');result['rust']=(out/'rust-tests.log').read_text()
        run(cc+['-O2']+service+['-o',str(out/'pointer-release')],out/'release-build.log')
        times=[]
        for i in range(3):
            t=time.perf_counter_ns();run([str(out/'pointer-release')],out/('release-%d.log'%i));times.append(time.perf_counter_ns()-t)
        result['optimized_host_samples_ns']=times
        common=REPO/'build/app-console-20261025T051241Z/attempt-1';oldphase=REPO/'build/n6-gui-20261025T083036Z'
        old=json.loads((oldphase/'bootstrap-hardening-1/result.json').read_text());previous=old['build']
        need(old['passed'],'accepted N6.2 hardening')
        result['performance_note']='Optimized hosted assertions measured separately from sanitizer and QEMU runs; workload includes new negative tests; no production latency/throughput claim.'

        reused={row['path']:row['sha256'] for row in previous['reused_objects']}
        reused[str(oldphase/'bootstrap-hardening-1/image/kernel/gui_verify.o')]=sha(oldphase/'bootstrap-hardening-1/image/kernel/gui_verify.o')
        apps={n:common/'image/user'/(n+'.elf') for n in ('vsh','hello','rixie')}
        apps.update({n:oldphase/'image-recipe-recovery-1/image/user'/(n+'.elf') for n in ('gui-demo','gui-probe')})
        for n,p in apps.items():need(sha(p)==previous['application_sha256'][n],'accepted native ELF '+n)
        disk=common/'disk.img';disk_hash=sha(disk)
        need(disk_hash=='5ef98c6e0b4dd983dfb7db1a8d73d0175cbb86c358e61f2957c689d164890dcc','accepted read-only disk')
        user=out/'user'
        for n in ('pointer-demo','pointer-probe'):
            apps[n]=user/(n+'.elf')
            need(sha(apps[n])==result['compact_recipe']['applications'][n]['sha256'],'verified compact ELF reused '+n)
        kernel=out/'kernel';kernel.mkdir()
        changed=('pointer','keyboard','user_console','gui','screen','syscall','app_payload','gui_verify')
        compile_flags=previous['compile'][:previous['compile'].index('-c')]
        for n in changed:run(compile_flags+['-c','arch/x86_64/'+n+'.c','-o',str(kernel/(n+'.o'))],out/(n+'-compile.log'))
        link=previous['link'][:];link[link.index('-o')+1]=str(kernel/'vos-verify.bin')
        for i,v in enumerate(link):
            if not v.endswith('.o'):continue
            n=Path(v).stem
            if n in changed:link[i]=str(kernel/(n+'.o'))
            else:need(v in reused and sha(Path(v))==reused[v],'verified reused object '+v)
        link.append(str(kernel/'pointer.o'));run(link,out/'kernel-link.log')
        run(['python3','-I','-B','scripts/check-kernel-layout.py',str(kernel/'vos-verify.bin')],out/'kernel-layout.log')
        # Normal boot uses a separately compiled non-verification launch route
        # and syscall dispatcher. Probe syscalls/ELFs are absent.
        normal_flags=[x for x in compile_flags if x!='-DVOS_GUI_VERIFY']
        for n in ('gui_verify','syscall','gui','app_payload'):run(normal_flags+['-c','arch/x86_64/'+n+'.c','-o',str(kernel/(n+'-normal.o'))],out/(n+'-normal-compile.log'))
        normal_link=[str(kernel/'vos-normal.bin') if v==str(kernel/'vos-verify.bin') else
                     str(kernel/(Path(v).stem+'-normal.o')) if v in tuple(str(kernel/(n+'.o')) for n in ('gui_verify','syscall','gui','app_payload')) else v for v in link]
        run(normal_link,out/'kernel-normal-link.log');run(['python3','-I','-B','scripts/check-kernel-layout.py',str(kernel/'vos-normal.bin')],out/'kernel-normal-layout.log')
        result['build']=dict(compile_flags=compile_flags,link=link,normal_link=normal_link,reused_objects=reused,
                             app_ELF_sha256={n:sha(p) for n,p in apps.items()},disk_sha256=disk_hash)
        # BIOS-only replay retains the accepted GRUB BIOS producer. EFI always
        # uses the established fresh GRUB producer, never imported GPT geometry.
        bios_input=oldphase/'bootstrap-hardening-1/image/kernel/vos.iso'
        need(sha(bios_input)==old['images']['bios']['ISO_sha256'],'accepted BIOS template')
        with bios_input.open('rb') as f:f.seek(512);need(f.read(8)!=b'EFI PART','BIOS input without GPT')
        efi=Path(previous['UEFI'][previous['UEFI'].index('-d')+1])
        cfg_text=(REPO/'arch/x86_64/grub-pointer.cfg').read_text();result['images']={}
        for profile in ('verify','normal'):
            selected=dict(apps)
            if profile=='normal':
                selected.pop('gui-probe');selected.pop('pointer-probe')
            text=cfg_text
            if profile=='verify':text=text.replace('vos.pointer=1','vos.pointer=1 vos.gui-verify=1').replace('    boot\n','    module2 /boot/gui-probe.elf gui-probe\n    module2 /boot/pointer-probe.elf pointer-probe\n    boot\n')
            cfg=out/(profile+'.cfg');cfg.write_text(text)
            bin_=kernel/('vos-'+profile+'.bin')
            bios=out/(profile+'-bios.iso');argv=['xorriso','-indev',str(bios_input),'-outdev',str(bios),'-boot_image','any','replay',
                '-map',str(bin_),'/boot/vos.bin','-map',str(cfg),'/boot/grub/grub.cfg']
            for n,p in selected.items():argv+=['-map',str(p),'/boot/'+n+'.elf']
            if profile=='normal':argv+=['-rm','/boot/gui-probe.elf','--']
            run(argv+['-commit','-end'],out/(profile+'-bios-build.log'),90)
            stage=out/(profile+'-stage');(stage/'boot/grub').mkdir(parents=True)
            # Hard links are phase-owned aliases: counted conservatively by
            # each path, never used to reduce historical storage charges.
            os.link(bin_,stage/'boot/vos.bin')
            for n,p in selected.items():shutil.copyfile(p,stage/'boot'/(n+'.elf'))
            efi_cfg=stage/'boot/grub/grub.cfg'
            efi_cfg.write_bytes((REPO/'arch/x86_64/grub-uefi-head.cfg').read_bytes()+cfg.read_bytes())
            image=out/(profile+'-uefi.iso')
            run(['grub-mkrescue','-d',str(efi),'-o',str(image),str(stage)],out/(profile+'-uefi-build.log'),120)
            expected={'/boot/vos.bin':bin_,'/boot/grub/grub.cfg':cfg}
            expected.update({'/boot/'+n+'.elf':p for n,p in selected.items()})
            for fw,iso in [('bios',bios),('uefi',image)]:
                folder=out/('validate-'+profile+'-'+fw);folder.mkdir()
                current=dict(expected)
                if fw=='uefi':current['/boot/grub/grub.cfg']=efi_cfg
                result['images'][profile+'-'+fw]=validate_image(iso,current,fw=='uefi',folder)
        result['firmware']={}
        for fw in ('bios','uefi'):
            result['firmware'][fw]=boot(out,fw,out/('verify-'+fw+'.iso'),disk)
            result['firmware']['normal-'+fw]=boot(out,fw,out/('normal-'+fw+'.iso'),disk,True)
        need(sha(disk)==disk_hash,'disk preserved after runtime')
        need(all(sha(REPO/n)==v for n,v in result['source_sha256'].items()),'tested source identities')
        result.update(passed=True,one_input_worker=True,mouse_packet_bytes=3,event_bytes=16,
                      kernel_private_cursor_underlay_bytes=384,queue_capacity=16,maximum_surface_bytes=163840,
                      unsupported=['USB','wheel','additional buttons','hotplug','multiple surfaces','window manager','physical-hardware validation'])
    except BaseException as e:
        result['error']=str(e);raise
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        entries=[dict(path=str(p.relative_to(out)),sha256=sha(p),size=p.stat().st_size) for p in sorted(out.rglob('*')) if p.is_file()]
        (out/'manifest.json').write_text(json.dumps(entries,indent=2)+'\n')
        print(json.dumps(result),flush=True)


def snapshot_tree(folder):
    folder=Path(folder);answer={}
    for p in sorted(folder.rglob('*')):
        need(not p.is_symlink(),'no symlink in shared producer inputs')
        if p.is_file():answer[str(p.relative_to(folder))]=dict(size=p.stat().st_size,sha256=sha(p))
    return answer

def pair_commands(args,c):
    """Only two exact argv tokens may differ; no GPT replay or flag changes."""
    need(args[:2]==['-as','mkisofs'],'expected GRUB mkisofs command')
    need(args.count('-o')==1,'exactly one image output')
    i=args.index('-o');need(i+1<len(args) and args[i+1]==c['verify_iso'],'admitted primary output')
    need(args.count(c['verify_stage'])==1,'exactly one admitted payload stage')
    need(c['normal_stage'] not in args and c['normal_iso'] not in args,'no repeated secondary inputs')
    root=Path(c['output_root']).resolve()
    for n in ('verify_iso','normal_iso','verify_stage','normal_stage'):
        p=Path(c[n]);need(p.is_absolute() and p.resolve().is_relative_to(root),'paired output/input root')
    need(not Path(c['verify_iso']).exists() and not Path(c['normal_iso']).exists(),'fresh image outputs')
    other=args[:];other[i+1]=c['normal_iso'];other[other.index(c['verify_stage'])]=c['normal_stage']
    need([j for j in range(len(args)) if args[j]!=other[j]]==sorted([i+1,args.index(c['verify_stage'])]),'only output and payload substitutions')
    return [args[:],other]

def xorriso_pair(args,contract_path):
    c=json.loads(Path(contract_path).read_text())
    real=c['xorriso'];need(sha(Path(real))==c['xorriso_sha256'],'pinned xorriso producer')
    probes=[['--version'],['-version'],['--help'],['-help'],
            ['-as','mkisofs','--help'],['-as','mkisofs','-help']]
    if args in probes:return subprocess.run([real,*args],cwd=REPO).returncode
    commands=pair_commands(args,c)
    need(not Path(c['receipt']).exists(),'one shared-input producer invocation')
    before=snapshot_tree(c['producer_tmp'])
    need(before,'actual private GRUB assets prepared')
    # The stock GRUB parent waits for this adapter. Its private inputs stay alive
    # during both original xorriso invocations and are never moved or rewritten.
    for argv in commands:
        need(snapshot_tree(c['producer_tmp'])==before,'shared GRUB inputs immutable')
        need(not Path(argv[argv.index('-o')+1]).exists(),'fresh output immediately before assembly')
        subprocess.run([real,*argv],cwd=REPO,check=True)
        need(snapshot_tree(c['producer_tmp'])==before,'xorriso did not mutate shared inputs')
    Path(c['receipt']).write_text(json.dumps(dict(passed=True,commands=commands,private_input_manifest=before,
        changed_tokens_only=['output','payload_stage'],scratch_trees_created_by_adapter=0,
        cleanup_performed_by_adapter=False,deletion_credits_taken=False),indent=2)+'\n')
    return 0

def test_pair_adapter(out):
    folder=out/'adapter-tests';folder.mkdir()
    c=dict(output_root=str(folder),verify_iso=str(folder/'verify.iso'),normal_iso=str(folder/'normal.iso'),
           verify_stage=str(folder/'verify-stage'),normal_stage=str(folder/'normal-stage'))
    a=['-as','mkisofs','-r','-V','ISOIMAGE','-o',c['verify_iso'],'--efi-boot','efi.img',
       '-efi-boot-part','--efi-boot-image',str(folder/'grub.fixture'),c['verify_stage']]
    positive=pair_commands(a,c)
    need(positive[0]==a and positive[1][-1]==c['normal_stage'] and positive[1][6]==c['normal_iso'],'fresh paired commands')
    cases=[dict(case='only-output-and-stage-change',passed=True)]
    bad=[('duplicate-output',a+['-o',c['verify_iso']]),
         ('missing-output',a[:5]+a[7:]),('wrong-output',a[:6]+[str(folder/'wrong.iso')]+a[7:]),
         ('missing-stage',a[:-1]),('duplicate-stage',a+[c['verify_stage']]),
         ('unexpected-command',['-indev','old.iso',*a]),
         ('secondary-already-present',a+[c['normal_stage']])]
    for label,args in bad:
        rejected=False
        try:pair_commands(args,c)
        except RuntimeError:rejected=True
        need(rejected,'adapter rejects '+label);cases.append(dict(case=label,passed=True))
    wrong=dict(c,normal_iso='/outside-allowance/normal.iso');rejected=False
    try:pair_commands(a,wrong)
    except RuntimeError:rejected=True
    need(rejected,'adapter rejects outside root');cases.append(dict(case='outside-output-root',passed=True))
    (folder/'verify.iso').write_bytes(b'preserved fixture');rejected=False
    try:pair_commands(a,c)
    except RuntimeError:rejected=True
    need(rejected and (folder/'verify.iso').read_bytes()==b'preserved fixture','no overwrite existing output')
    cases.append(dict(case='existing-output-preserved',passed=True))
    assets=folder/'assets';assets.mkdir();(assets/'immutable.bin').write_bytes(b'one')
    before=snapshot_tree(assets);need(before==snapshot_tree(assets),'stable input fingerprint')
    (assets/'immutable.bin').write_bytes(b'changed')
    need(before!=snapshot_tree(assets),'input mutation detected')
    cases.append(dict(case='shared-input-mutation-detected',passed=True,before=before,after=snapshot_tree(assets)))
    answer=dict(passed=True,cases=cases,count=len(cases),real_images_created=False)
    (out/'adapter-tests.json').write_text(json.dumps(answer,indent=2)+'\n')
    return answer

def relocate_rust_link(argv,directory,name):
    directory.mkdir(parents=True,exist_ok=True);a=argv[:]
    for i,v in enumerate(a):
        if v.startswith('--emit=link,dep-info='):a[i]='--emit=link,dep-info='+str(directory/(name+'.d'))
        if v.startswith('link-arg=-Map='):a[i]='link-arg=-Map='+str(directory/(name+'.map'))
    a[a.index('-o')+1]=str(directory/(name+'.elf'))
    return a

def prepare_paired_uefi(out,kernels,apps,cfgs,efi):
    stages={}
    for profile in ('verify','normal'):
        stage=out/(profile+'-stage');(stage/'boot/grub').mkdir(parents=True)
        shutil.copyfile(kernels[profile],stage/'boot/vos.bin')
        selected={n:p for n,p in apps.items() if profile=='verify' or n not in ('gui-probe','pointer-probe')}
        for n,p in selected.items():shutil.copyfile(p,stage/'boot'/(n+'.elf'))
        (stage/'boot/grub/grub.cfg').write_bytes((REPO/'arch/x86_64/grub-uefi-head.cfg').read_bytes()+cfgs[profile].read_bytes())
        stages[profile]=stage
    need(sum(p.stat().st_blocks*512 for stage in stages.values() for p in [stage,*stage.rglob('*')])<=5*1024**2,'5 MiB payload staging provision')
    producer=out/'producer';producer.mkdir();temporary=producer/'tmp';temporary.mkdir()
    real=Path(shutil.which('xorriso')).resolve()
    c=dict(output_root=str(out),verify_iso=str(out/'verify-uefi.iso'),normal_iso=str(out/'normal-uefi.iso'),
           verify_stage=str(stages['verify']),normal_stage=str(stages['normal']),xorriso=str(real),
           xorriso_sha256=sha(real),producer_tmp=str(temporary),receipt=str(producer/'paired-result.json'))
    contract=producer/'contract.json';contract.write_text(json.dumps(c,indent=2)+'\n')
    wrapper=producer/'xorriso-paired'
    wrapper.write_text('#!/usr/bin/python3 -B\nimport runpy,sys\nm=runpy.run_path('+repr(str(REPO/'scripts/test-pointer.py'))+',run_name="n63_xorriso_adapter")\nsys.exit(m["xorriso_pair"](sys.argv[1:],'+repr(str(contract))+'))\n')
    wrapper.chmod(0o500)
    env=os.environ.copy();env['TMPDIR']=str(temporary);env['PYTHONDONTWRITEBYTECODE']='1'
    grub=Path(shutil.which('grub-mkrescue')).resolve();grub_hash=sha(grub)
    command=[str(grub),'-d',str(efi),'--xorriso='+str(wrapper),'-o',c['verify_iso'],str(stages['verify'])]
    with (out/'paired-uefi-build.log').open('x') as log:
        subprocess.run(command,cwd=REPO,env=env,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
    receipt=json.loads(Path(c['receipt']).read_text());need(receipt['passed'],'paired producer complete')
    need(sha(grub)==grub_hash,'GRUB producer identity after both images')
    receipt['grub_sha256']=grub_hash
    need(sha(real)==c['xorriso_sha256'],'producer binary identity')
    (producer/'grub-command.json').write_text(json.dumps(command,indent=2)+'\n')
    return stages,receipt

def resume_main():
    ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);args=ap.parse_args()
    out=args.output.resolve();phase=Path(os.environ['N63_ROOT']).resolve();M=1024**2
    budget=json.loads((phase/'N63-BUDGET.json').read_text());control=phase/'final/build-peak-recovery-1'
    authority=json.loads((control/'RECOVERY.json').read_text())
    need(out==phase/'verification-continuation-1' and not out.exists(),'one distinct admitted verification continuation')
    need(authority['identity']=='n63-build-peak-recovery-1' and authority['execution_ceiling_bytes']==72*M and
         authority['phase_ceiling_bytes']==96*M and budget['attempt_limit_bytes']==72*M,'bound unchanged continuation authority')
    proof=phase/'attempt-1';need(sha(proof/'manifest.json')==authority['compact_manifest_sha256'],'immutable passing proof manifest')
    prior=json.loads((proof/'result.json').read_text())
    for row in json.loads((proof/'manifest.json').read_text()):need(sha(proof/row['path'])==row['sha256'],'53 preserved proof records')
    for n,h in prior['source_sha256'].items():
        if n not in ('scripts/test-pointer.py','user/pointer-demo/main.rs'):need(sha(REPO/n)==h,'unaffected source '+n)
    out.mkdir();(out/'tmp').mkdir();os.environ['TMPDIR']=str(out/'tmp')
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));resource.setrlimit(resource.RLIMIT_FSIZE,(32*M,32*M))
    result=dict(passed=False,scope='N6.3 bounded pointer verification continuation',physical_hardware_verified=False)
    try:
        names=set(prior['source_sha256'])
        result['source_sha256']={n:sha(REPO/n) for n in sorted(names)}
        result['admitted_peak_MiB']=dict(verification_images=20,normal_images=20,one_shared_GRUB_scratch=14,
            payload_staging=5,kernel_and_apps_including_old_proof=5,host_evidence=4,contingency=4)
        need(sum(result['admitted_peak_MiB'].values())==72,'complete cumulative provision')
        result['adapter_tests']=test_pair_adapter(out)
        # Reuse accepted SDK/runtime/probe outputs. Only the logging-affected demo
        # is rebuilt and reproduced, without changing its input/event semantics.
        lines=(proof/'compact-dry-run.log').read_text().splitlines()
        commands=[shlex.split(x) for x in lines if x.startswith('/home/') and 'user/pointer-demo/main.rs' in x]
        need(len(commands)==1,'accepted compiler-driven link recipe')
        command=commands[0];rustc=command[0]
        need(sha(Path(rustc))=='369640757edac99bfa1358946cfc2861be80ec39785416a46c06a990840facfb','pinned Rust compiler')
        for p,h in prior['compact_recipe']['shared_runtime_inputs'].items():need(sha(Path(p))==h,'matching immutable runtime input')
        user=out/'user';first=relocate_rust_link(command,user,'pointer-demo')
        run(first,out/'pointer-demo-build.log',60)
        second=relocate_rust_link(command,out/'repro','pointer-demo');run(second,out/'pointer-demo-repro.log',60)
        demo=user/'pointer-demo.elf';need(sha(demo)==sha(out/'repro/pointer-demo.elf'),'logging-affected ELF reproduces identically')
        run(['scripts/check-user-elf.sh',str(demo)],out/'pointer-demo-elf.log')
        run([str(proof/'compact-elfcheck'),str(demo)],out/'pointer-demo-kernel-validator.log')
        result['demo_rebuild']=dict(sha256=sha(demo),bit_identical=True,validators_passed=True)
        result['compact_proof_reused']=dict(manifest_sha256=sha(proof/'manifest.json'),entries=53,probe_rebuilt=False)
        cc=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Iarch/x86_64','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
        src=(REPO/'arch/x86_64/syscall.c').read_text();debug=src[src.index('void debug_emit('):src.index('/* debug_write')]
        (out/'debug.c').write_text('#include <stdint.h>\n'+debug)
        service=['-DVOS_GUI_HOST','-DVOS_CONSOLE_HOST','tests/host/pointer.c','arch/x86_64/gui.c','arch/x86_64/user_console.c','arch/x86_64/keyboard.c','arch/x86_64/pointer.c','arch/x86_64/input_console.c',str(out/'debug.c')]
        render=['-DPOINTER_RENDER','tests/host/pointer.c','arch/x86_64/desktop.c','arch/x86_64/font8x16.c']
        console=['-DVOS_CONSOLE_HOST','tests/host/user-console-handoff.c','arch/x86_64/user_console.c','arch/x86_64/keyboard.c','arch/x86_64/pointer.c','arch/x86_64/input_console.c',str(out/'debug.c')]
        result['host']={}
        for name,files in [('pointer-service',service),('pointer-renderer',render),('console-regression',console)]:
            run(cc+['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']+files+['-o',str(out/name)],out/(name+'-build.log'),60)
            run([str(out/name)],out/(name+'.log'),60)
            result['host'][name]=(out/(name+'.log')).read_text();need('PASS' in result['host'][name],'sanitizer host '+name)
            print('N63_HOST_PASS '+name+' '+result['host'][name].strip(),flush=True)
        run([rustc,'--cfg','test','--crate-name','vos','--crate-type=rlib','--edition=2024','user/rust/vos.rs','-o',str(out/'libvos.rlib')],out/'rust-vos.log')
        run([rustc,'--crate-name','vos_gui','--crate-type=rlib','--edition=2024','-L','dependency='+str(out),'--extern','vos='+str(out/'libvos.rlib'),'user/rust/gui.rs','-o',str(out/'libvos_gui.rlib')],out/'rust-gui.log')
        (out/'rust-checks.rs').write_text('#[path='+json.dumps(str(REPO/'tests/rust/gui.rs'))+'] mod baseline;\n#[path='+json.dumps(str(REPO/'tests/rust/pointer.rs'))+'] mod pointer;\n')
        run([rustc,'--test','-C','strip=symbols','--edition=2024','-L','dependency='+str(out),'--extern','vos_gui='+str(out/'libvos_gui.rlib'),str(out/'rust-checks.rs'),'-o',str(out/'rust-tests')],out/'rust-build.log')
        run([str(out/'rust-tests')],out/'rust-tests.log');result['rust']=(out/'rust-tests.log').read_text()
        run(cc+['-O2']+service+['-o',str(out/'pointer-release')],out/'release-build.log')
        times=[]
        for i in range(3):
            start=time.perf_counter_ns();run([str(out/'pointer-release')],out/('release-%d.log'%i));times.append(time.perf_counter_ns()-start)
        result['optimized_host_samples_ns']=times
        result['performance_limit']='Separate optimized hosted workload including assertions/startup; no native latency or production performance claim.'
        oldphase=REPO/'build/n6-gui-20261025T083036Z';old=json.loads((oldphase/'bootstrap-hardening-1/result.json').read_text());previous=old['build']
        common=REPO/'build/app-console-20261025T051241Z/attempt-1'
        apps={n:common/'image/user'/(n+'.elf') for n in ('vsh','hello','rixie')}
        apps.update({n:oldphase/'image-recipe-recovery-1/image/user'/(n+'.elf') for n in ('gui-demo','gui-probe')})
        for n,p in apps.items():need(sha(p)==previous['application_sha256'][n],'accepted ELF '+n)
        apps.update({'pointer-demo':demo,'pointer-probe':proof/'user/pointer-probe.elf'})
        need(sha(apps['pointer-probe'])==prior['compact_recipe']['applications']['pointer-probe']['sha256'],'unchanged verified probe')
        disk=common/'disk.img';disk_hash=sha(disk)
        need(disk_hash=='5ef98c6e0b4dd983dfb7db1a8d73d0175cbb86c358e61f2957c689d164890dcc','immutable disk')
        kernel=out/'kernel';kernel.mkdir()
        changed=('pointer','keyboard','user_console','gui','screen','syscall','app_payload','gui_verify')
        flags=previous['compile'][:previous['compile'].index('-c')]
        for n in changed:run(flags+['-c','arch/x86_64/'+n+'.c','-o',str(kernel/(n+'.o'))],out/(n+'-compile.log'))
        reused={x['path']:x['sha256'] for x in previous['reused_objects']}
        link=previous['link'][:];link[link.index('-o')+1]=str(kernel/'vos-verify.bin')
        for i,v in enumerate(link):
            if not v.endswith('.o'):continue
            if Path(v).stem in changed:link[i]=str(kernel/(Path(v).stem+'.o'))
            else:need(v in reused and sha(Path(v))==reused[v],'accepted reused object '+v)
        link.append(str(kernel/'pointer.o'));run(link,out/'kernel-link.log')
        normal_flags=[x for x in flags if x!='-DVOS_GUI_VERIFY']
        normal_names=('gui_verify','syscall','gui','app_payload')
        for n in normal_names:run(normal_flags+['-c','arch/x86_64/'+n+'.c','-o',str(kernel/(n+'-normal.o'))],out/(n+'-normal-build.log'))
        normal=[str(kernel/'vos-normal.bin') if x==str(kernel/'vos-verify.bin') else str(kernel/(Path(x).stem+'-normal.o')) if x in tuple(str(kernel/(n+'.o')) for n in normal_names) else x for x in link]
        run(normal,out/'kernel-normal-link.log')
        kernels={p:kernel/('vos-'+p+'.bin') for p in ('verify','normal')}
        for profile,p in kernels.items():
            run(['python3','-I','-B','scripts/check-kernel-layout.py',str(p)],out/(profile+'-layout.log'))
            run(['grub-file','--is-x86-multiboot2',str(p)],out/(profile+'-multiboot.log'))
        build_bytes=622592+sum(p.stat().st_blocks*512 for folder in (kernel,user,out/'repro') for p in [folder,*folder.rglob('*')])
        need(build_bytes<=5*M,'measured compact kernel/app/proof category <=5 MiB')
        result['build']=dict(compile_flags=flags,verify_link=link,normal_link=normal,kernel_app_proof_bytes=build_bytes,apps={n:sha(p) for n,p in apps.items()},kernels={n:sha(p) for n,p in kernels.items()})
        cfgs={}
        base=(REPO/'arch/x86_64/grub-pointer.cfg').read_text()
        for profile in ('verify','normal'):
            text=base
            if profile=='verify':text=text.replace('vos.pointer=1','vos.pointer=1 vos.gui-verify=1').replace('    boot\n','    module2 /boot/gui-probe.elf gui-probe\n    module2 /boot/pointer-probe.elf pointer-probe\n    boot\n')
            cfgs[profile]=out/(profile+'.cfg');cfgs[profile].write_text(text)
        bios_input=oldphase/'bootstrap-hardening-1/image/kernel/vos.iso'
        need(sha(bios_input)==old['images']['bios']['ISO_sha256'],'accepted BIOS-only producer input')
        with bios_input.open('rb') as f:f.seek(512);need(f.read(8)!=b'EFI PART','no imported GPT for BIOS replay')
        result['images']={};result['firmware']={}
        for profile in ('verify','normal'):
            selected={n:p for n,p in apps.items() if profile=='verify' or n not in ('gui-probe','pointer-probe')}
            iso=out/(profile+'-bios.iso')
            argv=['xorriso','-indev',str(bios_input),'-outdev',str(iso),'-boot_image','any','replay','-map',str(kernels[profile]),'/boot/vos.bin','-map',str(cfgs[profile]),'/boot/grub/grub.cfg']
            for n,p in selected.items():argv+=['-map',str(p),'/boot/'+n+'.elf']
            if profile=='normal':argv+=['-rm','/boot/gui-probe.elf','--']
            run(argv+['-commit','-end'],out/(profile+'-bios-build.log'),90)
            expected={'/boot/vos.bin':kernels[profile],'/boot/grub/grub.cfg':cfgs[profile]}
            expected.update({'/boot/'+n+'.elf':p for n,p in selected.items()})
            folder=out/('validate-'+profile+'-bios');folder.mkdir()
            result['images'][profile+'-bios']=validate_image(iso,expected,False,folder)
            if profile=='normal':
                table=G['D']['iso_records'](iso)
                need('/boot/gui-probe.elf' not in table and '/boot/pointer-probe.elf' not in table,'normal BIOS media omits probes')
            result['firmware'][profile+'-bios']=boot(out,'bios',iso,disk,profile=='normal')
            (out/(profile+'-bios-checkpoint.json')).write_text(json.dumps(result['firmware'][profile+'-bios'],indent=2)+'\n')
            print('N63_FIRMWARE_PASS '+profile+' BIOS',flush=True)
        efi=Path(previous['UEFI'][previous['UEFI'].index('-d')+1])
        stages,paired=prepare_paired_uefi(out,kernels,apps,cfgs,efi);result['paired_UEFI_producer']=paired
        for profile in ('verify','normal'):
            selected={n:p for n,p in apps.items() if profile=='verify' or n not in ('gui-probe','pointer-probe')}
            iso=out/(profile+'-uefi.iso')
            expected={'/boot/vos.bin':kernels[profile],'/boot/grub/grub.cfg':stages[profile]/'boot/grub/grub.cfg'}
            expected.update({'/boot/'+n+'.elf':p for n,p in selected.items()})
            folder=out/('validate-'+profile+'-uefi');folder.mkdir()
            result['images'][profile+'-uefi']=validate_image(iso,expected,True,folder)
            if profile=='normal':
                table=G['D']['iso_records'](iso)
                need('/boot/gui-probe.elf' not in table and '/boot/pointer-probe.elf' not in table,'normal media omits probes')
            result['firmware'][profile+'-uefi']=boot(out,'uefi',iso,disk,profile=='normal')
            (out/(profile+'-uefi-checkpoint.json')).write_text(json.dumps(result['firmware'][profile+'-uefi'],indent=2)+'\n')
            print('N63_FIRMWARE_PASS '+profile+' UEFI',flush=True)
        need(sha(disk)==disk_hash,'preserved disk after all boots')
        for row in json.loads((proof/'manifest.json').read_text()):need(sha(proof/row['path'])==row['sha256'],'compact proof immutable after continuation')
        need(all(sha(REPO/n)==v for n,v in result['source_sha256'].items()),'verified source identities')
        result.update(passed=True,one_input_worker=True,event_bytes=16,queue_capacity=16,cursor_underlay_bytes=384,
                      no_producer_scratch_deletion_by_adapter=True,no_physical_hardware_claim=True)
    except BaseException as e:
        result['error']=str(e);raise
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        entries=[dict(path=str(p.relative_to(out)),sha256=sha(p),size=p.stat().st_size) for p in sorted(out.rglob('*')) if p.is_file()]
        (out/'manifest.json').write_text(json.dumps(entries,indent=2)+'\n')
        print(json.dumps(result),flush=True)



def user_loader_view(data):
    need(data[:6]==b'\x7fELF\x02\x01' and len(data)>=64,'ELF64 input')
    phoff=struct.unpack_from('<Q',data,32)[0];num=struct.unpack_from('<H',data,56)[0]
    need(0<num<=8 and phoff+num*56<=len(data),'bounded program headers')
    headers=[]
    for i in range(num):
        typ,flags,off,va,pa,filesz,memsz,align=struct.unpack_from('<IIQQQQQQ',data,phoff+i*56)
        need(off+filesz<=len(data),'program payload extent')
        headers.append(dict(type=typ,flags=flags,vaddr=va,paddr=pa,filesz=filesz,memsz=memsz,align=align,payload_sha256=hashlib.sha256(data[off:off+filesz]).hexdigest()))
    return dict(ident=data[:16].hex(),type_machine_version=data[16:24].hex(),entry=struct.unpack_from('<Q',data,24)[0],flags=struct.unpack_from('<I',data,48)[0],headers=headers)

def pack_user_debug(out,source,validator):
    target=out/'payload';target.mkdir();repro=out/'packing-repro';repro.mkdir()
    objcopy=Path(shutil.which('objcopy')).resolve();toolhash=sha(objcopy);originalhash=sha(source)
    first=target/'pointer-demo.elf';second=repro/'pointer-demo.elf'
    for name,dest in [('payload',first),('repeat',second)]:
        run([str(objcopy),'--strip-debug',str(source),str(dest)],out/('pack-'+name+'.log'))
        need(dest.stat().st_size<=32768,'bounded packed ELF <=32 KiB')
        need(user_loader_view(dest.read_bytes())==user_loader_view(source.read_bytes()),'entry, mappings, permissions, ABI notes and loaded bytes unchanged')
        run(['scripts/check-user-elf.sh',str(dest)],out/('pack-'+name+'-shell.log'))
        run([str(validator),str(dest)],out/('pack-'+name+'-kernel.log'))
    need(first.read_bytes()==second.read_bytes(),'deterministic metadata-only packing')
    original=first.read_bytes();bad=bytearray(original)
    ph=struct.unpack_from('<Q',bad,32)[0];at=struct.unpack_from('<Q',bad,ph+8)[0]
    bad[at]^=1;negative=repro/'negative-mapped-byte.elf';negative.write_bytes(bad)
    need(user_loader_view(bytes(bad))!=user_loader_view(original),'mapped-byte mutation must be detected')
    need(sha(source)==originalhash and sha(objcopy)==toolhash,'original and tool unchanged')
    answer=dict(passed=True,operation='objcopy --strip-debug into fresh files only',source=str(source),source_sha256=originalhash,
        source_bytes=source.stat().st_size,packed_bytes=first.stat().st_size,packed_sha256=sha(first),objcopy=str(objcopy),objcopy_sha256=toolhash,
        deterministic=True,loader_memory_view_unchanged=True,negative_mapped_byte_rejected=True,original_preserved=True)
    (out/'packing-result.json').write_text(json.dumps(answer,indent=2)+'\n')
    return first,answer


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
def test_motion_transport(out):
    folder=out/'motion-transport-model';folder.mkdir();trace=folder/'ps2.trace'
    class Peer:
        def __init__(self):self.x=0;self.y=0;self.b=0
        def execute(self,name,a):
            if name=='trace-event-set-state':return {}
            need(name=='input-send-event','model command')
            for e in a['events']:
                d=e['data']
                if e['type']=='rel':
                    if d['axis']=='x':self.x+=d['value']
                    else:self.y-=d['value']
                else:self.b=int(d['down'])
            rows=[]
            for _ in range(5):
                x=max(-127,min(127,self.x));y=max(-127,min(127,self.y))
                h=8|(16 if x<0 else 0)|(32 if y<0 else 0)|self.b
                rows.append('ps2_mouse_send_packet 0x1234 x %d y %d z 0 bs 0x%x\n'%(x,y,h))
                for v in (h,x&255,y&255):rows+=['ps2_read_data 0x1234\n','pckbd_kbd_read_data 0x%02x\n'%v]
                self.x-=x;self.y-=y
                if self.x==self.y==0:break
            with trace.open('a') as f:f.writelines(rows)
            return {}
    q=Peer();events=[];m=PS2Motion(q,trace,events,time.monotonic());cases=[]
    for x,y in [(-3000,-3000),(3000,0),(0,3000),(-3000,0),(512,-377),(1,0)]:
        before=ps2_trace(trace)
        e=[dict(type='rel',data=dict(axis='x',value=x)),dict(type='rel',data=dict(axis='y',value=y))]
        after=m.send(e,x,y)
        need(after['dx']-before['dx']==x and after['dy']-before['dy']==-y and q.x==q.y==0,'no un-emitted vector or cancellation')
        cases.append(dict(vector=[x,y],generated_packets=len(after['emitted'])-len(before['emitted']),all_generated_bytes_read=after['drained']))
    for b in (True,False):
        m.send([dict(type='btn',data=dict(button='left',down=b))],0,0)
        need(ps2_trace(trace)['decoded'][-1]['buttons']==int(b),'button transitions retained')
    # Corrupt a generated/read byte in a fresh independent fixture.
    bad=folder/'corrupt.trace';bad.write_text(trace.read_text().replace('pckbd_kbd_read_data 0x81','pckbd_kbd_read_data 0x80',1))
    rejected=False
    try:ps2_trace(bad)
    except RuntimeError:rejected=True
    need(rejected,'read-byte mismatch rejected')
    result=dict(passed=True,vector_cases=cases,buttons=2,corrupt_read_rejected=True,scope='host transport model, not native verification')
    (folder/'result.json').write_text(json.dumps(result,indent=2)+'\n');return result
def motion_resume_main():
    ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);args=ap.parse_args()
    out=args.output.resolve();phase=Path(os.environ['N63_ROOT']).resolve();M=1024**2
    control=phase/'final/motion-recovery-1';authority=json.loads((control/'RECOVERY.json').read_text())
    need(out==phase/'motion-continuation-1' and not out.exists(),'one fresh image continuation')
    need(authority['identity']=='n63-motion-recovery-1' and authority['execution_ceiling_bytes']==72*M and authority['phase_ceiling_bytes']==96*M,'unchanged admitted scope')
    native=phase/'verification-continuation-1';proof=phase/'attempt-1'
    need(sha(native/'manifest.json')==authority['native_manifest_sha256'],'preserved native-build/host manifest')
    prior=json.loads((native/'result.json').read_text())
    need(not prior['passed'] and prior['error']=='measured compact kernel/app/proof category <=5 MiB','exact preserved planning stop')
    for row in json.loads((native/'manifest.json').read_text()):need(sha(native/row['path'])==row['sha256'],'all native/host results unchanged')
    for n,h in prior['source_sha256'].items():
        if n not in ('scripts/test-pointer.py','user/pointer-demo/main.rs'):need(sha(REPO/n)==h,'all compiled/tested sources unchanged '+n)
    # Reuse successful tests only when their actual helper bodies are identical.
    import ast,base64
    transport=json.loads((phase/'final/build-peak-recovery-1/requests/001.json').read_text())
    row=next(x for x in transport['request']['files'] if x['path']=='scripts/test-pointer.py')
    before=base64.b64decode(row['base64']).decode()
    need(hashlib.sha256(before.encode()).hexdigest()==prior['source_sha256']['scripts/test-pointer.py'],'original test-driver identity')
    current=(REPO/'scripts/test-pointer.py').read_text()
    def bodies(text):
        return {x.name:ast.get_source_segment(text,x) for x in ast.parse(text).body if isinstance(x,ast.FunctionDef)}
    oldb,newb=bodies(before),bodies(current)
    for n in ('snapshot_tree','pair_commands','xorriso_pair','test_pair_adapter','prepare_paired_uefi'):
        need(oldb[n]==newb[n],'unchanged tested/accepted media helper '+n)
    # Reuse native, hosted and Rust results only with every compiled input pinned.
    accepted=phase/'coordinate-log-continuation-1';compiled=json.loads((accepted/'result.json').read_text())
    for n,h in compiled['source_sha256'].items():
        if n!='scripts/test-pointer.py':need(sha(REPO/n)==h,'unchanged native implementation '+n)
    for row in json.loads((accepted/'manifest.json').read_text()):need(sha(accepted/row['path'])==row['sha256'],'preserved original corner failure')
    diagnosis=phase/'motion-diagnostic-1';observed=json.loads((diagnosis/'result.json').read_text())
    need(observed['passed'] and observed['actual_cursor']==[635,0] and observed['no_generated_packet_lost'] and observed['no_decode_mismatch'],'evidence-supported input synchronization correction')
    for row in json.loads((diagnosis/'manifest.json').read_text()):need(sha(diagnosis/row['path'])==row['sha256'],'immutable diagnostic evidence')
    stopped=phase/'image-continuation-1';stopped_result=json.loads((stopped/'result.json').read_text())
    need(stopped_result['error']=='bounded native marker deadline','preserved single-record logging failure')
    for row in json.loads((stopped/'manifest.json').read_text()):
        need(sha(stopped/row['path'])==row['sha256'],'all previous BIOS evidence preserved')
    prefix=(stopped/'bios/serial.log').read_bytes()
    need(all(('gui-verify: case %d PASS'%i).encode() in prefix for i in range(16)),'all sixteen existing BIOS cases preserved')
    need(b'gui-bootstrap: 18 repeated load failures PASS' in prefix and b'gui-verify: process exhaustion rollback PASS' in prefix,'preserved BIOS lifecycle checks')
    need(b'gui-demo: KEY a PRESENTED' in prefix and b'gui-demo: KEY b PRESENTED' in prefix and b'gui-demo: RELEASED; clean exit' in prefix,'preserved real BIOS checkerboard regression')
    out.mkdir();(out/'tmp').mkdir();os.environ['TMPDIR']=str(out/'tmp')
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));resource.setrlimit(resource.RLIMIT_FSIZE,(32*M,32*M))
    result=dict(passed=False,scope='N6.3 remaining BIOS/UEFI pointer verification',physical_hardware_verified=False)
    try:
        result['source_sha256']={n:sha(REPO/n) for n in prior['source_sha256']}
        result['host']=prior['host'];result['rust']=prior['rust'];result['adapter_tests']=prior['adapter_tests']
        result['reused_checks']=dict(native_manifest_sha256=sha(native/'manifest.json'),host_sanitizers=True,Rust=True,adapter_regressions=11,repeated=False)
        result['optimized_host_samples_ns']=prior['optimized_host_samples_ns'];result['performance_limit']=prior['performance_limit']
        result['motion_transport_tests']=test_motion_transport(out)
        result['diagnosis']=dict(path=str(diagnosis/'result.json'),manifest_sha256=sha(diagnosis/'manifest.json'),root_cause=observed['root_cause'])
        result['logger_check']=compiled['logger_check'];result['logger_rebuild']=compiled['logger_rebuild']
        result['packing']=compiled['packing'];packed=Path(compiled['build']['apps']['pointer-demo']['path'])
        need(sha(packed)==compiled['build']['apps']['pointer-demo']['sha256'],'logger corrected, validated app reused unchanged')
        result['BIOS_reused_prefix']=compiled['BIOS_reused_prefix']
        result['first_cursor_historical_screenshot']=compiled['first_cursor_historical_screenshot']
        oldphase=REPO/'build/n6-gui-20261025T083036Z';old=json.loads((oldphase/'bootstrap-hardening-1/result.json').read_text());previous=old['build']
        common=REPO/'build/app-console-20261025T051241Z/attempt-1'
        apps={n:common/'image/user'/(n+'.elf') for n in ('vsh','hello','rixie')}
        apps.update({n:oldphase/'image-recipe-recovery-1/image/user'/(n+'.elf') for n in ('gui-demo','gui-probe')})
        for n,p in apps.items():need(sha(p)==previous['application_sha256'][n],'accepted application '+n)
        apps.update({'pointer-demo':packed,'pointer-probe':proof/'user/pointer-probe.elf'})
        kernels={p:native/'kernel'/('vos-'+p+'.bin') for p in ('verify','normal')}
        disk=common/'disk.img';disk_hash=sha(disk)
        need(disk_hash=='5ef98c6e0b4dd983dfb7db1a8d73d0175cbb86c358e61f2957c689d164890dcc','immutable disk')
        # All old outputs remain charged. These exact copies plus the new packing
        # outputs fit the revised 5.5 MiB retained kernel/app/proof provision.
        result['kernel_app_proof_bytes']=compiled['kernel_app_proof_bytes']
        cfgbase=(REPO/'arch/x86_64/grub-pointer.cfg').read_text();cfgs={}
        # 6 directories and 2 config blocks, plus exact bounded source copies.
        projected_stage=8*4096
        for profile in ('verify','normal'):
            projected_stage+=(kernels[profile].stat().st_size+4095)//4096*4096
            for n,p in apps.items():
                if profile=='normal' and n in ('gui-probe','pointer-probe'):continue
                projected_stage+=(p.stat().st_size+4095)//4096*4096
        need(projected_stage<=int(4.75*M),'measured payload staging <=4.75 MiB')
        # Immutable reference ISO sizes + positive payload block deltas, plus
        # 512 KiB for directory/HFS/alignment changes. No historic shrink credit.
        reference_kernel=oldphase/'bootstrap-hardening-1/image/kernel/vos.bin'
        baseimages=2*((reference_kernel.parent/'vos.iso').stat().st_size+(reference_kernel.parent/'vos-uefi.iso').stat().st_size)
        delta=sum(max(0,(p.stat().st_size+2047)//2048*2048-(reference_kernel.stat().st_size+2047)//2048*2048) for p in kernels.values())*2
        delta+=4*((packed.stat().st_size+2047)//2048*2048)+2*((apps['pointer-probe'].stat().st_size+2047)//2048*2048)
        image_forecast=baseimages+delta+M//2
        need(image_forecast<=int(39.75*M),'conservative immutable-payload ISO bound <=39.75 MiB')
        result['admitted_peak_MiB']=dict(images=39.75,one_shared_GRUB_scratch=14,staging=4.75,kernel_app_proof=7,host_evidence=4,contingency=2.5)
        need(sum(result['admitted_peak_MiB'].values())==72,'unchanged cumulative 72 MiB execution provision')
        result['measured_forecast']=dict(projected_stage_bytes=projected_stage,image_upper_bytes=image_forecast,
            image_filesystem_alignment_margin_bytes=M//2,old_unstripped_outputs_retained=True)
        result['build']=dict(kernels={n:dict(path=str(p),sha256=sha(p)) for n,p in kernels.items()},apps={n:dict(path=str(p),sha256=sha(p)) for n,p in apps.items()},
            validated_kernel_layouts_reused=True,native_build_manifest_sha256=sha(native/'manifest.json'))
        cfgs={}
        base=(REPO/'arch/x86_64/grub-pointer.cfg').read_text()
        for profile in ('verify','normal'):
            text=base
            if profile=='verify':text=text.replace('vos.pointer=1','vos.pointer=1 vos.gui-verify=1').replace('    boot\n','    module2 /boot/gui-probe.elf gui-probe\n    module2 /boot/pointer-probe.elf pointer-probe\n    boot\n')
            cfgs[profile]=out/(profile+'.cfg');cfgs[profile].write_text(text)
        bios_input=oldphase/'bootstrap-hardening-1/image/kernel/vos.iso'
        need(sha(bios_input)==old['images']['bios']['ISO_sha256'],'accepted BIOS-only producer input')
        with bios_input.open('rb') as f:f.seek(512);need(f.read(8)!=b'EFI PART','no imported GPT for BIOS replay')
        result['images']={};result['firmware']={}
        for profile in (): # Existing validated normal BIOS image is immutable and reused.
            selected={n:p for n,p in apps.items() if profile=='verify' or n not in ('gui-probe','pointer-probe')}
            iso=out/(profile+'-bios.iso')
            argv=['xorriso','-indev',str(bios_input),'-outdev',str(iso),'-boot_image','any','replay','-map',str(kernels[profile]),'/boot/vos.bin','-map',str(cfgs[profile]),'/boot/grub/grub.cfg']
            for n,p in selected.items():argv+=['-map',str(p),'/boot/'+n+'.elf']
            if profile=='normal':argv+=['-rm','/boot/gui-probe.elf','--']
            run(argv+['-commit','-end'],out/(profile+'-bios-build.log'),90)
            expected={'/boot/vos.bin':kernels[profile],'/boot/grub/grub.cfg':cfgs[profile]}
            expected.update({'/boot/'+n+'.elf':p for n,p in selected.items()})
            folder=out/('validate-'+profile+'-bios');folder.mkdir()
            result['images'][profile+'-bios']=validate_image(iso,expected,False,folder)
            if profile=='normal':
                table=G['D']['iso_records'](iso)
                need('/boot/gui-probe.elf' not in table and '/boot/pointer-probe.elf' not in table,'normal BIOS media omits probes')
            result['firmware'][profile+'-bios']=boot(out,'bios',iso,disk,profile=='normal',full_pointer=True)
            (out/(profile+'-bios-checkpoint.json')).write_text(json.dumps(result['firmware'][profile+'-bios'],indent=2)+'\n')
            print('N63_FIRMWARE_PASS '+profile+' BIOS',flush=True)
        bios=accepted/'normal-bios.iso'
        need(sha(bios)==compiled['images']['normal-bios']['ISO_sha256'],'read-only BIOS media identity')
        result['images']['normal-bios']=compiled['images']['normal-bios']
        result['reused_normal_bios_iso']=dict(path=str(bios),sha256=sha(bios),no_rebuild=True)
        result['firmware']['normal-bios']=boot(out,'bios',bios,disk,True,full_pointer=True)
        (out/'normal-bios-checkpoint.json').write_text(json.dumps(result['firmware']['normal-bios'],indent=2)+'\n')
        print('N63_FIRMWARE_PASS normal BIOS',flush=True)
        efi=Path(previous['UEFI'][previous['UEFI'].index('-d')+1])
        stages,paired=prepare_paired_uefi(out,kernels,apps,cfgs,efi);result['paired_UEFI_producer']=paired
        need(sum(p.stat().st_blocks*512 for f in stages.values() for p in [f,*f.rglob('*')])<=int(4.75*M),'actual staging cap retained')
        for profile in ('verify','normal'):
            selected={n:p for n,p in apps.items() if profile=='verify' or n not in ('gui-probe','pointer-probe')}
            iso=out/(profile+'-uefi.iso')
            expected={'/boot/vos.bin':kernels[profile],'/boot/grub/grub.cfg':stages[profile]/'boot/grub/grub.cfg'}
            expected.update({'/boot/'+n+'.elf':p for n,p in selected.items()})
            folder=out/('validate-'+profile+'-uefi');folder.mkdir()
            result['images'][profile+'-uefi']=validate_image(iso,expected,True,folder)
            if profile=='normal':
                table=G['D']['iso_records'](iso)
                need('/boot/gui-probe.elf' not in table and '/boot/pointer-probe.elf' not in table,'normal media omits probes')
            result['firmware'][profile+'-uefi']=boot(out,'uefi',iso,disk,profile=='normal')
            (out/(profile+'-uefi-checkpoint.json')).write_text(json.dumps(result['firmware'][profile+'-uefi'],indent=2)+'\n')
            print('N63_FIRMWARE_PASS '+profile+' UEFI',flush=True)
        need(sum(x.stat().st_blocks*512 for x in out.glob('*.iso'))+(stopped/'verify-bios.iso').stat().st_blocks*512+bios.stat().st_blocks*512<=int(39.75*M),'all new and retained ISO outputs within admitted bound')
        need(sha(disk)==disk_hash,'preserved disk after all boots')
        for row in json.loads((native/'manifest.json').read_text()):need(sha(native/row['path'])==row['sha256'],'native and host history immutable')
        for row in json.loads((proof/'manifest.json').read_text()):need(sha(proof/row['path'])==row['sha256'],'compact proof immutable after continuation')
        need(all(sha(REPO/n)==v for n,v in result['source_sha256'].items()),'verified source identities')
        result.update(passed=True,one_input_worker=True,event_bytes=16,queue_capacity=16,cursor_underlay_bytes=384,
                      no_producer_scratch_deletion_by_adapter=True,no_physical_hardware_claim=True)

    except BaseException as e:
        result['error']=str(e);raise
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        entries=[dict(path=str(p.relative_to(out)),sha256=sha(p),size=p.stat().st_size) for p in sorted(out.rglob('*')) if p.is_file()]
        (out/'manifest.json').write_text(json.dumps(entries,indent=2)+'\n')
        print(json.dumps(result),flush=True)
if __name__=='__main__':motion_resume_main()
