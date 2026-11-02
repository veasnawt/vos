//! Stable kernel C ABI; no editor borrow crosses an existing C echo callback.
#[path = "../../../kernel/rust/input/console_accept.rs"]
pub mod core_accept;
use core_accept::Editor;
use core::mem::{size_of, align_of, offset_of};
#[repr(C)]
struct Line { length: u32, bytes: [u8;129], ready: i32, overflow: i32 }
const _: () = {
    assert!(size_of::<Line>() == 144 && align_of::<Line>() == 4);
    assert!(offset_of!(Line,length)==0 && offset_of!(Line,bytes)==4);
    assert!(offset_of!(Line,ready)==136 && offset_of!(Line,overflow)==140);
};
unsafe extern "C" {
    fn serial_puts(text: *const u8);
    fn serial_putc(byte: core::ffi::c_char);
    fn console_erase_previous();
}
struct Raw { line: *mut Line }
impl Editor for Raw {
    #[inline(always)] fn ready(&self)->bool {
        // SAFETY: entry requires mapped caller-owned editor; raw read, no borrow.
        unsafe { core::ptr::addr_of!((*self.line).ready).read_unaligned()!=0 }
    }
    #[inline(always)] fn length(&self)->u32 {
        // SAFETY: reload after callbacks, preserving the C storage sequence.
        unsafe { core::ptr::addr_of!((*self.line).length).read_unaligned() }
    }
    #[inline(always)] fn set_length(&mut self,n:u32) {
        // SAFETY: same mapped writable object; core proves n<=128.
        unsafe { core::ptr::addr_of_mut!((*self.line).length).write_unaligned(n) }
    }
    #[inline(always)] fn write(&mut self,index:u32,byte:u8) {
        // SAFETY: safe core proves index<=128, inside the 129-byte array.
        unsafe { self.line.cast::<u8>().add(4+index as usize).write(byte) }
    }
    #[inline(always)] fn seal(&mut self) {
        // SAFETY: mapped editor; happens after CRLF as in C.
        unsafe { core::ptr::addr_of_mut!((*self.line).ready).write_unaligned(1) }
    }
    #[inline(always)] fn overflow(&mut self) {
        // SAFETY: sticky overflow; never clear on Backspace or accepted bytes.
        unsafe { core::ptr::addr_of_mut!((*self.line).overflow).write_unaligned(1) }
    }
    #[inline(always)] fn newline(&mut self) {
        // SAFETY: static NUL-terminated bytes consumed synchronously by C.
        unsafe { serial_puts(b"\r\n\0".as_ptr()) }
    }
    #[inline(always)] fn erase(&mut self) {
        // SAFETY: existing trusted synchronous kernel callback. No editor borrow.
        unsafe { console_erase_previous() }
    }
    #[inline(always)] fn echo(&mut self,byte:u8) {
        // SAFETY: core admits printable ASCII; exact C char ABI, no retained borrow.
        unsafe { serial_putc(byte as core::ffi::c_char) }
    }
}
/// # Safety
/// line points to a mapped readable/writable 144-byte editor, exclusively owned
/// for synchronous editing. Callbacks must not free/unmap it; no concurrent
/// writers. Raw accesses permit defined callback observations/edits without
/// Rust aliasing references. Existing callback ordering and post-erase reload
/// are preserved. Null/wrapping editor is ignored; malformed lengths cannot
/// index outside the array. No ownership, IRQ or scheduling authority is added.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn input_console_accept(line:*mut u8,byte:core::ffi::c_char) {
    // keyboard_poll returns NUL when no key is available. It never edits or
    // echoes: avoid touching the editor for this common no-op.
    if byte == 0 { return; }
    if (line as usize).wrapping_sub(1) >= usize::MAX - size_of::<Line>() { return; }
    core_accept::accept(&mut Raw{line:line.cast()},byte as u8)
}
