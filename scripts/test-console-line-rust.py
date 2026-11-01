#!/usr/bin/env python3
"""Bounded production Rust console extraction: ABI, equivalence and performance.
Fresh confined outputs; failures retained. C sanitizers do not instrument Rust.
"""
import argparse,hashlib,json,os,struct,subprocess,csv,io,statistics
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
 need(result and 'input_console_take_line' in result,'actual ABI stack metadata')
 return result

def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--archive',required=True,type=Path);ap.add_argument('--rustc',required=True);ap.add_argument('--out',required=True,type=Path)
 a=ap.parse_args();out=a.out.resolve();archive=a.archive.resolve();need(out.is_relative_to(ROOT/'build') and not out.exists(),'fresh confined output');out.mkdir(parents=True)
 sources=['kernel/rust/input/console_line.rs','arch/x86_64/rust/console_line.rs','arch/x86_64/input_console.c','arch/x86_64/input_console.h','tests/reference/input-console-take-line.c','tests/host/console-line-differential.c','tests/rust/console_line.rs']
 report=dict(status='RUNNING',commands=[],Rust_sanitized=False,archive_sha256=sha(archive),seed='a76c209ecf315b84',source_sha256={n:sha(ROOT/n) for n in sources})
 def run(argv,name,expected=0):
  argv=list(map(str,argv));record=dict(argv=argv,log=name);report['commands'].append(record)
  with (out/name).open('x') as f:
   try:p=subprocess.run(argv,cwd=ROOT,stdout=f,stderr=subprocess.STDOUT,timeout=240,env=dict(os.environ,PYTHONDONTWRITEBYTECODE='1'))
   except subprocess.TimeoutExpired:record['timed_out']=True;raise
  record['exit_code']=p.returncode;need(p.returncode==expected,'unexpected exit: '+name)
  return (out/name).read_text()
 try:
  meta=json.loads((ROOT/'tests/reference/input-console-take-line.json').read_text());need(meta['reference_sha256']==sha(ROOT/'tests/reference/input-console-take-line.c'),'frozen reference identity')
  common=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Iarch/x86_64','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fno-pie','-no-pie']
  # Same integer-only architecture contract as production for the release C oracle.
  release=['-O2','-ffreestanding','-fno-builtin','-fno-stack-protector','-mgeneral-regs-only','-mno-red-zone','-fcf-protection=none','-fno-stack-clash-protection']
  for mode,flags in [('sanitizer',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer']),('release',release)]:
   obj=out/(mode+'-reference.o');binary=out/(mode+'-differential')
   run(common+flags+['-Dinput_console_take_line=input_console_take_line_reference','-c','tests/reference/input-console-take-line.c','-o',obj],mode+'-reference-build.txt')
   run(common+[x for x in flags if x!='-mgeneral-regs-only']+['tests/host/console-line-differential.c',obj,archive,'-o',binary],mode+'-build.txt');report[mode]=run([binary],mode+'.txt')
   if mode=='release':
    command=[binary,'--bench']
    if hasattr(os,'sched_getaffinity'):command=['taskset','-c',str(min(os.sched_getaffinity(0))),*command]
    perf=run(command,'performance.csv');rows=list(csv.DictReader(io.StringIO(perf)));medians={}
    for work in sorted({x['workload'] for x in rows}):
     c=statistics.median(float(x['ns_per_call']) for x in rows if x['workload']==work and x['implementation']=='C');r=statistics.median(float(x['ns_per_call']) for x in rows if x['workload']==work and x['implementation']=='Rust')
     medians[work]=dict(C_ns=c,Rust_ns=r,ratio=r/c,investigation_required=r/c>1.05)
    report['performance']=dict(medians=medians,method='7 alternating-order samples, 5000 batches x256 calls; 10 warm-up batches; identical state preparation outside each timed batch; monotonic clock aggregate, common timing overhead retained; CPU affinity; production Rust archive vs O2 integer-only freestanding C oracle',scope='hosted function-call latency/throughput, not BIOS/UEFI runtime timing',raw='performance.csv',C_release_flags=release)
  report['performance']['acceptance']='EXPLICIT_REVIEW_REQUIRED_FOR_REMAINING_REGRESSIONS' if any(v['ratio']>1 for v in report['performance']['medians'].values()) else 'NO_HOSTED_REGRESSION_MEASURED'
  root=out/'core-root.rs';root.write_text('#![no_std]\n#[path='+json.dumps(str(ROOT/'kernel/rust/input/console_line.rs'))+'] mod core_line; pub use core_line::*;\n')
  compiler=[a.rustc,'--edition=2024','-Dwarnings','-Copt-level=2','-Cdebuginfo=0','-Coverflow-checks=yes'];lib=out/'libconsole_line_core.rlib'
  run(compiler+['--crate-type=rlib','--crate-name=console_line_core',root,'-o',lib],'core-build.txt')
  def test(library,binary,name,target=None):
   run(compiler+['--test','-Cstrip=symbols','--extern','console_line_core='+str(library),'tests/rust/console_line.rs','-o',binary],name+'-build.txt')
   return run([binary,'--test-threads=1',*([target] if target else [])],name+'.txt',101 if target else 0)
  report['core_tests']=test(lib,out/'core-tests','core-tests');core=(ROOT/'kernel/rust/input/console_line.rs').read_text()
  mutations=[('empty-reset','        s.clear();\n        return 0;','        return 0;','zero_length'),('readiness','if !s.ready()','if s.ready()','pending_precedence'),('overflow-clear','if s.overflow() { s.clear(); return TOOLONG; }','if s.overflow() { return TOOLONG; }','overflow_reset'),('capacity-count','length >= s.capacity().min(MAX + 1)','length > s.capacity().min(MAX + 1)','capacity_equality'),('missing-output-check','!s.output_valid(length + 1)','false','null_output'),('terminator','for index in 0..=length','for index in 0..length','includes_terminator'),('copy-order','for index in 0..=length','for index in (0..=length).rev()','forward_overlap'),('length-bound','s.capacity().min(MAX + 1)','s.capacity().min(MAX + 2)','invalid_length'),('success-reset','    s.clear();\n    length as i32','    length as i32','repeated_reset'),('return-length','    length as i32','    (length + 1) as i32','includes_terminator')]
  report['mutations']={}
  for name,old,new,target in mutations:
   need(core.count(old)==1,'unique mutation anchor '+name);d=out/('mutant-'+name);d.mkdir();(d/'core.rs').write_text(core.replace(old,new));root=d/'root.rs';root.write_text('#![no_std]\n#[path="core.rs"] mod core_line; pub use core_line::*;\n');ml=d/'libconsole_line_core.rlib'
   run(compiler+['--crate-type=rlib','--crate-name=console_line_core',root,'-o',ml],'mutant-'+name+'-core-build.txt')
   t=test(ml,d/'tests','mutant-'+name,target);need(target+' ... FAILED' in t,'intended assertion detects '+name);report['mutations'][name]=dict(assertion=target,detected=True)
  root=out/'ffi-root.rs';root.write_text('#![no_std]\n#[path='+json.dumps(str(ROOT/'arch/x86_64/rust/console_line.rs'))+'] mod bridge;\n');obj=out/'host-ffi.o'
  run(compiler+['--crate-type=lib','-Cpanic=abort','--emit=obj',root,'-o',obj],'overflow-ffi-build.txt')
  run(common+['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','tests/host/console-line-differential.c',out/'sanitizer-reference.o',obj,'-o',out/'overflow-differential'],'overflow-build.txt');report['overflow_ffi']=run([out/'overflow-differential'],'overflow.txt')
  obj=out/'target-ffi.o';run(compiler+['--crate-type=lib','--target=x86_64-unknown-none','-Cpanic=abort','-Ccode-model=kernel','-Crelocation-model=static','-Cno-redzone=yes','-Cforce-unwind-tables=no','-Zemit-stack-sizes','--emit=obj',root,'-o',obj],'target-build.txt')
  report['stack_frames']=frames(obj);need(sum(report['stack_frames'].values())<=128,'complete object frame sum <=128')
  report['undefined']=run(['nm','-u',obj],'target-undefined.txt');need(all(l.split()[-1] in ['memcpy','memset'] for l in report['undefined'].splitlines()),'no unexpected runtime dependency')
  report['sections']=run(['size','-A',obj],'target-size.txt');report['disassembly']=run(['objdump','-dr',obj],'target-disassembly.txt')
  report['C_reference_disassembly']=run(['objdump','-dr',out/'release-reference.o'],'C-release-disassembly.txt')
  need(not any(x in report['disassembly'] for x in ['%xmm','%ymm','%zmm','%st(','%mm']),'general registers only');report['status']='PASS'
 except Exception as error:report.update(status='FAIL',error=str(error));raise
 finally:
  with (out/'result.json').open('x') as f:json.dump(report,f,indent=2)
 print(json.dumps({k:v for k,v in report.items() if k not in ['commands','disassembly','sections','C_reference_disassembly']},indent=2))
if __name__=='__main__':main()
