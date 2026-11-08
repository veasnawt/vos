//! R4-M1: `pmm_free_check_in`, `pmm_frame_state_in` and `pmm_check_in` are Rust (kernel/rust/mm/pmm.rs) behind their
//! unchanged pmm.h C ABI. pmm.c keeps the PMM instance, both bitmaps, init, alloc, free, reserve and poisoning; the
//! accepted C is frozen as tests/reference/pmm-r4m1.c and is not linked into the kernel.
//!
//! This adapter is the only unsafe code: it copies the caller's `struct pmm` and, only when the core asks for the
//! bitmaps (after every check the C makes before its first bitmap read), forms the two `frames / 64`-word slices —
//! and only for an instance within the documented bounds (`pmm_core::instance_ok`: frames, non-null, 8-byte aligned).
#[path = "../../../kernel/rust/mm/pmm.rs"]
mod pmm_core;
use self::pmm_core as m;
use core::mem::{align_of, offset_of, size_of};

/// `struct pmm` (pmm.h), field for field.
#[repr(C)]
pub struct Pmm {
    avail: *const u64,
    alloc: *const u64,
    frames: u64,
    usable: u64,
    free: u64,
    hint: u64,
    ignored_bytes: u64,
    state: i32,
}
const _: () = assert!(size_of::<Pmm>() == 64 && align_of::<Pmm>() == 8);
const _: () = assert!(offset_of!(Pmm, avail) == 0 && offset_of!(Pmm, alloc) == 8 && offset_of!(Pmm, frames) == 16);
const _: () = assert!(offset_of!(Pmm, usable) == 24 && offset_of!(Pmm, free) == 32 && offset_of!(Pmm, hint) == 40);
const _: () = assert!(offset_of!(Pmm, ignored_bytes) == 48 && offset_of!(Pmm, state) == 56);

impl m::Instance for Pmm {
    fn state(&self) -> i32 { self.state }
    fn frames(&self) -> u64 { self.frames }
    fn usable(&self) -> u64 { self.usable }
    fn free(&self) -> u64 { self.free }
    fn hint(&self) -> u64 { self.hint }
    /// The bitmaps of an instance within the documented bounds; `None` (no access) for any other instance.
    fn bitmaps(&self) -> Option<m::Bitmaps<'_>> {
        if !m::instance_ok(self.frames, self.avail.addr(), self.alloc.addr()) {
            return None;
        }
        let words = (self.frames / 64) as usize; // at most PMM_WORDS(PMM_MAX_FRAMES) = 4096
        // SAFETY: pmm.h's instance contract: `avail` and `alloc` each point to PMM_WORDS(frames) initialized words
        // (pmm_init_in zeroes exactly those words before an instance becomes READY), and both are checked non-null,
        // aligned and of a bounded length just above. The slices borrow `self` and live only within one check, in
        // which nothing writes the bitmaps: the kernel is single-CPU and runs PMM code with interrupts disabled, and
        // these read-only checks call no PMM function that writes. Shared slices may overlap.
        Some(unsafe { m::Bitmaps { avail: core::slice::from_raw_parts(self.avail, words), alloc: core::slice::from_raw_parts(self.alloc, words) } })
    }
}

/// `int pmm_free_check_in(const struct pmm *p, uint64_t phys)`.
///
/// # Safety
/// `p` points to a readable `struct pmm` (the C dereferences it unconditionally too); the bitmaps of a READY
/// instance satisfy pmm.h's contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pmm_free_check_in(p: *const Pmm, phys: u64) -> i32 {
    // SAFETY: `p` is valid for reads of a `struct pmm` (contract above); the reference lives only for this call.
    let s = unsafe { &*p };
    m::free_check(s, phys)
}

/// `int pmm_frame_state_in(const struct pmm *p, uint64_t phys)`.
///
/// # Safety
/// As for [`pmm_free_check_in`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pmm_frame_state_in(p: *const Pmm, phys: u64) -> i32 {
    // SAFETY: as in pmm_free_check_in.
    let s = unsafe { &*p };
    m::frame_state(s, phys)
}

/// `int pmm_check_in(const struct pmm *p)`.
///
/// # Safety
/// As for [`pmm_free_check_in`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pmm_check_in(p: *const Pmm) -> i32 {
    // SAFETY: as in pmm_free_check_in.
    let s = unsafe { &*p };
    m::check(s)
}
