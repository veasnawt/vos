#!/usr/bin/env python3
"""PS/2 keyboard differential, ABI, negative controls and resource checks.
Fresh outputs; all failures retained. C sanitizers do not instrument Rust.
"""
import argparse,hashlib,json,os,struct,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def need(test,message):
 if not test:raise RuntimeError(message)
def frames(path):
 data=path.read_bytes();need(data[:6]==b'\x7fELF\x02\x01','ELF64 little endian')
 off=struct.unpack_from('<Q',data,40)[0];size,count,names=struct.unpack_from('<HHH',data,58)
 sections=[struct.unpack_from('<IIQQQQIIQQ',data,off+i*size) for i in range(count)]
 table=sections[names];strings=data[table[4]:table[4]+table[5]]
 name=lambda n:strings[n:strings.find(b'\0',n)].decode()
 result={}
 for i,sec in enumerate(sections):
  if name(sec[0])!='.stack_sizes':continue
  rel=next((s for s in sections if s[1]==4 and s[7]==i),None);need(rel is not None,'stack relocations')
  symsec=sections[rel[6]];strsec=sections[symsec[6]];st=data[strsec[4]:strsec[4]+strsec[5]]
  symbols=[struct.unpack_from('<IBBHQQ',data,symsec[4]+n*symsec[9]) for n in range(symsec[5]//symsec[9])]
  for at in range(rel[4],rel[4]+rel[5],rel[9]):
   offset,info,addend=struct.unpack_from('<QQq',data,at);sym=symbols[info>>32]
   target=next((s for s in symbols if s[3]==sym[3] and s[4]==addend and s[1]&15==2),sym)
   symbol=st[target[0]:st.find(b'\0',target[0])].decode();cursor=sec[4]+offset+8;value=0;shift=0
   while True:
    b=data[cursor];cursor+=1;value|=(b&127)<<shift
    if b<128:break
    shift+=7;need(shift<=63,'bounded ULEB128')
   result[symbol]=value
 need(result and 'keyboard_decode' in result,'actual ABI stack metadata')
 return result

def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--archive',type=Path,required=True);ap.add_argument('--rustc',required=True);ap.add_argument('--out',type=Path,required=True)
 a=ap.parse_args();out=a.out.resolve();archive=a.archive.resolve();need(out.is_relative_to(ROOT/'build') and not out.exists(),'fresh confined output');out.mkdir(parents=True)
 report=dict(status='RUNNING',commands=[],Rust_sanitized=False,archive_sha256=sha(archive),seed='72b15e7deca0de01')
 sources=['kernel/rust/input/ps2_keyboard.rs','arch/x86_64/rust/ps2_keyboard.rs','arch/x86_64/keyboard.c','arch/x86_64/keyboard.h','tests/reference/ps2-keyboard.c','tests/host/ps2-keyboard-differential.c','tests/rust/ps2_keyboard.rs']
 report['source_sha256']={n:sha(ROOT/n) for n in sources}
 def run(argv,name,expected=0):
  argv=list(map(str,argv));record=dict(argv=argv,log=name);report['commands'].append(record)
  with (out/name).open('x') as f:
   try:p=subprocess.run(argv,cwd=ROOT,stdout=f,stderr=subprocess.STDOUT,timeout=180,env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1'))
   except subprocess.TimeoutExpired:record['timed_out']=True;raise
  record['exit_code']=p.returncode;need(p.returncode==expected,'unexpected exit: '+name)
  return (out/name).read_text()
 try:
  ref=json.loads((ROOT/'tests/reference/ps2-keyboard.json').read_text());need(ref['reference_sha256']==sha(ROOT/'tests/reference/ps2-keyboard.c'),'frozen reference identity')
  cc=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Iarch/x86_64','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fno-pie','-no-pie']
  for mode,flags in [('sanitizer',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer']),('release',['-O2','-fno-lto'])]:
   obj=out/(mode+'-reference.o');binary=out/(mode+'-differential')
   run(cc+flags+['-Dkeyboard_decode=keyboard_decode_reference','-c','tests/reference/ps2-keyboard.c','-o',obj],mode+'-reference-build.txt')
   run(cc+flags+['tests/host/ps2-keyboard-differential.c',obj,archive,'-o',binary],mode+'-build.txt');report[mode]=run([binary],mode+'.txt')
   if mode=='release':
    command=[binary,'--bench']
    if hasattr(os,'sched_getaffinity'):command=['taskset','-c',str(min(os.sched_getaffinity(0))),*command]
    report['performance']=run(command,'performance.csv')
  root=out/'core-root.rs';root.write_text('#![no_std]\n#[path='+json.dumps(str(ROOT/'kernel/rust/input/ps2_keyboard.rs'))+'] mod decoder;\npub use decoder::Decoder;\n')
  common=[a.rustc,'--edition=2024','-Dwarnings','-Copt-level=2','-Cdebuginfo=0','-Coverflow-checks=yes'];lib=out/'libps2_keyboard_core.rlib'
  run(common+['--crate-type=rlib','--crate-name=ps2_keyboard_core',root,'-o',lib],'core-build.txt')
  def test(library,binary,name,target=None):
   run(common+['--test','-Cstrip=symbols','--extern','ps2_keyboard_core='+str(library),'tests/rust/ps2_keyboard.rs','-o',binary],name+'-build.txt')
   return run([binary,'--test-threads=1',*([target] if target else [])],name+'.txt',101 if target else 0)
  report['core_tests']=test(lib,out/'core-tests','core-tests');core=(ROOT/'kernel/rust/input/ps2_keyboard.rs').read_text()
  mutations=[
   ('shift','bit = if code == 0x2a { 1 } else { 2 };','bit = if code == 0x2a { 1 } else { 1 };','independent_shift'),
   ('caps-repeat','if !up && self.caps_down == 0','if !up','caps_repeat'),
   ('caps-xor','u8::from(self.shift != 0) ^ self.caps','u8::from(self.shift != 0) | self.caps','caps_xor'),
   ('extended','if ext {\n            return 0;','if ext && code != 0x2a {\n            return 0;','fake_shift'),
   ('pause','self.pause_left = 5;','self.pause_left = 4;','pause'),
   ('ctrl','if up || self.ctrl != 0 || self.alt != 0','if up || self.alt != 0','ctrl'),
   ('alt','if up || self.ctrl != 0 || self.alt != 0','if up || self.ctrl != 0','alt'),
   ('break','if up || self.ctrl != 0 || self.alt != 0','if self.ctrl != 0 || self.alt != 0','breaks'),
   ('punctuation','bytes[0x02] = 33;','bytes[0x02] = 63;','punctuation')]
  report['mutations']={}
  for name,old,new,target in mutations:
   need(core.count(old)==1,'unique mutation anchor '+name);folder=out/('mutant-'+name);folder.mkdir();(folder/'decoder.rs').write_text(core.replace(old,new));root=folder/'root.rs'
   root.write_text('#![no_std]\n#[path="decoder.rs"] mod decoder; pub use decoder::Decoder;\n');ml=folder/'libps2_keyboard_core.rlib'
   run(common+['--crate-type=rlib','--crate-name=ps2_keyboard_core',root,'-o',ml],'mutant-'+name+'-core-build.txt')
   text=test(ml,folder/'tests','mutant-'+name,target);need(target+' ... FAILED' in text,'intended assertion detects '+name);report['mutations'][name]=dict(assertion=target,detected=True)
  root=out/'ffi-root.rs';root.write_text('#![no_std]\n#[path='+json.dumps(str(ROOT/'arch/x86_64/rust/ps2_keyboard.rs'))+'] mod bridge;\n');obj=out/'host-ffi.o'
  run(common+['--crate-type=lib','-Cpanic=abort','--emit=obj',root,'-o',obj],'overflow-ffi-build.txt')
  run(cc+['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','tests/host/ps2-keyboard-differential.c',out/'sanitizer-reference.o',obj,'-o',out/'overflow-differential'],'overflow-differential-build.txt')
  report['overflow_ffi']=run([out/'overflow-differential'],'overflow-differential.txt');obj=out/'target-ffi.o'
  run(common+['--crate-type=lib','--target=x86_64-unknown-none','-Cpanic=abort','-Ccode-model=kernel','-Crelocation-model=static','-Cno-redzone=yes','-Cforce-unwind-tables=no','-Zemit-stack-sizes','--emit=obj',root,'-o',obj],'target-build.txt')
  report['stack_frames']=frames(obj);need(sum(report['stack_frames'].values())<=128,'conservative complete-object frame sum <=128')
  report['undefined']=run(['nm','-u',obj],'target-undefined.txt');need(all(l.split()[-1] in ['memcpy','memset'] for l in report['undefined'].splitlines()),'no unexpected runtime dependency')
  report['sections']=run(['size','-A',obj],'target-size.txt');report['disassembly']=run(['objdump','-dr',obj],'target-disassembly.txt')
  need(not any(x in report['disassembly'] for x in ['%xmm','%ymm','%zmm','%st(','%mm']),'general registers only');report['status']='PASS'
 except Exception as error:report.update(status='FAIL',error=str(error));raise
 finally:
  with (out/'result.json').open('x') as f:json.dump(report,f,indent=2)
 print(json.dumps({k:v for k,v in report.items() if k not in ['commands','performance','disassembly','sections']},indent=2))
if __name__=='__main__':main()
