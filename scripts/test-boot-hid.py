#!/usr/bin/env python3
"""Focused H1-B hosted verification. No QEMU/device access. Fresh output required; failures preserved, no retries."""
import argparse,hashlib,json,os,re,shutil,struct,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
CORE=ROOT/'kernel/rust/usb/hid'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def stack_sizes(p):
    """Decode LLVM's ELF64 .stack_sizes relocations, not a source/assembly estimate."""
    b=p.read_bytes()
    if b[:6]!=b'\x7fELF\x02\x01':raise RuntimeError('expected little-endian ELF64')
    off=struct.unpack_from('<Q',b,40)[0];size,num,names=struct.unpack_from('<HHH',b,58)
    if size!=64:raise RuntimeError('section size')
    sh=[struct.unpack_from('<IIQQQQIIQQ',b,off+i*size) for i in range(num)]
    def data(i):return b[sh[i][4]:sh[i][4]+sh[i][5]]
    def z(s,i):return s[i:s.index(0,i)].decode()
    st=data(names);section=[z(st,x[0]) for x in sh];result={}
    for i,h in enumerate(sh):
        if h[1]!=4 or section[h[7]]!='.stack_sizes':continue
        target=data(h[7]);symbols=sh[h[6]];strings=data(symbols[6]);symdata=data(h[6])
        for j in range(0,h[5],h[9]):
            at,info,_=struct.unpack_from('<QQq',data(i),j)
            name,_,_,idx,_,_=struct.unpack_from('<IBBHQQ',symdata,(info>>32)*symbols[9])
            symbol=z(strings,name) if name else section[idx]
            pos=at+8;value=shift=0
            while True:
                v=target[pos];pos+=1;value|=(v&127)<<shift;shift+=7
                if not v&128:break
                if shift>63:raise RuntimeError('stack ULEB overflow')
            result[symbol]=value
    executable={section[i] for i,h in enumerate(sh) if h[1]==1 and h[2]&4 and h[5]}
    if not result or executable!=set(result):raise RuntimeError('missing stack size coverage: '+str(executable-set(result)))
    names=subprocess.check_output(['c++filt','-s','rust',*[n.removeprefix('.text.') for n in result]],text=True).splitlines()
    return dict(zip(names,result.values()))
def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--rustc',default=os.environ.get('RUSTC','rustc'));ap.add_argument('--stage',choices=['all','core','adapters','mutations','resources'],default='all');a=ap.parse_args()
    out=a.out.resolve()
    if out.exists():ap.error('output exists: retain it and choose a new path')
    out.mkdir(parents=True);report={'status':'RUNNING','scope':'hosted fixtures, NOT native USB input','commands':[],'source_sha256':{}}
    sources=[*CORE.glob('*.rs'),ROOT/'tests/rust/boot_hid.rs',*ROOT.glob('tests/host/boot-hid-*'),Path(__file__).resolve()]
    for p in sorted(sources):report['source_sha256'][str(p.relative_to(ROOT))]=sha(p)
    units=['tests/host/boot-hid-console.c','tests/host/boot-hid-gui.c','tests/host/pointer.c','arch/x86_64/keyboard.c','arch/x86_64/gui.c','arch/x86_64/user_console.c','arch/x86_64/input_console.c']
    deps=subprocess.check_output(['gcc','-MM','-DVOS_GUI_HOST','-DVOS_CONSOLE_HOST','-Iarch/x86_64','-Itests/host',*units],cwd=ROOT,text=True).replace('\\\n',' ')
    names={x for line in deps.splitlines() for x in line.split(':',1)[-1].split()}
    names.update(['arch/x86_64/syscall.c','arch/x86_64/rust/ps2_pointer.rs','kernel/rust/input/ps2_pointer.rs'])
    report['existing_input_sha256']={str(Path(n)):sha(ROOT/n) for n in sorted(names)}
    def run(args,name,expected=0):
        args=list(map(str,args));p=subprocess.run(args,cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=180)
        (out/name).write_text(p.stdout);report['commands'].append({'argv':args,'exit_code':p.returncode,'log':name})
        if (expected==0 and p.returncode!=0) or (expected!=0 and p.returncode==0):raise RuntimeError(name+' unexpected exit '+str(p.returncode))
        return p.stdout
    def lib(path,dest):run([a.rustc,'--edition=2024','--crate-name','boot_hid','--crate-type=rlib','-Dwarnings','-Coverflow-checks=yes','-Copt-level=2','-Cdebuginfo=0',path,'-o',dest],str(dest.relative_to(out))+'.build.txt')
    def test(libpath,dest):run([a.rustc,'--edition=2024','--test','-Copt-level=2','-Cdebuginfo=0','-Cstrip=symbols','--extern','boot_hid='+str(libpath),'tests/rust/boot_hid.rs','-o',dest],str(dest.relative_to(out))+'.build.txt')
    try:
        report['rustc']=run([a.rustc,'--version'],'rustc.txt').strip();report['gcc']=run(['gcc','--version'],'gcc.txt').splitlines()[0]
        libpath=out/'libboot_hid.rlib';lib(CORE/'lib.rs',libpath)
        if a.stage in ('all','core'):
            test(libpath,out/'core-tests');report['core']=run([out/'core-tests','--test-threads=1','--nocapture'],'core-tests.txt')
        if a.stage in ('all','adapters'):
            # Include the existing production PS/2 ABI glue unchanged in the test-only archive.
            bridge=out/'ffi.rs';bridge.write_text((ROOT/'tests/host/boot-hid-ffi.rs').read_text()+'\n#[path='+json.dumps(str(ROOT/'arch/x86_64/rust/ps2_pointer.rs'))+'] mod ps2;\n')
            run([a.rustc,'--edition=2024','--crate-name','hid_fixture','--crate-type=staticlib','-Cpanic=abort','-Copt-level=2','-Cdebuginfo=0','--extern','boot_hid='+str(libpath),bridge,'-o',out/'libfixture.a'],'ffi-build.txt')
            run(['strip','--strip-debug',out/'libfixture.a'],'ffi-strip-debug.txt')
            cc=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Iarch/x86_64','-Itests/host','-fno-pie','-no-pie','-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer']
            ren=['-Dkeyboard_'+k+'=ps2_'+v for k,v in [('init','init'),('poll','poll'),('suspend','suspend'),('resume','resume'),('pointer_start','pointer_start'),('pointer_stop','pointer_stop'),('pointer_poll','pointer_poll'),('pointer_lost','pointer_lost')]]
            run(cc+ren+['-c','arch/x86_64/keyboard.c','-o',out/'keyboard.o'],'keyboard-build.txt')
            sc=(ROOT/'arch/x86_64/syscall.c').read_text();debug=out/'debug.c';debug.write_text('#include <stdint.h>\n'+sc[sc.index('void debug_emit('):sc.index('/* debug_write')])
            report['adapters']={}
            for name,source,extra in [('hid-console','tests/host/boot-hid-console.c',[]),('hid-gui','tests/host/boot-hid-gui.c',['-DVOS_GUI_HOST','arch/x86_64/gui.c'])]:
                run(cc+['-DVOS_CONSOLE_HOST',*extra,source,'arch/x86_64/user_console.c','arch/x86_64/input_console.c',debug,out/'keyboard.o',out/'libfixture.a','-Wl,--gc-sections','-o',out/name],name+'.build.txt')
                report['adapters'][name]=run([out/name],name+'.txt')
            # Separate unchanged actual PS/2 demultiplexer + GUI regression, no HID adapter.
            run(cc+['-DVOS_CONSOLE_HOST','-DVOS_GUI_HOST','tests/host/pointer.c','arch/x86_64/gui.c','arch/x86_64/user_console.c','arch/x86_64/keyboard.c','arch/x86_64/input_console.c',debug,out/'libfixture.a','-Wl,--gc-sections','-o',out/'ps2-regression'],'ps2-regression.build.txt')
            report['adapters']['ps2-regression']=run([out/'ps2-regression'],'ps2-regression.txt')
        if a.stage in ('all','mutations'):
            mutations=[('protocol','input.rs','if !self.confirmed || self.disconnected','if self.disconnected','no_boot_data_before_transport_success'),('owner','input.rs','if self.focus!=Some(f)','if false && self.focus != Some(f)','keyboard_owner_epoch_and_disconnect'),('repeat','input.rs','usage!=0 && !other.contains(&usage)','usage!=0 && other.len()==6','keyboard_transitions_and_duplicates'),('direction','input.rs','let dy=i16::from(b[2] as i8)','let dy=-i16::from(b[2] as i8)','mouse_direction_buttons_and_relative_repetition'),('overflow','input.rs','self.clear();return Err(Error::Overflow)','return Err(Error::Overflow)','queue_saturation_is_atomic_and_reset_cannot_be_lost'),('length','descriptor.rs','usize::from(word(b,2))!=b.len()','false','malformed_descriptor_fields'),('count','descriptor.rs','interfaces!=b[4]','interfaces>MAX_INTERFACES','malformed_descriptor_fields'),('ambiguous','descriptor.rs','if slot.is_some()','if false','ambiguity_and_counts')]
            report['mutations']=[]
            for name,file,old,new,target in mutations:
                d=out/('mutant-'+name);d.mkdir()
                for p in CORE.glob('*.rs'):shutil.copyfile(p,d/p.name)
                path=d/file;s=path.read_text()
                pattern=''.join(re.escape(c)+r'\s*' for c in old if not c.isspace())
                matches=list(re.finditer(pattern,s))
                if len(matches)!=1:raise RuntimeError('mutation target drift '+name)
                m=matches[0];path.write_text(s[:m.start()]+new+' '+s[m.end():]);rlib=d/'libboot_hid.rlib';lib(d/'lib.rs',rlib);test(rlib,d/'tests')
                text=run([d/'tests','--exact',target,'--nocapture'],name+'.txt',expected=1)
                if target+' ... FAILED' not in text:raise RuntimeError('mutant failed outside intended assertion '+name)
                report['mutations'].append({'name':name,'caught_by':target,'binary_sha256':sha(d/'tests')})
        if a.stage in ('all','resources'):
            obj=out/'hid.o'
            run([a.rustc,'--edition=2024','--crate-name','boot_hid','--crate-type=lib','--target=x86_64-unknown-none','-Copt-level=2','-Cpanic=abort','-Cforce-frame-pointers=yes','-Cno-redzone=yes','-Zemit-stack-sizes','--emit=obj',CORE/'lib.rs','-o',obj],'freestanding-build.txt')
            frames=stack_sizes(obj)
            api=out/'api.rs';api.write_text('''#![no_std]
extern crate boot_hid;
use boot_hid::*;
#[unsafe(no_mangle)] pub fn api_new(b:Binding,d:u64,g:u64)->Result<Session,Error>{Session::new(b,d,g)}
#[unsafe(no_mangle)] pub fn api_focus(s:&mut Session,f:Focus)->Result<(),Error>{s.focus(f)}
#[unsafe(no_mangle)] pub fn api_revoke(s:&mut Session,e:u64)->Result<(),Error>{s.revoke(e)}
#[unsafe(no_mangle)] pub fn api_request(s:&mut Session)->Result<ProtocolRequest,Error>{s.request_protocol()}
#[unsafe(no_mangle)] pub fn api_setup(r:ProtocolRequest)->[u8;8]{r.setup()}
#[unsafe(no_mangle)] pub fn api_confirm(s:&mut Session,r:ProtocolRequest,c:Completion)->Result<(),Error>{s.confirm_protocol(r,c)}
#[unsafe(no_mangle)] pub fn api_reset(s:&mut Session,g:u64)->Result<(),Error>{s.reset(g)}
#[unsafe(no_mangle)] pub fn api_disconnect(s:&mut Session){s.disconnect()}
#[unsafe(no_mangle)] pub fn api_take(s:&mut Session,f:Focus)->Result<Option<Event>,Error>{s.take(f)}
''')
            targetlib=out/'libtarget.rlib'
            run([a.rustc,'--edition=2024','--crate-name','boot_hid','--crate-type=rlib','--target=x86_64-unknown-none','-Copt-level=2','-Cpanic=abort',CORE/'lib.rs','-o',targetlib],'target-library.txt')
            apiobj=out/'api.o'
            run([a.rustc,'--edition=2024','--crate-type=lib','--target=x86_64-unknown-none','-Copt-level=2','-Cpanic=abort','-Cforce-frame-pointers=yes','-Cno-redzone=yes','-Zemit-stack-sizes','--extern','boot_hid='+str(targetlib),'--emit=obj',api,'-o',apiobj],'api-build.txt')
            frames.update(stack_sizes(apiobj))
            if max(frames.values())>512:raise RuntimeError('stack frame exceeds 512: '+str(frames))
            report['stack_frames']=frames;report['max_frame']=max(frames.values());report['sections']=run(['size','-A',obj],'sections.txt');report['undefined']=run(['nm','-u',obj,apiobj],'undefined.txt')
            if re.search(r'(panic|slice_index_fail|__rust_alloc|__rg_alloc|malloc|realloc|pci_|xhci_|ehci_)',report['undefined']):raise RuntimeError('forbidden dependency')
            report['dma_pages']=0;report['heap_allocations']=0
        report['status']='PASS'
    except Exception as e:report['status']='FAIL';report['failure']=str(e)
    finally:
        (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:report[k] for k in ['status','scope']})+' '+str(report.get('failure','')))
    return 0 if report['status']=='PASS' else 1
if __name__=='__main__':sys.exit(main())
