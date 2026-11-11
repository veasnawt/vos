//! R4-M2: `pmm_alloc_in`, `pmm_free_in` and `pmm_reserve_in` are Rust (kernel/rust/mm/pmm_transition.rs) behind their
//! unchanged pmm.h C ABI. pmm.c keeps `pmm_init_in`, the PMM instance and its storage, the kernel wrappers (zeroing,
//! poisoning, panic policy) and physical memory. The accepted C is frozen as tests/reference/pmm-r4m2.c.
//!
//! Memory model. This adapter never creates a Rust reference (`&`/`&mut`, slices, `Cell`) to the instance, its bitmaps or
//! the output word: every access is one raw-pointer read or write, issued when the core asks, which is where and in the
//! order the C accesses memory. Raw pointers carry no aliasing guarantees, so any overlap the C contract allows (identical
//! or overlapping bitmaps, `*phys` or a bitmap word on a `uint64_t` field of `struct pmm`) has the C's result. Field
//! addresses are formed with `addr_of!` on the raw instance pointer (no intermediate reference). A bitmap access is made
//! only when its pointer is non-null and 8-byte aligned and the word index is below PMM_WORDS(frames) for the frames value
//! read for that access: the instance's documented storage. Anything else would be undefined behavior in the C; the core
//! then stops with ECORRUPT.
//!
//! Each `unsafe` block relies on the C ABI contract that the C relies on too: `p` points to a live, aligned, initialized
//! `struct pmm`; each bitmap points to PMM_WORDS(frames) initialized, aligned words (pmm.h; pmm_init_in zeroes them); `phys`
//! (alloc) is a writable, aligned `uint64_t`. The kernel is single-CPU and runs PMM code with interrupts disabled, so no
//! other access to this memory happens during a call.
#[path = "../../../kernel/rust/mm/pmm_transition.rs"]
mod transition;
use self::transition as t;
use core::mem::{align_of, offset_of, size_of};
use core::ptr::addr_of;
use core::ptr::addr_of_mut;

/// `struct pmm` (pmm.h), field for field; only ever accessed through raw pointers.
#[repr(C)]
pub struct PmmRaw {
    avail: *mut u64,
    alloc: *mut u64,
    frames: u64,
    usable: u64,
    free: u64,
    hint: u64,
    ignored_bytes: u64,
    state: i32,
}
const _: () = assert!(size_of::<PmmRaw>() == 64 && align_of::<PmmRaw>() == 8);
const _: () = assert!(offset_of!(PmmRaw, avail) == 0 && offset_of!(PmmRaw, alloc) == 8 && offset_of!(PmmRaw, frames) == 16);
const _: () = assert!(offset_of!(PmmRaw, usable) == 24 && offset_of!(PmmRaw, free) == 32 && offset_of!(PmmRaw, hint) == 40);
const _: () = assert!(offset_of!(PmmRaw, ignored_bytes) == 48 && offset_of!(PmmRaw, state) == 56);

/// The instance and (for alloc) the output word, as raw pointers.
struct Raw {
    p: *mut PmmRaw,
    phys: *mut u64,
}

impl Raw {
    /// Word `w` of `bitmap`, if it is in the instance's documented storage (non-null, aligned, w < PMM_WORDS(frames)).
    fn word(&self, bitmap: *mut u64, w: u64) -> Option<*mut u64> {
        // SAFETY: `self.p` is a live, aligned `struct pmm` (contract); one read of its `frames` field.
        let frames = unsafe { addr_of!((*self.p).frames).read() };
        if bitmap.is_null() || !bitmap.is_aligned() || w >= frames / 64 {
            return None;
        }
        // In bounds of the PMM_WORDS(frames)-word bitmap (contract), so the offset stays inside its allocation.
        Some(bitmap.wrapping_add(w as usize))
    }
}

impl t::Mem for Raw {
    fn state(&self) -> i32 {
        // SAFETY: one read of a field of the live instance (contract).
        unsafe { addr_of!((*self.p).state).read() }
    }
    fn set_state(&self, v: i32) {
        // SAFETY: one write of a field of the live instance (contract).
        unsafe { addr_of_mut!((*self.p).state).write(v) }
    }
    fn frames(&self) -> u64 {
        // SAFETY: as in `state`.
        unsafe { addr_of!((*self.p).frames).read() }
    }
    fn free(&self) -> u64 {
        // SAFETY: as in `state`.
        unsafe { addr_of!((*self.p).free).read() }
    }
    fn set_free(&self, v: u64) {
        // SAFETY: as in `set_state`.
        unsafe { addr_of_mut!((*self.p).free).write(v) }
    }
    fn hint(&self) -> u64 {
        // SAFETY: as in `state`.
        unsafe { addr_of!((*self.p).hint).read() }
    }
    fn set_hint(&self, v: u64) {
        // SAFETY: as in `set_state`.
        unsafe { addr_of_mut!((*self.p).hint).write(v) }
    }
    fn avail(&self, w: u64) -> Option<u64> {
        // SAFETY: one read of the instance's `avail` pointer field (contract).
        let bitmap = unsafe { addr_of!((*self.p).avail).read() };
        let word = self.word(bitmap, w)?;
        // SAFETY: `word` is word `w` < PMM_WORDS(frames) of a non-null, aligned bitmap of PMM_WORDS(frames) initialized
        // words (contract); one read.
        Some(unsafe { word.read() })
    }
    fn alloc(&self, w: u64) -> Option<u64> {
        // SAFETY: one read of the instance's `alloc` pointer field (contract).
        let bitmap = unsafe { addr_of!((*self.p).alloc).read() };
        let word = self.word(bitmap, w)?;
        // SAFETY: as in `avail`.
        Some(unsafe { word.read() })
    }
    fn set_alloc(&self, w: u64, v: u64) -> Option<()> {
        // SAFETY: one read of the instance's `alloc` pointer field (contract).
        let bitmap = unsafe { addr_of!((*self.p).alloc).read() };
        let word = self.word(bitmap, w)?;
        // SAFETY: as in `avail`, for one write; the bitmap is writable (pmm.h: the instance owns it).
        unsafe { word.write(v) };
        Some(())
    }
    fn set_phys(&self, v: u64) {
        // SAFETY: `phys` is a writable, aligned `uint64_t` (contract: the C writes `*phys` unconditionally too).
        unsafe { self.phys.write(v) }
    }
}

/// `int pmm_alloc_in(struct pmm *p, uint64_t *phys)`: `*phys = 0` first, then the first free frame from the hint.
///
/// # Safety
/// `p` is a live `struct pmm` and its bitmaps satisfy pmm.h's contract; `phys` is a writable, aligned `uint64_t`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pmm_alloc_in(p: *mut PmmRaw, phys: *mut u64) -> i32 {
    t::alloc(&Raw { p, phys })
}

/// `int pmm_free_in(struct pmm *p, uint64_t phys)`: `pmm_free_check_in` (R4-M1), then on PMM_OK the mutation.
///
/// # Safety
/// `p` is a live `struct pmm` and its bitmaps satisfy pmm.h's contract.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pmm_free_in(p: *mut PmmRaw, phys: u64) -> i32 {
    // SAFETY: the R4-M1 check's own contract is this function's; it reads only, and its references end before it returns.
    let err = unsafe { crate::pmm::pmm_free_check_in(p.cast_const().cast(), phys) };
    if err != t::OK {
        return err;
    }
    t::free_after_check(&Raw { p, phys: core::ptr::null_mut() }, phys)
}

/// `int pmm_reserve_in(struct pmm *p, uint64_t phys)`.
///
/// # Safety
/// As for [`pmm_free_in`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pmm_reserve_in(p: *mut PmmRaw, phys: u64) -> i32 {
    t::reserve(&Raw { p, phys: core::ptr::null_mut() }, phys)
}
