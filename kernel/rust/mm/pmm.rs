//! R4-M1: the three read-only checks of the C physical-memory manager (arch/x86_64/pmm.c) — `pmm_free_check_in`,
//! `pmm_frame_state_in` and `pmm_check_in` — as safe, panic-free Rust. The C keeps PMM ownership, both bitmaps,
//! initialization, allocation, freeing, reservation, poisoning, physical memory and interrupts; the adapter
//! (arch/x86_64/rust/pmm.rs) keeps the C ABI and is the only unsafe code.
//!
//! The core never touches the bitmaps on its own. Each check first makes, in the accepted C order, every decision
//! that needs no bitmap word (readiness, then alignment and range); only then does it ask the adapter for the bitmaps,
//! as slices of exactly `frames / 64` words. The adapter supplies them only for an instance within the documented
//! bounds that `pmm_init_in` establishes for every READY instance (see [`instance_ok`]); otherwise the check returns
//! its corruption result without any bitmap access. On every input where the accepted C returns before reading a
//! bitmap, the result is therefore the C result, whatever the remaining fields hold.
#![forbid(unsafe_code)]

pub const PAGE_SIZE: u64 = 0x1000;
pub const LOW_LIMIT: u64 = 0x10_0000;
pub const MAX_FRAMES: u64 = 262_144;

pub const READY: i32 = 1;

pub const OK: i32 = 0;
pub const ENOTREADY: i32 = 1;
pub const EALIGN: i32 = 3;
pub const ERANGE: i32 = 4;
pub const EPROTECTED: i32 = 5;
pub const ENOTALLOC: i32 = 6;
pub const ECORRUPT: i32 = 9;

pub const FRAME_UNAVAILABLE: i32 = 0;
pub const FRAME_FREE: i32 = 1;
pub const FRAME_ALLOCATED: i32 = 2;

/// Both bitmaps, each `frames / 64` words: bit `f % 64` of word `f / 64` is frame `f`.
#[derive(Clone, Copy)]
pub struct Bitmaps<'a> {
    pub avail: &'a [u64],
    pub alloc: &'a [u64],
}

/// A PMM instance as the checks see it: each field is read where the accepted C reads it, and the bitmaps are only
/// requested after every decision that needs no bitmap word. `bitmaps` is `None`, without any bitmap access, for an
/// instance outside the documented bounds.
pub trait Instance {
    fn state(&self) -> i32;
    fn frames(&self) -> u64;
    fn usable(&self) -> u64;
    fn free(&self) -> u64;
    fn hint(&self) -> u64;
    fn bitmaps(&self) -> Option<Bitmaps<'_>>;
}

/// The documented instance bounds, exactly `pmm_init_in`'s: `frames` a nonzero multiple of 64, at most `PMM_MAX_FRAMES`
/// (every frame below `frames` then has its bit inside the `frames / 64` bitmap words), and both bitmap addresses
/// non-null and 8-byte aligned. One branch-free predicate: the adapter evaluates it on every bitmap request.
pub fn instance_ok(frames: u64, avail: usize, alloc: usize) -> bool {
    !((frames.wrapping_sub(1) >= MAX_FRAMES) | (frames % 64 != 0) | (avail == 0) | (alloc == 0) | ((avail | alloc) % 8 != 0))
}

/// `count_bits`: the accepted C's clear-lowest-bit loop (no popcnt: the build targets baseline x86_64).
fn count_bits(mut x: u64) -> u64 {
    let mut n = 0u64;
    while x != 0 {
        x &= x.wrapping_sub(1);
        n = n.wrapping_add(1);
    }
    n
}

/// `test_bit`: `None` when the frame's word lies outside the bitmap (never, for an instance within the bounds).
fn bit(bm: &[u64], frame: u64) -> Option<bool> {
    let w = usize::try_from(frame / 64).ok()?;
    bm.get(w).map(|x| (x >> (frame % 64)) & 1 != 0)
}

/// `pmm_free_check_in`: may `phys` be freed? Readiness, alignment, range, allocatable, allocated, counters.
pub fn free_check(p: &impl Instance, phys: u64) -> i32 {
    if p.state() != READY {
        return ENOTREADY;
    }
    if phys % PAGE_SIZE != 0 {
        return EALIGN;
    }
    if phys >= p.frames().wrapping_mul(PAGE_SIZE) {
        return ERANGE;
    }
    let Some(b) = p.bitmaps() else { return ECORRUPT };
    let frame = phys / PAGE_SIZE;
    match bit(b.avail, frame) {
        Some(true) => {}
        Some(false) => return EPROTECTED,
        None => return ECORRUPT,
    }
    match bit(b.alloc, frame) {
        Some(true) => {}
        Some(false) => return ENOTALLOC,
        None => return ECORRUPT,
    }
    if p.free() >= p.usable() {
        return ECORRUPT;
    }
    OK
}

/// `pmm_frame_state_in`: unavailable, free or allocated.
pub fn frame_state(p: &impl Instance, phys: u64) -> i32 {
    let frame = phys / PAGE_SIZE;
    if p.state() != READY || frame >= p.frames() {
        return FRAME_UNAVAILABLE;
    }
    let Some(b) = p.bitmaps() else { return FRAME_UNAVAILABLE };
    if bit(b.avail, frame) != Some(true) {
        return FRAME_UNAVAILABLE;
    }
    match bit(b.alloc, frame) {
        Some(true) => FRAME_ALLOCATED,
        Some(false) => FRAME_FREE,
        None => FRAME_UNAVAILABLE,
    }
}

/// `pmm_check_in`: recount both bitmaps and check every invariant, word by word in the accepted order.
pub fn check(p: &impl Instance) -> i32 {
    if p.state() != READY {
        return ECORRUPT;
    }
    let words = p.frames() / 64;
    let (mut usable, mut free) = (0u64, 0u64);
    if words != 0 {
        let Some(b) = p.bitmaps() else { return ECORRUPT };
        if b.avail.len() as u64 != words || b.alloc.len() as u64 != words {
            return ECORRUPT;
        }
        for (w, (&avail, &alloc)) in b.avail.iter().zip(b.alloc).enumerate() {
            let w = w as u64;
            let m = avail & !alloc;
            if alloc & !avail != 0 {
                return ECORRUPT; // allocated frame that was never allocatable
            }
            if w < LOW_LIMIT / PAGE_SIZE / 64 && avail != 0 {
                return ECORRUPT; // allocatable frame below 1 MiB
            }
            if w < p.hint() && m != 0 {
                return ECORRUPT; // free frame below the hint
            }
            usable = usable.wrapping_add(count_bits(avail));
            free = free.wrapping_add(count_bits(m));
        }
    }
    if usable != p.usable() || free != p.free() || free > usable {
        return ECORRUPT;
    }
    OK
}
