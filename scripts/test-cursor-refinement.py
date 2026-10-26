#!/usr/bin/env python3
"""One guarded N6.3 visual refinement; normal previews have no test deadline."""
import argparse,ast,hashlib,json,os,re,resource,runpy,shutil,stat,subprocess,time
from pathlib import Path
REPO=Path(__file__).resolve().parents[1]
P=runpy.run_path(str(REPO/'scripts/test-pointer.py'),run_name='cursor_preserved_pointer_helpers')
run,sha,need,validate_image=(P[n] for n in ('run','sha','need','validate_image'))
POLYGON=[[0,0],[0,23],[6,18],[11,27],[15,25],[11,17],[20,17]]
def allocated(p):
    p=Path(p);s=p.lstat()
    return s.st_blocks*512+(sum(allocated(x) for x in p.iterdir()) if p.is_dir() else 0)
def inside(x,y):
    value=False
    for i,(bx,by) in enumerate(POLYGON):
        ax,ay=POLYGON[i-1]
        cross=(bx-ax)*(y-ay)-(x-ax)*(by-ay)
        if cross==0 and min(ax,bx)<=x<=max(ax,bx) and min(ay,by)<=y<=max(ay,by):return True
        if (ay>y)!=(by>y) and ((by>ay and cross>0) or (by<ay and cross<0)):value=not value
    return value
def geometry(path,x,y,panel=None):
    w,h,pixel=P['rgb'](Path(path));need(w==1024 and h==768,'native mode unchanged')
    count=white=0
    for ly in range(28):
        for lx in range(21):
            if x+lx>=w or y+ly>=h or not inside(lx,ly):continue
            inner=all(inside(nx,ny) for nx,ny in ((lx-1,ly),(lx+1,ly),(lx,ly-1),(lx,ly+1)))
            need(pixel(x+lx,y+ly)==(0xffffff if inner else 0x101a2a),'exact cursor mask/outline/hotspot pixel')
            count+=1;white+=int(inner)
    need(count>0 and pixel(x,y)==0x101a2a,'exact visible tip')
    if panel is not None:
        need(pixel(x+31,y+31)==panel,'transparent canvas and restored application pixels')
    return dict(passed=True,canvas=[32,32],visible_geometry=[21,28],hotspot=[x,y],checked_ink=count,white_pixels=white)
def map_specs(pairs):
    need(pairs and len(pairs)<=8,'bounded image payload list')
    seen=set();answer=[]
    for dest,source in pairs:
        source=Path(source)
        need(dest.startswith('/boot/') and '..' not in dest.split('/') and '=' not in dest and dest not in seen,'unique safe ISO destination')
        need(source.is_absolute() and source.is_file() and not source.is_symlink() and '=' not in str(source),'regular explicit immutable input')
        seen.add(dest);answer.append(dest+'='+str(source))
    return answer
def iso_payloads(iso,expected):
    rows=P['G']['D']['iso_records'](iso)
    with Path(iso).open('rb') as f:
        for name,source in expected.items():
            need(name in rows,'mapped ISO file '+name);row=rows[name]
            # Reuse the accepted byte-offset ISO extraction helper.
            data=P['G']['D']['iso_bytes'](iso,rows,name);size=row[1]
            need(len(data)==size and hashlib.sha256(data).hexdigest()==sha(source),'mapped payload identity '+name)
def graft_tests(out):
    folder=out/'graft-tests';folder.mkdir();inputs=folder/'inputs';inputs.mkdir()
    (inputs/'kernel').write_bytes(b'VOS graft fixture kernel\n')
    (inputs/'app').write_bytes(bytes(range(64)))
    pairs=[('/boot/vos.bin',inputs/'kernel'),('/boot/app.elf',inputs/'app')]
    specs=map_specs(pairs);iso=folder/'fixture.iso'
    run(['xorriso','-as','mkisofs','-r','-graft-points','-o',str(iso),*specs],folder/'build.log')
    iso_payloads(iso,dict(pairs));cases=['real-graft-image-payloads']
    for name,bad in [('duplicate-target',pairs+[pairs[0]]),('parent-traversal',[('/boot/../escape',inputs/'app')]),('outside-boot',[('/etc/escape',inputs/'app')]),('missing-source',[('/boot/x',inputs/'missing')])]:
        rejected=False
        try:map_specs(bad)
        except RuntimeError:rejected=True
        need(rejected,'negative mapping '+name);cases.append(name)
    rejected=False
    try:iso_payloads(iso,{'/boot/vos.bin':inputs/'app'})
    except RuntimeError:rejected=True
    need(rejected,'real payload mismatch detected');cases.append('real-content-mismatch')
    rejected=False
    try:iso_payloads(iso,{'/boot/missing':inputs/'app'})
    except RuntimeError:rejected=True
    need(rejected,'missing mapped payload detected');cases.append('missing-payload')
    need(allocated(folder)<1024**2,'bounded graft proof under 1 MiB contingency')
    result=dict(passed=True,count=len(cases),cases=cases)
    (folder/'result.json').write_text(json.dumps(result,indent=2)+'\n');return result
def audited_xorriso(argv,contract):
    c=json.loads(Path(contract).read_text());real=c['xorriso']
    need(sha(real)==c['xorriso_sha256'],'accepted xorriso identity')
    probes=[['--version'],['-version'],['--help'],['-help'],['-as','mkisofs','--help'],['-as','mkisofs','-help']]
    if argv in probes:return subprocess.run([real,*argv],cwd=REPO).returncode
    need(argv[:2]==['-as','mkisofs'] and '-graft-points' in argv,'stock GRUB mkisofs graft interface')
    need(argv.count('-o')==1 and argv[argv.index('-o')+1]==c['output'],'exact fresh output')
    need(not Path(c['output']).exists() and not Path(c['receipt']).exists(),'one fresh producer output')
    old=c['previous_argv'];prefix=lambda a:[x for x in a[:a.index('-o')] if not x.startswith('--modification-date=')]
    need(prefix(argv)==prefix(old),'all accepted boot/partition geometry flags unchanged')
    for spec in c['grafts']:need(argv.count(spec)==1,'every explicit graft exactly once')
    need(not any('-stage' in x for x in argv),'no payload staging tree')
    before=P['snapshot_tree'](c['private_tmp'])
    need(before and sum((x['size']+4095)//4096*4096 for x in before.values())<14*1024**2,'private GRUB inputs within unchanged scratch provision')
    payload_hashes=set(c['input_sha256'].values())
    need(not any(v['sha256'] in payload_hashes for v in before.values()),'no copied kernel/application in private GRUB inputs')
    for path,h in c['input_sha256'].items():need(sha(path)==h,'producer source preimage')
    subprocess.run([real,*argv],cwd=REPO,check=True)
    need(P['snapshot_tree'](c['private_tmp'])==before,'GRUB input tree unchanged by assembly')
    for path,h in c['input_sha256'].items():need(sha(path)==h,'producer source preserved')
    result=dict(passed=True,argv=argv,input_sha256=c['input_sha256'],private_inputs=before,
                direct_graft=True,payload_copies=0,boot_geometry_unchanged=True,manual_cleanup=False)
    Path(c['receipt']).write_text(json.dumps(result,indent=2)+'\n');return 0
def boot_helper(out):
    source=(REPO/'scripts/test-pointer.py').read_text()
    tree=ast.parse(source);node=next(n for n in tree.body if isinstance(n,ast.FunctionDef) and n.name=='boot')
    text=ast.get_source_segment(source,node)
    anchor="('bottom-right',0,3000,1023,767),('bottom-left',-3000,0,0,767)]"
    replacement="('bottom-right',0,3000,1023,767),('bottom-left',-3000,0,0,767),"+\
        "('left-middle',0,-383,0,384),('top-middle',512,-3000,512,0),"+\
        "('right-middle',3000,384,1023,384),('bottom-middle',-511,3000,512,767),"+\
        "('partial-bottom-right',501,-4,1013,763),('bottom-left-return',-3000,3000,0,767)]"
    need(text.count(anchor)==1,'exact original corner checks retained')
    text=text.replace(anchor,replacement)
    hold="            mouse(down=True)\n            shots['final']="
    need(text.count(hold)==1,'exact final hold checkpoint')
    text=text.replace(hold,"            before_hold=data();mouse(down=True)\n            wait(lambda d:d.count(b'pointer-demo: LEFT DOWN')>before_hold.count(b'pointer-demo: LEFT DOWN'))\n            shots['final']=")
    (out/'native-boot-helper.py').write_text(text+'\n')
    scope=dict(P['boot'].__globals__);exec(compile(text,str(out/'native-boot-helper.py'),'exec'),scope)
    return scope['boot']
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--output',required=True,type=Path);args=ap.parse_args()
    phase=Path(os.environ['N63_ROOT']).resolve();out=args.output.resolve();M=1024**2
    need(out==phase/'cursor-refinement-1' and not out.exists(),'one admitted cursor output')
    control=phase/'final/cursor-refinement-resume-1';need(json.loads((control/'ADMISSION.json').read_text())['passed'],'fresh guarded admission')
    out.mkdir();(out/'tmp').mkdir();os.environ['TMPDIR']=str(out/'tmp')
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));resource.setrlimit(resource.RLIMIT_FSIZE,(32*M,32*M))
    result=dict(passed=False,scope='N6.3 cursor visual refinement',physical_hardware_verified=False)
    try:
        base='78cc80c9ac2b8b9d36ffb411ab06f5a63811bbc3'
        oldtest=subprocess.check_output(['git','show',base+':tests/host/pointer.c'],cwd=REPO,text=True)
        newtest=(REPO/'tests/host/pointer.c').read_text()
        need(oldtest.split('\n#else\n',1)[1]==newtest.split('\n#else\n',1)[1],'unaffected service test body unchanged')
        oldscreen=subprocess.check_output(['git','show',base+':arch/x86_64/screen.c'],cwd=REPO,text=True)
        newscreen=(REPO/'arch/x86_64/screen.c').read_text()
        need(oldscreen.split('/* Fixed 8x12')[0]==newscreen.split('/* Fixed 32x32')[0],'renderer prefix unchanged')
        marker='/* ---- bounded pixels ---- */'
        need(oldscreen.split(marker,1)[1]==newscreen.split(marker,1)[1],'movement, event and noncursor renderer bodies unchanged')
        native=phase/'verification-continuation-1';old_result=json.loads((native/'result.json').read_text())
        for n,h in old_result['source_sha256'].items():
            if n not in ('arch/x86_64/screen.c','tests/host/pointer.c','scripts/test-pointer.py','user/pointer-demo/main.rs'):
                need(sha(REPO/n)==h,'unaffected compiled input '+n)
        current=json.loads((phase/'motion-continuation-1/result.json').read_text())
        need(current['passed'],'accepted full pointer verification')
        for n,h in current['source_sha256'].items():
            if n not in ('arch/x86_64/screen.c','tests/host/pointer.c'):need(sha(REPO/n)==h,'unchanged current implementation '+n)
        keys=set(current['source_sha256'])|{'scripts/test-cursor-refinement.py','arch/x86_64/screen.c','tests/host/pointer.c'}
        result['source_sha256']={n:sha(REPO/n) for n in sorted(keys)}
        result['reuse']=dict(service_assertions=987802,console_assertions=49499,Rust_tests=3,logger_cases=16,
            source_identity=True,service_test_preprocessed_branch_unchanged=True,old_runtime_manifest_sha256=sha(phase/'motion-continuation-1/manifest.json'))
        result['graft_tests']=graft_tests(out)
        print('CURSOR_GRAFT_HOST_PASS',flush=True)
        cc=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Iarch/x86_64','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DPOINTER_RENDER',
            '-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
            'tests/host/pointer.c','arch/x86_64/desktop.c','arch/x86_64/font8x16.c','-o',str(out/'renderer')]
        run(cc,out/'renderer-build.log',60);run([str(out/'renderer')],out/'renderer.log',60)
        result['host_renderer']=(out/'renderer.log').read_text();need('pointer renderer PASS' in result['host_renderer'],'actual geometry/restoration sanitizer')
        print('CURSOR_RENDERER_PASS '+result['host_renderer'].strip(),flush=True)
        oldphase=REPO/'build/n6-gui-20261025T083036Z'
        built=json.loads((oldphase/'bootstrap-hardening-1/result.json').read_text())['build']
        flags=[x for x in built['compile'][:built['compile'].index('-c')] if x!='-DVOS_GUI_VERIFY']
        kernel=out/'kernel';kernel.mkdir()
        run(flags+['-c','arch/x86_64/screen.c','-o',str(kernel/'screen.o')],out/'screen-build.log')
        changed=('pointer','keyboard','user_console','gui','screen','syscall','app_payload','gui_verify');normal=('gui_verify','syscall','gui','app_payload')
        argv=built['link'][:];argv[argv.index('-o')+1]=str(kernel/'vos.bin')
        manifest={str(native/row['path']):row['sha256'] for row in json.loads((native/'manifest.json').read_text())}
        immutable={x['path']:x['sha256'] for x in built['reused_objects']}
        reused={}
        for i,v in enumerate(argv):
            if v.endswith('.o'):
                name=Path(v).stem
                if name=='screen':argv[i]=str(kernel/'screen.o');continue
                if name in changed:argv[i]=str(native/'kernel'/(name+('-normal' if name in normal else '')+'.o'))
                source=Path(argv[i]);expected=manifest.get(str(source),immutable.get(str(source)))
                need(expected and sha(source)==expected,'pinned reused kernel object '+str(source));reused[str(source)]=expected
            elif v.startswith('-Map='):argv[i]='-Map='+str(kernel/'vos.map')
        pointer=native/'kernel/pointer.o';need(sha(pointer)==manifest[str(pointer)],'pinned pointer object');argv.append(str(pointer));reused[str(pointer)]=sha(pointer)
        run(argv,out/'kernel-link.log');(out/'kernel-link-command.json').write_text(json.dumps(argv,indent=2)+'\n')
        binary=kernel/'vos.bin'
        run(['python3','-I','-B','scripts/check-kernel-layout.py',str(binary)],out/'kernel-layout.log')
        run(['grub-file','--is-x86-multiboot2',str(binary)],out/'multiboot.log')
        symbols=subprocess.check_output(['nm','-S','--defined-only',str(binary)],text=True)
        oldsymbols=subprocess.check_output(['nm','-S','--defined-only',str(native/'kernel/vos-normal.bin')],text=True)
        def cursor_bytes(s):return int(next(x.split()[1] for x in s.splitlines() if x.split()[-1]=='cursor'),16)
        need(cursor_bytes(symbols)==4112 and cursor_bytes(oldsymbols)==400,'fixed cursor record growth')
        result['memory']=dict(old_cursor_record=400,new_cursor_record=4112,old_underlay=384,new_underlay=4096,underlay_delta=3712,mask_bytes=256,dynamic_allocation=False)
        apps={n:Path(row['path']) for n,row in current['build']['apps'].items() if n not in ('gui-probe','pointer-probe')}
        for n,p in apps.items():need(sha(p)==current['build']['apps'][n]['sha256'],'accepted application '+n)
        disk=REPO/'build/app-console-20261025T051241Z/attempt-1/disk.img';diskhash=sha(disk)
        need(diskhash=='5ef98c6e0b4dd983dfb7db1a8d73d0175cbb86c358e61f2957c689d164890dcc','immutable disk')
        cfg=out/'normal.cfg';cfg.write_text((REPO/'arch/x86_64/grub-pointer.cfg').read_text())
        eficonfig=out/'uefi.cfg';eficonfig.write_text((REPO/'arch/x86_64/grub-uefi-head.cfg').read_text()+cfg.read_text())
        pairs=[('/boot/vos.bin',binary),('/boot/grub/grub.cfg',eficonfig)]+[('/boot/'+n+'.elf',p) for n,p in sorted(apps.items())]
        specs=map_specs(pairs)
        producer=out/'producer';producer.mkdir();private=producer/'tmp';private.mkdir()
        efi=Path(built['UEFI'][built['UEFI'].index('-d')+1])
        grub=Path(shutil.which('grub-mkrescue')).resolve();xorriso=Path(shutil.which('xorriso')).resolve()
        old_producer=current['paired_UEFI_producer']
        need(sha(grub)==old_producer['grub_sha256'],'same accepted GRUB producer')
        iso=out/'vos-cursor-uefi.iso';contract=producer/'contract.json'
        c=dict(xorriso=str(xorriso),xorriso_sha256=sha(xorriso),output=str(iso),receipt=str(producer/'graft-result.json'),
               grafts=specs,input_sha256={str(p):sha(p) for _,p in pairs},previous_argv=old_producer['commands'][1],private_tmp=str(private))
        contract.write_text(json.dumps(c,indent=2)+'\n')
        wrapper=producer/'xorriso-graft'
        wrapper.write_text('#!/usr/bin/python3 -B\nimport runpy,sys\nm=runpy.run_path('+repr(str(REPO/'scripts/test-cursor-refinement.py'))+',run_name="cursor_graft_helper")\nsys.exit(m["audited_xorriso"](sys.argv[1:],'+repr(str(contract))+'))\n');wrapper.chmod(0o500)
        env=os.environ.copy();env.update(TMPDIR=str(private),PYTHONDONTWRITEBYTECODE='1')
        command=[str(grub),'-d',str(efi),'--xorriso='+str(wrapper),'-o',str(iso),*specs]
        with (out/'uefi-build.log').open('x') as log:subprocess.run(command,cwd=REPO,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=120,check=True)
        (out/'uefi-build-command.json').write_text(json.dumps(command,indent=2)+'\n')
        need(sha(grub)==old_producer['grub_sha256'],'producer unchanged after assembly')
        result['optimized_staging']=json.loads((producer/'graft-result.json').read_text())
        need(result['optimized_staging']['passed'],'optimized producer proof before native boot')
        configs=[cfg,eficonfig,contract,wrapper,producer/'graft-result.json']
        need(sum(p.stat().st_blocks*512 for p in configs)<int(.5*M),'reduced configuration/mapping provision')
        result['images']={};folder=out/'validate-uefi';folder.mkdir()
        result['images']['uefi']=validate_image(iso,dict(pairs),True,folder)
        print('CURSOR_UEFI_IMAGE_PASS direct graft; no kernel/application staging copies',flush=True)
        bios=out/'vos-cursor-bios.iso';seed=phase/'coordinate-log-continuation-1/normal-bios.iso'
        known=json.loads((phase/'coordinate-log-continuation-1/result.json').read_text())
        need(sha(seed)==known['images']['normal-bios']['ISO_sha256'],'immutable accepted BIOS-only seed')
        run(['xorriso','-indev',str(seed),'-outdev',str(bios),'-boot_image','any','replay','-map',str(binary),'/boot/vos.bin','-map',str(cfg),'/boot/grub/grub.cfg','-commit','-end'],out/'bios-build.log',90)
        expected={'/boot/vos.bin':binary,'/boot/grub/grub.cfg':cfg};expected.update({'/boot/'+n+'.elf':p for n,p in apps.items()})
        folder=out/'validate-bios';folder.mkdir();result['images']['bios']=validate_image(bios,expected,False,folder)
        for image in (bios,iso):
            records=P['G']['D']['iso_records'](image)
            need('/boot/gui-probe.elf' not in records and '/boot/pointer-probe.elf' not in records,'normal media omit verification probes')
        need(bios.stat().st_blocks*512+iso.stat().st_blocks*512<=int(20.5*M),'image provision')
        boot=boot_helper(out);result['firmware']={}
        locations={'first-cursor':(512,390,0x409ddd),'movement':(524,382,0x409ddd),'press':(524,382,0xe8b34e),'click':(524,382,0x42c497),'final':(513,390,0xe8b34e),
            'top-left':(0,0,None),'top-right':(1023,0,None),'bottom-right':(1023,767,None),'bottom-left':(0,767,None),
            'left-middle':(0,384,None),'top-middle':(512,0,None),'right-middle':(1023,384,None),'bottom-middle':(512,767,None),'partial-bottom-right':(1013,763,None),'bottom-left-return':(0,767,None)}
        for fw,image in (('bios',bios),('uefi',iso)):
            checks=boot(out,fw,image,disk,True,full_pointer=True)
            checks['cursor_geometry']={n:geometry(checks['screenshots'][n]['path'],*at) for n,at in locations.items()}
            result['firmware'][fw]=checks
            (out/(fw+'-checkpoint.json')).write_text(json.dumps(checks,indent=2)+'\n')
            print('CURSOR_NATIVE_PASS '+fw+'; white 21x28 arrow, exact tip/corners, movement/click/keys/cleanup/apps',flush=True)
        need(sha(disk)==diskhash,'disk preserved after both boots')
        for p,h in reused.items():need(sha(p)==h,'reused object preserved')
        for n,h in result['source_sha256'].items():need(sha(REPO/n)==h,'tested source unchanged')
        result.update(passed=True,kernel_sha256=sha(binary),reused_objects=reused,applications={n:dict(path=str(p),sha256=sha(p)) for n,p in apps.items()},before=dict(canvas=[8,12],visible=[6,11],underlay=384),after=dict(canvas=[32,32],visible=[21,28],underlay=4096,hotspot=[0,0]),no_physical_hardware_claim=True)
    except BaseException as e:
        result['error']=str(e);raise
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        rows=[dict(path=str(p.relative_to(out)),sha256=sha(p),size=p.stat().st_size) for p in sorted(out.rglob('*')) if p.is_file()]
        (out/'manifest.json').write_text(json.dumps(rows,indent=2)+'\n')
        print(json.dumps(dict(passed=result['passed'],error=result.get('error'))),flush=True)
if __name__=='__main__':main()
