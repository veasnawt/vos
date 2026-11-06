extern crate ps2_ordinary_core;
use ps2_ordinary_core::{Ordinary, poll, LIMIT};
struct F {ready:bool,script:[(u8,u8);96],at:usize,ports:Vec<u16>,resets:u32,decodes:Vec<u8>,clear_ready:bool}
impl F {fn new()->Self {Self{ready:true,script:[(0,0);96],at:0,ports:Vec::new(),resets:0,decodes:Vec::new(),clear_ready:false}}}
impl Ordinary for F {
 fn ready(&self)->bool{self.ready}
 fn read(&mut self,p:u16)->u8 {
  self.ports.push(p);let (s,b)=self.script[self.at];
  if p==0x64 {if self.clear_ready {self.ready=false;} s} else {assert_eq!(p,0x60);self.at+=1;b}
 }
 fn reset_decoder(&mut self){self.resets+=1;}
 // Test decoder: identity, except 3 decodes to "no character" (like releases).
 fn decode(&mut self,b:u8)->u8{self.decodes.push(b);if b==3 {0}else{b}}
}
#[test] fn unready_no_reads(){let mut f=F::new();f.ready=false;f.script[0]=(1,b'a');assert_eq!(poll(&mut f),0);assert!(f.ports.is_empty());}
#[test] fn empty_one_status_read(){let mut f=F::new();assert_eq!(poll(&mut f),0);assert_eq!(f.ports,[0x64]);}
#[test] fn status_before_data(){let mut f=F::new();f.script[0]=(1,b'a');assert_eq!(poll(&mut f),b'a');assert_eq!(f.ports,[0x64,0x60]);}
#[test] fn no_data_when_obf_clear(){let mut f=F::new();f.script[0]=(0xfe,b'a');assert_eq!(poll(&mut f),0);assert_eq!(f.ports,[0x64]);}
#[test] fn aux_consumed_undecoded(){let mut f=F::new();f.script[0]=(0x21,b'x');f.script[1]=(1,b'a');assert_eq!(poll(&mut f),b'a');assert_eq!(f.decodes,[b'a']);assert_eq!(f.resets,0);}
#[test] fn aux_precedes_error(){let mut f=F::new();f.script[0]=(0xe1,0);assert_eq!(poll(&mut f),0);assert_eq!((f.resets,f.decodes.len()),(0,0));}
#[test] fn error_resets(){for s in [0x41u8,0x81,0xc1]{let mut f=F::new();f.script[0]=(s,b'a');f.script[1]=(1,b'b');assert_eq!(poll(&mut f),b'b');assert_eq!(f.resets,1);assert_eq!(f.decodes,[b'b']);}}
#[test] fn zero_resets(){let mut f=F::new();f.script[0]=(1,0);assert_eq!(poll(&mut f),0);assert_eq!((f.resets,f.decodes.len()),(1,0));assert_eq!(f.ports,[0x64,0x60,0x64]);}
#[test] fn ff_resets(){let mut f=F::new();f.script[0]=(1,0xff);assert_eq!(poll(&mut f),0);assert_eq!((f.resets,f.decodes.len()),(1,0));}
#[test] fn no_character_continues(){let mut f=F::new();f.script[0]=(1,3);f.script[1]=(1,b'c');assert_eq!(poll(&mut f),b'c');assert_eq!(f.decodes,[3,b'c']);}
#[test] fn returns_first_character(){let mut f=F::new();f.script[0]=(1,b'x');f.script[1]=(1,b'y');assert_eq!(poll(&mut f),b'x');assert_eq!((f.at,f.ports.len()),(1,2));}
#[test] fn exact_bound(){let mut f=F::new();f.script=[(1,3);96];f.script[32]=(1,b'z');assert_eq!(poll(&mut f),0);assert_eq!((f.at,f.ports.len(),f.decodes.len()),(32,64,32));assert_eq!(LIMIT,32);}
#[test] fn bound_with_aux_and_resets(){let mut f=F::new();for i in 0..96{f.script[i]=if i%2==0 {(0x21,1)} else {(0x41,1)};}assert_eq!(poll(&mut f),0);assert_eq!((f.ports.len(),f.resets),(64,16));}
#[test] fn ready_read_once(){let mut f=F::new();f.clear_ready=true;f.script[0]=(1,3);f.script[1]=(1,b'q');assert_eq!(poll(&mut f),b'q');}
#[test] fn all_bytes(){for b in 0..=255u8{let mut f=F::new();f.script[0]=(1,b);let want=if b==0||b==0xff||b==3 {0} else {b};assert_eq!(poll(&mut f),want);}}
#[test] fn high_bytes_returned_unchanged(){let mut f=F::new();f.script[0]=(1,0x80);assert_eq!(poll(&mut f),0x80);let mut f=F::new();f.script[0]=(1,0xfe);assert_eq!(poll(&mut f),0xfe);}
#[test] fn ordering_trace(){let mut f=F::new();f.script[0]=(0x21,9);f.script[1]=(0x41,9);f.script[2]=(1,0);f.script[3]=(1,3);f.script[4]=(1,b'k');assert_eq!(poll(&mut f),b'k');assert_eq!(f.ports,[0x64,0x60,0x64,0x60,0x64,0x60,0x64,0x60,0x64,0x60]);assert_eq!((f.resets,f.decodes.len()),(2,2));}
