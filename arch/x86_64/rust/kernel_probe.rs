//! Scalar helpers, read-only USB.1, opt-in non-device resource fixtures and
//! USB.2-C1 read-only qualification. C platform services retain allocator and
//! mapping authority; no live xHCI backend.
#![no_std]

// The single canonical USB.2 tree: USB.1 discovery, USB.2-B ownership, C1
// qualification and the linked-only USB.2-A controller share these types.
// Public at the crate root so the complete model API is retained (not
// dead-code pruned at the lint level); only C-ABI symbols are exported.
#[path = "../../../kernel/rust/usb/xhci/mod.rs"]
pub mod xhci;
mod xhci_discovery;
mod resource_ownership;
mod xhci_qualify;
mod xhci_link;
mod xhci_dma;
mod xhci_residue;
mod ps2_pointer;

// SAFETY: this private kernel symbol is defined exactly once, with the scalar
// System V C ABI declared in rust_kernel.h. It does not accept pointers.
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_probe(seed: u64, salt: u64) -> u64 {
    seed.rotate_left(17).wrapping_add(salt.rotate_right(9)) ^ 0x564f_5352_5553_5430
}

// SAFETY: unique private kernel C-ABI symbol; u64 arguments and i32 return
// match rust_kernel.h. Short-circuiting makes the subtraction underflow-free.
#[unsafe(no_mangle)]
pub extern "C" fn vos_rust_elf_inside(size: u64, off: u64, len: u64) -> i32 {
    (off <= size && len <= size - off) as i32
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
