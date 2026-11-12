//! R4-V1: the scalar C ABI of the Rust virtual-memory validation decisions (kernel/rust/mm/vmm_check.rs), declared in
//! rust_kernel.h. vmm.c's `canonical`, `check_va` and `check_nonleaf` and usercopy.c's `user_range` forward here; every
//! caller, page-table access and user copy stays C. Arguments and results are integers only: this file contains no
//! unsafe block, takes no pointer and keeps no state (the `no_mangle` attributes are its only `unsafe` tokens).
#[path = "../../../kernel/rust/mm/vmm_check.rs"]
mod vmm_check_core;
use self::vmm_check_core as v;

// `size_t` and `uint64_t` are both 64-bit here: `len` reaches the core without truncation or extension.
const _: () = assert!(size_of::<usize>() == size_of::<u64>());

/// `static int canonical(uint64_t va)`: exactly 0 or 1.
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_vmm_canonical(va: u64) -> i32 {
    v::canonical(va) as i32
}

/// `static int check_va(uint64_t va)`: a `VMM_*` code.
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_vmm_check_va(va: u64) -> i32 {
    v::check_va(va)
}

/// `static int check_nonleaf(uint64_t e, int level)`: a `VMM_*` code.
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_vmm_check_nonleaf(e: u64, level: i32) -> i32 {
    v::check_nonleaf(e, level)
}

/// `static int user_range(uint64_t uptr, size_t len)`: exactly 0 or 1.
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_user_range(uptr: u64, len: usize) -> i32 {
    v::user_range(uptr, len as u64) as i32
}
