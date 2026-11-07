extern crate ps2_controller_read_core;
extern crate ps2_controller_write_core;
use ps2_controller_read_core::ControllerRead;
use ps2_controller_write_core::{aux_command, command, write_port, ControllerWrite, Port, WAIT_LIMIT};
#[derive(Clone,Copy,PartialEq,Eq,Debug)] enum Op {R(u16),W(u16,u8)}
struct F {script:Vec<u8>,at:usize,fill_status:u8,fill_data:u8,ops:Vec<Op>,write_ok:bool}
impl F {fn new(script:&[u8],fill_status:u8,fill_data:u8)->Self {Self{script:script.to_vec(),at:0,fill_status,fill_data,ops:Vec::new(),write_ok:true}}
 fn writes(&self)->Vec<Op> {self.ops.iter().copied().filter(|o| matches!(o,Op::W(..))).collect()}
 fn reads(&self,p:u16)->usize {self.ops.iter().filter(|&&o| o==Op::R(p)).count()}}
impl ControllerRead for F {
 fn read(&mut self,p:u16)->u8 {
  assert!(p==0x60||p==0x64);self.ops.push(Op::R(p));
  if self.at<self.script.len() {self.at+=1;self.script[self.at-1]} else if p==0x64 {self.fill_status} else {self.fill_data}
 }
}
impl ControllerWrite for F {fn write(&mut self,p:u16,v:u8)->bool {if self.write_ok {self.ops.push(Op::W(p,v));} self.write_ok}}
fn busy(n:usize,tail:&[u8])->Vec<u8> {let mut s=vec![0x02u8;n];s.extend_from_slice(tail);s}
#[test] fn write_immediate(){let mut f=F::new(&[0x00],0x00,0);assert!(write_port(&mut f,Port::Data,0x42));assert_eq!(f.ops,[Op::R(0x64),Op::W(0x60,0x42)]);}
#[test] fn write_status_port(){let mut f=F::new(&[0x00],0x00,0);assert!(write_port(&mut f,Port::Status,0xae));assert_eq!(f.ops,[Op::R(0x64),Op::W(0x64,0xae)]);}
#[test] fn write_waits_while_busy(){let mut f=F::new(&[0x02,0x02,0x00],0x00,0);assert!(write_port(&mut f,Port::Data,7));assert_eq!(f.ops,[Op::R(0x64),Op::R(0x64),Op::R(0x64),Op::W(0x60,7)]);}
#[test] fn write_ignores_other_status_bits(){let mut f=F::new(&[0xfd],0x02,0);assert!(write_port(&mut f,Port::Data,9));assert_eq!(f.ops,[Op::R(0x64),Op::W(0x60,9)]);}
#[test] fn timeout_no_write(){let mut f=F::new(&[],0x02,0);assert!(!write_port(&mut f,Port::Data,1));assert_eq!((f.reads(0x64),f.ops.len()),(100_000,100_000));}
#[test] fn last_iteration_writes(){let mut f=F::new(&busy(99_999,&[0x00]),0x02,0);assert!(write_port(&mut f,Port::Data,3));assert_eq!(f.reads(0x64),100_000);assert_eq!(f.writes(),[Op::W(0x60,3)]);}
#[test] fn one_past_limit_fails(){let mut f=F::new(&busy(100_000,&[0x00]),0x00,0);assert!(!write_port(&mut f,Port::Data,3));assert_eq!(f.reads(0x64),100_000);assert!(f.writes().is_empty());}
#[test] fn write_value_exact(){for v in 0..=255u8 {for (p,a) in [(Port::Data,0x60),(Port::Status,0x64)] {let mut f=F::new(&[0x00],0x00,0);assert!(write_port(&mut f,p,v));assert_eq!(f.writes(),[Op::W(a,v)]);}}}
#[test] fn write_failure_reported(){let mut f=F::new(&[0x00],0x00,0);f.write_ok=false;assert!(!write_port(&mut f,Port::Data,1));assert_eq!(f.ops,[Op::R(0x64)]);}
#[test] fn port_mapping(){assert_eq!(Port::from_address(0x60),Some(Port::Data));assert_eq!(Port::from_address(0x64),Some(Port::Status));
 for a in [0u16,0x5f,0x61,0x62,0x63,0x65,0x160,0x164,0xffff] {assert_eq!(Port::from_address(a),None);}
 assert_eq!((Port::Data.address(),Port::Status.address()),(0x60,0x64));}
#[test] fn command_success(){let mut f=F::new(&[0x00,0x01,0xfa],0x00,0);assert!(command(&mut f,0xf4));assert_eq!(f.ops,[Op::R(0x64),Op::W(0x60,0xf4),Op::R(0x64),Op::R(0x60)]);}
#[test] fn command_write_failure_skips_read(){let mut f=F::new(&[],0x02,0xfa);assert!(!command(&mut f,0xf4));assert_eq!((f.ops.len(),f.reads(0x60)),(100_000,0));assert!(f.writes().is_empty());}
#[test] fn command_nak(){let mut f=F::new(&[0x00,0x01,0xfe],0x00,0);assert!(!command(&mut f,0xf4));assert_eq!(f.ops.len(),4);}
#[test] fn command_err(){let mut f=F::new(&[0x00,0x41,0xfa],0x00,0);assert!(!command(&mut f,0xf4));assert_eq!(f.ops.len(),4);}
#[test] fn command_consumes_aux(){let mut f=F::new(&[0x00,0x21,0xfa,0x01,0xfa],0x00,0);assert!(command(&mut f,0xf4));assert_eq!(f.reads(0x60),2);}
#[test] fn command_ack_timeout(){let mut f=F::new(&[0x00],0x00,0);assert!(!command(&mut f,0xf4));assert_eq!(f.reads(0x64),100_001);assert_eq!(f.writes(),[Op::W(0x60,0xf4)]);}
#[test] fn aux_success(){let mut f=F::new(&[0x00,0x00,0x21,0xfa],0x00,0);assert!(aux_command(&mut f,0xf6));
 assert_eq!(f.ops,[Op::R(0x64),Op::W(0x64,0xd4),Op::R(0x64),Op::W(0x60,0xf6),Op::R(0x64),Op::R(0x60)]);}
#[test] fn aux_prefix_failure_stops(){let mut f=F::new(&[],0x02,0xfa);assert!(!aux_command(&mut f,0xf6));assert_eq!(f.ops.len(),100_000);assert!(f.writes().is_empty());}
#[test] fn aux_data_failure_stops(){let mut f=F::new(&[0x00],0x02,0xfa);assert!(!aux_command(&mut f,0xf6));assert_eq!(f.ops.len(),100_002);assert_eq!(f.writes(),[Op::W(0x64,0xd4)]);}
#[test] fn aux_nak(){let mut f=F::new(&[0x00,0x00,0x21,0xfe],0x00,0);assert!(!aux_command(&mut f,0xf6));assert_eq!(f.ops.len(),6);}
#[test] fn aux_err(){let mut f=F::new(&[0x00,0x00,0x61,0xfa],0x00,0);assert!(!aux_command(&mut f,0xf6));assert_eq!(f.ops.len(),6);}
#[test] fn aux_consumes_keyboard(){let mut f=F::new(&[0x00,0x00,0x01,0xfa,0x21,0xfa],0x00,0);assert!(aux_command(&mut f,0xf6));assert_eq!(f.reads(0x60),2);}
#[test] fn aux_ack_timeout(){let mut f=F::new(&[0x00,0x00],0x00,0);assert!(!aux_command(&mut f,0xf6));assert_eq!(f.reads(0x64),100_002);assert_eq!(f.writes().len(),2);}
#[test] fn wait_limit_value(){assert_eq!(WAIT_LIMIT,100_000);}
