//! Stable kernel C ABI. Device I/O and input ownership remain in C.
#[path = "../../../kernel/rust/input/console_line.rs"]
pub mod core_line;
use core_line::Storage;
use core::mem::{size_of, align_of, offset_of};
#[repr(C)]
struct Line { length: u32, bytes: [u8; 129], ready: i32, overflow: i32 }
const _: () = {
    assert!(size_of::<Line>() == 144 && align_of::<Line>() == 4);
    assert!(offset_of!(Line, length) == 0 && offset_of!(Line, bytes) == 4);
    assert!(offset_of!(Line, ready) == 136 && offset_of!(Line, overflow) == 140);
};
struct Raw { line: *mut Line, out: *mut u8, capacity: u32 }
impl Storage for Raw {
    #[inline(always)] fn ready(&self) -> bool {
        // SAFETY: entry contract provides a mapped editor; no reference is made.
        unsafe { core::ptr::addr_of!((*self.line).ready).read_unaligned() != 0 }
    }
    #[inline(always)] fn overflow(&self) -> bool {
        // SAFETY: same caller-owned editor; all i32 encodings are valid.
        unsafe { core::ptr::addr_of!((*self.line).overflow).read_unaligned() != 0 }
    }
    #[inline(always)] fn length(&self) -> u32 {
        // SAFETY: same mapped editor, raw read cannot create an aliased borrow.
        unsafe { core::ptr::addr_of!((*self.line).length).read_unaligned() }
    }
    #[inline(always)] fn capacity(&self) -> u32 { self.capacity }
    #[inline(always)] fn output_valid(&self, count: u32) -> bool {
        // count is 1..=129, admitted by the safe core. The single interval test
        // is exactly non-null && checked_add(count).is_some(), including both
        // ends of usize. It does not waive the caller's mapping obligation.
        (self.out as usize).wrapping_sub(1) <
            usize::MAX - count as usize
    }
    #[inline(always)] fn read(&self, index: u32) -> u8 {
        // SAFETY: the safe core admits length<=128 before this bounded loop.
        // Read just before each write, preserving defined forward-copy overlap.
        unsafe { self.line.cast::<u8>().add(4 + index as usize).read() }
    }
    #[inline(always)] fn write(&mut self, index: u32, byte: u8) {
        // SAFETY: admitted output count is mapped/writable by the entry contract.
        unsafe { self.out.add(index as usize).write(byte) }
    }
    #[inline(always)] fn clear(&mut self) {
        // SAFETY: exclusive mapped 144-byte editor, 18 exact eight-byte stores.
        // x86-64 allows unaligned stores; SysV ABI requires DF clear (the old C
        // implementation already uses rep stosq). No stack access or flags
        // change. Default asm memory clobber orders the copy before this reset.
        // Explicit bounded string-store avoids a byte-loop C memset dependency.
        unsafe {
            core::arch::asm!("rep stosq", inout("rdi") self.line => _,
                inout("rcx") 18usize => _, in("rax") 0usize,
                options(nostack, preserves_flags));
        }
    }
}

/// # Safety
/// line identifies a mapped readable/writable editor exclusively owned for this
/// synchronous operation. For an admitted copy, out identifies length+1 mapped
/// writable bytes. Output may overlap editor storage: forward reads/writes and
/// final reset preserve the C contract without overlapping Rust references.
/// No callbacks, yielding or retained borrows occur. Mapping validity remains
/// the caller's duty. Null/wrapping editor or output and oversized copy lengths
/// fail closed as NOSPACE; PENDING/TOOLONG precedence remains as in C otherwise.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn input_console_take_line(line: *mut u8, out: *mut u8, capacity: u32) -> i32 {
    if (line as usize).wrapping_sub(1) >= usize::MAX - size_of::<Line>() {
        return core_line::NOSPACE;
    }
    core_line::take_line(&mut Raw { line: line.cast(), out, capacity })
}
