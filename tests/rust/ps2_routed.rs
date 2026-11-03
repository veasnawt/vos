extern crate ps2_routed_core;
use ps2_routed_core::{Queue,Route,drain};
struct F {q:Queue,pending:i32,lost:u32,script:[(u8,u8);64],at:usize,reads:usize,kreset:u32,preset:u32,kcalls:u32,pcalls:u32,mutate:bool}
impl F {fn new()->Self {Self{q:Queue{head:0,count:0,keys:[0;32]},pending:0,lost:0,script:[(0,0);64],at:0,reads:0,kreset:0,preset:0,kcalls:0,pcalls:0,mutate:false}}}
impl Route for F {
 fn pointer_pending(&self)->i32{self.pending} fn key_count(&self)->u32{self.q.count}
 fn read(&mut self,p:u16)->u8 {self.reads+=1;let (s,b)=self.script[self.at];if p==0x64 {if self.mutate {self.q.head=31;} s} else {assert_eq!(p,0x60);self.at+=1;b}}
 fn reset_pointer(&mut self){self.preset+=1;} fn decode_pointer(&mut self,b:u8)->i32 {self.pcalls+=1;match b {1=>1,2=>-1,_=>0}}
 fn lost(&mut self)->&mut u32{&mut self.lost} fn set_pointer_pending(&mut self){self.pending=1;}
 fn reset_keyboard(&mut self){self.kreset+=1;} fn decode_keyboard(&mut self,b:u8)->u8{self.kcalls+=1;if b==3 {0}else{b}}
 fn queue(&mut self)->&mut Queue{&mut self.q}
}
#[test] fn empty(){let mut f=F::new();drain(&mut f);assert_eq!((f.reads,f.at,f.q.count),(1,0,0));}
#[test] fn pending_blocks(){let mut f=F::new();f.pending=-1;drain(&mut f);assert_eq!(f.reads,0);}
#[test] fn full_blocks(){let mut f=F::new();f.q.count=32;drain(&mut f);assert_eq!(f.reads,0);}
#[test] fn bound_and_enqueue(){let mut f=F::new();f.q.head=31;f.script=[(1,b'a');64];drain(&mut f);assert_eq!((f.at,f.reads,f.q.count),(32,64,32));assert_eq!(f.q.keys,[b'a';32]);}
#[test] fn pointer_complete(){let mut f=F::new();f.script[0]=(0x21,1);drain(&mut f);assert_eq!((f.pending,f.at,f.reads,f.q.count),(1,1,2,0));}
#[test] fn pointer_partial(){let mut f=F::new();f.script[0]=(0x21,4);drain(&mut f);assert_eq!((f.pending,f.pcalls,f.reads),(0,1,3));}
#[test] fn pointer_reject(){let mut f=F::new();f.script[0]=(0x21,2);drain(&mut f);assert_eq!((f.lost,f.preset,f.kreset),(1,0,0));}
#[test] fn aux_error_precedence(){let mut f=F::new();f.script[0]=(0x61,0);drain(&mut f);assert_eq!((f.lost,f.preset,f.kreset,f.kcalls,f.pcalls),(1,1,0,0,0));}
#[test] fn keyboard_error(){let mut f=F::new();f.q.count=1;f.q.keys[0]=b'x';f.script[0]=(0x41,b'a');drain(&mut f);assert_eq!((f.kreset,f.preset,f.kcalls,f.q.count,f.q.keys[0]),(1,0,0,1,b'x'));}
#[test] fn zero_resets(){let mut f=F::new();f.script[0]=(1,0);drain(&mut f);assert_eq!((f.kreset,f.kcalls),(1,0));}
#[test] fn ff_resets(){let mut f=F::new();f.script[0]=(1,255);drain(&mut f);assert_eq!((f.kreset,f.kcalls),(1,0));}
#[test] fn saturation(){let mut f=F::new();f.lost=u32::MAX-1;f.script[0]=(0x21,2);f.script[1]=(0x61,2);drain(&mut f);assert_eq!(f.lost,u32::MAX);}
#[test] fn wrapping_queue(){let mut f=F::new();f.q.head=u32::MAX;f.q.count=1;f.script[0]=(1,b'b');drain(&mut f);assert_eq!((f.q.keys[0],f.q.count),(b'b',2));}
#[test] fn count_wrap(){let mut f=F::new();f.q.count=u32::MAX;f.script[0]=(1,b'b');drain(&mut f);assert_eq!((f.q.keys[31],f.q.count),(b'b',0));}
#[test] fn zero_character(){let mut f=F::new();f.script[0]=(1,3);drain(&mut f);assert_eq!((f.q.count,f.kcalls),(0,1));}
#[test] fn callback_change(){let mut f=F::new();f.mutate=true;f.script[0]=(1,b'z');drain(&mut f);assert_eq!((f.q.keys[31],f.q.count),(b'z',1));}

use ps2_routed_core::{Packet, PointerQueue, pointer_poll, pointer_lost_and_clear};

impl PointerQueue for F {
    fn is_ready(&self) -> bool { self.kreset != 999 }
    fn is_pointer_ready(&self) -> bool { self.preset != 999 }
    fn drain(&mut self) { drain(self); }
    fn poll_idle(&mut self) -> bool { self.preset == 100 }
    fn take_packet(&mut self) -> Option<Packet> {
        if self.pending != 0 {
            self.pending = 0;
            Some(Packet { dx: 12, dy: -34, buttons: 1 })
        } else {
            None
        }
    }
}

#[test] fn pointer_poll_not_ready() {
    let mut f = F::new();
    f.kreset = 999;
    f.pending = 1;
    assert_eq!(pointer_poll(&mut f), None);
    assert_eq!(f.pending, 1);
}

#[test] fn pointer_poll_pointer_not_ready() {
    let mut f = F::new();
    f.preset = 999;
    f.pending = 1;
    assert_eq!(pointer_poll(&mut f), None);
    assert_eq!(f.pending, 1);
}

#[test] fn pointer_poll_packet_delivery() {
    let mut f = F::new();
    f.pending = 1;
    assert_eq!(pointer_poll(&mut f), Some(Packet { dx: 12, dy: -34, buttons: 1 }));
    assert_eq!(f.pending, 0);
}

#[test] fn pointer_poll_idle_loss() {
    let mut f = F::new();
    f.preset = 100; // causes poll_idle to return true
    assert_eq!(pointer_poll(&mut f), None);
    assert_eq!(f.lost, 1);
}

#[test] fn pointer_poll_idle_saturated() {
    let mut f = F::new();
    f.preset = 100;
    f.lost = u32::MAX;
    assert_eq!(pointer_poll(&mut f), None);
    assert_eq!(f.lost, u32::MAX);
}

#[test] fn pointer_lost_and_clear_test() {
    let mut lost = 42u32;
    assert_eq!(pointer_lost_and_clear(&mut lost), 42);
    assert_eq!(lost, 0);
}

impl ps2_routed_core::KeyboardQueue for F {
 fn keyboard_ready(&self)->bool{self.kreset!=999}
 fn fill_keyboard_queue(&mut self){drain(self)}
 fn keyboard_queue(&mut self)->&mut Queue{&mut self.q}
}
#[test] fn queue_unready(){let mut f=F::new();f.kreset=999;f.script[0]=(1,b'a');assert_eq!(ps2_routed_core::keyboard_poll(&mut f),0);assert_eq!(f.reads,0);}
#[test] fn queue_empty(){let mut f=F::new();assert_eq!(ps2_routed_core::keyboard_poll(&mut f),0);assert_eq!((f.reads,f.q.count),(1,0));}
#[test] fn queue_fifo(){let mut f=F::new();f.pending=1;f.q.head=31;f.q.count=2;f.q.keys[31]=b'a';f.q.keys[0]=b'b';assert_eq!(ps2_routed_core::keyboard_poll(&mut f),b'a');assert_eq!((f.q.head,f.q.count,f.q.keys[31],f.pending),(0,1,0,1));assert_eq!(ps2_routed_core::keyboard_poll(&mut f),b'b');assert_eq!((f.q.head,f.q.count),(1,0));}
#[test] fn queue_nul(){let mut f=F::new();f.pending=1;f.q.head=2;f.q.count=1;assert_eq!(ps2_routed_core::keyboard_poll(&mut f),0);assert_eq!((f.q.head,f.q.count),(3,0));}
#[test] fn queue_callback(){let mut f=F::new();f.q.count=1;f.q.keys[0]=b'a';f.q.keys[31]=b'b';f.mutate=true;assert_eq!(ps2_routed_core::keyboard_poll(&mut f),b'b');assert_eq!((f.q.head,f.q.count,f.q.keys[0]),(0,0,b'a'));}
#[test] fn queue_full(){let mut f=F::new();f.q.count=32;f.q.head=5;f.q.keys[5]=b'x';f.script=[(1,b'a');64];assert_eq!(ps2_routed_core::keyboard_poll(&mut f),b'x');assert_eq!((f.reads,f.q.count,f.q.head),(0,31,6));}
#[test] fn queue_all_bytes(){for b in 0..=255u8{let mut f=F::new();f.pending=1;f.q.count=1;f.q.keys[0]=b;assert_eq!(ps2_routed_core::keyboard_poll(&mut f),b);assert_eq!((f.q.keys[0],f.q.head,f.q.count),(0,1,0));}}
#[test] fn queue_noncanonical_count(){let mut f=F::new();f.pending=1;f.q.count=u32::MAX;f.q.keys[0]=b'b';assert_eq!(ps2_routed_core::keyboard_poll(&mut f),b'b');assert_eq!(f.q.count,u32::MAX-1);}
#[test] fn queue_invalid_head(){let mut f=F::new();f.pending=1;f.q.head=32;f.q.count=1;assert_eq!(ps2_routed_core::keyboard_poll(&mut f),0);assert_eq!((f.q.head,f.q.count),(32,1));}

#[test] fn queue_guard_equivalence(){
 let check=|p:usize|{let old=p!=0 && p%8==0 && p.checked_add(88).is_some();assert_eq!(ps2_routed_core::keyboard_range_valid(p),old,"{p:x}");};
 for p in 0..256 {check(p);check(usize::MAX-p);}
 let mut x=0x92837463usize;for _ in 0..100000 {x=x.wrapping_mul(6364136223846793005).wrapping_add(1);check(x);}
 for p in [4096usize,0xffff800000000000,0xffffffff80000000] {check(p);}
}
