#!/usr/bin/env python3
"""R2-W bounded PS/2 controller writes: actual archive, frozen accepted C oracle (74abed0), read+write traces, lifecycle, isolated mutants.
Fresh confined outputs only. No cleanup or historical evidence changes.
"""
import argparse,hashlib,json,os,re,subprocess,csv,io,statistics
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
COVERAGE='PASS 553774 controller write comparisons and 3000 lifecycle scenarios'
CALLERS=['keyboard_init','keyboard_suspend','keyboard_resume','keyboard_pointer_start','keyboard_pointer_stop']
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def need(x,s):
 if not x:raise RuntimeError(s)
MUTATIONS=[ # (name, exact source text, replacement, intended failing test)
 ('bound','for _ in 0..WAIT_LIMIT {','for _ in 0..WAIT_LIMIT - 1 {','last_iteration_writes'),
 ('limit','pub const WAIT_LIMIT: u32 = 100_000;','pub const WAIT_LIMIT: u32 = 100_001;','one_past_limit_fails'),
 ('ibf','if s.read(STATUS) & IBF == 0 {','if s.read(STATUS) & (IBF >> 1) == 0 {','write_waits_while_busy'),
 ('status-port','if s.read(STATUS) & IBF == 0 {','if s.read(DATA) & IBF == 0 {','write_immediate'),
 ('write-order','        if s.read(STATUS) & IBF == 0 {\n            return s.write(port.address(), value);\n        }',
  '        let wrote = s.write(port.address(), value);\n        if s.read(STATUS) & IBF == 0 {\n            return wrote;\n        }','write_immediate'),
 ('missing-write','return s.write(port.address(), value);','let _ = (port, value);\n            return true;','write_immediate'),
 ('extra-write','return s.write(port.address(), value);','return s.write(port.address(), value) && s.write(port.address(), value);','write_immediate'),
 ('timeout-write','    }\n    false\n}','    }\n    s.write(port.address(), value)\n}','timeout_no_write'),
 ('write-result','return s.write(port.address(), value);','s.write(port.address(), value);\n            return true;','write_failure_reported'),
 ('value','s.write(port.address(), value)','s.write(port.address(), value | 1)','write_value_exact'),
 ('port-address','Port::Data => DATA,','Port::Data => STATUS,','write_immediate'),
 ('port-map','DATA => Some(Port::Data),','DATA => Some(Port::Status),','port_mapping'),
 ('port-refusal','            _ => None,','            _ => Some(Port::Data),','port_mapping'),
 ('command-port','write_port(s, Port::Data, value) && read_channel(s, Channel::Keyboard)','write_port(s, Port::Status, value) && read_channel(s, Channel::Keyboard)','command_success'),
 ('command-channel','read_channel(s, Channel::Keyboard) == Some(ACK)','read_channel(s, Channel::Aux) == Some(ACK)','command_success'),
 ('command-ack','read_channel(s, Channel::Keyboard) == Some(ACK)','read_channel(s, Channel::Keyboard).is_some()','command_nak'),
 ('command-short-circuit','write_port(s, Port::Data, value) && read_channel','(write_port(s, Port::Data, value) | true) && read_channel','command_write_failure_skips_read'),
 ('ack-value','const ACK: u8 = 0xFA;','const ACK: u8 = 0xFE;','command_success'),
 ('aux-prefix','const AUX_PREFIX: u8 = 0xD4;','const AUX_PREFIX: u8 = 0xD3;','aux_success'),
 ('aux-prefix-port','write_port(s, Port::Status, AUX_PREFIX)','write_port(s, Port::Data, AUX_PREFIX)','aux_success'),
 ('aux-missing-prefix','    write_port(s, Port::Status, AUX_PREFIX)\n        && write_port(s, Port::Data, value)','    AUX_PREFIX != 0\n        && write_port(s, Port::Data, value)','aux_success'),
 ('aux-order','    write_port(s, Port::Status, AUX_PREFIX)\n        && write_port(s, Port::Data, value)',
  '    write_port(s, Port::Data, value)\n        && write_port(s, Port::Status, AUX_PREFIX)','aux_success'),
 ('aux-short-circuit','write_port(s, Port::Status, AUX_PREFIX)\n        && write_port','(write_port(s, Port::Status, AUX_PREFIX) | true)\n        && write_port','aux_prefix_failure_stops'),
 ('aux-channel','read_channel(s, Channel::Aux) == Some(ACK)','read_channel(s, Channel::Keyboard) == Some(ACK)','aux_success'),
 ('aux-ack','read_channel(s, Channel::Aux) == Some(ACK)','read_channel(s, Channel::Aux).is_some()','aux_nak'),
]
WRITE_ENTRIES=['vos_rust_keyboard_write_port','vos_rust_keyboard_command','vos_rust_keyboard_aux_command']
def frame(asm,name):
 m=re.search(r'^[0-9a-f]+ <'+re.escape(name)+r'>:\n(.*?)(?=\n\n|\Z)',asm,re.M|re.S);need(m,'function '+name)
 return 8*len(re.findall(r'\tpush\s',m[1]))+sum(int(x,16) for x in re.findall(r'\tsub\s+\$0x([0-9a-f]+),%rsp',m[1]))
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--archive',type=Path,required=True);ap.add_argument('--rustc',required=True);ap.add_argument('--out',type=Path,required=True);a=ap.parse_args()
 out=a.out.resolve();archive=a.archive.resolve();need(not out.exists() and out.is_relative_to(ROOT/'build') and archive.is_file(),'fresh confined output');out.mkdir(parents=True)
 report={'status':'RUNNING','archive_sha256':sha(archive),'commands':[],'Rust_sanitized':False,'seed':'7232772d77726974'}
 def run(cmd,name,expect=0):
  cmd=list(map(str,cmd));row={'argv':cmd,'log':name};report['commands'].append(row)
  with (out/name).open('x') as f:p=subprocess.run(cmd,cwd=ROOT,stdout=f,stderr=subprocess.STDOUT,timeout=900)
  row['exit']=p.returncode;need(p.returncode==expect,'unexpected exit '+name);return (out/name).read_text()
 try:
  ref=json.loads((ROOT/'tests/reference/ps2-controller-write.json').read_text())
  need(sha(ROOT/'tests/reference/ps2-controller-write.c')==ref['reference_sha256']==ref['accepted_source_sha256'],'frozen accepted source')
  need(sha(ROOT/'tests/reference/ps2-controller-write-wrappers.c')==ref['wrappers_sha256'],'frozen test-only wrappers')
  for h,v in ref['headers_sha256'].items():need(sha(ROOT/'tests/reference/controller-write-headers'/h)==v,'frozen header '+h)
  cc=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Iarch/x86_64','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fno-pie','-no-pie']
  defs=['-D'+n+'='+n+'_reference' for n in ['keyboard_init','keyboard_poll','keyboard_suspend','keyboard_resume','keyboard_pointer_start','keyboard_pointer_stop','keyboard_pointer_poll','keyboard_pointer_lost']]
  refcc=[*cc[:5],'-Itests/reference/controller-write-headers',*cc[5:]]
  for mode,flags in [('sanitizer',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer']),('release',['-O2','-fno-lto'])]:
   scalar=flags+(['-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic','-mno-red-zone','-mgeneral-regs-only','-mcmodel=kernel'] if mode=='release' else [])
   run(refcc+scalar+defs+['-c','tests/reference/ps2-controller-write-wrappers.c','-o',out/(mode+'-reference.o')],mode+'-reference-build.txt')
   run(cc+scalar+['-c','arch/x86_64/keyboard.c','-o',out/(mode+'-driver.o')],mode+'-driver-build.txt')
   run(cc+flags+['tests/host/keyboard-controller-write-differential.c',out/(mode+'-reference.o'),out/(mode+'-driver.o'),archive,'-o',out/(mode+'-differential')],mode+'-build.txt')
   report[mode]=run([out/(mode+'-differential')],mode+'.txt');need(COVERAGE in report[mode],'exact differential coverage '+mode)
  # Unwrapped accepted object with production kernel flags: the frame oracle for the C callers.
  run(refcc+['-O2','-fno-lto','-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic','-mno-red-zone','-mgeneral-regs-only','-mcmodel=kernel']+defs+['-c','tests/reference/ps2-controller-write.c','-o',out/'release-reference-plain.o'],'reference-plain-build.txt')
  report['cpu']=min(os.sched_getaffinity(0));raw=run(['taskset','-c',str(report['cpu']),out/'release-differential','--bench'],'performance.csv')
  records=list(csv.DictReader(io.StringIO(raw)));labels=['write_immediate','write_delay10','command_ack','aux_command_ack','command_nak','write_timeout100k'];report['performance']={}
  for n,label in enumerate(labels):
   rows=[x for x in records if int(x['workload'])==n];c=statistics.median(float(x['C_ns']) for x in rows);r=statistics.median(float(x['Rust_ns']) for x in rows)
   report['performance'][label]={'C_ns':c,'Rust_ns':r,'percent':100*(r/c-1)}
  report['regressions_over_5_percent']=sorted(k for k,v in report['performance'].items() if v['percent']>5)
  source=(ROOT/'kernel/rust/input/ps2_controller_write.rs').read_text()
  units=(ROOT/'tests/rust/ps2_controller_write.rs').read_text().replace('extern crate ps2_controller_read_core;','').replace('extern crate ps2_controller_write_core;','').replace('ps2_controller_write_core::','self::policy::').replace('ps2_controller_read_core::','self::controller_read::')
  readmod='#[path='+json.dumps(str(ROOT/'kernel/rust/input/ps2_controller_read.rs'))+'] pub mod controller_read;\n'
  for name,old,new,target in MUTATIONS:need(source.count(old)==1,'unique named mutation '+name);need('fn '+target+'(' in units,'mutation target exists '+target)
  harness=out/'shared-tests.rs';chunks=[];rows=[]
  for name,text,target in [('baseline',source,None),*[(n,source.replace(o,w),t) for n,o,w,t in MUTATIONS]]:
   ident=name.replace('-','_');d=out/ident;d.mkdir();policy=d/'policy.rs';policy.write_text(text)
   chunks.append('pub mod '+ident+' {\n'+readmod+'#[path='+json.dumps(str(policy))+'] pub mod policy;\n'+units+'\n}\n');rows.append({'module':ident,'target':target,'policy_sha256':sha(policy)})
  harness.write_text(''.join(chunks));common=[a.rustc,'--edition=2024','-Dwarnings','-Copt-level=2','-Cdebuginfo=0','-Coverflow-checks=yes','-Ccodegen-units=1']
  run(common+['--test','-Cstrip=symbols',harness,'-o',out/'shared-tests'],'shared-build.txt');need((out/'shared-tests').stat().st_size<=3500000,'shared test-runtime budget3.5MB')
  report['core_tests']=run([out/'shared-tests','--test-threads=1','baseline::'],'baseline-tests.txt');need('24 passed' in report['core_tests'],'24 positive core tests');report['mutations']={}
  for row in rows[1:]:
   target=row['module']+'::'+row['target'];t=run([out/'shared-tests','--test-threads=1','--exact',target],row['module']+'.txt',101)
   need(target+' ... FAILED' in t,'intended runtime assertion '+target);report['mutations'][row['module']]={'assertion':target,'detected':True,'policy_sha256':row['policy_sha256']}
  report['shared_harness']={'binary_bytes':(out/'shared-tests').stat().st_size,'binary_sha256':sha(out/'shared-tests'),'variants':len(rows),'runtime_executables':1,
   'method':'Distinct namespace per variant, identical positive fixture; intended assertions run individually, isolated process per mutant.'}
  root=out/'checked-root.rs';root.write_text('#![no_std]\n'+''.join('#[path='+json.dumps(str(ROOT/'arch/x86_64/rust'/n))+'] mod '+n[:-3]+';\n' for n in ['ps2_routed.rs','ps2_pointer.rs','ps2_keyboard.rs'])+'unsafe extern "C" {fn abort()->!;}\n#[panic_handler] fn checked_panic(_: &core::panic::PanicInfo)->! {unsafe{abort()}}\n')
  checked=out/'libchecked.a';run(common+['--crate-type=staticlib','-Cpanic=abort',root,'-o',checked],'checked-rust-build.txt')
  run(cc+['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','tests/host/keyboard-controller-write-differential.c',out/'sanitizer-reference.o',out/'sanitizer-driver.o',checked,'-o',out/'checked-differential'],'checked-build.txt')
  report['checked_Rust']=run([out/'checked-differential'],'checked.txt');need(COVERAGE in report['checked_Rust'],'checked complete coverage')
  asm=run(['objdump','-d',archive],'production-disassembly.txt');cref=run(['objdump','-d',out/'release-reference-plain.o'],'reference-disassembly.txt');cdrv=run(['objdump','-d',out/'release-driver.o'],'driver-disassembly.txt')
  frames={n:frame(asm,n) for n in WRITE_ENTRIES+['vos_rust_keyboard_read_keyboard','vos_rust_keyboard_read_aux','vos_rust_keyboard_ordinary_poll','vos_rust_keyboard_routed_poll']}
  frames['callers']={n:{'C_reference':frame(cref,n+'_reference'),'candidate':frame(cdrv,n)} for n in CALLERS}
  report['frames']=frames
  need(all(frames[n]<=40 for n in WRITE_ENTRIES),'own write-helper frames <= 40 B')
  need(frames['vos_rust_keyboard_read_keyboard']<=40 and frames['vos_rust_keyboard_read_aux']<=40,'retained read-helper frames <= 40 B')
  need(all(v['candidate']<=v['C_reference'] for v in frames['callers'].values()),'C caller frames not larger than accepted C')
  need(frames['vos_rust_keyboard_ordinary_poll']<=40 and frames['vos_rust_keyboard_routed_poll']<=24,'retained ordinary/routed budgets')
  symbols=run(['nm','--defined-only',archive],'archive-owners.txt')
  for n in WRITE_ENTRIES:need(sum(x.endswith(' T '+n) for x in symbols.splitlines())==1,'one Rust owner '+n)
  c=run(['nm',out/'release-driver.o'],'C-symbols.txt');need(all(' U '+n in c for n in WRITE_ENTRIES),'C only forwards')
  driver_src=(ROOT/'arch/x86_64/keyboard.c').read_text();need('->out(' not in driver_src and '#define IBF' not in driver_src,'no C controller write or IBF wait loop remains in the driver')
  report['source_sha256']={n:sha(ROOT/n) for n in ['kernel/rust/input/ps2_controller_write.rs','kernel/rust/input/ps2_controller_read.rs','arch/x86_64/rust/ps2_routed.rs','arch/x86_64/keyboard.c','arch/x86_64/rust_kernel.h',
   'tests/host/keyboard-controller-write-differential.c','tests/rust/ps2_controller_write.rs','tests/reference/ps2-controller-write.c','tests/reference/ps2-controller-write-wrappers.c','scripts/test-ps2-controller-write-rust.py']};report['status']='PASS'
 except Exception as e:report.update(status='FAIL',error=str(e));raise
 finally:
  with (out/'result.json').open('x') as f:json.dump(report,f,indent=2)
 print(json.dumps({k:v for k,v in report.items() if k!='commands'},indent=2))
if __name__=='__main__':main()
