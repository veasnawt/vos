//! Existing bootinfo.h ABI: `mb2_parse` is now Rust. The callers (kernel.c, the self-tests), `struct boot_info` and every
//! constant are unchanged. The accepted C parser is kept only as the frozen differential reference
//! (tests/host/reference/bootinfo_ref.c) and is not linked into the kernel. `boot_framebuffer`, `boot_mmap_entry` and
//! `boot_reserve` stay C in bootinfo.c.
//!
//! This glue is the only unsafe code: it reads the loader's block, which is untrusted memory, and writes the caller's
//! `struct boot_info`. The walk itself (kernel/rust/mb2/parse.rs) is safe and pure.
#[path = "../../../kernel/rust/mb2/parse.rs"]
mod parse;
use self::parse::{Out, BOOT_MAX_MODULES, E_ADDRESS, E_SIZE};

#[repr(C)]
pub struct BootModule {
    start: u64,
    end: u64,
    cmdline: *const u8,
}

#[repr(C)]
pub struct BootInfo {
    info_start: u64,
    info_end: u64,
    loader: *const u8,
    cmdline: *const u8,
    mmap: *const u8,
    mmap_entries: u32,
    modules: [BootModule; BOOT_MAX_MODULES as usize],
    module_count: u32,
    fb: *const u8,
    fb_tags: u32,
    efi: i32,
    efi_mmap: *const u8,
    efi_mmap_tags: u32,
    efi_bs_active: i32,
}

// The layout of `struct boot_info` in bootinfo.h, pinned. tests/host/mb2-differential.c asserts the same numbers against
// the C compiler's own `sizeof` and `offsetof`.
const _: () = {
    assert!(::core::mem::size_of::<BootModule>() == 24);
    assert!(::core::mem::size_of::<BootInfo>() == 1624);
    assert!(::core::mem::align_of::<BootInfo>() == 8);
    assert!(::core::mem::offset_of!(BootInfo, info_start) == 0);
    assert!(::core::mem::offset_of!(BootInfo, info_end) == 8);
    assert!(::core::mem::offset_of!(BootInfo, loader) == 16);
    assert!(::core::mem::offset_of!(BootInfo, cmdline) == 24);
    assert!(::core::mem::offset_of!(BootInfo, mmap) == 32);
    assert!(::core::mem::offset_of!(BootInfo, mmap_entries) == 40);
    assert!(::core::mem::offset_of!(BootInfo, modules) == 48);
    assert!(::core::mem::offset_of!(BootInfo, module_count) == 1584);
    assert!(::core::mem::offset_of!(BootInfo, fb) == 1592);
    assert!(::core::mem::offset_of!(BootInfo, fb_tags) == 1600);
    assert!(::core::mem::offset_of!(BootInfo, efi) == 1604);
    assert!(::core::mem::offset_of!(BootInfo, efi_mmap) == 1608);
    assert!(::core::mem::offset_of!(BootInfo, efi_mmap_tags) == 1616);
    assert!(::core::mem::offset_of!(BootInfo, efi_bs_active) == 1620);
    assert!(BOOT_MAX_MODULES == 64);
};

/// Writes the walk's results into the caller's structure, field by field, in the order the C wrote them.
struct Fill {
    bi: *mut BootInfo,
    base: *const u8,
}

impl Fill {
    /// A pointer into the loader's block. No dereference happens here, so no unsafe is needed.
    fn at(&self, offset: u32) -> *const u8 {
        self.base.wrapping_add(offset as usize)
    }
}

// SAFETY (all the writes below): `Fill` is only built by `mb2_parse`, after it has checked that `bi` is non-null and aligned
// for `BootInfo`; the caller's contract makes it writable storage for one `struct boot_info`. Each write stores one field.
impl Out for Fill {
    fn info(&mut self, start: u64, end: u64) {
        unsafe {
            ::core::ptr::addr_of_mut!((*self.bi).info_start).write(start);
            ::core::ptr::addr_of_mut!((*self.bi).info_end).write(end);
        }
    }
    fn loader(&mut self, offset: u32) {
        let p = self.at(offset);
        unsafe { ::core::ptr::addr_of_mut!((*self.bi).loader).write(p) }
    }
    fn cmdline(&mut self, offset: u32) {
        let p = self.at(offset);
        unsafe { ::core::ptr::addr_of_mut!((*self.bi).cmdline).write(p) }
    }
    fn module(&mut self, index: u32, start: u64, end: u64, cmdline: u32) {
        let p = self.at(cmdline);
        // `index` < BOOT_MAX_MODULES is the walk's own check (the C tests `module_count >= BOOT_MAX_MODULES` first).
        let slot = unsafe { ::core::ptr::addr_of_mut!((*self.bi).modules).cast::<BootModule>().add(index as usize) };
        unsafe {
            ::core::ptr::addr_of_mut!((*slot).start).write(start);
            ::core::ptr::addr_of_mut!((*slot).end).write(end);
            ::core::ptr::addr_of_mut!((*slot).cmdline).write(p);
        }
    }
    fn module_count(&mut self, count: u32) {
        unsafe { ::core::ptr::addr_of_mut!((*self.bi).module_count).write(count) }
    }
    fn fb(&mut self, tag: u32) {
        let p = self.at(tag);
        unsafe { ::core::ptr::addr_of_mut!((*self.bi).fb).write(p) }
    }
    fn fb_tags(&mut self, count: u32) {
        unsafe { ::core::ptr::addr_of_mut!((*self.bi).fb_tags).write(count) }
    }
    fn efi(&mut self) {
        unsafe { ::core::ptr::addr_of_mut!((*self.bi).efi).write(1) }
    }
    fn efi_mmap(&mut self, tag: u32) {
        let p = self.at(tag);
        unsafe { ::core::ptr::addr_of_mut!((*self.bi).efi_mmap).write(p) }
    }
    fn efi_mmap_tags(&mut self, count: u32) {
        unsafe { ::core::ptr::addr_of_mut!((*self.bi).efi_mmap_tags).write(count) }
    }
    fn efi_bs_active(&mut self) {
        unsafe { ::core::ptr::addr_of_mut!((*self.bi).efi_bs_active).write(1) }
    }
    fn mmap(&mut self, tag: u32, entries: u32) {
        let p = self.at(tag);
        unsafe {
            ::core::ptr::addr_of_mut!((*self.bi).mmap).write(p);
            ::core::ptr::addr_of_mut!((*self.bi).mmap_entries).write(entries);
        }
    }
}

fn overlap(a: usize, a_len: usize, b: usize, b_len: usize) -> bool {
    match (a.checked_add(a_len), b.checked_add(b_len)) {
        (Some(end_a), Some(end_b)) => a < end_b && b < end_a,
        _ => true,
    }
}

/// Parse the Multiboot2 information block read through `info`, which lives at physical address `info_phys`. Returns NULL
/// on success, else a static error string. As in the C, `bi` is zeroed first and keeps whatever the walk had stored when it
/// stopped; `info_phys` is never dereferenced.
///
/// # Safety
/// `bi` is writable, aligned storage for one `struct boot_info`. When `info_phys` passes the address checks, `info` is
/// readable for the block's `total_size` bytes (at most 0x10000) and nobody writes them during this synchronous call.
///
/// Defensive differences from the C, for caller bugs the C would fault on or corrupt itself on (never a valid call), all
/// reported with the existing "info block address invalid" string: a null or misaligned `bi` is refused without writing; a
/// block that overlaps `bi` is refused without writing (the C would zero the block it is about to parse; Rust must not hold a
/// shared slice over memory it writes); a null `info` is refused after `bi` has been zeroed, as an invalid address is.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mb2_parse(info: *const u8, info_phys: u64, bi: *mut BootInfo) -> *const u8 {
    if bi.is_null() || !(bi as usize).is_multiple_of(::core::mem::align_of::<BootInfo>()) {
        return E_ADDRESS.as_ptr();
    }
    let addr = parse::check_address(info_phys);
    if addr.is_err() || info.is_null() {
        // SAFETY: `bi` is non-null, aligned and writable by the caller's contract.
        unsafe { ::core::ptr::write_bytes(bi, 0, 1) };
        return E_ADDRESS.as_ptr();
    }
    // SAFETY: `info` is non-null and, `info_phys` being valid, readable for at least the 8-byte header; the read has no
    // alignment requirement.
    let total = unsafe { ::core::ptr::read_unaligned(info.cast::<u32>()) };
    let size = parse::check_total(total, info_phys);
    if size.is_ok() && overlap(info as usize, total as usize, bi as usize, ::core::mem::size_of::<BootInfo>()) {
        return E_ADDRESS.as_ptr();
    }
    // SAFETY: as above for `bi`; the block does not overlap it unless the size was invalid, and then nothing reads the block.
    unsafe { ::core::ptr::write_bytes(bi, 0, 1) };
    if size.is_err() {
        return E_SIZE.as_ptr();
    }
    let mut out = Fill { bi, base: info };
    out.info(info_phys, info_phys + u64::from(total));
    // SAFETY: `info` is readable for `total` (16 ..= 0x10000) bytes by the caller's contract and does not overlap `bi`
    // (checked above), so no write below can alias this shared slice.
    let block = unsafe { ::core::slice::from_raw_parts(info, total as usize) };
    match parse::walk(block, &mut out) {
        Ok(()) => ::core::ptr::null(),
        Err(e) => e.as_ptr(),
    }
}
