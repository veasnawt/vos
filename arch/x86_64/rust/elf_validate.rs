//! Existing elf.h ABI: `elf_validate` and `elf_strerror` are now Rust. The C callers (loader.c, app_catalog.c), the structures
//! and the constants are unchanged; arch/x86_64/elf.c stays byte-identical only as the frozen differential reference and is not
//! linked into the kernel.
#[path = "../../../kernel/rust/elf/validate.rs"]
mod validate;
use self::validate::{Image, Segment, ELF_ESIZE, ELF_MAX_FILE, ELF_MAX_LOADS};

const _: () = {
    assert!(::core::mem::size_of::<Segment>() == 40);
    assert!(::core::mem::align_of::<Segment>() == 8);
    assert!(::core::mem::offset_of!(Segment, vaddr) == 0);
    assert!(::core::mem::offset_of!(Segment, memsz) == 8);
    assert!(::core::mem::offset_of!(Segment, offset) == 16);
    assert!(::core::mem::offset_of!(Segment, filesz) == 24);
    assert!(::core::mem::offset_of!(Segment, flags) == 32);
    assert!(::core::mem::size_of::<Image>() == 192);
    assert!(::core::mem::align_of::<Image>() == 8);
    assert!(::core::mem::offset_of!(Image, entry) == 0);
    assert!(::core::mem::offset_of!(Image, nload) == 8);
    assert!(::core::mem::offset_of!(Image, load) == 16);
    assert!(::core::mem::offset_of!(Image, pages) == 176);
    assert!(::core::mem::offset_of!(Image, abi) == 184);
    assert!(ELF_MAX_LOADS == 4);
};

fn overlap(a: usize, a_len: usize, b: usize, b_len: usize) -> bool {
    match (a.checked_add(a_len), b.checked_add(b_len)) {
        (Some(end_a), Some(end_b)) => a < end_b && b < end_a,
        _ => true,
    }
}

/// Write every field of `image` (never the padding, which C leaves unspecified) into the caller's `struct elf_image`.
///
/// # Safety
/// `out` points to writable, aligned storage for one `struct elf_image`.
unsafe fn store(out: *mut Image, image: &Image) {
    // SAFETY: per the contract above; each write targets one field of the C structure.
    unsafe {
        ::core::ptr::addr_of_mut!((*out).entry).write(image.entry);
        ::core::ptr::addr_of_mut!((*out).nload).write(image.nload);
        for (i, s) in image.load.iter().enumerate() {
            let slot = ::core::ptr::addr_of_mut!((*out).load).cast::<Segment>().add(i);
            ::core::ptr::addr_of_mut!((*slot).vaddr).write(s.vaddr);
            ::core::ptr::addr_of_mut!((*slot).memsz).write(s.memsz);
            ::core::ptr::addr_of_mut!((*slot).offset).write(s.offset);
            ::core::ptr::addr_of_mut!((*slot).filesz).write(s.filesz);
            ::core::ptr::addr_of_mut!((*slot).flags).write(s.flags);
        }
        ::core::ptr::addr_of_mut!((*out).pages).write(image.pages);
        ::core::ptr::addr_of_mut!((*out).abi).write(image.abi);
    }
}

/// # Safety
/// `img` points to `size` readable bytes (it is not read at all unless 64 <= size <= 1 MiB) and `out` to writable, aligned
/// storage for one `struct elf_image` that does not overlap them. The two checks below (null, alignment, overlap) cannot
/// establish that arbitrary pointers are valid; they turn the commonest caller bugs into a refusal instead of a fault. Like
/// the C function, the image is left all zero on every failure.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn elf_validate(img: *const u8, size: u64, out: *mut Image) -> i32 {
    if out.is_null() || !(out as usize).is_multiple_of(8) {
        return ELF_ESIZE;
    }
    // SAFETY: `out` is non-null, aligned and writable by the caller's contract.
    unsafe { store(out, &Image::default()) };
    if !(64..=ELF_MAX_FILE).contains(&size) {
        return ELF_ESIZE;
    }
    let len = size as usize; // at most 1 MiB
    if img.is_null() || overlap(img as usize, len, out as usize, ::core::mem::size_of::<Image>()) {
        return ELF_ESIZE;
    }
    // SAFETY: `img` is non-null and, by the caller's contract, readable for `len` bytes that no one writes during this
    // synchronous call; `out` does not overlap it (checked above).
    let bytes = unsafe { ::core::slice::from_raw_parts(img, len) };
    let (code, image) = validate::validate(bytes);
    if code == validate::ELF_OK {
        // SAFETY: as for the zeroing store above.
        unsafe { store(out, &image) };
    }
    code
}

/// # Safety
/// No pointer is read. The result points to static, NUL-terminated kernel read-only data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn elf_strerror(err: i32) -> *const u8 {
    validate::strerror(err).as_ptr()
}
