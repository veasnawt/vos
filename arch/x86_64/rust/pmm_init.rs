//! R4-M3: `pmm_init_in` is Rust (kernel/rust/mm/pmm_init.rs) behind its unchanged pmm.h C ABI. pmm.c keeps the kernel
//! instance and its bitmap storage, `pmm_init` and the other wrappers (zeroing, poisoning, panic policy) and physical
//! memory. The accepted C is frozen as tests/reference/pmm-r4m3.c.
//!
//! Memory model. This adapter never creates a Rust reference (`&`/`&mut`, slices, `Cell`) to the instance, the bitmaps, the
//! boot information, the memory map or the reservation table: every access is one raw-pointer read or write, issued when
//! the core asks, which is where and in the order the C accesses memory. Raw pointers carry no aliasing guarantees, so any
//! overlap between those objects that is defined in the C has the C's result.
//!
//! Each `unsafe` block relies on the contract the C relies on: `p` is a writable `struct pmm`; `avail` and `alloc` each
//! point to `frames / 64` writable words; `bi`, when not NULL, is a readable `struct boot_info` whose `mmap`, when not
//! NULL, is a readable memory-map tag with `mmap_entries` entries of `entry_size` bytes; `t`, when not NULL, is a readable
//! `struct resv_table`. The kernel is single-CPU and initializes the PMM once, at boot, with interrupts disabled.
//!
//! It checks more than the C does, and only where the C would be undefined: a NULL or misaligned `p`, `avail` or `alloc`
//! is refused before any access through it; a bitmap word is touched only below `frames / 64` for the `frames` argument;
//! the loader-supplied tag, entries and table are read without an alignment assumption; a reservation index at or past
//! RESV_MAX is not read.
#[path = "../../../kernel/rust/mm/pmm_init.rs"]
mod init_core;
use self::init_core as c;
use core::ffi::c_void;

// The layouts this file reads and writes, as byte offsets (pmm.h, bootinfo.h, multiboot2.h, resv.h).
// tests/host/pmm-init-differential.c asserts every one of them against the C compiler's `offsetof` and `sizeof`.
const PMM_SIZE: usize = 64;
const PMM_AVAIL: usize = 0;
const PMM_ALLOC: usize = 8;
const PMM_FRAMES: usize = 16;
const PMM_USABLE: usize = 24;
const PMM_FREE: usize = 32;
const PMM_HINT: usize = 40;
const PMM_IGNORED: usize = 48;
const PMM_STATE: usize = 56;
const BI_MMAP: usize = 32;
const BI_MMAP_ENTRIES: usize = 40;
const TAG_ENTRY_SIZE: usize = 8;
const TAG_ENTRY_VERSION: usize = 12;
const ENTRY_BASE: usize = 0;
const ENTRY_LENGTH: usize = 8;
const ENTRY_TYPE: usize = 16;
const RESV_MAX: u64 = 80;
const RANGE_SIZE: usize = 24;
const RANGE_END: usize = 8;
const RESV_COUNT: usize = 1920;
const _: () = assert!(size_of::<usize>() == 8 && size_of::<*const u8>() == 8);

unsafe extern "C" {
    /// bootinfo.h: entry `i` of the validated memory map (arch/x86_64/rust/boot_resv.rs in the kernel); NULL if it has none.
    fn boot_mmap_entry(bi: *const c_void, i: u32) -> *const u8;
}

/// One memory-map entry: the pointer `boot_mmap_entry` returned, never dereferenced as a reference.
#[derive(Clone, Copy)]
pub struct EntryPtr(*const u8);

/// The caller's arguments, as raw pointers.
struct Raw {
    p: *mut u8,
    avail: *mut u64,
    alloc: *mut u64,
    frames: u64,
    bi: *const u8,
    t: *const u8,
}

impl Raw {
    /// Field at byte offset `at` of the instance.
    fn field<T>(&self, at: usize) -> *mut T {
        self.p.wrapping_add(at).cast()
    }
    /// Word `w` of `bitmap`, if it is one of the caller's `frames / 64` words.
    fn word(&self, bitmap: *mut u64, w: u64) -> Option<*mut u64> {
        if bitmap.is_null() || !bitmap.is_aligned() || w >= self.frames / 64 {
            return None;
        }
        Some(bitmap.wrapping_add(w as usize))
    }
    /// `bi->mmap`.
    fn mmap(&self) -> *const u8 {
        // SAFETY: `bi` is a readable `struct boot_info` (contract; the core asks only when `bi` is not NULL); one read.
        unsafe { self.bi.wrapping_add(BI_MMAP).cast::<*const u8>().read_unaligned() }
    }
}

impl c::Mem for Raw {
    type Entry = EntryPtr;
    fn zero_instance(&self) {
        let mut at = 0;
        while at < PMM_SIZE {
            // SAFETY: `p` is a writable, 8-byte aligned `struct pmm` of 64 bytes (contract; checked non-NULL and aligned).
            unsafe { self.field::<u64>(at).write(0) };
            at += 8;
        }
    }
    fn set_state(&self, v: i32) {
        // SAFETY: one write of a field of the writable instance (contract).
        unsafe { self.field::<i32>(PMM_STATE).write(v) }
    }
    fn set_bitmaps(&self) {
        // SAFETY: two writes of pointer fields of the writable instance (contract), in the C's order.
        unsafe {
            self.field::<*mut u64>(PMM_AVAIL).write(self.avail);
            self.field::<*mut u64>(PMM_ALLOC).write(self.alloc);
        }
    }
    fn set_frames(&self, frames: u64) {
        // SAFETY: as in `set_state`.
        unsafe { self.field::<u64>(PMM_FRAMES).write(frames) }
    }
    fn instance_avail_is_bitmap(&self) -> bool {
        // SAFETY: one read of a field of the instance (contract).
        unsafe { self.field::<*mut u64>(PMM_AVAIL).read() == self.avail }
    }
    fn ignored(&self) -> u64 {
        // SAFETY: as in `instance_avail_is_bitmap`.
        unsafe { self.field::<u64>(PMM_IGNORED).read() }
    }
    fn set_ignored(&self, v: u64) {
        // SAFETY: as in `set_state`.
        unsafe { self.field::<u64>(PMM_IGNORED).write(v) }
    }
    fn usable(&self) -> u64 {
        // SAFETY: as in `instance_avail_is_bitmap`.
        unsafe { self.field::<u64>(PMM_USABLE).read() }
    }
    fn set_usable(&self, v: u64) {
        // SAFETY: as in `set_state`.
        unsafe { self.field::<u64>(PMM_USABLE).write(v) }
    }
    fn set_free(&self, v: u64) {
        // SAFETY: as in `set_state`.
        unsafe { self.field::<u64>(PMM_FREE).write(v) }
    }
    fn set_hint(&self, v: u64) {
        // SAFETY: as in `set_state`.
        unsafe { self.field::<u64>(PMM_HINT).write(v) }
    }
    fn avail(&self, w: u64) -> Option<u64> {
        let word = self.word(self.avail, w)?;
        // SAFETY: word `w` < frames / 64 of the caller's non-NULL, aligned `avail` bitmap (contract); one read. The core
        // reads a word only after zeroing all of them, so it is initialized.
        Some(unsafe { word.read() })
    }
    fn set_avail(&self, w: u64, v: u64) -> Option<()> {
        let word = self.word(self.avail, w)?;
        // SAFETY: as in `avail`, for one write; the bitmap is writable (contract).
        unsafe { word.write(v) };
        Some(())
    }
    fn set_alloc(&self, w: u64, v: u64) -> Option<()> {
        let word = self.word(self.alloc, w)?;
        // SAFETY: word `w` < frames / 64 of the caller's non-NULL, aligned, writable `alloc` bitmap (contract); one write.
        unsafe { word.write(v) };
        Some(())
    }
    fn mmap_present(&self) -> bool {
        !self.mmap().is_null()
    }
    fn mmap_entries(&self) -> u32 {
        // SAFETY: as in `mmap`.
        unsafe { self.bi.wrapping_add(BI_MMAP_ENTRIES).cast::<u32>().read_unaligned() }
    }
    fn entry_version(&self) -> Option<u32> {
        let tag = self.mmap();
        if tag.is_null() {
            return None;
        }
        // SAFETY: a non-NULL `bi->mmap` is a readable tag header of 16 bytes (contract); one read, any alignment.
        Some(unsafe { tag.wrapping_add(TAG_ENTRY_VERSION).cast::<u32>().read_unaligned() })
    }
    fn entry_size(&self) -> Option<u32> {
        let tag = self.mmap();
        if tag.is_null() {
            return None;
        }
        // SAFETY: as in `entry_version`.
        Some(unsafe { tag.wrapping_add(TAG_ENTRY_SIZE).cast::<u32>().read_unaligned() })
    }
    fn entry(&self, i: u32) -> Option<EntryPtr> {
        // SAFETY: bootinfo.h's function, with the caller's `bi` (not NULL here) and an index below `bi->mmap_entries`.
        let e = unsafe { boot_mmap_entry(self.bi.cast(), i) };
        if e.is_null() { None } else { Some(EntryPtr(e)) }
    }
    fn base(&self, e: EntryPtr) -> u64 {
        // SAFETY: entry `i` < `bi->mmap_entries` of the map is readable for 24 bytes (contract); one read, any alignment.
        unsafe { e.0.wrapping_add(ENTRY_BASE).cast::<u64>().read_unaligned() }
    }
    fn length(&self, e: EntryPtr) -> u64 {
        // SAFETY: as in `base`.
        unsafe { e.0.wrapping_add(ENTRY_LENGTH).cast::<u64>().read_unaligned() }
    }
    fn kind(&self, e: EntryPtr) -> u32 {
        // SAFETY: as in `base`.
        unsafe { e.0.wrapping_add(ENTRY_TYPE).cast::<u32>().read_unaligned() }
    }
    fn resv_count(&self) -> Option<u64> {
        if self.t.is_null() {
            return None;
        }
        // SAFETY: a non-NULL `t` is a readable `struct resv_table` (contract); one read, any alignment.
        Some(unsafe { self.t.wrapping_add(RESV_COUNT).cast::<u64>().read_unaligned() })
    }
    fn resv(&self, i: u64) -> Option<(u64, u64)> {
        if self.t.is_null() || i >= RESV_MAX {
            return None;
        }
        let r = self.t.wrapping_add(i as usize * RANGE_SIZE);
        // SAFETY: range `i` < RESV_MAX lies inside the readable table (contract); two reads, any alignment.
        Some(unsafe { (r.cast::<u64>().read_unaligned(), r.wrapping_add(RANGE_END).cast::<u64>().read_unaligned()) })
    }
}

/// `int pmm_init_in(struct pmm *p, uint64_t *avail, uint64_t *alloc, uint64_t frames, const struct boot_info *bi,
/// const struct resv_table *t)`.
///
/// # Safety
/// The pmm.h contract, as in the module documentation. A NULL or misaligned `p` is refused (`PMM_EINVAL`) untouched.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pmm_init_in(p: *mut c_void, avail: *mut u64, alloc: *mut u64, frames: u64, bi: *const c_void, t: *const c_void) -> i32 {
    let p = p.cast::<u8>();
    if p.is_null() || !p.cast::<u64>().is_aligned() {
        return c::EINVAL;
    }
    let unusable = avail.is_null() || alloc.is_null() || !avail.is_aligned() || !alloc.is_aligned();
    c::init(&Raw { p, avail, alloc, frames, bi: bi.cast(), t: t.cast() }, unusable, frames, bi.is_null())
}
