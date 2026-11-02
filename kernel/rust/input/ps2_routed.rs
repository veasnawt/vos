//! Bounded shared PS/2 routing policy. No hardware, allocation or unsafe code.
#![forbid(unsafe_code)]
pub const LIMIT:u32=32;
#[repr(C)]
pub struct Queue {pub head:u32,pub count:u32,pub keys:[u8;32]}
/// Synchronous adapter. Each returned borrow ends before the next foreign
/// callback. Only read() may call the controller backend; decode methods reuse
/// accepted pure decoders. No ownership, initialization or port-write authority.
pub trait Route {
 fn pointer_pending(&self)->i32;
 fn key_count(&self)->u32;
 fn read(&mut self,port:u16)->u8;
 fn reset_pointer(&mut self);
 fn decode_pointer(&mut self,byte:u8)->i32;
 fn lost(&mut self)->&mut u32;
 fn set_pointer_pending(&mut self);
 fn reset_keyboard(&mut self);
 fn decode_keyboard(&mut self,byte:u8)->u8;
 fn queue(&mut self)->&mut Queue;
}
#[inline(always)]
pub fn drain<S:Route>(s:&mut S) {
 for _ in 0..LIMIT {
  if s.pointer_pending()!=0 || s.key_count()==LIMIT {break;}
  let status=s.read(0x64);
  if status&1==0 {break;}
  let byte=s.read(0x60);
  if status&0x20!=0 {
   if status&0xc0!=0 {
    s.reset_pointer();
    let lost=s.lost();*lost=lost.wrapping_add(u32::from(*lost!=u32::MAX));
    continue;
   }
   let n=s.decode_pointer(byte);
   if n<0 {let lost=s.lost();*lost=lost.wrapping_add(u32::from(*lost!=u32::MAX));}
   if n>0 {s.set_pointer_pending();}
  } else if status&0xc0!=0 || byte==0 || byte==0xff {
   s.reset_keyboard();
  } else {
   let byte=s.decode_keyboard(byte);
   if byte!=0 {
    let q=s.queue();
    q.keys[(q.head.wrapping_add(q.count)%LIMIT) as usize]=byte;
    q.count=q.count.wrapping_add(1);
   }
  }
 }
}
