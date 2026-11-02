use console_accept_core::{accept,Editor};
#[derive(Clone)] struct M {n:u32,ready:bool,overflow:bool,bytes:[u8;129],trace:Vec<(u8,u32,bool)>,after_erase:Option<u32>}
impl Default for M {fn default()->Self{Self{n:0,ready:false,overflow:false,bytes:[0;129],trace:vec![],after_erase:None}}}
impl Editor for M {
 fn ready(&self)->bool{self.ready} fn length(&self)->u32{self.n}
 fn set_length(&mut self,n:u32){self.n=n;} fn write(&mut self,i:u32,b:u8){self.bytes[i as usize]=b;}
 fn seal(&mut self){self.ready=true;} fn overflow(&mut self){self.overflow=true;}
 fn newline(&mut self){self.trace.push((10,self.n,self.ready));}
 fn erase(&mut self){self.trace.push((8,self.n,self.ready));if let Some(n)=self.after_erase{self.n=n;}}
 fn echo(&mut self,b:u8){assert_eq!(self.bytes[self.n as usize],0);self.trace.push((b,self.n,self.ready));}
}
#[test] fn ready_ignores(){let mut s=M{ready:true,..M::default()};for b in 0..=255{accept(&mut s,b);}assert!(s.trace.is_empty());assert_eq!(s.n,0);}
#[test] fn newline_before_seal(){let mut s=M::default();accept(&mut s,10);assert_eq!(s.trace,[(10,0,false)]);assert!(s.ready);}
#[test] fn erase_empty(){let mut s=M::default();accept(&mut s,8);assert!(s.trace.is_empty());assert_eq!(s.n,0);}
#[test] fn erase_order(){let mut s=M::default();accept(&mut s,b'x');s.trace.clear();accept(&mut s,8);assert_eq!(s.trace,[(8,1,false)]);assert_eq!(s.n,0);assert_eq!(s.bytes[0],0);}
#[test] fn erase_reload(){let mut s=M{n:8,after_erase:Some(3),..M::default()};accept(&mut s,8);assert_eq!(s.n,2);}
#[test] fn printable_before_echo(){let mut s=M::default();accept(&mut s,b'x');assert_eq!(s.trace,[(b'x',1,false)]);assert_eq!(&s.bytes[..2],b"x\0");}
#[test] fn maximum_terminator(){let mut s=M::default();for _ in 0..128{accept(&mut s,b'y');}assert_eq!(s.n,128);assert_eq!(s.bytes[128],0);assert!(!s.overflow);}
#[test] fn sticky_overflow(){let mut s=M{n:128,..M::default()};accept(&mut s,b'z');accept(&mut s,8);assert!(s.overflow);assert_eq!(s.n,127);}
#[test] fn overflow_no_echo(){let mut s=M{n:128,..M::default()};accept(&mut s,b'z');assert!(s.trace.is_empty());assert!(s.overflow);}
#[test] fn filtering(){let mut s=M::default();for b in 0..=255{if !(32..=126).contains(&b)&&b!=8&&b!=10{accept(&mut s,b);}}assert_eq!(s.n,0);assert!(s.trace.is_empty());}
#[test] fn invalid_length(){let mut s=M{n:u32::MAX,..M::default()};accept(&mut s,b'x');assert!(s.trace.is_empty());assert_eq!(s.n,u32::MAX);accept(&mut s,8);assert_eq!(s.n,u32::MAX);}
#[test] fn invalid_after_callback(){let mut s=M{n:1,after_erase:Some(0),..M::default()};accept(&mut s,8);assert_eq!(s.n,0);assert_eq!(s.trace.len(),1);}
