#!/usr/bin/env python3
"""R2 routed PS/2 policy: frozen-driver differential, mutations and resources.
Fresh confined outputs. C sanitizers do not instrument the production Rust archive.
Hosted model timing is not guest or physical-device latency.
"""
import argparse,hashlib,json,os,re,subprocess,statistics,csv,io,importlib.util
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def need(x,s):
 if not x:raise RuntimeError(s)
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--archive',type=Path,required=True);ap.add_argument('--rustc',required=True);ap.add_argument('--out',type=Path,required=True);a=ap.parse_args()
 out=a.out.resolve();archive=a.archive.resolve();need(out.is_relative_to(ROOT/'build') and not out.exists() and archive.is_file(),'fresh confined output and existing archive');out.mkdir(parents=True)
 report=dict(status='RUNNING',commands=[],Rust_sanitized=False,archive_sha256=sha(archive),seed='7232647261696e31')
 def run(cmd,name,expected=0):
  cmd=list(map(str,cmd));row=dict(argv=cmd,log=name);report['commands'].append(row)
  with (out/name).open('x') as f:q=subprocess.run(cmd,cwd=ROOT,stdout=f,stderr=subprocess.STDOUT,timeout=180)
  row['exit_code']=q.returncode;need(q.returncode==expected,'unexpected exit '+name);return (out/name).read_text()
 try:
  ref=json.loads((ROOT/'tests/reference/ps2-routed-drain.json').read_text());need(ref['reference_sha256']==sha(ROOT/'tests/reference/ps2-routed-drain.c'),'frozen accepted driver hash')
  names=['keyboard_init','keyboard_poll','keyboard_suspend','keyboard_resume','keyboard_pointer_start','keyboard_pointer_stop','keyboard_pointer_poll','keyboard_pointer_lost'];defs=['-D'+n+'='+n+'_reference' for n in names]
  cc=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Iarch/x86_64','-ffunction-sections','-fdata-sections','-fno-pie','-no-pie','-Wl,--gc-sections']
  for mode,flags in [('sanitizer',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer']),('release',['-O2','-fno-lto'])]:
   scalar=flags+(['-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic','-mno-red-zone','-mgeneral-regs-only','-mcmodel=kernel'] if mode=='release' else [])
   run(cc+scalar+defs+['-c','tests/reference/ps2-routed-drain.c','-o',out/(mode+'-reference.o')],mode+'-reference-build.txt')
   run(cc+scalar+['-c','arch/x86_64/keyboard.c','-o',out/(mode+'-driver.o')],mode+'-driver-build.txt')
   run(cc+flags+['tests/host/ps2-routed-differential.c',out/(mode+'-reference.o'),out/(mode+'-driver.o'),archive,'-o',out/(mode+'-differential')],mode+'-build.txt')
   report[mode]=run([out/(mode+'-differential')],mode+'.txt');need('185637 routed driver comparisons' in report[mode],'expected comparison coverage')
  cmd=[out/'release-differential','--bench']
  if hasattr(os,'sched_getaffinity'):report['benchmark_cpu']=min(os.sched_getaffinity(0));cmd=['taskset','-c',str(report['benchmark_cpu']),*cmd]
  perf=run(cmd,'performance.csv');rows=list(csv.DictReader(io.StringIO(perf)));report['performance']={}
  for n in range(8):
   pair=[r for r in rows if int(r['workload'])==n];c=statistics.median(float(x['C_ns']) for x in pair);r=statistics.median(float(x['Rust_ns']) for x in pair);report['performance'][str(n)]=dict(C_ns=c,Rust_ns=r,ratio=r/c,regression_percent=(r/c-1)*100)
  common=[a.rustc,'--edition=2024','-Dwarnings','-Copt-level=2','-Cdebuginfo=0','-Coverflow-checks=yes']
  root=out/'core-root.rs';root.write_text('#![no_std]\n#[path='+json.dumps(str(ROOT/'kernel/rust/input/ps2_routed.rs'))+'] mod routing; pub use routing::*;\n');lib=out/'libps2_routed_core.rlib'
  run(common+['--crate-type=rlib','--crate-name=ps2_routed_core',root,'-o',lib],'core-build.txt')
  def test(library,binary,name,target=None):
   run(common+['--test','-Cstrip=symbols','--extern','ps2_routed_core='+str(library),'tests/rust/ps2_routed.rs','-o',binary],name+'-build.txt')
   return run([binary,'--test-threads=1',*([target] if target else [])],name+'.txt',101 if target else 0)
  report['core_tests']=test(lib,out/'core-tests','core-tests');source=(ROOT/'kernel/rust/input/ps2_routed.rs').read_text()
  mutations=[('limit','pub const LIMIT:u32=32;','pub const LIMIT:u32=31;','bound_and_enqueue'),('pending','s.pointer_pending()!=0 || s.key_count()==LIMIT','s.pointer_pending()!=0 && s.key_count()==LIMIT','pending_blocks'),('obf','if status&1==0','if status&2==0','bound_and_enqueue'),('aux','if status&0x20!=0','if status&0x40!=0','pointer_complete'),('aux-error','if status&0xc0!=0 {','if status&0x80!=0 {','aux_error_precedence'),('pointer-negative','if n<0 {','if n< -1 {','pointer_reject'),('pointer-pending','if n>0 {','if n>=0 {','pointer_partial'),('keyboard-error','} else if status&0xc0!=0','} else if status&0x80!=0','keyboard_error'),('zero',' || byte==0 || byte==0xff',' || byte==0xff','zero_resets'),('ff',' || byte==0 || byte==0xff',' || byte==0','ff_resets'),('saturation','u32::from(*lost!=u32::MAX)','1','saturation'),('index','q.head.wrapping_add(q.count)','q.head.wrapping_add(q.count).wrapping_add(1)','wrapping_queue'),('count','q.count=q.count.wrapping_add(1);','','bound_and_enqueue'),('zero-character','if byte!=0 {','if true {','zero_character')]
  report['mutations']={}
  for name,old,new,target in mutations:
   need(source.count(old)>=1,'mutation anchor '+name);d=out/('mutant-'+name);d.mkdir();(d/'routing.rs').write_text(source.replace(old,new));root=d/'root.rs';root.write_text('#![no_std]\n#[path="routing.rs"] mod routing; pub use routing::*;\n');ml=d/'libps2_routed_core.rlib'
   run(common+['--crate-type=rlib','--crate-name=ps2_routed_core',root,'-o',ml],name+'-core-build.txt');text=test(ml,d/'tests',name,target);need(target+' ... FAILED' in text,'intended mutation assertion '+name);report['mutations'][name]=dict(assertion=target,detected=True)
  root=out/'ffi-root.rs';root.write_text('#![no_std]\n'+''.join('#[path='+json.dumps(str(ROOT/'arch/x86_64/rust'/n))+'] mod '+n[:-3]+';\n' for n in ['ps2_routed.rs','ps2_pointer.rs','ps2_keyboard.rs']))
  with root.open('a') as f:f.write('unsafe extern "C" {fn abort()->!;}\n#[panic_handler] fn checked_panic(_: &core::panic::PanicInfo)->! {unsafe{abort()}}\n')
  obj=out/'libchecked_ffi.a';run(common+['--crate-type=staticlib','-Cpanic=abort',root,'-o',obj],'checked-ffi-build.txt')
  run(cc+['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','tests/host/ps2-routed-differential.c',out/'sanitizer-reference.o',out/'sanitizer-driver.o',obj,'-o',out/'checked-differential'],'checked-build.txt');report['checked_Rust']=run([out/'checked-differential'],'checked.txt')
  target=out/'target.o';run(common+['--crate-type=lib','--target=x86_64-unknown-none','-Cpanic=abort','-Ccode-model=kernel','-Crelocation-model=static','-Cno-redzone=yes','-Cforce-unwind-tables=no','-Zemit-stack-sizes','--emit=obj',root,'-o',target],'target-build.txt')
  spec=importlib.util.spec_from_file_location('keyboard_verifier',ROOT/'scripts/test-ps2-keyboard-rust.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);allframes=m.frames(target);report['target_frames']=allframes
  report['target_disassembly']=run(['objdump','-dr',target],'target-disassembly.txt');need(not any(s in report['target_disassembly'] for s in ['%xmm','%ymm','%zmm','%st(','%mm']),'scalar target')
  prod=run(['objdump','-d',archive],'production-disassembly.txt');routing={}
  for m in re.finditer(r'^[0-9a-f]+ <([^>]+)>:\n(.*?)(?=\n\n)',prod,re.M|re.S):
   if m[1]=='vos_rust_keyboard_routed_drain' or 'ps2_routed' in m[1]:
    f=8*len(re.findall(r'\tpush\s',m[2]))+sum(int(x,16) for x in re.findall(r'\tsub\s+\$0x([0-9a-f]+),%rsp',m[2]));routing[m[1]]=f
  need('vos_rust_keyboard_routed_drain' in routing and sum(routing.values())<=40,'actual archive total routing own frames <=40B');report['production_routing_frames']=routing
  symbols=run(['nm','--defined-only',archive],'archive-owners.txt');need(sum(l.endswith(' T vos_rust_keyboard_routed_drain') for l in symbols.splitlines())==1,'one production Rust owner')
  c_symbols=run(['nm',out/'release-driver.o'],'driver-symbols.txt');need(' U vos_rust_keyboard_routed_drain' in c_symbols and ' T vos_rust_keyboard_routed_drain' not in c_symbols,'C only calls private Rust ABI')
  report['source_sha256']={n:sha(ROOT/n) for n in ['kernel/rust/input/ps2_routed.rs','arch/x86_64/rust/ps2_routed.rs','arch/x86_64/keyboard.c','arch/x86_64/keyboard.h','tests/host/ps2-routed-differential.c','tests/rust/ps2_routed.rs','tests/reference/ps2-routed-drain.c']};report['status']='PASS'
 except Exception as e:report.update(status='FAIL',error=str(e));raise
 finally:
  with (out/'result.json').open('x') as f:json.dump(report,f,indent=2)
 print(json.dumps({k:v for k,v in report.items() if k not in ['commands','target_disassembly']},indent=2))
if __name__=='__main__':main()
