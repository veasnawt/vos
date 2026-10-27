//! R0-B: pure integer C-ABI proof; no device, allocator or userspace authority.
#![no_std]

// SAFETY: this private kernel symbol is defined exactly once, with the scalar
// System V C ABI declared in rust_kernel.h. It does not accept pointers.
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_probe(seed: u64, salt: u64) -> u64 {
    seed.rotate_left(17).wrapping_add(salt.rotate_right(9)) ^ 0x564f_5352_5553_5430
}

unsafe extern "C" {
    // panic.h: the existing kernel panic disables interrupts and never returns.
    #[link_name = "panic"]
    fn c_kernel_panic(message: *const u8) -> !;
}

#[panic_handler]
fn kernel_panic(_: &core::panic::PanicInfo<'_>) -> ! {
    // SAFETY: the fixed NUL-terminated bytes have static kernel-rodata lifetime;
    // C's panic(const char *) consumes them synchronously and cannot return.
    unsafe { c_kernel_panic(b"Rust kernel panic\0".as_ptr()) }
}

// Compiled only for the existing, explicitly selected VOS_TEST case.
#[cfg(vos_rust_panic_test)]
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_panic_test() -> ! {
    panic!("R0-B deliberate panic");
}
