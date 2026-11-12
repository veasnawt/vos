//! R4-V1: the four pure validation decisions of the C virtual-memory code — `canonical`, `check_va` and `check_nonleaf`
//! (arch/x86_64/vmm.c) and `user_range` (arch/x86_64/usercopy.c) — as safe, panic-free Rust. Each takes scalars and returns
//! a scalar: no pointer, slice, owned memory or state. The C keeps every page-table walk and modification, mapping and
//! unmapping, frame ownership, the PMM, TLB operations, user copies, the audit and exception handling, and calls these
//! through its unchanged static functions; the accepted C is frozen as tests/reference/vmm-r4v1.c and usercopy-r4v1.c.
//!
//! The constants repeat memlayout.h and vmm.h; scripts/test-vmm-check.py binds them to those headers.
#![forbid(unsafe_code)]

pub const PAGE_SIZE: u64 = 0x1000;
pub const LOWER_HALF_END: u64 = 0x0000_8000_0000_0000; // first non-canonical address
pub const UPPER_HALF_START: u64 = 0xFFFF_8000_0000_0000; // first canonical upper-half address
pub const NULL_GUARD_END: u64 = 0x0000_0000_0040_0000; // [0, 4 MiB) is never mapped
pub const LOWER_MAP_END: u64 = 0x0000_7FFF_FFFF_F000; // the highest lower-half page is never mapped
pub const USER_IMAGE_START: u64 = 0x0000_0000_0040_0000;

pub const PTE_P: u64 = 1 << 0;
pub const PTE_W: u64 = 1 << 1;
pub const PTE_U: u64 = 1 << 2;
pub const PTE_A: u64 = 1 << 5;
pub const PTE_PS: u64 = 1 << 7;
pub const PTE_ADDR: u64 = 0x000F_FFFF_FFFF_F000;
pub const NONLEAF_BITS: u64 = PTE_P | PTE_W | PTE_U | PTE_A | PTE_ADDR;

pub const OK: i32 = 0;
pub const EALIGN: i32 = 1;
pub const ENONCANON: i32 = 2;
pub const ERANGE: i32 = 3;
pub const EKERNEL: i32 = 4;
pub const ELARGE: i32 = 9;
pub const ECORRUPT: i32 = 11;

/// `canonical`: `va` is in the lower or the upper canonical half.
pub fn canonical(va: u64) -> bool {
    va < LOWER_HALF_END || va >= UPPER_HALF_START
}

/// `check_va`, the validation shared by map and unmap, in the accepted order: non-canonical, upper half, alignment,
/// then the null guard and the top lower-half page.
pub fn check_va(va: u64) -> i32 {
    if !canonical(va) {
        return ENONCANON;
    }
    if va >= UPPER_HALF_START {
        return EKERNEL;
    }
    if va % PAGE_SIZE != 0 {
        return EALIGN;
    }
    if va < NULL_GUARD_END || va >= LOWER_MAP_END {
        return ERANGE;
    }
    OK
}

/// `check_nonleaf`: a present non-leaf entry at `level` (3..1) that may be followed. PS is decided first: it is reserved
/// in a PML4E (`level == 3`) and marks a large page at any other level; then any bit outside `NONLEAF_BITS` is corrupt.
pub fn check_nonleaf(e: u64, level: i32) -> i32 {
    if e & PTE_PS != 0 {
        return if level == 3 { ECORRUPT } else { ELARGE };
    }
    if e & !NONLEAF_BITS != 0 {
        return ECORRUPT;
    }
    OK
}

/// `user_range`: `[uptr, uptr + len)` lies inside the user range, without overflow. The subtraction is only evaluated
/// for `uptr < LOWER_HALF_END` (short-circuit, as in the C), where it cannot wrap.
pub fn user_range(uptr: u64, len: u64) -> bool {
    uptr >= USER_IMAGE_START && uptr < LOWER_HALF_END && len <= LOWER_HALF_END.wrapping_sub(uptr)
}
