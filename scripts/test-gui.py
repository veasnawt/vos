#!/usr/bin/env python3
"""One guarded N6.2 attempt: actual GUI service, Rust demo and firmware."""
import argparse,hashlib,json,os,re,resource,runpy,shutil,subprocess,time,shlex
from pathlib import Path
REPO=Path(__file__).resolve().parents[1]
H=runpy.run_path(str(REPO/'scripts/test-app-handoff.py'),run_name='gui_helpers')
run,QMP,capture,sha,need=(H[n] for n in ('run','QMP','capture','sha','require'))
D=runpy.run_path(str(REPO/'scripts/test-n6-desktop.py'),run_name='gui_image_helpers')
validate_image=D['validate_image']

def verify_pixels(path,palette):
    import struct,zlib
    blob=path.read_bytes();need(blob[:8]==b'\x89PNG\r\n\x1a\n','PNG')
    offset=8;stream=b'';width=height=kind=0
    while offset<len(blob):
        n=struct.unpack_from('>I',blob,offset)[0];tag=blob[offset+4:offset+8];value=blob[offset+8:offset+8+n]
        if tag==b'IHDR':
            width,height,depth,kind,compression,filtration,interlace=struct.unpack('>IIBBBBB',value)
            need(depth==8 and kind in (2,6) and not interlace,'supported authentic PNG encoding')
        if tag==b'IDAT':stream+=value
        offset+=n+12
    bpp=3 if kind==2 else 4;stride=width*bpp;raw=zlib.decompress(stream)
    need(len(raw)==(stride+1)*height,'PNG pixel bounds')
    rows=[];previous=bytearray(stride);at=0
    def paeth(a,b,c):
        p=a+b-c;pa=abs(p-a);pb=abs(p-b);pc=abs(p-c)
        return a if pa<=pb and pa<=pc else b if pb<=pc else c
    for _ in range(height):
        filter_=raw[at];at+=1;row=bytearray(raw[at:at+stride]);at+=stride
        need(filter_<=4,'PNG filter')
        for x in range(stride):
            a=row[x-bpp] if x>=bpp else 0;b=previous[x];c=previous[x-bpp] if x>=bpp else 0
            if filter_==1:row[x]=(row[x]+a)&255
            elif filter_==2:row[x]=(row[x]+b)&255
            elif filter_==3:row[x]=(row[x]+(a+b)//2)&255
            elif filter_==4:row[x]=(row[x]+paeth(a,b,c))&255
        rows.append(row);previous=row
    x0=(width-256)//2;y0=84+(height-156-160)//2
    region=bytearray()
    for y in range(160):
        for x in range(256):
            expected=0x80baff if x<3 or y<3 or x>=253 or y>=157 else \
                (0x409ddd if palette==0 else 0x42c497) if 16<=y<40 and 16<=x<240 else \
                (0x20456b+palette*0x102000 if ((x-16)//28+(y-56)//28)%2==0 else 0x142e4a) if 56<=y<140 and 16<=x<240 else 0x101e30
            pixel=rows[y0+y][(x0+x)*bpp:(x0+x)*bpp+3]
            need(bytes(pixel)==bytes((expected>>16,expected>>8&255,expected&255)),'independent native RGB oracle')
            region+=pixel
    return hashlib.sha256(region).hexdigest()

def boot(out,fw,iso,disk):
    folder=out/fw;folder.mkdir();serial=folder/'serial.log';socket=folder/'qmp'
    args=['qemu-system-x86_64','-machine','q35','-accel','tcg','-m','128M','-cdrom',str(iso),
          '-boot','d','-display','none','-nic','none','-vga','none','-device','VGA,edid=on,xres=1024,yres=768',
          '-serial','file:'+str(serial),'-qmp','unix:'+str(socket)+',server=on,wait=off','-no-reboot','-no-shutdown',
          '-drive','file='+str(disk)+',if=none,id=vs0,format=raw,readonly=on',
          '-device','virtio-blk-pci,drive=vs0,disable-legacy=on']
    if fw=='uefi':
        vars=folder/'OVMF_VARS_4M.fd';shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',vars)
        args+=['-drive','if=pflash,format=raw,unit=0,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd',
               '-drive','if=pflash,format=raw,unit=1,file='+str(vars)]
    (folder/'command.json').write_text(json.dumps(args,indent=2)+'\n')
    proc=None;q=None;cases=[];start=time.monotonic()
    with (folder/'qemu.log').open('x') as log:
        proc=subprocess.Popen(args,cwd=REPO,stdout=log,stderr=subprocess.STDOUT)
        try:
            def data():
                need(proc.poll() is None,'QEMU exited')
                d=serial.read_bytes() if serial.exists() else b''
                need(not any(x in d for x in (b'panic:',b'boot: check failed',b'boot: FAILED',b'native: vsh FAIL')),'guest failure')
                return d
            def wait(f,seconds=12):
                end=time.monotonic()+seconds
                while True:
                    d=data()
                    if f(d):return d
                    need(time.monotonic()<end,'bounded desktop marker timeout');time.sleep(.02)
            wait(lambda d:b'gui-probe: OVERFLOW INPUT WINDOW' in d,40)
            q=QMP(socket)
            for _ in range(40):q.tap('a')
            first_gui=wait(lambda d:b'gui-demo: PRESENTED;' in d,15)
            need(all(('gui-verify: case %d PASS'%i).encode() in first_gui for i in range(10)),'ten actual ring-3 GUI cases')
            need(b'gui-verify: process exhaustion rollback PASS' in first_gui,'native process exhaustion')
            initial=capture(q,folder,'gui-initial');initial_pixels=verify_pixels(Path(initial['path']),0)
            q.tap('a');wait(lambda d:b'gui-demo: KEY a PRESENTED' in d)
            gui_picture=capture(q,folder,'gui');changed_pixels=verify_pixels(Path(gui_picture['path']),1)
            need(initial_pixels!=changed_pixels,'real keyboard changes native GUI pixels')
            q.tap('b');wait(lambda d:b'gui-demo: KEY b PRESENTED' in d)
            q.tap('q');first=wait(lambda d:b'user[vsh]: VOS> ' in d,15)
            need(b'gui: PASS; kernel-CR3 reclamation, all resources restored' in first,'GUI resources reclaimed before shell')
            need(b'user[vsh]: a' not in first and b'user[vsh]: q' not in first,'GUI input not echoed to shell')
            need(b'desktop: scene ready; kernel-owned BGRX surface, bounded console viewport' in first,'actual desktop enabled')
            need(b'catalog: vsh id=4097 ABI=4 policy=1' in first,'real ABI4 vsh')
            need(b'BGRX' in first and b'screen: framebuffer' in first,'validated boot framebuffer')
            def text(s):
                for c in s:q.tap({' ':'spc','/':'slash','.':'dot','-':'minus'}.get(c,c))
            def prompts(d):return re.findall(rb'(?m)^user\[vsh\]: (VOS>[^\r\n]* )\r?$',d)
            def command(s,expect='',prompt='VOS> '):
                before=data();n=len(prompts(before));text(s);q.tap('ret')
                after=wait(lambda d:len(prompts(d))>n)
                need(expect.encode() in after[len(before):] and prompts(after)[-1].decode()==prompt,'actual shell '+s)
                cases.append(dict(command=s,expected=expect,prompt=prompt,passed=True))
            command('apps','hello');command('hello','Hello from VOS!')
            command('clear');command('echo graphical console','graphical console')
            command('cd docs','','VOS>docs> ');command('pwd','/docs','VOS>docs> ')
            before=data();n=len(prompts(before));nr=before.count(b'user[rixie]: Rixie> ')
            text('rixie');q.tap('ret');wait(lambda d:d.count(b'user[rixie]: Rixie> ')>nr)
            for s,expect in [('hello','Hello! I'),('about','deterministic prototype'),('help','Topics: hello')]:
                before=data();nr=before.count(b'user[rixie]: Rixie> ');text(s);q.tap('ret')
                after=wait(lambda d:d.count(b'user[rixie]: Rixie> ')>nr)
                need(expect.encode() in after[len(before):],'Rixie '+s)
                need(len(prompts(after))==n,'shell input suspended')
                cases.append(dict(rixie_command=s,passed=True))
            text('exit');q.tap('ret');wait(lambda d:len(prompts(d))>n)
            command('pwd','/docs','VOS>docs> ')
            command('hello','Hello from VOS!','VOS>docs> ')
            command('cat welcome.txt','Unified apps: real read-only filesystem.','VOS>docs> ')
            q.tap('backspace');text('echo editingx');q.tap('backspace');q.tap('ret')
            wait(lambda d:b'user[vsh]: editing\r\n' in d)
            command('cd missing','operation failed','VOS>docs> ')
            picture=capture(q,folder,'desktop')
            text('exit');q.tap('ret')
            final=wait(lambda d:b'native: vsh PASS, process reclaimed' in d)
            end=time.monotonic()+5
            while True:
                regs=q.execute('human-monitor-command',{'command-line':'info registers'})
                if 'HLT=1' in regs:break
                need(time.monotonic()<end,'final halt');time.sleep(.02)
            (folder/'registers.txt').write_text(regs)
            return dict(passed=True,firmware=fw,cases=cases,actual_vsh=True,actual_rixie=True,desktop=True,
                        cwd_restored=True,child_reclaimed=True,parent_reclaimed=True,halted=True,
                        screenshot=gui_picture,shell_screenshot=picture,native_GUI_cases=10,native_process_exhaustion=True,GUI_event_isolation=True,pixel_oracle=True,elapsed_seconds=time.monotonic()-start)
        finally:
            if q:q.close()
            if proc and proc.poll() is None:proc.terminate()
            if proc:
                try:proc.wait(timeout=3)
                except subprocess.TimeoutExpired:proc.kill();proc.wait()

CATALOG_HARNESS="/* Real payload/catalog/ELF validation; only boot physical aliases/reservations\n * and serial are modeled. Inputs are the exact preserved native ELFs. */\n#include <stdio.h>\n#include <stdlib.h>\n#include <stdint.h>\n#include <string.h>\n#include \"bootinfo.h\"\n#include \"app_payload.h\"\n#include \"app_catalog.h\"\n#include \"memlayout.h\"\nstatic unsigned checks;\n#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,\"catalog FAIL %d %s\\n\",__LINE__,#x);exit(1);}}while(0)\nstatic unsigned char *image[5],*memory[5];\nstatic size_t size_[5];\nstatic int coverage=1;\nstatic void *host_alias(uint64_t pa){\n    CHECK(pa>=0x100000);\n    unsigned index=(unsigned)((pa-0x100000)/0x200000);\n    CHECK(index<5 && pa==0x100000+(uint64_t)index*0x200000);\n    return image[index];\n}\n#define phys_to_virt(pa) host_alias(pa)\n#include \"PAYLOAD_SOURCE\"\nvoid serial_puts(const char *s){(void)s;}\nvoid serial_dec(uint64_t n){(void)n;}\nint resv_overlaps(const struct resv_table *t,uint64_t begin,uint64_t end){\n    CHECK(t && begin<end && end<=BOOT_MAP_LIMIT);return coverage;\n}\nstatic struct boot_info bi;\nstatic struct resv_table reserved;\nstatic void prepare(unsigned n) {\n    static const char *names[]={\"vsh\",\"hello\",\"rixie\",\"gui-demo\",\"gui-probe\"};\n    memset(&bi,0,sizeof(bi));memset(&reserved,0,sizeof(reserved));reserved.count=1;\n    bi.cmdline=n==3 ? \"vos.apps=1\" : n==4 ? \"vos.apps=1 vos.gui=1\" : \"vos.apps=1 vos.gui=1 vos.gui-verify=1\";\n    bi.module_count=n;\n    for(unsigned i=0;i<n;i++) {\n        uint64_t start=0x100000+(uint64_t)i*0x200000;\n        bi.modules[i]=(struct boot_module){start,start+size_[i],names[i]};\n    }\n}\nstatic void denied(void) {\n    CHECK(!app_payload_prepare(&bi,&reserved));\n    CHECK(app_payload_catalog()->valid==0 && app_payload_catalog()->count==0);\n}\nint main(int argc,char **argv) {\n    CHECK(argc==6);\n    for(unsigned i=0;i<5;i++) {\n        FILE *file=fopen(argv[i+1],\"rb\");CHECK(file);\n        CHECK(!fseek(file,0,SEEK_END));long n=ftell(file);CHECK(n>0 && n<=1048576);\n        rewind(file);size_[i]=(size_t)n;memory[i]=malloc(size_[i]+32);CHECK(memory[i]);\n        memset(memory[i],0xa5,size_[i]+32);image[i]=memory[i]+16;\n        CHECK(fread(image[i],1,size_[i],file)==size_[i]);CHECK(!fclose(file));\n    }\n    for(unsigned n=3;n<=5;n++) {\n        prepare(n);CHECK(app_payload_prepare(&bi,&reserved)==1);\n        CHECK(app_payload_catalog()->valid==1 && app_payload_catalog()->count==n);\n        for(unsigned i=0;i<n;i++)CHECK(app_catalog_at(app_payload_catalog(),i)->identity==0x1001+i);\n    }\n    prepare(5);bi.module_count=6;denied();\n    prepare(4);bi.module_count=3;denied();\n    prepare(3);bi.modules[1].cmdline=\"vsh\";denied();\n    prepare(5);bi.modules[4].cmdline=\"unknown\";denied();\n    prepare(5);bi.modules[4].end=bi.modules[4].start;denied();\n    prepare(5);bi.modules[4].end=BOOT_MAP_LIMIT+1;denied();\n    prepare(5);coverage=0;denied();coverage=1;\n    prepare(5);unsigned char saved=image[3][0];image[3][0]=0;denied();image[3][0]=saved;\n    prepare(5);reserved.count=RESV_MAX+1;denied();\n    prepare(3);CHECK(app_payload_prepare(&bi,&reserved)==1);\n    for(unsigned i=0;i<5;i++) {\n        for(unsigned j=0;j<16;j++)CHECK(memory[i][j]==0xa5 && memory[i][size_[i]+16+j]==0xa5);\n        free(memory[i]);\n    }\n    printf(\"catalog bootstrap PASS %u assertions; actual 3/4/5 ELF payloads, strict negative controls and fail-closed clearing\\n\",checks);\n    return 0;\n}\n"

def catalog_image_recovery(out,phase,user,cfg,efi):
    previous=phase/'image-recipe-recovery-1';old_kernel=previous/'image/kernel'
    prior=json.loads((previous/'result.json').read_text())
    need(not prior['passed'] and prior['error']=='guest failure','preserved native failure')
    pins={row['path']:row['sha256'] for row in json.loads((previous/'manifest.json').read_text())}
    def verified(path):
        relative=str(path.relative_to(previous))
        need(relative in pins and sha(path)==pins[relative],'read-only verified build input '+relative)
        return path
    built_user=previous/'image/user'
    apps={n:user/(n+'.elf') for n in ('vsh','hello','rixie')}
    apps.update({n:verified(built_user/(n+'.elf')) for n in ('gui-demo','gui-probe')})
    for n,path in apps.items():need(sha(path)==prior['images']['bios']['files']['/boot/'+n+'.elf'],'exact actual ELF '+n)
    source=out/'catalog-bootstrap.c'
    source.write_text(CATALOG_HARNESS.replace('PAYLOAD_SOURCE',str(REPO/'arch/x86_64/app_payload.c')))
    cc=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Iarch/x86_64','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
        '-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-DVOS_GUI_VERIFY',
        str(source),'arch/x86_64/app_catalog.c','arch/x86_64/elf.c','arch/x86_64/gui_verify.c','-o',str(out/'catalog-bootstrap')]
    run(cc,out/'catalog-bootstrap-build.log',60)
    run([str(out/'catalog-bootstrap')]+[str(apps[n]) for n in ('vsh','hello','rixie','gui-demo','gui-probe')],out/'catalog-bootstrap.log',30)
    need('catalog bootstrap PASS' in (out/'catalog-bootstrap.log').read_text(),'actual bootstrap regression')
    kernel=out/'image/kernel';kernel.mkdir(parents=True)
    lines=(previous/'image-build.log').read_text().splitlines()
    compiles=[shlex.split(line) for line in lines if line.startswith('gcc ') and ' -c arch/x86_64/app_payload.c -o ' in line]
    need(len(compiles)==1,'unique accepted payload compilation recipe')
    compile_=compiles[0];need(compile_[-2:]==['-o',str(old_kernel/'app_payload.o')],'original compile output')
    compile_[-1]=str(kernel/'app_payload.o')
    links=[shlex.split(line) for line in lines if line.startswith('ld ') and ' -o '+str(old_kernel/'vos.bin')+' ' in line]
    need(len(links)==1,'unique accepted kernel link recipe')
    link=links[0];link[link.index('-o')+1]=str(kernel/'vos.bin')
    reused=[]
    for i,value in enumerate(link):
        if not value.endswith('.o'):continue
        path=Path(value)
        if path.name=='app_payload.o':link[i]=str(kernel/'app_payload.o')
        else:verified(path);reused.append(dict(path=str(path),sha256=sha(path)))
    for command in (cc,compile_,link):
        target=Path(command[command.index('-o')+1]).resolve()
        need(target.is_relative_to(out),'bounded generated output')
    run(compile_,out/'payload-build.log',60);run(link,out/'kernel-link.log',60)
    run(['python3','-I','-B','scripts/check-kernel-layout.py',str(kernel/'vos.bin')],out/'kernel-layout.log')
    run(['grub-file','--is-x86-multiboot2',str(kernel/'vos.bin')],out/'multiboot.log')
    need(cfg.read_bytes()==verified(previous/'gui-verify.cfg').read_bytes(),'identical verification boot configuration')
    bios_input=verified(old_kernel/'vos.iso')
    with bios_input.open('rb') as reader:reader.seek(512);need(reader.read(8)!=b'EFI PART','known BIOS-only replay input, no imported GPT')
    bios_command=['xorriso','-indev',str(bios_input),'-outdev',str(kernel/'vos.iso'),'-boot_image','any','replay','-map',str(kernel/'vos.bin'),'/boot/vos.bin','-commit','-end']
    run(bios_command,out/'bios-image-build.log',90)
    stage=kernel/'isodir-uefi';(stage/'boot/grub').mkdir(parents=True)
    shutil.copyfile(kernel/'vos.bin',stage/'boot/vos.bin')
    for name,path in apps.items():shutil.copyfile(path,stage/'boot'/(name+'.elf'))
    (stage/'boot/grub/grub.cfg').write_bytes((REPO/'arch/x86_64/grub-uefi-head.cfg').read_bytes()+cfg.read_bytes())
    uefi_command=['grub-mkrescue','-d',str(efi),'-o',str(kernel/'vos-uefi.iso'),str(stage)]
    run(uefi_command,out/'uefi-image-build.log',120)
    (out/'recovery-build.json').write_text(json.dumps(dict(passed=True,compile=compile_,link=link,reused_objects=reused,BIOS=bios_command,UEFI=uefi_command,
        unchanged_ELFs={name:sha(path) for name,path in apps.items()},bootstrap_regression=(out/'catalog-bootstrap.log').read_text(),
        prior_legacy_elf_tests=str(previous/'legacy-elf-tests.log'),new_execution_provision=40*1024**2),indent=2)+'\n')
    return dict(passed=True,bootstrap=(out/'catalog-bootstrap.log').read_text(),reused_objects=len(reused),no_Rust_rebuild=True,no_legacy_test_rerun=True)

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);args=ap.parse_args()
    out=args.output.resolve();phase=Path(os.environ['N6_PHASE_ROOT']).resolve()
    budget=json.loads((phase/'N62-BUDGET.json').read_text())
    catalog_recovery=out==phase/'catalog-recovery-1'
    image_recovery=out==phase/'image-recipe-recovery-1' or catalog_recovery
    recovering=out==phase/'verification-continuation-1' or image_recovery
    need((out==phase/'attempt-1' or recovering) and not out.exists() and budget['attempt_limit_bytes']==104*1024**2 and budget['phase_limit_bytes']==128*1024**2,'fresh admitted N6.2 output')
    if recovering:
        receipt=json.loads((phase/'final'/out.name/'RECOVERY.json').read_text())
        owner='call_258b4edfd53241c0a130f729e1eee097' if catalog_recovery else 'call_579b7edb7a244d52a9c7f163b32c5139' if image_recovery else 'call_90dc41d7ff0e49d9b2f9b3b00aaa004e'
        previous_output=phase/('image-recipe-recovery-1' if catalog_recovery else 'verification-continuation-1' if image_recovery else 'attempt-1')
        need(receipt['identity']==out.name and receipt['question_id']==owner and receipt['failed_manifest_sha256']==sha(previous_output/'manifest.json'),'bound recovery authority')
    out.mkdir();(out/'tmp').mkdir();os.environ['TMPDIR']=str(out/'tmp')
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));file_limit=(40 if catalog_recovery else 104)*1024**2
    resource.setrlimit(resource.RLIMIT_FSIZE,(file_limit,file_limit))
    result=dict(passed=False,scope='N6.2 native bounded GUI surfaces/events',physical_hardware_verified=False)
    try:
        inputs=[n for n in budget['allowed_source_files'] if not n.startswith('docs/') and (REPO/n).is_file()]
        inputs+=['scripts/test-user-elf.sh','arch/x86_64/exec.c','arch/x86_64/keyboard.c','arch/x86_64/input_console.c','arch/x86_64/desktop.c','arch/x86_64/font8x16.c','user/vsh/main.rs','user/rixie/main.rs','scripts/test-n6-desktop.py','scripts/test-app-handoff.py']
        result['source_sha256']={n:sha(REPO/n) for n in inputs}
        cc=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Iarch/x86_64','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
        service=['-DVOS_GUI_HOST','-DVOS_CONSOLE_HOST','tests/host/gui.c','arch/x86_64/gui.c','arch/x86_64/user_console.c','arch/x86_64/keyboard.c','arch/x86_64/input_console.c']
        render=['-DGUI_RENDER_HOST','tests/host/gui.c','arch/x86_64/desktop.c','arch/x86_64/font8x16.c']
        console=['-DVOS_CONSOLE_HOST','tests/host/user-console-handoff.c','arch/x86_64/user_console.c','arch/x86_64/keyboard.c','arch/x86_64/input_console.c']
        syscall=(REPO/'arch/x86_64/syscall.c').read_text()
        debug=syscall[syscall.index('void debug_emit('):syscall.index('/* debug_write')]
        (out/'debug.c').write_text('#include <stdint.h>\n'+debug)
        service+=[str(out/'debug.c')];console+=[str(out/'debug.c')]
        if recovering:
            previous=json.loads((phase/'attempt-1/result.json').read_text())
            need(not previous['passed'] and 'rustc' in previous['error'],'preserved failed host Rust test')
            for name,value in previous['source_sha256'].items():
                if name not in (('scripts/test-gui.py','Makefile','arch/x86_64/app_payload.c') if catalog_recovery else ('scripts/test-gui.py','Makefile') if image_recovery else ('scripts/test-gui.py',)):need(sha(REPO/name)==value,'reused sanitizer source identity '+name)
            result['host']=previous['host']
            result['host_reused_from']=str(phase/'attempt-1')
            result['sanitizer_campaign_rerun']=False
        else:
            result['host']={}
            for name,files in [('gui-service',service),('gui-renderer',render),('console-regression',console)]:
                run(cc+['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']+files+['-o',str(out/name)],out/(name+'-build.log'),60)
                run([str(out/name)],out/(name+'.log'),60)
                need('PASS' in (out/(name+'.log')).read_text(),'passing actual host component '+name)
                result['host'][name]=(out/(name+'.log')).read_text()
        if image_recovery:
            previous_rust=json.loads((phase/'verification-continuation-1/result.json').read_text())
            need(previous_rust['error']=='GUI Make dry-run' and '2 passed; 0 failed' in previous_rust['rust_tests'],'passing prior Rust ABI tests')
            for name,value in previous_rust['source_sha256'].items():
                if name not in (('scripts/test-gui.py','Makefile','arch/x86_64/app_payload.c') if catalog_recovery else ('scripts/test-gui.py','Makefile')):need(sha(REPO/name)==value,'reused Rust/performance source identity '+name)
            result['rust_tests']=previous_rust['rust_tests']
            result['rust_reused_from']=str(phase/'verification-continuation-1')
            result['optimized_host_baseline']=previous_rust['optimized_host_baseline']
        else:
            rustc=os.environ['RUSTC']
            run([rustc,'--cfg','test','--crate-name','vos','--crate-type=rlib','--edition=2024','user/rust/vos.rs','-o',str(out/'libvos.rlib')],out/'rust-vos.log')
            run([rustc,'--crate-name','vos_gui','--crate-type=rlib','--edition=2024','-L','dependency='+str(out),'--extern','vos='+str(out/'libvos.rlib'),'user/rust/gui.rs','-o',str(out/'libvos_gui.rlib')],out/'rust-gui.log')
            run([rustc,'--test','--edition=2024','-L','dependency='+str(out),'--extern','vos_gui='+str(out/'libvos_gui.rlib'),'tests/rust/gui.rs','-o',str(out/'rust-tests')],out/'rust-test-build.log')
            run([str(out/'rust-tests')],out/'rust-tests.log')
            result['rust_tests']=(out/'rust-tests.log').read_text()
            run(cc+['-O2']+service+['-o',str(out/'gui-release')],out/'release-build.log')
            samples=[]
            for i in range(3):
                start=time.perf_counter_ns();run([str(out/'gui-release')],out/('release-%d.log'%i),60);samples.append(time.perf_counter_ns()-start)
            result['optimized_host_baseline']=dict(samples_ns=samples,median_ns=sorted(samples)[1],new_workload_no_comparable_prior_baseline=True,includes_process_start_and_assertions=True)
        old=REPO/'build/app-console-20261025T051241Z/attempt-1';user=old/'image/user'
        accepted=json.loads((REPO/'docs/results/app-handoff/result.json').read_text())
        for n in ('vsh','hello','rixie'):
            need(sha(user/(n+'.elf'))==accepted['application_sha256'][n],'accepted ELF input '+n)
        disk=old/'disk.img';disk_hash=sha(disk);need(disk_hash==accepted['disk_sha256'],'read-only disk identity')
        cfg=out/'gui-verify.cfg'
        text=(REPO/'arch/x86_64/grub-gui.cfg').read_text().replace('vos.gui=1','vos.gui=1 vos.gui-verify=1').replace('    boot\n','    module2 /boot/gui-probe.elf gui-probe\n    boot\n')
        cfg.write_text(text)
        efi=Path('/home/veasnawt/vos-evidence/n36-stage3-gatea-cont-20261021T220119Z/efi-tools/usr/lib/grub/x86_64-efi')
        need((efi/'kernel.img').is_file(),'accepted EFI producer inputs')
        if catalog_recovery:
            result['catalog_image_recovery']=catalog_image_recovery(out,phase,user,cfg,efi)
            previous_image=json.loads((phase/'image-recipe-recovery-1/result.json').read_text())
            result['legacy_elf_tests']=previous_image['legacy_elf_tests']
            result['legacy_elf_reused_from']=str(phase/'image-recipe-recovery-1')
            built_user=phase/'image-recipe-recovery-1/image/user'
        else:
            argv=['make','--no-print-directory','gui-uefi-iso','GUI_BUILD='+str(out/'image'),'GUI_APPS='+str(user),'GUI_VERIFY=1','GUI_CFG='+str(cfg),'GRUB_EFI_DIR='+str(efi)]
            dry=subprocess.run(argv[:2]+['-n']+argv[2:],cwd=REPO,capture_output=True,text=True)
            (out/'image-dry-run.log').write_text(dry.stdout+dry.stderr)
            need(dry.returncode==0,'GUI Make dry-run')
            need('-o '+str(out/'image/user/gui-demo.elf') in dry.stdout and '-o '+str(out/'image/user/gui-probe.elf') in dry.stdout,'nonempty intended application linker outputs')
            validated_outputs=[]
            for line in dry.stdout.splitlines():
                try:words=shlex.split(line)
                except ValueError:continue
                if '-o' in words:
                    index=words.index('-o')
                    need(index+1<len(words),'nonempty generated output')
                    destination=Path(words[index+1])
                    if not destination.is_absolute():destination=REPO/destination
                    need(destination.resolve().is_relative_to(out),'generated output outside admitted root: '+str(destination))
                    validated_outputs.append(str(destination))
            need(validated_outputs and not re.search(r'(?:-o|mkdir -p|rm -rf)\s+build/x86_64',dry.stdout),'no default protected output targets')
            (out/'dry-run-output-validation.json').write_text(json.dumps(dict(passed=True,output_root=str(out),outputs=validated_outputs,forwarded_USER_BUILD=str(out/'image/user')),indent=2)+'\n')
            run(argv,out/'image-build.log',180)
            for name in ('gui-demo','gui-probe'):
                run(['scripts/check-user-elf.sh',str(out/'image/user'/(name+'.elf'))],out/(name+'-elf.log'),30)
                need('VOS ABI 5' in (out/(name+'-elf.log')).read_text(),'actual ABI5 ELF')
            run(['make','--no-print-directory','ARCH=x86_64','BUILD='+str(out/'image/kernel'),'USER_BUILD='+str(out/'image/user'),'ELFCHECK='+str(out/'image/kernel/tools/elfcheck'),'user-test'],out/'legacy-elf-tests.log',120)
            result['legacy_elf_tests']=(out/'legacy-elf-tests.log').read_text()
            built_user=out/'image/user'
        expected={'/boot/vos.bin':out/'image/kernel/vos.bin','/boot/grub/grub.cfg':cfg}
        expected.update({'/boot/'+n+'.elf':user/(n+'.elf') for n in ('vsh','hello','rixie')})
        expected.update({'/boot/'+n+'.elf':built_user/(n+'.elf') for n in ('gui-demo','gui-probe')})
        result['images']={};result['firmware']={}
        for fw,name in [('bios','vos.iso'),('uefi','vos-uefi.iso')]:
            folder=out/('validate-'+fw);folder.mkdir();current=dict(expected)
            if fw=='uefi':current['/boot/grub/grub.cfg']=out/'image/kernel/isodir-uefi/boot/grub/grub.cfg'
            result['images'][fw]=validate_image(out/'image/kernel'/name,current,fw=='uefi',folder)
            result['firmware'][fw]=boot(out,fw,out/'image/kernel'/name,disk)
        need(sha(disk)==disk_hash,'immutable disk after firmware sessions')
        need(all(sha(REPO/n)==h for n,h in result['source_sha256'].items()),'tested source identities')
        result.update(passed=True,surface_maximum_bytes=163840,maximum_frames=40,maximum_surfaces=1,queue_capacity=16,upload_maximum_bytes=4096,app_ELF_sha256={n:sha(built_user/(n+'.elf')) for n in ('gui-demo','gui-probe')},reused_app_ELF_sha256={n:sha(user/(n+'.elf')) for n in ('vsh','hello','rixie')},disk_sha256=disk_hash)
    except BaseException as ex:
        result['error']=str(ex);raise
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        entries=[dict(path=str(p.relative_to(out)),sha256=sha(p),size=p.stat().st_size) for p in sorted(out.rglob('*')) if p.is_file()]
        (out/'manifest.json').write_text(json.dumps(entries,indent=2)+'\n')
        print(json.dumps(result),flush=True)
if __name__=='__main__':main()
