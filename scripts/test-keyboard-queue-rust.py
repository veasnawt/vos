#!/usr/bin/env python3
"""Actual archive queue extraction, frozen C oracle and isolated shared mutants.
Fresh confined outputs only. No cleanup or historical evidence changes.
"""
import argparse,ast,hashlib,json,os,re,subprocess,csv,io,statistics
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def need(x,s):
 if not x:raise RuntimeError(s)
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--archive',type=Path,required=True);ap.add_argument('--rustc',required=True);ap.add_argument('--out',type=Path,required=True);a=ap.parse_args();out=a.out.resolve();archive=a.archive.resolve();need(not out.exists() and out.is_relative_to(ROOT/'build') and archive.is_file(),'fresh confined output');out.mkdir(parents=True)
 report={'status':'RUNNING','archive_sha256':sha(archive),'commands':[],'Rust_sanitized':False,'seed':'7232647261696e31'}
 def run(cmd,name,expect=0):
  cmd=list(map(str,cmd));row={'argv':cmd,'log':name};report['commands'].append(row)
  with (out/name).open('x') as f:p=subprocess.run(cmd,cwd=ROOT,stdout=f,stderr=subprocess.STDOUT,timeout=240)
  row['exit']=p.returncode;need(p.returncode==expect,'unexpected exit '+name);return (out/name).read_text()
 try:
  ref=json.loads((ROOT/'tests/reference/ps2-keyboard-queue.json').read_text());need(sha(ROOT/'tests/reference/ps2-keyboard-queue.c')==ref['reference_sha256'],'frozen accepted source')
  cc=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Iarch/x86_64','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fno-pie','-no-pie'];defs=['-D'+n+'='+n+'_reference' for n in ['keyboard_init','keyboard_poll','keyboard_suspend','keyboard_resume','keyboard_pointer_start','keyboard_pointer_stop','keyboard_pointer_poll','keyboard_pointer_lost']]
  for mode,flags in [('sanitizer',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer']),('release',['-O2','-fno-lto'])]:
   scalar=flags+(['-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic','-mno-red-zone','-mgeneral-regs-only','-mcmodel=kernel'] if mode=='release' else [])
   run([*cc[:5],'-Itests/reference/keyboard-queue-headers',*cc[5:],*scalar,*defs,'-c','tests/reference/ps2-keyboard-queue.c','-o',out/(mode+'-reference.o')],mode+'-reference-build.txt')
   run(cc+scalar+['-c','arch/x86_64/keyboard.c','-o',out/(mode+'-driver.o')],mode+'-driver-build.txt');run(cc+flags+['tests/host/keyboard-queue-differential.c',out/(mode+'-reference.o'),out/(mode+'-driver.o'),archive,'-o',out/(mode+'-differential')],mode+'-build.txt');report[mode]=run([out/(mode+'-differential')],mode+'.txt');need('782855 routed driver comparisons' in report[mode],'exact differential coverage')
  bench=[out/'release-differential','--bench'];report['cpu']=min(os.sched_getaffinity(0));raw=run(['taskset','-c',str(report['cpu']),*bench],'performance.csv');records=list(csv.DictReader(io.StringIO(raw)));labels=['unready','empty','queued','full','pointer_pending','refill','mixed','burst32','ordinary_control'];report['performance']={}
  for n,label in enumerate(labels):
   rows=[x for x in records if int(x['workload'])==n];c=statistics.median(float(x['C_ns']) for x in rows);r=statistics.median(float(x['Rust_ns']) for x in rows);report['performance'][label]={'C_ns':c,'Rust_ns':r,'percent':100*(r/c-1)}
  source=(ROOT/'kernel/rust/input/ps2_routed.rs').read_text();units=(ROOT/'tests/rust/ps2_routed.rs').read_text().replace('extern crate ps2_routed_core;','').replace('ps2_routed_core::','self::policy::')
  oldtree=ast.parse((ROOT/'scripts/test-ps2-routed-rust.py').read_text());old=next(ast.literal_eval(n.value) for n in ast.walk(oldtree) if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='mutations' for t in n.targets));need(len(old)==19,'retained legacy controls')
  mutations=old+[
   ('queue-ready','if !s.keyboard_ready()','if false','queue_unready'),('queue-drain','    s.fill_keyboard_queue();','','queue_empty'),('queue-empty','if q.count == 0','if q.count == 1','queue_fifo'),('queue-slot','q.keys.get_mut(q.head as usize)','q.keys.get_mut((q.head.wrapping_add(1)%LIMIT) as usize)','queue_fifo'),('queue-clear','    *slot = 0;','','queue_fifo'),('queue-head','q.head.wrapping_add(1) % LIMIT','q.head % LIMIT','queue_fifo'),('queue-count','q.count.wrapping_sub(1)','q.count.wrapping_sub(2)','queue_fifo'),('queue-nul','    let byte = *slot;','    let byte = *slot; if byte==0 {return 0;}','queue_nul'),('queue-bound','q.head as usize','(q.head % LIMIT) as usize','queue_invalid_head'),('queue-return','    byte\n}','    byte ^ 1\n}','queue_all_bytes'),('guard-zero','address.wrapping_sub(1) < usize::MAX - 88','address <= usize::MAX - 88','queue_guard_equivalence'),('guard-high','usize::MAX - 88','usize::MAX - 80','queue_guard_equivalence')]
  harness=out/'shared-tests.rs';chunks=[];rows=[]
  for name,text,target in [('baseline',source,None),*[(n,source.replace(old,new),target) for n,old,new,target in mutations]]:
   ident=name.replace('-','_');d=out/ident;d.mkdir();policy=d/'policy.rs';policy.write_text(text);chunks.append('pub mod '+ident+' {\n#[path='+json.dumps(str(policy))+'] pub mod policy;\n'+units+'\n}\n');rows.append({'module':ident,'target':target,'policy_sha256':sha(policy)})
  for name,old,new,target in mutations:need(old in source,'unique named mutation '+name)
  harness.write_text(''.join(chunks));common=[a.rustc,'--edition=2024','-Dwarnings','-Copt-level=2','-Cdebuginfo=0','-Coverflow-checks=yes','-Ccodegen-units=1']
  run(common+['--test','-Cstrip=symbols',harness,'-o',out/'shared-tests'],'shared-build.txt');need((out/'shared-tests').stat().st_size<=3500000,'shared test-runtime budget3.5MB')
  report['core_tests']=run([out/'shared-tests','--test-threads=1','baseline::'],'baseline-tests.txt');need('32 passed' in report['core_tests'],'32 positive core tests');report['mutations']={}
  for row in rows[1:]:
   target=row['module']+'::'+row['target'];t=run([out/'shared-tests','--test-threads=1','--exact',target],row['module']+'.txt',101);need(target+' ... FAILED' in t,'intended runtime assertion '+target);report['mutations'][row['module']]={'assertion':target,'detected':True,'policy_sha256':row['policy_sha256']}
  report['shared_harness']={'binary_bytes':(out/'shared-tests').stat().st_size,'binary_sha256':sha(out/'shared-tests'),'variants':len(rows),'sources':[r['policy_sha256'] for r in rows],'runtime_executables':1,'method':'Distinct namespace/type per variant, identical positive test fixture; intended assertions run individually, isolated process per mutant. No state shared between variants.'}
  root=out/'checked-root.rs';root.write_text('#![no_std]\n'+''.join('#[path='+json.dumps(str(ROOT/'arch/x86_64/rust'/n))+'] mod '+n[:-3]+';\n' for n in ['ps2_routed.rs','ps2_pointer.rs','ps2_keyboard.rs'])+'unsafe extern "C" {fn abort()->!;}\n#[panic_handler] fn checked_panic(_: &core::panic::PanicInfo)->! {unsafe{abort()}}\n')
  checked=out/'libchecked.a';run(common+['--crate-type=staticlib','-Cpanic=abort',root,'-o',checked],'checked-rust-build.txt');run(cc+['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','tests/host/keyboard-queue-differential.c',out/'sanitizer-reference.o',out/'sanitizer-driver.o',checked,'-o',out/'checked-differential'],'checked-build.txt');report['checked_Rust']=run([out/'checked-differential'],'checked.txt');need('782855 routed driver comparisons' in report['checked_Rust'],'checked complete coverage')
  asm=run(['objdump','-d',archive],'production-disassembly.txt');frames={}
  for m in re.finditer(r'^[0-9a-f]+ <([^>]+)>:\n(.*?)(?=\n\n)',asm,re.M|re.S):
   if m[1].startswith('vos_rust_keyboard_') or 'ps2_routed12pointer_step' in m[1]:frames[m[1]]=8*len(re.findall(r'\tpush\s',m[2]))+sum(int(x,16) for x in re.findall(r'\tsub\s+\$0x([0-9a-f]+),%rsp',m[2]))
  need(frames.get('vos_rust_keyboard_routed_poll',999)<=24,'actual production own queue frame<=24');need(frames['vos_rust_keyboard_routed_drain']<=40 and frames['vos_rust_keyboard_pointer_poll']<=24 and frames['vos_rust_keyboard_pointer_lost']==0,'retained budgets');report['frames']=frames
  symbols=run(['nm','--defined-only',archive],'archive-owners.txt');need(sum(x.endswith(' T vos_rust_keyboard_routed_poll') for x in symbols.splitlines())==1,'one Rust owner');c=run(['nm',out/'release-driver.o'],'C-symbols.txt');need(' U vos_rust_keyboard_routed_poll' in c and ' T vos_rust_keyboard_routed_poll' not in c,'C only forwards')
  report['source_sha256']={n:sha(ROOT/n) for n in ['kernel/rust/input/ps2_routed.rs','arch/x86_64/rust/ps2_routed.rs','arch/x86_64/keyboard.c','arch/x86_64/keyboard.h','tests/host/keyboard-queue-differential.c','tests/rust/ps2_routed.rs','scripts/test-keyboard-queue-rust.py']};report['status']='PASS'
 except Exception as e:report.update(status='FAIL',error=str(e));raise
 finally:
  with (out/'result.json').open('x') as f:json.dump(report,f,indent=2)
 print(json.dumps({k:v for k,v in report.items() if k!='commands'},indent=2))
if __name__=='__main__':main()
