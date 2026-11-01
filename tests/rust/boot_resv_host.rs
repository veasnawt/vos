//! Hosted build of the production boot reservation glue for the differential and overflow checks: the same source files the
//! kernel links (the glue reads the A2 `BootInfo` type and calls the EFI exclusion entry points, so those are part of the root),
//! compiled for the host with arithmetic overflow checks and debug assertions ON and panic = abort, so that any overflow, any
//! failed unsafe precondition or any panic on any input of the differential aborts the test. Never linked into the kernel.
#![no_std]

#[path = "../../kernel/rust/boot/efi_memory.rs"]
pub mod efi_memory;
#[path = "../../arch/x86_64/rust/efi_exclusion.rs"]
mod efi_exclusion;
#[path = "../../arch/x86_64/rust/mb2_parse.rs"]
mod mb2_parse;
#[path = "../../arch/x86_64/rust/boot_resv.rs"]
mod boot_resv;

#[panic_handler]
fn panic(_: &core::panic::PanicInfo<'_>) -> ! {
    unsafe extern "C" {
        fn abort() -> !;
    }
    // SAFETY: libc abort never returns; any Rust panic must fail the hosted test.
    unsafe { abort() }
}

// The prebuilt host `core` carries unwinding tables for some of its functions and names this personality routine, which only
// `std` defines. With panic = abort nothing ever unwinds, so a stub that is never called satisfies the linker. Host test root only.
#[unsafe(no_mangle)]
pub extern "C" fn rust_eh_personality() {}
