#!/usr/bin/env python3
"""R2-L PS/2 controller lifecycle: actual archive, frozen accepted C oracle (ed3b496), read+write+object traces, isolated mutants.
Fresh confined outputs only. No cleanup or historical evidence changes.
"""
import argparse,hashlib,json,os,re,subprocess,csv,io,statistics
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
COVERAGE='PASS 25133 lifecycle comparisons and 3000 random fault scenarios'
CALLERS=['keyboard_init','keyboard_suspend','keyboard_resume','keyboard_pointer_start','keyboard_pointer_stop']
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def need(x,s):
 if not x:raise RuntimeError(s)
MUTATIONS=[ # (name, exact source text, replacement, intended failing test)
 ('init-reset-late','    s.reset_object();\n    if !s.io_complete() {\n        return false;\n    }','    if !s.io_complete() {\n        return false;\n    }\n    s.reset_object();','init_rejects_incomplete_io'),
 ('disable-order','s.write_port(Port::Status, 0xAD) || !s.write_port(Port::Status, 0xA7)','s.write_port(Port::Status, 0xA7) || !s.write_port(Port::Status, 0xAD)','init_success_sequence'),
 ('drain-bound','while drained < DRAIN_LIMIT &&','while drained <= DRAIN_LIMIT &&','init_drain_edges'),
 ('drain-limit','pub const DRAIN_LIMIT: u32 = 32;','pub const DRAIN_LIMIT: u32 = 33;','drain_limit_value'),
 ('drain-final','    if s.read_original(STATUS) & OBF != 0 {\n        return false;\n    }\n','    if s.read_original(STATUS) & OBF != 0 && OBF == 0 {\n        return false;\n    }\n','init_drain_edges'),
 ('drain-object-io','while drained < DRAIN_LIMIT && s.read_original(STATUS) & OBF != 0 {','while drained < DRAIN_LIMIT && s.read(STATUS) & OBF != 0 {','init_success_sequence'),
 ('mode-port','if !s.write_port(Port::Status, 0x20) {','if !s.write_port(Port::Data, 0x20) {','init_success_sequence'),
 ('mode-set','const MODE_SET: u8 = 0x60;','const MODE_SET: u8 = 0x40;','init_mode_mask'),
 ('mode-clear','let mode = (mode | MODE_SET) & !MODE_CLEAR;','let mode = (mode | MODE_SET) & !(MODE_CLEAR & 0x03);','init_mode_mask'),
 ('mode-order','    if !s.write_port(Port::Status, 0x60)\n        || !s.write_port(Port::Data, mode)','    if !s.write_port(Port::Data, mode)\n        || !s.write_port(Port::Status, 0x60)','init_success_sequence'),
 ('command-order','        || !s.command(0xF0)\n        || !s.command(0x02)','        || !s.command(0x02)\n        || !s.command(0xF0)','init_success_sequence'),
 ('missing-command','        || !s.command(0x02)\n        || !s.command(0xF4)\n','        || !s.command(0x02)\n','init_success_sequence'),
 ('ready-early','    let mode = (mode | MODE_SET) & !MODE_CLEAR;\n','    let mode = (mode | MODE_SET) & !MODE_CLEAR;\n    s.set_ready(true);\n','init_fails_at_every_step'),
 ('suspend-noop','    if !s.ready() {\n        return true;\n    }','    if !s.ready() {\n        return false;\n    }','suspend_not_ready_noop'),
 ('suspend-ready-late','    s.set_ready(false);\n    if !s.write_port(Port::Data, 0xF5) {\n        return false;\n    }','    if !s.write_port(Port::Data, 0xF5) {\n        return false;\n    }\n    s.set_ready(false);','suspend_write_failure'),
 ('ack-value','const ACK: u8 = 0xFA;','const ACK: u8 = 0xFE;','suspend_success'),
 ('ack-search-bound','    for _ in 0..DRAIN_LIMIT {\n        match','    for _ in 0..DRAIN_LIMIT + 1 {\n        match','suspend_ack_search_limits'),
 ('ack-read-failure','            None => break,','            None => {}','suspend_read_failure_stops_search'),
 ('suspend-drain-bound','    for _ in 0..DRAIN_LIMIT {\n        if s.read(STATUS)','    for _ in 0..DRAIN_LIMIT + 1 {\n        if s.read(STATUS)','suspend_drain_edges'),
 ('decoder-reset','            s.clear_decoder();\n            return true;','            return true;','suspend_success'),
 ('resume-ready','    if !s.io_present() || s.ready() {','    if !s.io_present() {','resume_requires_io_and_not_ready'),
 ('resume-decoder-late','    s.clear_decoder();\n    if !s.command(0xF4) {\n        return false;\n    }\n    s.set_ready(true);','    if !s.command(0xF4) {\n        return false;\n    }\n    s.clear_decoder();\n    s.set_ready(true);','resume_success'),
 ('resume-ready-missing','        return false;\n    }\n    s.set_ready(true);\n    true\n}\n\n#[inline(always)]\npub fn pointer_start','        return false;\n    }\n    true\n}\n\n#[inline(always)]\npub fn pointer_start','resume_success'),
 ('start-guard','    if !s.ready() || s.pointer_ready() {','    if !s.ready() {','start_requires_ready_not_pointer'),
 ('start-suspend','    if !s.suspend() {\n        return false;\n    }','    s.suspend();','start_suspend_failure_no_writes'),
 ('enable-a8','s.write_port(Port::Status, 0xA8)','s.write_port(Port::Status, 0xAE)','start_success_sequence'),
 ('bat','const BAT_OK: u8 = 0xAA;','const BAT_OK: u8 = 0xAB;','start_bat_values'),
 ('id','const MOUSE_ID: u8 = 0x00;','const MOUSE_ID: u8 = 0x01;','start_id_values'),
 ('aux-order','        && s.aux_command(0xF6)\n        && s.aux_command(0xE6)','        && s.aux_command(0xE6)\n        && s.aux_command(0xF6)','start_success_sequence'),
 ('rollback-missing','    if !ok {\n        s.write_port(Port::Status, 0xA7);\n    }\n','','start_fails_at_every_step_with_rollback'),
 ('rollback-final','        s.write_port(Port::Status, 0xA7);\n        return false;','        return false;','start_resume_failure_rollback'),
 ('resume-skip','    if !s.write_port(Port::Status, 0xAE) || !s.resume() {','    if !s.write_port(Port::Status, 0xAE) {','start_resume_failure_rollback'),
 ('resume-always','    if !s.write_port(Port::Status, 0xAE) || !s.resume() {','    if !s.write_port(Port::Status, 0xAE) | !s.resume() {','start_enable_failure_skips_resume'),
 ('start-reset-order','    s.clear_pointer_lost();\n    s.clear_key_count();\n    s.clear_key_head();\n    s.set_pointer_ready(true);','    s.clear_pointer_lost();\n    s.clear_key_head();\n    s.clear_key_count();\n    s.set_pointer_ready(true);','start_state_reset_order'),
 ('pointer-ready-early','    s.clear_pointer_decoder();\n    s.clear_pointer_pending();\n    s.clear_pointer_lost();\n    s.clear_key_count();\n    s.clear_key_head();\n    s.set_pointer_ready(true);',
  '    s.set_pointer_ready(true);\n    s.clear_pointer_decoder();\n    s.clear_pointer_pending();\n    s.clear_pointer_lost();\n    s.clear_key_count();\n    s.clear_key_head();','start_state_reset_order'),
 ('stop-guard','    if !s.pointer_ready() {\n        return;\n    }','    if !s.pointer_ready() && false {\n        return;\n    }','stop_noop_when_not_pointer'),
 ('stop-best-effort','    s.aux_command(0xF5);','    if !s.aux_command(0xF5) {\n        return;\n    }','stop_best_effort_all_steps'),
 ('stop-reset-missing','    s.clear_key_head();\n    s.clear_pointer_lost();\n}','    s.clear_key_head();\n}','stop_best_effort_all_steps'),
]
LIFE_ENTRIES=['vos_rust_keyboard_init','vos_rust_keyboard_suspend','vos_rust_keyboard_resume','vos_rust_keyboard_pointer_start','vos_rust_keyboard_pointer_stop']
HELPER_ENTRIES=['vos_rust_keyboard_read_keyboard','vos_rust_keyboard_read_aux','vos_rust_keyboard_write_port','vos_rust_keyboard_command','vos_rust_keyboard_aux_command']
def frame(asm,name):
 m=re.search(r'^[0-9a-f]+ <'+re.escape(name)+r'>:\n(.*?)(?=\n\n|\Z)',asm,re.M|re.S);need(m,'function '+name)
 return 8*len(re.findall(r'\tpush\s',m[1]))+sum(int(x,16) for x in re.findall(r'\tsub\s+\$0x([0-9a-f]+),%rsp',m[1]))
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--archive',type=Path,required=True);ap.add_argument('--rustc',required=True);ap.add_argument('--out',type=Path,required=True);a=ap.parse_args()
 out=a.out.resolve();archive=a.archive.resolve();need(not out.exists() and out.is_relative_to(ROOT/'build') and archive.is_file(),'fresh confined output');out.mkdir(parents=True)
 report={'status':'RUNNING','archive_sha256':sha(archive),'commands':[],'Rust_sanitized':False,'seed':'9e3779b9'}
 def run(cmd,name,expect=0):
  cmd=list(map(str,cmd));row={'argv':cmd,'log':name};report['commands'].append(row)
  with (out/name).open('x') as f:p=subprocess.run(cmd,cwd=ROOT,stdout=f,stderr=subprocess.STDOUT,timeout=900)
  row['exit']=p.returncode;need(p.returncode==expect,'unexpected exit '+name);return (out/name).read_text()
 try:
  ref=json.loads((ROOT/'tests/reference/ps2-lifecycle.json').read_text())
  need(sha(ROOT/'tests/reference/ps2-lifecycle.c')==ref['reference_sha256']==ref['accepted_source_sha256'],'frozen accepted source')
  for h,v in ref['headers_sha256'].items():need(sha(ROOT/'tests/reference/lifecycle-headers'/h)==v,'frozen header '+h)
  cc=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Iarch/x86_64','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fno-pie','-no-pie']
  defs=['-D'+n+'='+n+'_reference' for n in ['keyboard_init','keyboard_poll','keyboard_suspend','keyboard_resume','keyboard_pointer_start','keyboard_pointer_stop','keyboard_pointer_poll','keyboard_pointer_lost']]
  refcc=[*cc[:5],'-Itests/reference/lifecycle-headers',*cc[5:]]
  for mode,flags in [('sanitizer',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer']),('release',['-O2','-fno-lto'])]:
   scalar=flags+(['-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic','-mno-red-zone','-mgeneral-regs-only','-mcmodel=kernel'] if mode=='release' else [])
   run(refcc+scalar+defs+['-c','tests/reference/ps2-lifecycle.c','-o',out/(mode+'-reference.o')],mode+'-reference-build.txt')
   run(cc+scalar+['-c','arch/x86_64/keyboard.c','-o',out/(mode+'-driver.o')],mode+'-driver-build.txt')
   run(cc+flags+['tests/host/keyboard-lifecycle-differential.c',out/(mode+'-reference.o'),out/(mode+'-driver.o'),archive,'-o',out/(mode+'-differential')],mode+'-build.txt')
   report[mode]=run([out/(mode+'-differential')],mode+'.txt');need(COVERAGE in report[mode],'exact differential coverage '+mode)
  # Unwrapped accepted object with production kernel flags: the frame oracle for the C callers.
  run(refcc+['-O2','-fno-lto','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic','-mno-red-zone','-mgeneral-regs-only','-mcmodel=kernel']+defs+['-c','tests/reference/ps2-lifecycle.c','-o',out/'release-reference-plain.o'],'reference-plain-build.txt')
  report['cpu']=min(os.sched_getaffinity(0));raw=run(['taskset','-c',str(report['cpu']),out/'release-differential','--bench'],'performance.csv')
  records=list(csv.DictReader(io.StringIO(raw)));labels=['init','suspend_resume','pointer_start','pointer_stop','resume_refused','init_no_io'];report['performance']={}
  for n,label in enumerate(labels):
   rows=[x for x in records if int(x['workload'])==n];c=statistics.median(float(x['C_ns']) for x in rows);r=statistics.median(float(x['Rust_ns']) for x in rows)
   report['performance'][label]={'C_ns':c,'Rust_ns':r,'percent':100*(r/c-1)}
  report['regressions_over_5_percent']=sorted(k for k,v in report['performance'].items() if v['percent']>5)
  source=(ROOT/'kernel/rust/input/ps2_lifecycle.rs').read_text()
  units=(ROOT/'tests/rust/ps2_lifecycle.rs').read_text().replace('extern crate ps2_controller_write_core;','').replace('extern crate ps2_lifecycle_core;','').replace('ps2_controller_write_core::','self::controller_write::').replace('ps2_lifecycle_core::','self::policy::')
  readmod=''.join('#[path='+json.dumps(str(ROOT/'kernel/rust/input'/f))+'] pub mod '+m+';\n' for f,m in (('ps2_controller_read.rs','controller_read'),('ps2_controller_write.rs','controller_write')))
  for name,old,new,target in MUTATIONS:need(source.count(old)==1,'unique named mutation '+name);need('fn '+target+'(' in units,'mutation target exists '+target)
  harness=out/'shared-tests.rs';chunks=[];rows=[]
  for name,text,target in [('baseline',source,None),*[(n,source.replace(o,w),t) for n,o,w,t in MUTATIONS]]:
   ident=name.replace('-','_');d=out/ident;d.mkdir();policy=d/'policy.rs';policy.write_text(text)
   chunks.append('pub mod '+ident+' {\n'+readmod+'#[path='+json.dumps(str(policy))+'] pub mod policy;\n'+units+'\n}\n');rows.append({'module':ident,'target':target,'policy_sha256':sha(policy)})
  harness.write_text(''.join(chunks));common=[a.rustc,'--edition=2024','-Dwarnings','-Copt-level=2','-Cdebuginfo=0','-Coverflow-checks=yes','-Ccodegen-units=1']
  run(common+['--test','-Cstrip=symbols',harness,'-o',out/'shared-tests'],'shared-build.txt');need((out/'shared-tests').stat().st_size<=3500000,'shared test-runtime budget3.5MB')
  report['core_tests']=run([out/'shared-tests','--test-threads=1','baseline::'],'baseline-tests.txt');need('29 passed' in report['core_tests'],'29 positive core tests');report['mutations']={}
  for row in rows[1:]:
   target=row['module']+'::'+row['target'];t=run([out/'shared-tests','--test-threads=1','--exact',target],row['module']+'.txt',101)
   need(target+' ... FAILED' in t,'intended runtime assertion '+target);report['mutations'][row['module']]={'assertion':target,'detected':True,'policy_sha256':row['policy_sha256']}
  report['shared_harness']={'binary_bytes':(out/'shared-tests').stat().st_size,'binary_sha256':sha(out/'shared-tests'),'variants':len(rows),'runtime_executables':1,
   'method':'Distinct namespace per variant, identical positive fixture; intended assertions run individually, isolated process per mutant.'}
  root=out/'checked-root.rs';root.write_text('#![no_std]\n'+''.join('#[path='+json.dumps(str(ROOT/'arch/x86_64/rust'/n))+'] mod '+n[:-3]+';\n' for n in ['ps2_routed.rs','ps2_pointer.rs','ps2_keyboard.rs'])+'unsafe extern "C" {fn abort()->!;}\n#[panic_handler] fn checked_panic(_: &core::panic::PanicInfo)->! {unsafe{abort()}}\n')
  checked=out/'libchecked.a';run(common+['--crate-type=staticlib','-Cpanic=abort',root,'-o',checked],'checked-rust-build.txt')
  run(cc+['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','tests/host/keyboard-lifecycle-differential.c',out/'sanitizer-reference.o',out/'sanitizer-driver.o',checked,'-o',out/'checked-differential'],'checked-build.txt')
  report['checked_Rust']=run([out/'checked-differential'],'checked.txt');need(COVERAGE in report['checked_Rust'],'checked complete coverage')
  asm=run(['objdump','-d',archive],'production-disassembly.txt');cref=run(['objdump','-d',out/'release-reference-plain.o'],'reference-disassembly.txt');cdrv=run(['objdump','-d',out/'release-driver.o'],'driver-disassembly.txt')
  frames={n:frame(asm,n) for n in LIFE_ENTRIES+HELPER_ENTRIES+['vos_rust_keyboard_ordinary_poll','vos_rust_keyboard_routed_poll']}
  frames['callers']={n:{'C_reference':frame(cref,n+'_reference'),'candidate':frame(cdrv,n)} for n in CALLERS}
  report['frames']=frames
  frames['lifecycle_vs_C']={n:{'C_reference':frame(cref,c+'_reference'),'Rust':frames[n]} for n,c in zip(LIFE_ENTRIES,CALLERS)}
  need(all(v['Rust']<=v['C_reference'] for v in frames['lifecycle_vs_C'].values()),'Rust lifecycle frames not larger than accepted C')
  need(all(frames[n]<=40 for n in HELPER_ENTRIES),'retained read/write helper frames <= 40 B')
  need(all(v['candidate']<=v['C_reference'] for v in frames['callers'].values()),'C ABI wrapper frames not larger than accepted C')
  need(frames['vos_rust_keyboard_ordinary_poll']<=40 and frames['vos_rust_keyboard_routed_poll']<=24,'retained ordinary/routed budgets')
  symbols=run(['nm','--defined-only',archive],'archive-owners.txt')
  for n in LIFE_ENTRIES+HELPER_ENTRIES:need(sum(x.endswith(' T '+n) for x in symbols.splitlines())==1,'one Rust owner '+n)
  c=run(['nm',out/'release-driver.o'],'C-symbols.txt');need(all(' U '+n in c for n in LIFE_ENTRIES),'C only forwards')
  driver_src=(ROOT/'arch/x86_64/keyboard.c').read_text();need('->out(' not in driver_src and '->in(' not in driver_src and '#define' not in driver_src,'no C controller access remains in the driver')
  report['source_sha256']={n:sha(ROOT/n) for n in ['kernel/rust/input/ps2_lifecycle.rs','kernel/rust/input/ps2_controller_write.rs','kernel/rust/input/ps2_controller_read.rs','arch/x86_64/rust/ps2_routed.rs','arch/x86_64/keyboard.c','arch/x86_64/rust_kernel.h',
   'tests/host/keyboard-lifecycle-differential.c','tests/rust/ps2_lifecycle.rs','tests/reference/ps2-lifecycle.c','tests/reference/ps2-lifecycle.json','scripts/test-ps2-lifecycle-rust.py','scripts/test-ps2-controller-read-rust.py','scripts/test-ps2-controller-write-rust.py']};report['status']='PASS'
 except Exception as e:report.update(status='FAIL',error=str(e));raise
 finally:
  with (out/'result.json').open('x') as f:json.dump(report,f,indent=2)
 print(json.dumps({k:v for k,v in report.items() if k!='commands'},indent=2))
if __name__=='__main__':main()
