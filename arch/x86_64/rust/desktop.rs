//! R3-G1: `desktop_requested` and `desktop_compute` (desktop.h, unchanged C ABI) are Rust. Their callers (kernel.c,
//! screen.c, desktop_paint), `struct desktop_layout` and every layout value are unchanged. The accepted C is kept only as
//! the frozen differential reference (tests/reference/desktop-r3g1.c) and is not linked into the kernel.
//!
//! This glue is the only unsafe code: it reads the caller's NUL-terminated command line and writes the caller's
//! `struct desktop_layout`. The decisions (kernel/rust/gfx/desktop.rs) are safe and pure.
#[path = "../../../kernel/rust/gfx/desktop.rs"]
mod desktop_core;
use self::desktop_core::{compute, requested_by};

#[repr(C)]
pub struct DesktopLayout {
    x: u32,
    y: u32,
    w: u32,
    h: u32,
    text_x: u32,
    text_y: u32,
    text_w: u32,
    text_h: u32,
}

// The layout of `struct desktop_layout` in desktop.h, pinned. tests/host/desktop-differential.c asserts the same numbers
// against the C compiler's own `sizeof` and `offsetof`.
const _: () = {
    assert!(::core::mem::size_of::<DesktopLayout>() == 32);
    assert!(::core::mem::align_of::<DesktopLayout>() == 4);
    assert!(::core::mem::offset_of!(DesktopLayout, x) == 0);
    assert!(::core::mem::offset_of!(DesktopLayout, h) == 12);
    assert!(::core::mem::offset_of!(DesktopLayout, text_x) == 16);
    assert!(::core::mem::offset_of!(DesktopLayout, text_h) == 28);
};

/// `int desktop_requested(const char *cmdline)`: 1 if a space/tab-separated field is exactly `vos.desktop=1`, else 0;
/// 0 for NULL, as in the C.
///
/// # Safety
/// `cmdline` is NULL or points at a NUL-terminated string that nobody writes during this synchronous call (the boot
/// command line, which mb2_parse verified to be terminated inside its tag). Reading stops at the NUL or at the first
/// matching field, as in the C.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn desktop_requested(cmdline: *const u8) -> i32 {
    if cmdline.is_null() {
        return 0;
    }
    // SAFETY: `requested_by` reads index `i` only after reading a non-NUL byte at every index below `i`, so every
    // read is inside the NUL-terminated string (contract above). Verified by the guard-page differential.
    requested_by(|i| unsafe { cmdline.add(i).read() }) as i32
}

/// `int desktop_compute(uint32_t width, uint32_t height, struct desktop_layout *out)`: 1 and `*out` filled for a
/// supported mode; otherwise 0 and `*out` untouched, as in the C (including a NULL `out`).
///
/// # Safety
/// `out` is NULL or writable, aligned storage for one `struct desktop_layout`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn desktop_compute(width: u32, height: u32, out: *mut DesktopLayout) -> i32 {
    // The C tests `out` first; testing it after the dimensions refuses exactly the same calls with no write, and lets the
    // compiler keep the C's separate branches (R3-G1 performance investigation).
    match compute(width, height) {
        None => 0,
        Some(_) if out.is_null() => 0,
        Some(l) => {
            let v = DesktopLayout {
                x: l.x,
                y: l.y,
                w: l.w,
                h: l.h,
                text_x: l.text_x,
                text_y: l.text_y,
                text_w: l.text_w,
                text_h: l.text_h,
            };
            // SAFETY: `out` is non-null, aligned and writable (contract above).
            unsafe { out.write(v) };
            1
        }
    }
}
