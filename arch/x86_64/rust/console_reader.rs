//! Narrow synchronous kernel C ABI. Foreign state remains opaque raw storage.
#[path="../../../kernel/rust/input/console_reader.rs"]
pub mod core_reader;
use core_reader::Reader;
use core::{ffi::{c_char,c_void},mem::MaybeUninit};
#[repr(C,align(4))]
struct Editor([u8;144]);
const _:()={assert!(core::mem::size_of::<Editor>()==144);assert!(core::mem::align_of::<Editor>()==4);};
unsafe extern "C" {
 fn keyboard_poll(keyboard:*mut u8)->c_char;
 fn input_console_accept(line:*mut u8,byte:c_char);
 fn input_console_take_line(line:*mut u8,out:*mut u8,capacity:u32)->i32;
 fn serial_puts(text:*const u8);
 fn thread_sleep(ticks:u64);
}
type Cancel=Option<unsafe extern "C" fn(*mut c_void)->i32>;
struct Raw {keyboard:*mut u8,out:*mut u8,capacity:u32,prompt:*const u8,cancel:Cancel,ctx:*mut c_void,line:*mut Editor}
impl Reader for Raw {
 #[inline(always)] fn keyboard_ready(&self)->bool {
  // SAFETY: non-null/non-wrapping prefix is checked first. Caller provides the
  // complete live C keyboard (ready offset16, verified against keyboard.h).
  if (self.keyboard as usize).wrapping_sub(1)>=usize::MAX-20 {return false;}
  unsafe {self.keyboard.add(16).cast::<i32>().read_unaligned()!=0}
 }
 #[inline(always)] fn output_present(&self)->bool {!self.out.is_null()}
 #[inline(always)] fn capacity(&self)->u32 {self.capacity}
 #[inline(always)] fn initialize(&mut self) {
  // SAFETY: private aligned 144-byte stack allocation; full padding included.
  // No Editor reference is formed and no uninitialized byte is exposed.
  // Same bounded memory primitive as accepted extraction and C zeroing:
  // x86-64 unaligned stores allowed, SysV DF=0, exactly18 qwords, default
  // compiler memory clobber; no stack/flags change or device operation.
  unsafe {core::arch::asm!("rep stosq",inout("rdi") self.line=>_,
      inout("rcx") 18usize=>_,in("rax") 0usize,options(nostack,preserves_flags));}
 }
 #[inline(always)] fn prompt(&mut self) {
  // SAFETY: caller's trusted NUL-terminated prompt remains live across waits.
  unsafe {serial_puts(self.prompt)}
 }
 #[inline(always)] fn cancelled(&mut self)->bool {
  // SAFETY: optional C callback/context contract; no Rust pointee references.
  self.cancel.is_some_and(|f|unsafe {f(self.ctx)!=0})
 }
 #[inline(always)] fn poll_accept(&mut self) {
  // SAFETY: caller-owned live keyboard and initialized private editor. Existing
  // synchronous poll/edit ABIs; references do not cross either callback.
  unsafe {let byte=keyboard_poll(self.keyboard);input_console_accept(self.line.cast(),byte);}
 }
 #[inline(always)] fn line_ready(&self)->bool {
  // SAFETY: safe core initializes the editor before poll/read, offset136.
  unsafe {self.line.cast::<u8>().add(136).cast::<i32>().read_unaligned()!=0}
 }
 #[inline(always)] fn take_line(&mut self)->i32 {
  // SAFETY: initialized editor; caller supplies mapped output for a valid copy.
  // Existing Rust extraction retains all capacity/range/error semantics.
  unsafe {input_console_take_line(self.line.cast(),self.out,self.capacity)}
 }
 #[inline(always)] fn overlong(&mut self) {
  // SAFETY: fixed static trusted C string. No controller/authority operation.
  unsafe {serial_puts(b"input: line too long; discarded (128 characters maximum)\r\n\0".as_ptr())}
 }
 #[inline(always)] fn sleep(&mut self) {
  // SAFETY: existing thread-context one-tick wait; stack remains live. No Rust
  // pointee reference or ownership/IRQ change spans the scheduler call.
  unsafe {thread_sleep(1)}
 }
}
/// # Safety
/// keyboard/output are live mapped caller-owned storage for the synchronous
/// operation. Keyboard layout is the unchanged keyboard.h contract. Prompt is
/// trusted NUL-terminated text; optional callback and opaque ctx remain valid
/// across waits and must not unmap/free the supplied buffers. No concurrent
/// access violating existing C ownership. Neither references nor callbacks are
/// retained after return. IF/thread rules and cancellation authority stay in C.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn input_console_read_line_for(keyboard:*mut u8,out:*mut u8,capacity:u32,prompt:*const u8,cancel:Cancel,ctx:*mut c_void)->i32 {
 let probe=Raw{keyboard,out,capacity,prompt,cancel,ctx,line:core::ptr::null_mut()};
 if let Some(error)=core_reader::admit(&probe) {return error;}
 // SAFETY: readiness/output admission checked; unchanged caller mapping and
 // callback lifetime contracts. No callback occurs between these checks.
 unsafe {admitted(keyboard,out,capacity,prompt,cancel,ctx)}
}
// Private frame-owning path; the same six scalar ABI arguments permit a tail
// call. This is not another public C symbol or a change to admission semantics.
#[inline(never)]
unsafe extern "C" fn admitted(keyboard:*mut u8,out:*mut u8,capacity:u32,prompt:*const u8,cancel:Cancel,ctx:*mut c_void)->i32 {
 let mut editor=MaybeUninit::<Editor>::uninit();
 core_reader::run_admitted(&mut Raw{keyboard,out,capacity,prompt,cancel,ctx,line:editor.as_mut_ptr()})
}
