extern crate ps2_controller_read_core;
use ps2_controller_read_core::{Channel, ControllerRead, WAIT_LIMIT, read_channel};
struct F {script:Vec<(u8,u8)>,fill:(u8,u8),at:usize,ports:Vec<u16>,statuses:u64}
impl F {fn new(script:&[(u8,u8)],fill:(u8,u8))->Self {Self{script:script.to_vec(),fill,at:0,ports:Vec::new(),statuses:0}}}
impl ControllerRead for F {
 fn read(&mut self,p:u16)->u8 {
  self.ports.push(p);let c=if self.at<self.script.len() {self.script[self.at]} else {self.fill};
  if p==0x64 {self.statuses+=1;if c.0&1==0 {self.at+=1;} c.0} else {assert_eq!(p,0x60);self.at+=1;c.1}
 }
}
#[test] fn immediate_keyboard(){let mut f=F::new(&[(1,0x42)],(0,0));assert_eq!(read_channel(&mut f,Channel::Keyboard),Some(0x42));assert_eq!(f.ports,[0x64,0x60]);}
#[test] fn immediate_aux(){let mut f=F::new(&[(0x21,0x42)],(0,0));assert_eq!(read_channel(&mut f,Channel::Aux),Some(0x42));assert_eq!(f.ports,[0x64,0x60]);}
#[test] fn skips_obf_clear(){let mut f=F::new(&[(0,0),(0,0),(1,7)],(0,0));assert_eq!(read_channel(&mut f,Channel::Keyboard),Some(7));assert_eq!(f.ports,[0x64,0x64,0x64,0x60]);}
#[test] fn consumes_keyboard_byte_waiting_for_aux(){let mut f=F::new(&[(1,9),(0x21,5)],(0,0));assert_eq!(read_channel(&mut f,Channel::Aux),Some(5));assert_eq!(f.ports.len(),4);}
#[test] fn consumes_aux_byte_waiting_for_keyboard(){let mut f=F::new(&[(0x21,9),(1,5)],(0,0));assert_eq!(read_channel(&mut f,Channel::Keyboard),Some(5));assert_eq!(f.ports.len(),4);}
#[test] fn err_precedence_own_channel(){let mut f=F::new(&[(0x41,5),(1,6)],(0,0));assert_eq!(read_channel(&mut f,Channel::Keyboard),None);assert_eq!(f.ports,[0x64,0x60]);}
#[test] fn err_precedence_other_channel(){let mut f=F::new(&[(0x61,5),(1,6)],(0,0));assert_eq!(read_channel(&mut f,Channel::Keyboard),None);assert_eq!(f.ports,[0x64,0x60]);}
#[test] fn err_without_obf_is_skipped(){let mut f=F::new(&[(0x40,0),(1,3)],(0,0));assert_eq!(read_channel(&mut f,Channel::Keyboard),Some(3));}
#[test] fn timeout_exact(){let mut f=F::new(&[],(0,0));assert_eq!(read_channel(&mut f,Channel::Keyboard),None);assert_eq!(f.statuses,100_000);assert!(f.ports.iter().all(|&p| p==0x64));}
#[test] fn timeout_other_channel_flood(){let mut f=F::new(&[],(0x21,1));assert_eq!(read_channel(&mut f,Channel::Keyboard),None);assert_eq!((f.statuses,f.ports.len()),(100_000,200_000));}
#[test] fn last_iteration_succeeds(){let mut s=vec![(0u8,0u8);99_999];s.push((1,8));let mut f=F::new(&s,(0,0));assert_eq!(read_channel(&mut f,Channel::Keyboard),Some(8));}
#[test] fn one_past_limit_fails(){let mut s=vec![(0u8,0u8);100_000];s.push((1,8));let mut f=F::new(&s,(0,0));assert_eq!(read_channel(&mut f,Channel::Keyboard),None);assert_eq!(f.statuses,100_000);}
#[test] fn all_bytes(){for b in 0..=255u8 {let mut f=F::new(&[(1,b)],(0,0));assert_eq!(read_channel(&mut f,Channel::Keyboard),Some(b));let mut f=F::new(&[(0x21,b)],(0,0));assert_eq!(read_channel(&mut f,Channel::Aux),Some(b));}}
#[test] fn wait_limit_value(){assert_eq!(WAIT_LIMIT,100_000);}
