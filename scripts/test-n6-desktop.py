#!/usr/bin/env python3
"""Focused N6 scene/console verification. One fresh guarded attempt; no historic writes."""
import argparse,hashlib,json,os,re,resource,runpy,shutil,subprocess,time,shlex
from pathlib import Path
REPO=Path(__file__).resolve().parents[1]
helpers=runpy.run_path(str(REPO/'scripts/test-app-handoff.py'),run_name='desktop_helpers')
run,QMP,capture,sha,need=(helpers[n] for n in ('run','QMP','capture','sha','require'))
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
            first=wait(lambda d:b'user[vsh]: VOS> ' in d,40)
            need(b'desktop: scene ready; kernel-owned BGRX surface, bounded console viewport' in first,'actual desktop enabled')
            need(b'catalog: vsh id=4097 ABI=4 policy=1' in first,'real ABI4 vsh')
            need(b'BGRX' in first and b'screen: framebuffer' in first,'validated boot framebuffer')
            q=QMP(socket)
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
                        screenshot=picture,elapsed_seconds=time.monotonic()-start)
        finally:
            if q:q.close()
            if proc and proc.poll() is None:proc.terminate()
            if proc:
                try:proc.wait(timeout=3)
                except subprocess.TimeoutExpired:proc.kill();proc.wait()
def iso_records(image,log=None):
    command=['xorriso','-indev',str(image),'-find','/','-type','f','-exec','report_lba','--']
    proc=subprocess.run(command,cwd=REPO,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True,text=True)
    if log:log.write_text(proc.stdout+proc.stderr)
    result={}
    for line in proc.stdout.splitlines():
        m=re.match(r"File data lba:\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*'([^']+)'",line)
        if not m:continue
        need(int(m[1])==0 and m[5] not in result,'single plain ISO extent')
        offset=int(m[2])*2048;length=int(m[4])
        need(length<=int(m[3])*2048 and offset+length<=image.stat().st_size,'ISO file extent bounds')
        result[m[5]]=(offset,length)
    return result
def iso_bytes(image,records,name):
    need(name in records,'required ISO boot file '+name)
    offset,size=records[name];need(size<=32*1024**2,'bounded ISO file inspection')
    with image.open('rb') as f:f.seek(offset);data=f.read(size)
    need(len(data)==size,'complete ISO file')
    return data
def image_sha(data):return hashlib.sha256(data).hexdigest()
def gpt_validate(image):
    import struct,zlib,uuid
    length=image.stat().st_size
    with image.open('rb') as f:
        mbr=f.read(512);need(mbr[510:512]==b'\x55\xaa' and any(mbr[446+i*16+4]==0xee for i in range(4)),'protective MBR')
        def header(lba):
            need(0<=lba*512<=length-512,'GPT header position')
            f.seek(lba*512);block=f.read(512)
            need(block[:8]==b'EFI PART','GPT signature')
            count=struct.unpack_from('<I',block,12)[0];need(92<=count<=512,'GPT header size')
            recorded=struct.unpack_from('<I',block,16)[0];copy=bytearray(block[:count]);copy[16:20]=b'\0'*4
            need(zlib.crc32(copy)&0xffffffff==recorded,'GPT header CRC')
            own,backup,first,last=struct.unpack_from('<QQQQ',block,24)
            table_lba,n,stride,crc=struct.unpack_from('<QIII',block,72)
            need(own==lba and 0<n<=4096 and 128<=stride<=1024 and n*stride<=1024**2,'GPT table bounds')
            need(table_lba*512+n*stride<=length,'GPT table file bounds')
            f.seek(table_lba*512);table=f.read(n*stride);need(zlib.crc32(table)&0xffffffff==crc,'GPT array CRC')
            return dict(own=own,backup=backup,first=first,last=last,table=table,n=n,stride=stride)
        primary=header(1);backup=header(primary['backup'])
        need(backup['backup']==1 and primary['table']==backup['table'],'matching backup GPT')
        partitions=[]
        for n in range(primary['n']):
            entry=primary['table'][n*primary['stride']:(n+1)*primary['stride']]
            if entry[:16]==b'\0'*16:continue
            start,end=struct.unpack_from('<QQ',entry,32)
            need(primary['first']<=start<=end<=primary['last'] and (end+1)*512<=length,'GPT partition bounds')
            partitions.append(dict(index=n+1,start=start,end=end,type=str(uuid.UUID(bytes_le=entry[:16])),name=entry[56:128].decode('utf-16le').rstrip('\0')))
        ordered=sorted(partitions,key=lambda x:x['start'])
        need(all(a['end']<b['start'] for a,b in zip(ordered,ordered[1:])),'GPT partitions do not overlap')
        return partitions
def validate_image(image,expected,uefi,folder=None):
    import struct
    records=iso_records(image,folder/'iso-files.txt' if folder else None);files={}
    for name,path in expected.items():
        value=iso_bytes(image,records,name);need(image_sha(value)==sha(path),'boot-file SHA-256 '+name)
        files[name]=image_sha(value)
    proc=subprocess.run(['xorriso','-indev',str(image),'-report_system_area','plain','-report_el_torito','plain'],
                        cwd=REPO,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True,text=True)
    text=proc.stdout+proc.stderr
    if folder:(folder/'partition-report.txt').write_text(text)
    platform='UEFI' if uefi else 'BIOS'
    need(re.search(r'El Torito boot img\s*:\s*\d+\s+'+platform+r'\b',text,re.I),'expected firmware boot catalog')
    result=dict(passed=True,platform=platform,files=files,ISO_sha256=sha(image))
    if uefi:
        parts=gpt_validate(image);efis=[p for p in parts if p['type']=='c12a7328-f81f-11d2-ba4b-00a0c93ec93b']
        need(len(efis)==1 and '/efi.img' in records,'one EFI system partition')
        offset,size=records['/efi.img'];part=efis[0]
        need(part['start']*512==offset and (part['end']-part['start']+1)*512==size,'ESP matches ISO boot image')
        efi=iso_bytes(image,records,'/efi/boot/bootx64.efi')
        need(efi[:2]==b'MZ' and len(efi)>=64,'EFI DOS header')
        pe=struct.unpack_from('<I',efi,60)[0]
        need(pe+24+112<=len(efi) and efi[pe:pe+4]==b'PE\0\0','EFI PE bounds')
        need(struct.unpack_from('<H',efi,pe+4)[0]==0x8664 and struct.unpack_from('<H',efi,pe+24)[0]==0x20b,'EFI x86_64 PE32+')
        need(struct.unpack_from('<H',efi,pe+24+68)[0]==10,'EFI application subsystem')
        embedded=subprocess.check_output(['mtype','-i',str(image)+'@@'+str(offset),'::/EFI/BOOT/BOOTX64.EFI'],cwd=REPO)
        need(embedded==efi,'EFI executable in FAT matches ISO boot file')
        result.update(partitions=parts,GPT_primary_backup_CRC=True,nonoverlapping_GPT=True,EFI_PE32_valid=True,ESP_file_sha256=image_sha(efi))
    return result

def repair_main(out,phase):
    import shlex
    control=phase/'final/layout-image-repair-1'
    admission=json.loads((control/'ADMISSION.json').read_text())
    need(admission['identity']=='layout-image-repair-1' and admission['owner_sha256']=='01ef126fd9902b8093fa12920b613102e5852fb56d82d6df8f1c8a8c3924c6a5','bound image-repair authorization')
    budget=json.loads((phase/'N6-BUDGET.json').read_text())
    need(admission['passed'] and admission['phase']==str(phase) and admission['accepted_main']==budget['accepted_main'] and budget['phase_limit_bytes']==96*1024**2 and budget['attempt_limit_bytes']==72*1024**2,'unchanged bound N6 admission')
    failed=phase/'layout-refinement-1'
    need(sha(failed/'manifest.json')==admission['failed_manifest_sha256'],'preserved failed image manifest')
    need(not out.exists() and out==phase/'layout-image-repair-1','fresh image-repair output')
    previous=json.loads((failed/'result.json').read_text())
    need(not previous['passed'] and 'xorriso' in previous['error'],'honest previous failure')
    # Image/tooling changes do not invalidate the passing drawing C tests.
    for n,h in previous['source_sha256'].items():
        if n!='scripts/test-n6-desktop.py':need(sha(REPO/n)==h,'reused host evidence source '+n)
    out.mkdir();(out/'tmp').mkdir();os.environ['TMPDIR']=str(out/'tmp')
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));resource.setrlimit(resource.RLIMIT_FSIZE,(30*1024**2,30*1024**2))
    result=dict(passed=False,scope='layout-image-repair-1: image construction and pending native checks only',
                host_reused_from=str(failed),host_log=(failed/'host.log').read_text(),
                console_regression=(failed/'console-regression.log').read_text(),
                optimized_host_baseline=previous['optimized_host_baseline'],
                source_sha256={n:sha(REPO/n) for n in previous['source_sha256']})
    try:
        need('562324 assertions' in result['host_log'] and 'PASS' in result['console_regression'],'reused passing host logs')
        original=phase/'attempt-1';kernel=failed/'kernel/vos.bin'
        user=REPO/'build/app-console-20261025T051241Z/attempt-1/image/user'
        stage=out/'stage';(stage/'boot/grub').mkdir(parents=True)
        shutil.copyfile(kernel,stage/'boot/vos.bin')
        for name in ('vsh','hello','rixie'):
            need(sha(user/(name+'.elf'))==previous['reused_applications'][name],'accepted application ELF')
            shutil.copyfile(user/(name+'.elf'),stage/'boot'/(name+'.elf'))
        config=(REPO/'arch/x86_64/grub-uefi-head.cfg').read_bytes()+(REPO/'arch/x86_64/grub-desktop.cfg').read_bytes()
        need(config==(original/'kernel/isodir-uefi/boot/grub/grub.cfg').read_bytes(),'established UEFI configuration')
        (stage/'boot/grub/grub.cfg').write_bytes(config)
        lines=[shlex.split(line) for line in (original/'image-build.log').read_text().splitlines() if line.startswith('grub-mkrescue -d ')]
        need(len(lines)==1,'unique previously successful UEFI producer')
        accepted=lines[0];efi_dir=Path(accepted[accepted.index('-d')+1])
        need((efi_dir/'kernel.img').is_file() and (efi_dir/'multiboot2.mod').is_file(),'accepted EFI modules')
        iso=out/'vos-uefi.iso'
        run(['grub-mkrescue','-d',str(efi_dir),'-o',str(iso),str(stage)],out/'uefi-image-build.log',120)
        result['producer']=dict(command=['grub-mkrescue','-d',str(efi_dir),'-o',str(iso),str(stage)],fresh_stage=True,imported_GPT_metadata=False)
        expected={ '/boot/vos.bin':kernel,**{'/boot/'+n+'.elf':user/(n+'.elf') for n in ('vsh','hello','rixie')}}
        uefi_expected={**expected,'/boot/grub/grub.cfg':stage/'boot/grub/grub.cfg'}
        bios_expected={**expected,'/boot/grub/grub.cfg':REPO/'arch/x86_64/grub-desktop.cfg'}
        result['image_validation']={}
        for fw,image,inputs in [('bios',failed/'kernel/vos.iso',bios_expected),('uefi',iso,uefi_expected)]:
            folder=out/('validate-'+fw);folder.mkdir()
            result['image_validation'][fw]=validate_image(image,inputs,fw=='uefi',folder)
        result['firmware']={}
        disk=REPO/'build/app-console-20261025T051241Z/attempt-1/disk.img';disk_hash=sha(disk)
        for fw,image in [('bios',failed/'kernel/vos.iso'),('uefi',iso)]:
            result['firmware'][fw]=boot(out,fw,image,disk)
        need(sha(disk)==disk_hash,'read-only fixture unchanged')
        need(all(sha(REPO/n)==h for n,h in result['source_sha256'].items()),'verified source unchanged')
        result.update(passed=True,physical_hardware_verified=False,new_GUI_ABI=False,framebuffer_user_mapping=False,
                      normal_console_fallback='historical host-tested behavior',no_new_C_builds=True,no_host_sanitizer_rerun=True,
                      reused_applications=previous['reused_applications'])
    except BaseException as ex:
        result['error']=str(ex);raise
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        entries=[dict(path=str(p.relative_to(out)),sha256=sha(p),size=p.stat().st_size) for p in sorted(out.rglob('*')) if p.is_file()]
        (out/'manifest.json').write_text(json.dumps(entries,indent=2)+'\n')
        print(json.dumps(result),flush=True)

def main():
    ap=argparse.ArgumentParser();ap.add_argument('--output',type=Path,required=True);ap.add_argument('--refinement',action='store_true');ap.add_argument('--image-repair',action='store_true');options=ap.parse_args();out=options.output.resolve()
    phase=Path(os.environ.get('N6_PHASE_ROOT','')).resolve()
    if options.image_repair:
        need(not options.refinement,'one workflow mode')
        return repair_main(out,phase)
    need((phase/'N6-BUDGET.json').is_file() and out==phase/('layout-refinement-1' if options.refinement else 'attempt-1') and not out.exists(),'fresh admitted N6 output')
    if options.refinement:
        receipt=json.loads((phase/'final/layout-refinement-1/ADMISSION.json').read_text())
        need(receipt['identity']=='layout-refinement-1' and receipt['original_manifest_sha256']==sha(phase/'attempt-1/manifest.json'),'bound refinement authority')
        need(receipt['owner_sha256']=='23861f3968e02e8c7140b062d869b785ccb120cc774bfaa01d35395fdccde9c5','owner authorization')
    budget=json.loads((phase/'N6-BUDGET.json').read_text())
    need(budget['phase_limit_bytes']==96*1024**2 and budget['attempt_limit_bytes']==72*1024**2,'approved N6 limits')
    out.mkdir();(out/'tmp').mkdir();os.environ['TMPDIR']=str(out/'tmp')
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));resource.setrlimit(resource.RLIMIT_FSIZE,(72*1024**2,72*1024**2))
    result=dict(passed=False,scope='N6 kernel-owned scene with real existing ring-3 shell; no GUI ABI')
    try:
        names=[n for n in budget['allowed_source_files'] if not n.startswith('docs/') and (REPO/n).is_file()]
        names+=['arch/x86_64/bootinfo.c','arch/x86_64/devmap.c','arch/x86_64/user_console.c','arch/x86_64/exec.c','arch/x86_64/keyboard.c','arch/x86_64/input_console.c','user/vsh/main.rs','user/rixie/main.rs','scripts/test-app-handoff.py','scripts/test-native-vsh.py']
        result['source_sha256']={n:sha(REPO/n) for n in names}
        cc=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Iarch/x86_64','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
        inputs=['tests/host/desktop.c','arch/x86_64/desktop.c','arch/x86_64/font8x16.c']
        run(cc+['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']+inputs+['-o',str(out/'desktop-host')],out/'host-build.log')
        run([str(out/'desktop-host')],out/'host.log',30)
        run(cc+['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','tests/host/screen-input.c','arch/x86_64/desktop.c','arch/x86_64/font8x16.c','-o',str(out/'console-regression')],out/'console-regression-build.log')
        run([str(out/'console-regression')],out/'console-regression.log',30)
        run(cc+['-O2']+inputs+['-o',str(out/'desktop-release')],out/'release-build.log')
        samples=[]
        for n in range(5):
            start=time.perf_counter_ns();run([str(out/'desktop-release')],out/('release-'+str(n)+'.log'));samples.append(time.perf_counter_ns()-start)
        result['optimized_host_baseline']=dict(samples_ns=samples,median_ns=sorted(samples)[2],includes_process_start_and_assertions=True,not_native_hardware_latency=True,no_matching_previous_baseline=True)
        old=REPO/'build/app-console-20261025T051241Z/attempt-1'
        user=old/'image/user';accepted=json.loads((REPO/'docs/results/app-handoff/result.json').read_text())
        result['reused_applications']={}
        for n in ('vsh','hello','rixie'):
            need(sha(user/(n+'.elf'))==accepted['application_sha256'][n],'immutable accepted ELF '+n)
            run(['scripts/check-user-elf.sh',str(user/(n+'.elf'))],out/(n+'-elf.log'))
            result['reused_applications'][n]=sha(user/(n+'.elf'))
        disk=old/'disk.img';disk_hash=sha(disk);need(disk_hash==accepted['disk_sha256'],'unchanged read-only fixture')
        kernel=out/'kernel'
        if options.refinement:
            kernel.mkdir()
            original=phase/'attempt-1/kernel';build_log=(phase/'attempt-1/image-build.log').read_text().splitlines()
            pins={row['path']:row['sha256'] for row in json.loads((phase/'attempt-1/manifest.json').read_text())}
            for name in ('desktop','screen'):
                src='arch/x86_64/'+name+'.c'
                lines=[shlex.split(line) for line in build_log if line.startswith('gcc ') and ' -c '+src+' -o ' in line]
                need(len(lines)==1,'unique accepted compile recipe '+name)
                command=lines[0];need(command[-2]=='-o' and command[-1]==str(original/(name+'.o')),'accepted output path')
                command[-1]=str(kernel/(name+'.o'))
                run(command,out/(name+'-build.log'),60)
            links=[shlex.split(line) for line in build_log if line.startswith('ld ') and ' -o '+str(original/'vos.bin')+' ' in line]
            need(len(links)==1,'unique accepted link recipe');link=links[0]
            link[link.index('-o')+1]=str(kernel/'vos.bin')
            for i,arg in enumerate(link):
                if not arg.endswith('.o'):continue
                name=Path(arg).stem
                if name in ('desktop','screen'):link[i]=str(kernel/(name+'.o'))
                else:
                    key=str(Path(arg).relative_to(phase/'attempt-1'))
                    need(sha(Path(arg))==pins[key],'read-only accepted object '+name)
            run(link,out/'kernel-link.log',60)
            run(['python3','-I','-B','scripts/check-kernel-layout.py',str(kernel/'vos.bin')],out/'kernel-layout.log')
            run(['grub-file','--is-x86-multiboot2',str(kernel/'vos.bin')],out/'multiboot.log')
            # Replay accepted firmware boot metadata into new images, read-only input.
            for fw,name in [('bios','vos.iso'),('uefi','vos-uefi.iso')]:
                image=original/name;key=str(image.relative_to(phase/'attempt-1'))
                need(sha(image)==pins[key],'immutable input ISO')
                run(['xorriso','-indev',str(image),'-outdev',str(kernel/name),'-boot_image','any','replay',
                     '-map',str(kernel/'vos.bin'),'/boot/vos.bin','-commit','-end'],out/(fw+'-image-build.log'),60)
            result['reused_original_objects']=True
            result['original_runtime_evidence']=str(phase/'attempt-1/result.json')
            result['scope']='N6 layout-refinement-1 only; original attempt preserved'
        else:
            argv=['make','--no-print-directory','desktop-uefi-iso','DESKTOP_BUILD='+str(kernel),'DESKTOP_APPS='+str(user)]
            dry=subprocess.run(argv[:2]+['-n']+argv[2:],cwd=REPO,capture_output=True,text=True)
            need(dry.returncode==0,'image recipe dry run')
            (out/'image-dry-run.log').write_text(dry.stdout+dry.stderr)
            need('rustc' not in dry.stdout and str(user/'hello.elf') in dry.stdout,'no Rust rebuild or historical write target')
            run(argv,out/'image-build.log',120)
        result['images']={}
        for fw,filename in [('bios','vos.iso'),('uefi','vos-uefi.iso')]:
            extract=out/('extract-'+fw);extract.mkdir()
            for n in ('vos.bin','vsh.elf','hello.elf','rixie.elf'):
                run(['xorriso','-osirrox','on','-indev',str(kernel/filename),'-extract','/boot/'+n,str(extract/n)],out/(fw+'-'+n+'-extract.log'))
                target=kernel/n if n=='vos.bin' else user/n
                need(sha(extract/n)==sha(target),'exact payload '+fw+' '+n)
            result['images'][fw]=dict(sha256=sha(kernel/filename),kernel_sha256=sha(kernel/'vos.bin'))
        result['firmware']={}
        for fw,filename in [('bios','vos.iso'),('uefi','vos-uefi.iso')]:result['firmware'][fw]=boot(out,fw,kernel/filename,disk)
        need(sha(disk)==disk_hash,'fixture unchanged after runtime')
        need(all(sha(REPO/n)==h for n,h in result['source_sha256'].items()),'tested source unchanged')
        result.update(passed=True,host_log=(out/'host.log').read_text(),console_regression=(out/'console-regression.log').read_text(),physical_hardware_verified=False,framebuffer_user_mapping=False,new_GUI_ABI=False,normal_console_fallback=True)
    except BaseException as ex:
        result['error']=str(ex);raise
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        entries=[dict(path=str(p.relative_to(out)),sha256=sha(p),size=p.stat().st_size) for p in sorted(out.rglob('*')) if p.is_file()]
        (out/'manifest.json').write_text(json.dumps(entries,indent=2)+'\n');print(json.dumps(result),flush=True)
if __name__=='__main__':main()
