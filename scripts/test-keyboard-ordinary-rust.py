#!/usr/bin/env python3
"""R2 ordinary keyboard_poll: actual archive, frozen accepted C oracle, isolated mutants.
Fresh confined outputs only. No cleanup or historical evidence changes.
"""
import argparse,hashlib,json,os,re,subprocess,csv,io,statistics
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
COVERAGE='301778 ordinary keyboard_poll comparisons'
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def need(x,s):
 if not x:raise RuntimeError(s)
MUTATIONS=[ # (name, exact source text, replacement, intended failing test)
 ('ready','if !s.ready() {','if false {','unready_no_reads'),
 ('bound','for _ in 0..LIMIT {','for _ in 0..LIMIT + 1 {','exact_bound'),
 ('obf','if status & OBF == 0 {','if status == 0 && OBF != 0 {','no_data_when_obf_clear'),
 ('order','let status = s.read(STATUS);','let status = { let _ = s.read(DATA); s.read(STATUS) };','status_before_data'),
 ('aux','if status & AUX != 0 {','if status & AUX == 0xff {','aux_consumed_undecoded'),
 ('aux-precedence','if status & AUX != 0 {\n            continue;','if status & AUX != 0 && status & ERR == 0 {\n            continue;','aux_precedes_error'),
 ('error','status & ERR != 0 ||','status & ERR & 0x80 != 0 ||','error_resets'),
 ('zero','|| byte == 0 ||','||','zero_resets'),
 ('ff','|| byte == 0xFF','','ff_resets'),
 ('reset','            s.reset_decoder();\n','','zero_resets'),
 ('reset-continue','s.reset_decoder();\n            continue;','s.reset_decoder();','zero_resets'),
 ('first','            return c;','            continue;','returns_first_character'),
 ('signed-high','if c != 0 {','if c != 0 && c < 0x80 {','high_bytes_returned_unchanged'),
 ('final-zero','    0\n}','    1\n}','empty_one_status_read'),
]
def frame(asm,name):
 m=re.search(r'^[0-9a-f]+ <'+re.escape(name)+r'>:\n(.*?)(?=\n\n|\Z)',asm,re.M|re.S);need(m,'function '+name)
 return 8*len(re.findall(r'\tpush\s',m[1]))+sum(int(x,16) for x in re.findall(r'\tsub\s+\$0x([0-9a-f]+),%rsp',m[1]))
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--archive',type=Path,required=True);ap.add_argument('--rustc',required=True);ap.add_argument('--out',type=Path,required=True);a=ap.parse_args()
 out=a.out.resolve();archive=a.archive.resolve();need(not out.exists() and out.is_relative_to(ROOT/'build') and archive.is_file(),'fresh confined output');out.mkdir(parents=True)
 report={'status':'RUNNING','archive_sha256':sha(archive),'commands':[],'Rust_sanitized':False,'seed':'72326f7264696e31'}
 def run(cmd,name,expect=0):
  cmd=list(map(str,cmd));row={'argv':cmd,'log':name};report['commands'].append(row)
  with (out/name).open('x') as f:p=subprocess.run(cmd,cwd=ROOT,stdout=f,stderr=subprocess.STDOUT,timeout=600)
  row['exit']=p.returncode;need(p.returncode==expect,'unexpected exit '+name);return (out/name).read_text()
 try:
  ref=json.loads((ROOT/'tests/reference/ps2-keyboard-ordinary.json').read_text())
  need(sha(ROOT/'tests/reference/ps2-keyboard-ordinary.c')==ref['reference_sha256']==ref['accepted_source_sha256'],'frozen accepted source')
  for h,v in ref['headers_sha256'].items():need(sha(ROOT/'tests/reference/keyboard-ordinary-headers'/h)==v,'frozen header '+h)
  cc=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Iarch/x86_64','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fno-pie','-no-pie']
  defs=['-D'+n+'='+n+'_reference' for n in ['keyboard_init','keyboard_poll','keyboard_suspend','keyboard_resume','keyboard_pointer_start','keyboard_pointer_stop','keyboard_pointer_poll','keyboard_pointer_lost']]
  for mode,flags in [('sanitizer',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer']),('release',['-O2','-fno-lto'])]:
   scalar=flags+(['-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic','-mno-red-zone','-mgeneral-regs-only','-mcmodel=kernel'] if mode=='release' else [])
   run([*cc[:5],'-Itests/reference/keyboard-ordinary-headers',*cc[5:],*scalar,*defs,'-c','tests/reference/ps2-keyboard-ordinary.c','-o',out/(mode+'-reference.o')],mode+'-reference-build.txt')
   run(cc+scalar+['-c','arch/x86_64/keyboard.c','-o',out/(mode+'-driver.o')],mode+'-driver-build.txt')
   run(cc+flags+['tests/host/keyboard-ordinary-differential.c',out/(mode+'-reference.o'),out/(mode+'-driver.o'),archive,'-o',out/(mode+'-differential')],mode+'-build.txt')
   report[mode]=run([out/(mode+'-differential')],mode+'.txt');need(COVERAGE in report[mode],'exact differential coverage '+mode)
  report['cpu']=min(os.sched_getaffinity(0));raw=run(['taskset','-c',str(report['cpu']),out/'release-differential','--bench'],'performance.csv')
  records=list(csv.DictReader(io.StringIO(raw)));labels=['unready','empty','one_key','aux_then_key','error_then_key','bound32','shifted','routed_control'];report['performance']={}
  for n,label in enumerate(labels):
   rows=[x for x in records if int(x['workload'])==n];c=statistics.median(float(x['C_ns']) for x in rows);r=statistics.median(float(x['Rust_ns']) for x in rows)
   report['performance'][label]={'C_ns':c,'Rust_ns':r,'percent':100*(r/c-1)}
  report['regressions_over_5_percent']=sorted(k for k,v in report['performance'].items() if v['percent']>5)
  source=(ROOT/'kernel/rust/input/ps2_ordinary.rs').read_text()
  units=(ROOT/'tests/rust/ps2_ordinary.rs').read_text().replace('extern crate ps2_ordinary_core;','').replace('ps2_ordinary_core::','self::policy::')
  for name,old,new,target in MUTATIONS:need(source.count(old)==1,'unique named mutation '+name);need('fn '+target+'(' in units,'mutation target exists '+target)
  harness=out/'shared-tests.rs';chunks=[];rows=[]
  for name,text,target in [('baseline',source,None),*[(n,source.replace(o,w),t) for n,o,w,t in MUTATIONS]]:
   ident=name.replace('-','_');d=out/ident;d.mkdir();policy=d/'policy.rs';policy.write_text(text)
   chunks.append('pub mod '+ident+' {\n#[path='+json.dumps(str(policy))+'] pub mod policy;\n'+units+'\n}\n');rows.append({'module':ident,'target':target,'policy_sha256':sha(policy)})
  harness.write_text(''.join(chunks));common=[a.rustc,'--edition=2024','-Dwarnings','-Copt-level=2','-Cdebuginfo=0','-Coverflow-checks=yes','-Ccodegen-units=1']
  run(common+['--test','-Cstrip=symbols',harness,'-o',out/'shared-tests'],'shared-build.txt');need((out/'shared-tests').stat().st_size<=3500000,'shared test-runtime budget3.5MB')
  report['core_tests']=run([out/'shared-tests','--test-threads=1','baseline::'],'baseline-tests.txt');need('17 passed' in report['core_tests'],'17 positive core tests');report['mutations']={}
  for row in rows[1:]:
   target=row['module']+'::'+row['target'];t=run([out/'shared-tests','--test-threads=1','--exact',target],row['module']+'.txt',101)
   need(target+' ... FAILED' in t,'intended runtime assertion '+target);report['mutations'][row['module']]={'assertion':target,'detected':True,'policy_sha256':row['policy_sha256']}
  report['shared_harness']={'binary_bytes':(out/'shared-tests').stat().st_size,'binary_sha256':sha(out/'shared-tests'),'variants':len(rows),'runtime_executables':1,
   'method':'Distinct namespace per variant, identical positive fixture; intended assertions run individually, isolated process per mutant.'}
  root=out/'checked-root.rs';root.write_text('#![no_std]\n'+''.join('#[path='+json.dumps(str(ROOT/'arch/x86_64/rust'/n))+'] mod '+n[:-3]+';\n' for n in ['ps2_routed.rs','ps2_pointer.rs','ps2_keyboard.rs'])+'unsafe extern "C" {fn abort()->!;}\n#[panic_handler] fn checked_panic(_: &core::panic::PanicInfo)->! {unsafe{abort()}}\n')
  checked=out/'libchecked.a';run(common+['--crate-type=staticlib','-Cpanic=abort',root,'-o',checked],'checked-rust-build.txt')
  run(cc+['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','tests/host/keyboard-ordinary-differential.c',out/'sanitizer-reference.o',out/'sanitizer-driver.o',checked,'-o',out/'checked-differential'],'checked-build.txt')
  report['checked_Rust']=run([out/'checked-differential'],'checked.txt');need(COVERAGE in report['checked_Rust'],'checked complete coverage')
  asm=run(['objdump','-d',archive],'production-disassembly.txt');cref=run(['objdump','-d',out/'release-reference.o'],'reference-disassembly.txt')
  frames={'vos_rust_keyboard_ordinary_poll':frame(asm,'vos_rust_keyboard_ordinary_poll'),'C_reference_keyboard_poll':frame(cref,'keyboard_poll_reference'),
          'vos_rust_keyboard_routed_poll':frame(asm,'vos_rust_keyboard_routed_poll'),'vos_rust_keyboard_routed_drain':frame(asm,'vos_rust_keyboard_routed_drain')}
  report['frames']=frames
  need(frames['vos_rust_keyboard_ordinary_poll']<=40 and frames['vos_rust_keyboard_ordinary_poll']<=frames['C_reference_keyboard_poll'],'own ordinary frame <= measured accepted C and <= 40 B')
  need(frames['vos_rust_keyboard_routed_poll']<=24 and frames['vos_rust_keyboard_routed_drain']<=40,'retained routed budgets')
  symbols=run(['nm','--defined-only',archive],'archive-owners.txt');need(sum(x.endswith(' T vos_rust_keyboard_ordinary_poll') for x in symbols.splitlines())==1,'one Rust owner')
  c=run(['nm',out/'release-driver.o'],'C-symbols.txt');need(' U vos_rust_keyboard_ordinary_poll' in c and ' T vos_rust_keyboard_ordinary_poll' not in c,'C only forwards')
  need(' U keyboard_decode' not in c,'ordinary decode loop no longer in C')
  report['source_sha256']={n:sha(ROOT/n) for n in ['kernel/rust/input/ps2_ordinary.rs','arch/x86_64/rust/ps2_routed.rs','arch/x86_64/keyboard.c','arch/x86_64/rust_kernel.h',
   'tests/host/keyboard-ordinary-differential.c','tests/rust/ps2_ordinary.rs','tests/reference/ps2-keyboard-ordinary.c','scripts/test-keyboard-ordinary-rust.py']};report['status']='PASS'
 except Exception as e:report.update(status='FAIL',error=str(e));raise
 finally:
  with (out/'result.json').open('x') as f:json.dump(report,f,indent=2)
 print(json.dumps({k:v for k,v in report.items() if k!='commands'},indent=2))
if __name__=='__main__':main()
