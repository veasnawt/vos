//! Hosted build of the production ELF validator glue for the differential and overflow checks: the same two source files the
//! kernel links, compiled for the host with arithmetic overflow checks and debug assertions ON and panic = abort, so that any
//! overflow or panic on any input of the differential aborts the test. Never linked into the kernel.
#![no_std]

#[path = "../../arch/x86_64/rust/elf_validate.rs"]
mod elf_validate;

#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    unsafe extern "C" {
        fn abort() -> !;
    }
    // SAFETY: libc abort never returns; any Rust panic must fail the hosted test.
    unsafe { abort() }
}
