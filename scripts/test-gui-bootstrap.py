#!/usr/bin/env python3
"""One owner-bound GUI bootstrap hardening attempt; immutable prior inputs."""
import argparse,hashlib,json,os,resource,runpy,shutil,subprocess
from pathlib import Path
REPO=Path(__file__).resolve().parents[1]
H=runpy.run_path(str(REPO/'scripts/test-gui.py'),run_name='bootstrap_gui_helpers')
run,need,sha,boot,validate_image=(H[n] for n in ('run','need','sha','boot','validate_image'))
BASE='e639008949358d80561c149b94fd14654d108a7b'
def main():
    ap=argparse.ArgumentParser();ap.add_argument('--output',required=True,type=Path);args=ap.parse_args()
    phase=Path(os.environ['N6_PHASE_ROOT']).resolve()
    control=Path(os.environ['N6_HARDENING_CONTROLLER']).resolve();out=args.output.resolve()
    authority=json.loads((control/'CAPACITY-AUTHORIZATION.json').read_text())
    need(control==phase/'final/bootstrap-hardening-1' and out==phase/'bootstrap-hardening-1' and not out.exists(),'fresh bound attempt')
    need(authority['identity']=='bootstrap-hardening-1' and authority['accepted_candidate']==BASE and
         authority['execution_limit_bytes']==152*1048576 and authority['phase_limit_bytes']==176*1048576 and
         authority['maximum_new_execution_bytes']==40*1048576,'capacity authorization')
    out.mkdir();(out/'tmp').mkdir();os.environ['TMPDIR']=str(out/'tmp')
    resource.setrlimit(resource.RLIMIT_CORE,(0,0));resource.setrlimit(resource.RLIMIT_FSIZE,(40*1048576,40*1048576))
    result=dict(passed=False,scope='GUI initial ELF-load worker reclamation',baseline=BASE,physical_hardware_verified=False)
    try:
        prior_root=phase/'catalog-recovery-1';prior=json.loads((prior_root/'result.json').read_text())
        need(prior['passed'],'accepted runtime')
        for n,h in prior['source_sha256'].items():
            if n!='arch/x86_64/gui_verify.c':need(sha(REPO/n)==h,'unaffected source '+n)
        inputs=list(prior['source_sha256'])+['scripts/test-gui-bootstrap.py','tests/host/gui-bootstrap.c','arch/x86_64/loader.c','arch/x86_64/elf.c']
        result['source_sha256']={n:sha(REPO/n) for n in sorted(set(inputs))}
        manifests={}
        def verified(path):
            relative=path.relative_to(phase);identity=relative.parts[0]
            if identity not in manifests:manifests[identity]={r['path']:r['sha256'] for r in json.loads((phase/identity/'manifest.json').read_text())}
            key=str(Path(*relative.parts[1:]))
            need(key in manifests[identity] and sha(path)==manifests[identity][key],'verified reused input '+str(relative))
            return path
        recipe=json.loads(verified(prior_root/'recovery-build.json').read_text())
        applications={}
        for name in ('vsh','hello','rixie','gui-demo','gui-probe'):
            expected=prior['images']['bios']['files']['/boot/'+name+'.elf']
            if name in ('vsh','hello','rixie'):
                path=REPO/'build/app-console-20261025T051241Z/attempt-1/image/user'/(name+'.elf')
            else:path=phase/'image-recipe-recovery-1/image/user'/(name+'.elf')
            need(sha(path)==expected,'accepted application '+name);applications[name]=path
        disk=REPO/'build/app-console-20261025T051241Z/attempt-1/disk.img'
        need(sha(disk)==prior['disk_sha256'],'accepted read-only disk')
        cc=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Iarch/x86_64','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
        common=['tests/host/gui-bootstrap.c','arch/x86_64/loader.c','arch/x86_64/elf.c','arch/x86_64/app_catalog.c']
        original=out/'baseline-gui-verify.c'
        original.write_bytes(subprocess.check_output(['git','--no-optional-locks','show',BASE+':arch/x86_64/gui_verify.c'],cwd=REPO))
        run(cc+['-O2']+common+[str(original),'-o',str(out/'baseline-host')],out/'baseline-build.log',60)
        with (out/'baseline-negative-control.log').open('x') as f:
            negative=subprocess.run([str(out/'baseline-host'),str(applications['gui-demo'])],cwd=REPO,stdout=f,stderr=subprocess.STDOUT,timeout=30)
        negative_text=(out/'baseline-negative-control.log').read_text()
        need(negative.returncode==1 and 'bootstrap FAIL' in negative_text and 'worker==0' in negative_text,'original defect detected')
        result['original_defect_negative_control']=dict(passed=True,expected_returncode=negative.returncode,diagnostic=negative_text)
        run(cc+['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer']+common+['arch/x86_64/gui_verify.c','-o',str(out/'bootstrap-host')],out/'host-build.log',60)
        run([str(out/'bootstrap-host'),str(applications['gui-demo'])],out/'host.log',30)
        result['host']=(out/'host.log').read_text();need('bootstrap host PASS' in result['host'],'targeted host bootstrap')
        kernel=out/'image/kernel';kernel.mkdir(parents=True)
        compile_=list(recipe['compile']);compile_[compile_.index('-c')+1]='arch/x86_64/gui_verify.c';compile_[compile_.index('-o')+1]=str(kernel/'gui_verify.o')
        flags=compile_[1:compile_.index('-c')]
        for mode in ('normal','verify'):
            options=[x for x in flags if x not in ('-MMD','-MP') and (mode=='verify' or x!='-DVOS_GUI_VERIFY')]
            run(['gcc']+options+['-Werror','-fsyntax-only','arch/x86_64/gui_verify.c'],out/('syntax-'+mode+'.log'),60)
        link=list(recipe['link']);link[link.index('-o')+1]=str(kernel/'vos.bin')
        reused=[]
        for i,value in enumerate(link):
            if not value.endswith('.o'):continue
            if Path(value).name=='gui_verify.o':link[i]=str(kernel/'gui_verify.o')
            else:
                path=verified(Path(value));reused.append(dict(path=str(path),sha256=sha(path)))
        for command in (compile_,link):
            destination=Path(command[command.index('-o')+1]).resolve()
            need(destination.is_relative_to(out),'bounded generated output')
        run(compile_,out/'kernel-compile.log',60);run(link,out/'kernel-link.log',60)
        run(['python3','-I','-B','scripts/check-kernel-layout.py',str(kernel/'vos.bin')],out/'kernel-layout.log',30)
        run(['grub-file','--is-x86-multiboot2',str(kernel/'vos.bin')],out/'multiboot.log',30)
        cfg=verified(prior_root/'gui-verify.cfg')
        bios=verified(prior_root/'image/kernel/vos.iso')
        with bios.open('rb') as f:f.seek(512);need(f.read(8)!=b'EFI PART','BIOS-only replay source')
        bios_command=['xorriso','-indev',str(bios),'-outdev',str(kernel/'vos.iso'),'-boot_image','any','replay','-map',str(kernel/'vos.bin'),'/boot/vos.bin','-commit','-end']
        run(bios_command,out/'bios-image-build.log',90)
        stage=kernel/'isodir-uefi';(stage/'boot/grub').mkdir(parents=True)
        shutil.copyfile(kernel/'vos.bin',stage/'boot/vos.bin')
        for name,path in applications.items():shutil.copyfile(path,stage/'boot'/(name+'.elf'))
        (stage/'boot/grub/grub.cfg').write_bytes((REPO/'arch/x86_64/grub-uefi-head.cfg').read_bytes()+cfg.read_bytes())
        efi=Path('/home/veasnawt/vos-evidence/n36-stage3-gatea-cont-20261021T220119Z/efi-tools/usr/lib/grub/x86_64-efi')
        uefi_command=['grub-mkrescue','-d',str(efi),'-o',str(kernel/'vos-uefi.iso'),str(stage)]
        run(uefi_command,out/'uefi-image-build.log',120)
        result['build']=dict(compile=compile_,link=link,reused_objects=reused,BIOS=bios_command,UEFI=uefi_command,application_sha256={n:sha(p) for n,p in applications.items()},no_Rust_or_ELF_rebuild=True)
        expected={'/boot/vos.bin':kernel/'vos.bin','/boot/grub/grub.cfg':cfg}
        expected.update({'/boot/'+n+'.elf':p for n,p in applications.items()})
        result['images']={};result['firmware']={}
        for fw,name in [('bios','vos.iso'),('uefi','vos-uefi.iso')]:
            folder=out/('validate-'+fw);folder.mkdir();current=dict(expected)
            if fw=='uefi':current['/boot/grub/grub.cfg']=stage/'boot/grub/grub.cfg'
            result['images'][fw]=validate_image(kernel/name,current,fw=='uefi',folder)
            result['firmware'][fw]=boot(out,fw,kernel/name,disk)
            serial=(out/fw/'serial.log').read_text()
            for i in range(18):need('gui-bootstrap: rejected ELF '+str(i)+' PASS; worker blocked, awakened and reaped; resources restored' in serial,'actual initial ELF-load failure '+str(i))
            need('gui-bootstrap: 18 repeated load failures PASS; no retained workers' in serial,'all failure iterations')
            result['firmware'][fw]['initial_load_failures']=18
            result['firmware'][fw]['actual_worker_block_wake_reap']=True
        need(sha(disk)==prior['disk_sha256'],'read-only disk unchanged')
        need(all(sha(REPO/n)==h for n,h in result['source_sha256'].items()),'tested source identity')
        result.update(passed=True,prior_runtime_manifest_sha256=sha(prior_root/'manifest.json'),unaffected_sanitizer_Rust_ELF_evidence_reused=True,old_monitoring_gap_preserved=True,no_physical_hardware_claim=True)
    except BaseException as error:
        result['error']=str(error);raise
    finally:
        (out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
        entries=[dict(path=str(p.relative_to(out)),size=p.stat().st_size,sha256=sha(p)) for p in sorted(out.rglob('*')) if p.is_file()]
        (out/'manifest.json').write_text(json.dumps(entries,indent=2)+'\n')
        print(json.dumps(result),flush=True)
if __name__=='__main__':main()
