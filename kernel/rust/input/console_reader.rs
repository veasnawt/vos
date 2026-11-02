//! Synchronous console-reader coordination; no hardware or ownership authority.
#![forbid(unsafe_code)]
pub const UNAVAILABLE:i32=-4;
pub const NOSPACE:i32=-2;
pub const INTERRUPTED:i32=-5;
pub const TOOLONG:i32=-3;
/// Operations are synchronous. Adapters must not retain editor/output/keyboard
/// references across foreign callbacks or a scheduler wait. initialize precedes
/// every editor access. Production monomorphizes this trait without allocation.
pub trait Reader {
 fn keyboard_ready(&self)->bool;
 fn output_present(&self)->bool;
 fn capacity(&self)->u32;
 fn initialize(&mut self);
 fn prompt(&mut self);
 fn cancelled(&mut self)->bool;
 fn poll_accept(&mut self);
 fn line_ready(&self)->bool;
 fn take_line(&mut self)->i32;
 fn overlong(&mut self);
 fn sleep(&mut self);
}
#[inline(always)]
pub fn admit<S:Reader>(s:&S)->Option<i32> {
 if !s.keyboard_ready() {return Some(UNAVAILABLE);}
 if !s.output_present() || s.capacity()<129 {return Some(NOSPACE);}
 None
}
#[inline(always)]
#[allow(dead_code)]
pub fn read_line<S:Reader>(s:&mut S)->i32 {
 if let Some(error)=admit(s) {return error;}
 run_admitted(s)
}
/// Admission precedes this initialized synchronous flow in the raw adapter.
#[inline(always)]
pub fn run_admitted<S:Reader>(s:&mut S)->i32 {
 s.initialize();
 s.prompt();
 loop {
  if s.cancelled() {return INTERRUPTED;}
  s.poll_accept();
  if s.line_ready() {
   let result=s.take_line();
   if result!=TOOLONG {return result;}
   s.overlong();
   s.prompt();
  }
  s.sleep();
 }
}
