extern crate ps2_controller_write_core;
extern crate ps2_lifecycle_core;
use ps2_controller_write_core::Port;
use ps2_lifecycle_core::{init, pointer_start, pointer_stop, resume, suspend, Init, Lifecycle, DRAIN_LIMIT};
use std::collections::VecDeque;
#[derive(Clone,Copy,PartialEq,Eq,Debug)]
enum Op {Reset,W(u16,u8),Cmd(u8),Aux(u8),RK,RA,R(u16),O(u16),Suspend,Resume,Ready(bool),PtrReady(bool),ClrDec,ClrPtr,ClrPend,ClrLost,ClrHead,ClrCount}
use Op::*;
struct F {ops:Vec<Op>,fail_at:Option<usize>,bools:usize,kbd:VecDeque<Option<u8>>,aux:VecDeque<Option<u8>>,status:VecDeque<u8>,ready:bool,ptr:bool,io:bool,io_complete:bool}
impl F {
 fn new()->Self {F{ops:Vec::new(),fail_at:None,bools:0,kbd:VecDeque::new(),aux:VecDeque::new(),status:VecDeque::new(),ready:false,ptr:false,io:true,io_complete:true}}
 fn ok(&mut self,op:Op)->bool {self.ops.push(op);let i=self.bools;self.bools+=1;self.fail_at!=Some(i)}
 fn status(&mut self)->u8 {self.status.pop_front().unwrap_or(0)}
 fn writes(&self)->Vec<Op> {self.ops.iter().copied().filter(|o| matches!(o,W(..)|Cmd(_)|Aux(_))).collect()}
 fn count(&self,op:Op)->usize {self.ops.iter().filter(|&&o| o==op).count()}
}
impl Lifecycle for F {
 fn write_port(&mut self,p:Port,v:u8)->bool {self.ok(W(p.address(),v))}
 fn command(&mut self,v:u8)->bool {self.ok(Cmd(v))}
 fn aux_command(&mut self,v:u8)->bool {self.ok(Aux(v))}
 fn read_keyboard(&mut self)->Option<u8> {self.ops.push(RK);self.kbd.pop_front().unwrap_or(Some(0xfa))}
 fn read_aux(&mut self)->Option<u8> {self.ops.push(RA);self.aux.pop_front().unwrap_or(Some(0xfa))}
 fn read(&mut self,p:u16)->u8 {self.ops.push(R(p));if p==0x64 {self.status()} else {0x1e}}
 fn suspend(&mut self)->bool {let r=self.ok(Suspend);if r {self.ready=false;} r}
 fn resume(&mut self)->bool {let r=self.ok(Resume);if r {self.ready=true;} r}
 fn io_present(&self)->bool {self.io}
 fn ready(&self)->bool {self.ready}
 fn set_ready(&mut self,r:bool) {self.ops.push(Ready(r));self.ready=r;}
 fn pointer_ready(&self)->bool {self.ptr}
 fn set_pointer_ready(&mut self,r:bool) {self.ops.push(PtrReady(r));self.ptr=r;}
 fn clear_decoder(&mut self) {self.ops.push(ClrDec)}
 fn clear_pointer_decoder(&mut self) {self.ops.push(ClrPtr)}
 fn clear_pointer_pending(&mut self) {self.ops.push(ClrPend)}
 fn clear_pointer_lost(&mut self) {self.ops.push(ClrLost)}
 fn clear_key_head(&mut self) {self.ops.push(ClrHead)}
 fn clear_key_count(&mut self) {self.ops.push(ClrCount)}
}
impl Init for F {
 fn reset_object(&mut self) {self.ops.push(Reset);self.ready=false;self.ptr=false;}
 fn io_complete(&self)->bool {self.io_complete}
 fn read_original(&mut self,p:u16)->u8 {self.ops.push(O(p));if p==0x64 {self.status()} else {0x1e}}
}
const INIT_OK:[Op;15]=[Reset,W(0x64,0xad),W(0x64,0xa7),O(0x64),O(0x64),W(0x64,0x20),RK,W(0x64,0x60),W(0x60,0x64),W(0x64,0xae),Cmd(0xf5),Cmd(0xf0),Cmd(0x02),Cmd(0xf4),Ready(true)];
fn init_with_mode(m:u8)->F {let mut f=F::new();f.kbd.push_back(Some(m));f}
#[test] fn init_success_sequence(){let mut f=init_with_mode(0x47);assert!(init(&mut f));assert_eq!(f.ops,INIT_OK);}
#[test] fn init_rejects_incomplete_io(){let mut f=F::new();f.io_complete=false;assert!(!init(&mut f));assert_eq!(f.ops,[Reset]);}
#[test] fn init_mode_mask(){for m in 0..=255u8 {let mut f=init_with_mode(m);assert!(init(&mut f));assert_eq!(f.ops[8],W(0x60,(m|0x60)&!0x13));}}
#[test] fn init_fails_at_every_step(){for k in 0..10 {let mut f=init_with_mode(0x47);f.fail_at=Some(k);assert!(!init(&mut f));
 assert!(!f.ops.contains(&Ready(true)));assert_eq!(f.writes().len(),k+1);assert!(!f.ready);}}
#[test] fn init_mode_read_failure(){let mut f=F::new();f.kbd.push_back(None);assert!(!init(&mut f));assert_eq!(*f.ops.last().unwrap(),RK);assert_eq!(f.writes().len(),3);}
#[test] fn init_drain_edges(){for (n,want) in [(31usize,true),(32,true),(33,false)] {let mut f=init_with_mode(0x47);f.status.extend(std::iter::repeat(1u8).take(n));
 assert_eq!(init(&mut f),want,"n={n}");assert_eq!(f.count(O(0x60)),n.min(32));assert_eq!(f.count(O(0x64)),if n<32 {n+2} else {33});
 assert_eq!(f.count(R(0x64))+f.count(R(0x60)),0);}}
#[test] fn init_ready_only_at_end(){let mut f=init_with_mode(0x47);assert!(init(&mut f));assert_eq!(f.ops.iter().position(|&o| o==Ready(true)),Some(f.ops.len()-1));}
#[test] fn suspend_without_io(){let mut f=F::new();f.io=false;f.ready=true;assert!(!suspend(&mut f));assert!(f.ops.is_empty());}
#[test] fn suspend_not_ready_noop(){let mut f=F::new();assert!(suspend(&mut f));assert!(f.ops.is_empty());}
#[test] fn suspend_success(){let mut f=F::new();f.ready=true;assert!(suspend(&mut f));assert_eq!(f.ops,[Ready(false),W(0x60,0xf5),RK,R(0x64),ClrDec]);}
#[test] fn suspend_write_failure(){let mut f=F::new();f.ready=true;f.fail_at=Some(0);assert!(!suspend(&mut f));assert_eq!(f.ops,[Ready(false),W(0x60,0xf5)]);}
#[test] fn suspend_ack_search_limits(){for (n,want) in [(31usize,true),(32,false)] {let mut f=F::new();f.ready=true;f.kbd.extend(std::iter::repeat(Some(0x1e)).take(n));
 f.kbd.push_back(Some(0xfa));assert_eq!(suspend(&mut f),want,"n={n}");assert_eq!(f.count(RK),(n+1).min(32));}}
#[test] fn suspend_read_failure_stops_search(){let mut f=F::new();f.ready=true;f.kbd.push_back(Some(0x1e));f.kbd.push_back(None);assert!(!suspend(&mut f));assert_eq!(f.count(RK),2);assert!(!f.ops.contains(&ClrDec));}
#[test] fn suspend_drain_edges(){for (n,want) in [(31usize,true),(32,false),(33,false)] {let mut f=F::new();f.ready=true;f.status.extend(std::iter::repeat(1u8).take(n));
 assert_eq!(suspend(&mut f),want,"n={n}");assert_eq!(f.count(R(0x60)),n.min(32));assert_eq!(f.ops.contains(&ClrDec),want);}}
#[test] fn resume_requires_io_and_not_ready(){let mut f=F::new();f.ready=true;assert!(!resume(&mut f));assert!(f.ops.is_empty());
 let mut g=F::new();g.io=false;assert!(!resume(&mut g));assert!(g.ops.is_empty());}
#[test] fn resume_success(){let mut f=F::new();assert!(resume(&mut f));assert_eq!(f.ops,[ClrDec,Cmd(0xf4),Ready(true)]);}
#[test] fn resume_failure(){let mut f=F::new();f.fail_at=Some(0);assert!(!resume(&mut f));assert_eq!(f.ops,[ClrDec,Cmd(0xf4)]);}
fn ready_f()->F {let mut f=F::new();f.ready=true;f.aux.push_back(Some(0xfa));f.aux.push_back(Some(0xaa));f.aux.push_back(Some(0x00));f}
const START_OK:[Op;17]=[Suspend,W(0x64,0xad),W(0x64,0xa8),Aux(0xff),RA,RA,Aux(0xf6),Aux(0xe6),Aux(0xf4),W(0x64,0xae),Resume,ClrPtr,ClrPend,ClrLost,ClrCount,ClrHead,PtrReady(true)];
#[test] fn start_requires_ready_not_pointer(){let mut f=F::new();assert!(!pointer_start(&mut f));assert!(f.ops.is_empty());
 let mut g=F::new();g.ready=true;g.ptr=true;assert!(!pointer_start(&mut g));assert!(g.ops.is_empty());}
#[test] fn start_success_sequence(){let mut f=ready_f();f.aux.pop_front();assert!(pointer_start(&mut f));assert_eq!(f.ops,START_OK);}
#[test] fn start_suspend_failure_no_writes(){let mut f=ready_f();f.fail_at=Some(0);assert!(!pointer_start(&mut f));assert_eq!(f.ops,[Suspend]);}
#[test] fn start_bat_values(){for b in 0..=255u8 {let mut f=F::new();f.ready=true;f.aux.extend([Some(b),Some(0)]);assert_eq!(pointer_start(&mut f),b==0xaa,"bat={b:#x}");}}
#[test] fn start_id_values(){for i in 0..=255u8 {let mut f=F::new();f.ready=true;f.aux.extend([Some(0xaa),Some(i)]);assert_eq!(pointer_start(&mut f),i==0,"id={i:#x}");}}
#[test] fn start_fails_at_every_step_with_rollback(){for k in 1..7 {let mut f=ready_f();f.aux.pop_front();f.fail_at=Some(k);assert!(!pointer_start(&mut f));
 let w=f.writes();let ae=w.iter().position(|&o| o==W(0x64,0xae)).unwrap();
 assert_eq!(w[ae-1],W(0x64,0xa7),"k={k}");assert_eq!(*w.last().unwrap(),W(0x64,0xa7));assert_eq!(f.count(W(0x64,0xa7)),2);
 assert!(f.ops.contains(&Resume));assert!(!f.ops.contains(&PtrReady(true))&&!f.ops.contains(&ClrPtr));}}
#[test] fn start_resume_failure_rollback(){let mut f=ready_f();f.aux.pop_front();f.fail_at=Some(8);assert!(!pointer_start(&mut f));
 assert_eq!(&f.ops[9..],[W(0x64,0xae),Resume,W(0x64,0xa7)]);assert_eq!(f.count(W(0x64,0xa7)),1);}
#[test] fn start_enable_failure_skips_resume(){let mut f=ready_f();f.aux.pop_front();f.fail_at=Some(7);assert!(!pointer_start(&mut f));
 assert_eq!(&f.ops[9..],[W(0x64,0xae),W(0x64,0xa7)]);assert!(!f.ops.contains(&Resume));}
#[test] fn start_state_reset_order(){let mut f=ready_f();f.aux.pop_front();assert!(pointer_start(&mut f));assert_eq!(&f.ops[11..],[ClrPtr,ClrPend,ClrLost,ClrCount,ClrHead,PtrReady(true)]);}
#[test] fn stop_noop_when_not_pointer(){let mut f=F::new();pointer_stop(&mut f);assert!(f.ops.is_empty());}
#[test] fn stop_best_effort_all_steps(){for k in 0..5 {let mut f=F::new();f.ptr=true;f.fail_at=Some(k);pointer_stop(&mut f);
 assert_eq!(f.ops,[W(0x64,0xad),Aux(0xf5),W(0x64,0xa7),W(0x64,0xae),ClrPend,PtrReady(false),ClrPtr,ClrCount,ClrHead,ClrLost],"k={k}");}}
#[test] fn drain_limit_value(){assert_eq!(DRAIN_LIMIT,32);}
