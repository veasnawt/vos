//! R4-M2: the PMM state transitions of the accepted C (arch/x86_64/pmm.c at main 74c2341b, frozen as
//! tests/reference/pmm-r4m2.c) — `pmm_alloc_in`, the mutation and state update of `pmm_free_in`, and `pmm_reserve_in` —
//! as safe, panic-free Rust. The C keeps `pmm_init_in`, the PMM instance and its backing storage, the kernel wrappers,
//! zeroing, poisoning, physical mapping, interrupts and the panic policy.
//!
//! The core never holds a reference to instance or bitmap memory. Every read and write goes through [`Mem`], one access
//! per C access, in the accepted C's source order: a field read where the C reads `p->field`, a bitmap word read or write
//! where the C reads or writes `p->avail[w]` / `p->alloc[w]`, the output write where the C writes `*phys`. So every
//! aliasing arrangement the C contract allows — `avail` and `alloc` identical or overlapping, `*phys` or a bitmap word
//! overlapping a `uint64_t` field of the instance — behaves as in the C, access for access.
//!
//! A bitmap access answers `None` only where the C would access memory outside the instance's documented storage
//! (`PMM_WORDS(frames)` words at non-null, 8-byte-aligned bitmaps): undefined behavior in the C. There the transition
//! stops with ECORRUPT and makes no further access; earlier accesses (as in the C) have already happened.
#![forbid(unsafe_code)]

pub const PAGE_SIZE: u64 = 0x1000;
pub const LOW_LIMIT: u64 = 0x10_0000;

pub const READY: i32 = 1;
pub const FAILED: i32 = 2;

pub const OK: i32 = 0;
pub const ENOTREADY: i32 = 1;
pub const ENOMEM: i32 = 2;
pub const EALIGN: i32 = 3;
pub const ERANGE: i32 = 4;
pub const EPROTECTED: i32 = 5;
pub const ECORRUPT: i32 = 9;
pub const EINUSE: i32 = 10;

/// One PMM instance and the caller's output word, as the C accesses them. Each method is exactly one memory access.
pub trait Mem {
    fn state(&self) -> i32;
    fn set_state(&self, v: i32);
    fn frames(&self) -> u64;
    fn free(&self) -> u64;
    fn set_free(&self, v: u64);
    fn hint(&self) -> u64;
    fn set_hint(&self, v: u64);
    /// `p->avail[w]` (reads `p->avail`, `p->frames`, then the word); `None` outside the documented storage.
    fn avail(&self, w: u64) -> Option<u64>;
    /// `p->alloc[w]`; `None` outside the documented storage.
    fn alloc(&self, w: u64) -> Option<u64>;
    /// `p->alloc[w] = v`; `None` (and no write) outside the documented storage.
    fn set_alloc(&self, w: u64, v: u64) -> Option<()>;
    /// `*phys = v` (alloc only).
    fn set_phys(&self, v: u64);
}

/// `test_bit(bm, frame)`: the word read, then the bit.
fn bit(word: u64, frame: u64) -> bool {
    (word >> (frame % 64)) & 1 != 0
}

/// `pmm_alloc_in`: the first free frame from the hint, or ENOMEM; ECORRUPT (and FAILED) when the counters or the bitmap
/// contradict each other.
pub fn alloc(m: &impl Mem) -> i32 {
    m.set_phys(0);
    if m.state() != READY {
        return ENOTREADY;
    }
    let mut w = m.hint();
    while w < m.frames() / 64 {
        let Some(avail) = m.avail(w) else { return ECORRUPT };
        let Some(alloc) = m.alloc(w) else { return ECORRUPT };
        let free_bits = avail & !alloc;
        if free_bits == 0 {
            w = w.wrapping_add(1);
            continue;
        }
        let frame = w.wrapping_mul(64).wrapping_add(free_bits.trailing_zeros() as u64);
        if m.free() == 0 || frame.wrapping_mul(PAGE_SIZE) < LOW_LIMIT || frame >= m.frames() {
            m.set_state(FAILED);
            return ECORRUPT;
        }
        let Some(word) = m.alloc(w) else { return ECORRUPT };
        if m.set_alloc(w, word | 1u64 << (frame % 64)).is_none() {
            return ECORRUPT;
        }
        m.set_free(m.free().wrapping_sub(1));
        m.set_hint(w);
        m.set_phys(frame.wrapping_mul(PAGE_SIZE));
        return OK;
    }
    if m.free() != 0 {
        m.set_state(FAILED);
        return ECORRUPT; // the count claims frames the bitmap lacks
    }
    m.set_hint(m.frames() / 64);
    ENOMEM
}

/// The mutation and state update of `pmm_free_in`, after `pmm_free_check_in(p, phys)` returned PMM_OK.
pub fn free_after_check(m: &impl Mem, phys: u64) -> i32 {
    let frame = phys / PAGE_SIZE;
    let w = frame / 64;
    let Some(word) = m.alloc(w) else { return ECORRUPT };
    if m.set_alloc(w, word & !(1u64 << (frame % 64))).is_none() {
        return ECORRUPT;
    }
    m.set_free(m.free().wrapping_add(1));
    if w < m.hint() {
        m.set_hint(w);
    }
    OK
}

/// `pmm_reserve_in`: mark one specific free frame allocated.
pub fn reserve(m: &impl Mem, phys: u64) -> i32 {
    if m.state() != READY {
        return ENOTREADY;
    }
    if phys % PAGE_SIZE != 0 {
        return EALIGN;
    }
    if phys >= m.frames().wrapping_mul(PAGE_SIZE) {
        return ERANGE;
    }
    let frame = phys / PAGE_SIZE;
    let w = frame / 64;
    match m.avail(w) {
        Some(word) if bit(word, frame) => {}
        Some(_) => return EPROTECTED,
        None => return ECORRUPT,
    }
    match m.alloc(w) {
        Some(word) if bit(word, frame) => return EINUSE,
        Some(_) => {}
        None => return ECORRUPT,
    }
    if m.free() == 0 {
        return ECORRUPT;
    }
    let Some(word) = m.alloc(w) else { return ECORRUPT };
    if m.set_alloc(w, word | 1u64 << (frame % 64)).is_none() {
        return ECORRUPT;
    }
    m.set_free(m.free().wrapping_sub(1));
    OK
}
