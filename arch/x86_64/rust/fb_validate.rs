//! Existing bootinfo.h ABI: `boot_framebuffer` is now Rust. Its caller (screen.c), the self-tests, `struct boot_fb`, `struct
//! boot_info` and every constant and message are unchanged. The accepted C is kept only as the frozen differential reference
//! (tests/host/reference/bootinfo_ref.c) and is not linked into the kernel. `boot_mmap_entry` and `boot_reserve` stay C.
//!
//! This glue is the only unsafe code: it reads two fields of the caller's `struct boot_info`, the tag they point at (loader
//! memory, untrusted) and writes the caller's `struct boot_fb`. The validation (kernel/rust/mb2/framebuffer.rs) is safe and pure.
#[path = "../../../kernel/rust/mb2/framebuffer.rs"]
mod framebuffer;
use self::framebuffer::{precheck, validate, E_NONE, READ_MAX};
use crate::mb2_parse::BootInfo;

#[repr(C)]
pub struct BootFb {
    kind: i32,
    addr: u64,
    size: u64,
    pitch: u32,
    width: u32,
    height: u32,
}

// The layout of `struct boot_fb` in bootinfo.h, pinned. tests/host/fb-differential.c asserts the same numbers against the C
// compiler's own `sizeof` and `offsetof`.
const _: () = {
    assert!(::core::mem::size_of::<BootFb>() == 40);
    assert!(::core::mem::align_of::<BootFb>() == 8);
    assert!(::core::mem::offset_of!(BootFb, kind) == 0);
    assert!(::core::mem::offset_of!(BootFb, addr) == 8);
    assert!(::core::mem::offset_of!(BootFb, size) == 16);
    assert!(::core::mem::offset_of!(BootFb, pitch) == 24);
    assert!(::core::mem::offset_of!(BootFb, width) == 28);
    assert!(::core::mem::offset_of!(BootFb, height) == 32);
    assert!(::core::mem::size_of::<BootInfo>() == 1624);
};

fn overlap(a: usize, a_len: usize, b: usize, b_len: usize) -> bool {
    match (a.checked_add(a_len), b.checked_add(b_len)) {
        (Some(end_a), Some(end_b)) => a < end_b && b < end_a,
        _ => true,
    }
}

/// Validate the framebuffer tag that `mb2_parse` recorded in `bi` and fill `fb`. Returns NULL on success, else a static error
/// string. As in the C, `*fb` is zeroed first and is populated only on success; the tag is not read at all unless exactly one
/// tag was recorded, and then at most `min(size, 38)` of its bytes are.
///
/// # Safety
/// `bi` is readable `struct boot_info` storage and `fb` is writable, aligned storage for one `struct boot_fb`. When `bi->fb_tags`
/// is 1 and `bi->fb` is non-null, `bi->fb` points at a tag that is readable for its 8-byte header and for `min(size, 38)` bytes
/// of its own claimed size, and nobody writes it during this synchronous call (`mb2_parse` bounded it inside the loader's block).
///
/// Defensive differences from the C, for caller bugs the C would fault or corrupt itself on (never a valid call), all reported
/// with the existing "no framebuffer information" string and without writing `*fb`: a null or misaligned `bi` or `fb`, an `fb`
/// that overlaps `bi`, an `fb` that overlaps the tag. Nothing here checks that an arbitrary pointer is mapped.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn boot_framebuffer(bi: *const BootInfo, fb: *mut BootFb) -> *const u8 {
    let aligned = |p: usize, a: usize| p.is_multiple_of(a);
    if bi.is_null()
        || !aligned(bi as usize, ::core::mem::align_of::<BootInfo>())
        || fb.is_null()
        || !aligned(fb as usize, ::core::mem::align_of::<BootFb>())
        || overlap(fb as usize, ::core::mem::size_of::<BootFb>(), bi as usize, ::core::mem::size_of::<BootInfo>())
    {
        return E_NONE.as_ptr();
    }
    // SAFETY: `bi` is non-null, aligned and readable by the caller's contract; these are two field reads.
    let (fb_tags, tag) = unsafe { (::core::ptr::addr_of!((*bi).fb_tags).read(), ::core::ptr::addr_of!((*bi).fb).read()) };
    if let Err(e) = precheck(fb_tags, !tag.is_null()) {
        // SAFETY: `fb` is non-null, aligned and writable by the caller's contract.
        unsafe { ::core::ptr::write_bytes(fb, 0, 1) };
        return e.as_ptr();
    }
    // SAFETY: one tag was recorded and its pointer is non-null; the caller's contract makes the 8-byte header readable.
    let size = unsafe { ::core::ptr::read_unaligned(tag.wrapping_add(4).cast::<u32>()) };
    let len = size.min(READ_MAX) as usize; // at most 38
    if overlap(fb as usize, ::core::mem::size_of::<BootFb>(), tag as usize, len.max(8)) {
        return E_NONE.as_ptr();
    }
    // SAFETY: as above for `fb`; the tag does not overlap it (checked above).
    unsafe { ::core::ptr::write_bytes(fb, 0, 1) };
    // SAFETY: the tag is readable for `min(size, 38)` bytes by the caller's contract and does not overlap `fb`, so the write
    // below cannot alias this shared slice.
    let bytes = unsafe { ::core::slice::from_raw_parts(tag, len) };
    match validate(size, bytes) {
        Ok(f) => {
            // SAFETY: as for the zeroing store; each write stores one field.
            unsafe {
                ::core::ptr::addr_of_mut!((*fb).kind).write(f.kind);
                ::core::ptr::addr_of_mut!((*fb).addr).write(f.addr);
                ::core::ptr::addr_of_mut!((*fb).size).write(f.size);
                ::core::ptr::addr_of_mut!((*fb).pitch).write(f.pitch);
                ::core::ptr::addr_of_mut!((*fb).width).write(f.width);
                ::core::ptr::addr_of_mut!((*fb).height).write(f.height);
            }
            ::core::ptr::null()
        }
        Err(e) => e.as_ptr(),
    }
}
