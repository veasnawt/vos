//! B1: the boot reservation boundary is Rust. `resv_init`, `resv_add`, `resv_overlaps` (resv.h), `boot_mmap_entry` and
//! `boot_reserve` (bootinfo.h) and the C half of the EFI exclusion (`efi_memory_exclude`, `efi_memory_firmware_frame`,
//! `efi_memory_strerror`, `efi_memory_what`; efi_memory.h) keep their names, signatures and layouts; their callers (kernel.c,
//! pmm.c, xhci_qualify.c, the self-tests) are unchanged. The accepted C of the three files is kept only as frozen differential
//! references (tests/host/reference/) and is not linked into the kernel.
//!
//! This glue is the only unsafe code. It reads the caller's `struct boot_info`, the memory-map tag it points at (loader memory,
//! untrusted) and reads and writes the caller's `struct resv_table`, which stays in the caller's storage: nothing here
//! allocates or keeps state. The decisions (kernel/rust/boot/reserve.rs) are safe and pure; the EFI classification is
//! kernel/rust/boot/efi_memory.rs through the unchanged `vos_efi_exclusions_v1` / `vos_efi_firmware_frame_v1`.
#[path = "../../../kernel/rust/boot/reserve.rs"]
mod reserve;
use self::reserve::{Map, Modules, PhysRange, Sink, E_FULL, E_NO_MAP, MMAP_HEADER, RESV_BAD_RANGE, RESV_MAX, RESV_OK};
use crate::efi_exclusion::{vos_efi_exclusions_v1, vos_efi_firmware_frame_v1, EfiInput, EfiReport};
use crate::mb2_parse::{BootInfo, BootModule};
use core::ffi::c_void;
use core::mem::{align_of, offset_of, size_of};
use core::ptr::{addr_of, addr_of_mut};

/// `struct resv_table` of resv.h, in the caller's storage.
#[repr(C)]
pub struct ResvTable {
    r: [PhysRange; RESV_MAX],
    count: usize,
}

// The layout of `struct resv_table` and `struct phys_range` in resv.h, pinned. tests/host/boot-resv-differential.c asserts the
// same numbers against the C compiler's own `sizeof` and `offsetof`.
const _: () = {
    assert!(size_of::<PhysRange>() == 24 && align_of::<PhysRange>() == 8);
    assert!(offset_of!(PhysRange, start) == 0 && offset_of!(PhysRange, end) == 8 && offset_of!(PhysRange, what) == 16);
    assert!(size_of::<ResvTable>() == 1928 && align_of::<ResvTable>() == 8);
    assert!(offset_of!(ResvTable, r) == 0 && offset_of!(ResvTable, count) == 1920);
    assert!(RESV_MAX == 80);
    assert!(size_of::<BootInfo>() == 1624 && size_of::<BootModule>() == 24);
};

/// `BOOT_MAX_MODULES` of bootinfo.h.
const MAX_MODULES: usize = 64;
/// `PMM_LOW_LIMIT` and `PMM_LIMIT` of pmm.h: the tracked physical window handed to the EFI classification.
const WINDOW_LO: u64 = 0x10_0000;
const WINDOW_HI: u64 = 0x4000_0000;
/// The most bytes of a loader tag read here (the Multiboot2 block is at most 64 KiB; the EFI glue accepts tags up to 1 MiB).
const TAG_MAX: u32 = 1 << 20;

/// The one object whose address marks a reservation as entered by the EFI exclusion (`efi_memory.h`: checked by identity).
#[unsafe(no_mangle)]
pub static efi_memory_what: [u8; 22] = *b"firmware memory (EFI)\0";

fn overlap(a: usize, a_len: usize, b: usize, b_len: usize) -> bool {
    match (a.checked_add(a_len), b.checked_add(b_len)) {
        (Some(end_a), Some(end_b)) => a < end_b && b < end_a,
        _ => true,
    }
}

fn valid<T>(p: *const T) -> bool {
    !p.is_null() && (p as usize).is_multiple_of(align_of::<T>())
}

/// The used part of the table: `count` clamped to the capacity (the C trusts it; a larger value is a caller bug).
unsafe fn used(t: *const ResvTable) -> usize {
    // SAFETY: the caller made `t` non-null, aligned and readable; this is one field read.
    unsafe { addr_of!((*t).count).read() }.min(RESV_MAX)
}

/// `resv_add` on a validated table pointer.
unsafe fn table_add(t: *mut ResvTable, start: u64, end: u64, what: *const u8) -> i32 {
    // SAFETY: `t` is non-null, aligned and readable by the caller's contract.
    let count = unsafe { addr_of!((*t).count).read() };
    match reserve::plan_add(count, start, end) {
        Ok((s, e)) => {
            // SAFETY: `plan_add` says `count < RESV_MAX`, so slot `count` lies inside the table; the stores initialise exactly
            // that slot and then publish it by advancing `count`. Nothing is read from a slot that was never written.
            unsafe {
                let slot = addr_of_mut!((*t).r).cast::<PhysRange>().add(count);
                slot.write(PhysRange { start: s, end: e, what });
                addr_of_mut!((*t).count).write(count + 1);
            }
            RESV_OK
        }
        Err(code) => code,
    }
}

/// Reset `t` to an empty table.
///
/// # Safety
/// `t` is writable, aligned storage for one `struct resv_table`. A null or misaligned `t` (where the C would fault) is ignored.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn resv_init(t: *mut ResvTable) {
    if valid(t) {
        // SAFETY: non-null, aligned and writable by the caller's contract; only `count` is stored, as in the C.
        unsafe { addr_of_mut!((*t).count).write(0) };
    }
}

/// Reserve [start, end), rounded outward to whole pages. `RESV_OK`, `RESV_BAD_RANGE` (empty, reversed or overflowing when
/// rounded) or `RESV_FULL`; the table is unchanged unless the result is `RESV_OK`. `what` is only stored, never read.
///
/// # Safety
/// `t` is a `struct resv_table` that was initialised by `resv_init` or an earlier `resv_add` (or, at least, has a valid `count`).
/// A null or misaligned `t` (where the C would fault) is refused with `RESV_BAD_RANGE`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn resv_add(t: *mut ResvTable, start: u64, end: u64, what: *const u8) -> i32 {
    if !valid(t) {
        return RESV_BAD_RANGE;
    }
    // SAFETY: validated pointer, caller's table contract.
    unsafe { table_add(t, start, end, what) }
}

/// Nonzero if any reserved page intersects [start, end).
///
/// # Safety
/// `t` is a `struct resv_table` whose first `count` entries were written by `resv_add`. A null or misaligned `t` gives 0.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn resv_overlaps(t: *const ResvTable, start: u64, end: u64) -> i32 {
    if !valid(t) {
        return 0;
    }
    // SAFETY: validated pointer; the first `used(t)` entries are initialised by `resv_add`.
    let ranges = unsafe { core::slice::from_raw_parts(addr_of!((*t).r).cast::<PhysRange>(), used(t)) };
    i32::from(reserve::overlaps(ranges, start, end))
}

/// The table behind a `boot_reserve` or EFI call, as a [`Sink`].
struct TableSink(*mut ResvTable);

impl Sink for TableSink {
    fn add(&mut self, start: u64, end: u64, what: &'static [u8]) -> i32 {
        // SAFETY: only built from a validated table pointer that is exclusive for the call.
        unsafe { table_add(self.0, start, end, what.as_ptr()) }
    }
}

/// The modules recorded in a validated `struct boot_info`, read in place.
struct BiModules(*const BootInfo, usize);

impl Modules for BiModules {
    fn len(&self) -> usize {
        self.1
    }

    fn get(&self, i: usize) -> (u64, u64) {
        if i >= self.1 {
            return (0, 0);
        }
        // SAFETY: only built by `boot_reserve` from a non-null, aligned, readable `struct boot_info` that stays unchanged for the
        // call; `i < self.1 <= 64` indexes the `modules` array inside it, and each access is one field read.
        unsafe {
            let m = addr_of!((*self.0).modules).cast::<BootModule>().add(i);
            (addr_of!((*m).start).read(), addr_of!((*m).end).read())
        }
    }
}

/// The tag header's `size` field (offset 4), read without alignment.
unsafe fn tag_size(tag: *const u8) -> u32 {
    // SAFETY: the caller's contract makes the 8-byte tag header readable.
    unsafe { core::ptr::read_unaligned(tag.wrapping_add(4).cast::<u32>()) }
}

/// Entry `i` of the validated memory map, as the C computes it: the address, no bounds check (the caller keeps `i` below
/// `bi->mmap_entries`).
///
/// # Safety
/// `bi` is a readable `struct boot_info` whose `mmap` is the validated tag (readable for its 16-byte header) or null. A null or
/// misaligned `bi` and a null `mmap` (where the C would fault) give null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn boot_mmap_entry(bi: *const BootInfo, i: u32) -> *const u8 {
    if !valid(bi) {
        return core::ptr::null();
    }
    // SAFETY: `bi` is non-null, aligned and readable by the caller's contract.
    let mmap = unsafe { addr_of!((*bi).mmap).read() };
    if mmap.is_null() {
        return core::ptr::null();
    }
    // SAFETY: the tag header is readable by the caller's contract (`entry_size` is at offset 8).
    let entry_size = unsafe { core::ptr::read_unaligned(mmap.wrapping_add(8).cast::<u32>()) };
    mmap.wrapping_add(MMAP_HEADER).wrapping_add((i as usize).wrapping_mul(entry_size as usize))
}

/// Add the boot reservations to `t`. NULL on success, else a static error string; entries added before an error are kept.
///
/// # Safety
/// `bi` is a readable `struct boot_info` produced by `mb2_parse`: its `mmap` is null or the memory-map tag, readable for the
/// tag's own `size` (at most the 64 KiB info block) and not written during the call. `t` is an initialised, exclusive
/// `struct resv_table`. Defensive differences from the C, for caller bugs the C would fault or corrupt itself on (never a valid
/// call): a null or misaligned `bi` gives "no memory map", a null or misaligned `t`, a `t` overlapping `bi` or the tag, and a
/// tag too short for its own entries give "reservation table full" and "no memory map" respectively, without writing `t`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn boot_reserve(bi: *const BootInfo, kstart: u64, kend: u64, t: *mut ResvTable) -> *const u8 {
    if !valid(bi) {
        return E_NO_MAP.as_ptr();
    }
    // SAFETY: `bi` is non-null, aligned and readable by the caller's contract; these are field reads.
    let (mmap, entries, module_count, info) = unsafe {
        (addr_of!((*bi).mmap).read(), addr_of!((*bi).mmap_entries).read(), addr_of!((*bi).module_count).read(),
         (addr_of!((*bi).info_start).read(), addr_of!((*bi).info_end).read()))
    };
    if mmap.is_null() {
        return E_NO_MAP.as_ptr();
    }
    if !valid(t) || overlap(t as usize, size_of::<ResvTable>(), bi as usize, size_of::<BootInfo>()) {
        return E_FULL.as_ptr();
    }
    // SAFETY: the tag header is readable by the caller's contract.
    let (size, entry_size) = unsafe { (tag_size(mmap), core::ptr::read_unaligned(mmap.wrapping_add(8).cast::<u32>())) };
    let len = size.min(0x1_0000) as usize; // the info block is bounded by MB2_INFO_MAX
    if overlap(t as usize, size_of::<ResvTable>(), mmap as usize, len) {
        return E_FULL.as_ptr();
    }
    // SAFETY: the tag is readable for `size` bytes by the caller's contract and does not overlap the table (checked above), so
    // the stores through `t` below cannot alias this shared slice.
    let tag = unsafe { core::slice::from_raw_parts(mmap, len) };
    let Some(map) = Map::new(tag, entry_size as usize, entries) else {
        return E_NO_MAP.as_ptr();
    };
    match reserve::reserve(&map, (kstart, kend), info, &BiModules(bi, (module_count as usize).min(MAX_MODULES)), &mut TableSink(t)) {
        Ok(()) => core::ptr::null(),
        Err(e) => e.as_ptr(),
    }
}

/// `struct efi_memory_input` for this boot, as `efi_memory.c` built it.
///
/// # Safety
/// `bi` is a readable `struct boot_info`, `t` a readable `struct resv_table`, and each non-null tag pointer in `bi` has a
/// readable 8-byte header.
unsafe fn efi_input(bi: *const BootInfo, t: *const ResvTable) -> EfiInput {
    // SAFETY: field reads of the caller's structures, and the tag headers by the caller's contract.
    unsafe {
        let (mmap, efi_mmap) = (addr_of!((*bi).mmap).read(), addr_of!((*bi).efi_mmap).read());
        let size = |tag: *const u8| if tag.is_null() { 0 } else { tag_size(tag) };
        let count = addr_of!((*t).count).read();
        EfiInput {
            mmap,
            efi_mmap,
            window_lo: WINDOW_LO,
            window_hi: WINDOW_HI,
            mmap_size: size(mmap),
            efi_mmap_size: size(efi_mmap),
            efi_mmap_tags: addr_of!((*bi).efi_mmap_tags).read(),
            efi_firmware: u32::from(addr_of!((*bi).efi).read() != 0),
            boot_services_active: u32::from(addr_of!((*bi).efi_bs_active).read() != 0),
            capacity: if count < RESV_MAX { (RESV_MAX - count) as u32 } else { 0 },
        }
    }
}

/// The sink of the EFI exclusion: enter [start, end) into the table as firmware memory. Nonzero when the table refuses.
unsafe extern "C" fn efi_sink(ctx: *mut c_void, start: u64, end: u64) -> i32 {
    // SAFETY: `ctx` is the validated table pointer that `efi_memory_exclude` passed; the call is synchronous.
    i32::from(unsafe { table_add(ctx.cast::<ResvTable>(), start, end, efi_memory_what.as_ptr()) } != RESV_OK)
}

/// Whether the tags `bi` points at lie clear of the table (a table on top of a tag would be rewritten while it is read).
unsafe fn tags_clear_of(bi: *const BootInfo, t: *const ResvTable) -> bool {
    // SAFETY: field reads of the caller's structure; a tag header is readable by the caller's contract.
    let tags = unsafe { [addr_of!((*bi).mmap).read(), addr_of!((*bi).efi_mmap).read()] };
    tags.iter().all(|&p| {
        // SAFETY: as above, for non-null tags only.
        let len = if p.is_null() { 0 } else { unsafe { tag_size(p) }.min(TAG_MAX) as usize };
        !overlap(t as usize, size_of::<ResvTable>(), p as usize, len)
    })
}

/// Classify the EFI memory map against the loader's e820-style map and enter the ranges that must never be allocatable into
/// `t`. Returns `report->status`: `EFI_MEMORY_APPLIED` (0), `EFI_MEMORY_NOT_EFI` (1, legacy BIOS) or `EFI_MEMORY_REFUSED` (-1).
///
/// # Safety
/// As `boot_reserve` for `bi` and `t`, and `report` is writable, aligned storage for one `struct efi_memory_report`; each tag in
/// `bi` is readable for its own `size` and not written during the call. A null or misaligned `bi` or `t`, or a table on top
/// of `bi` or a tag (where the C would fault or rewrite its own input), is reported as a refusal without entering anything.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn efi_memory_exclude(bi: *const BootInfo, t: *mut ResvTable, report: *mut EfiReport) -> i32 {
    // SAFETY (all calls below): `report` is passed through to the unchanged exclusion entry point, which checks it and writes
    // it whole before reading any field; `input` is a local, valid for the call.
    let clear = valid(bi) && valid(t) && !overlap(t as usize, size_of::<ResvTable>(), bi as usize, size_of::<BootInfo>())
        && unsafe { tags_clear_of(bi, t) };
    if !clear {
        // the unchanged entry point answers a null input with a refusal (status -1, or -2 for a bad `report`)
        return unsafe { vos_efi_exclusions_v1(core::ptr::null(), None, core::ptr::null_mut(), report) };
    }
    // SAFETY: `bi` and `t` were validated above; tags readable by the caller's contract.
    let input = unsafe { efi_input(bi, t) };
    unsafe { vos_efi_exclusions_v1(&input, Some(efi_sink), t.cast::<c_void>(), report) }
}

/// 1 if `frame` lies in an exclusion this module entered into `t` and the EFI map confirms the whole frame is excluded RAM;
/// 0 otherwise (including BIOS and any invalid input).
///
/// # Safety
/// As `efi_memory_exclude` for `bi` and `t`, both read only. A null or misaligned `bi` or `t` gives 0, as a table with no
/// entries does.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn efi_memory_firmware_frame(bi: *const BootInfo, t: *const ResvTable, frame: u64) -> i32 {
    if !valid(bi) || !valid(t) {
        return 0;
    }
    // SAFETY: validated pointer; the first `used(t)` entries are initialised by `resv_add`.
    let ranges = unsafe { core::slice::from_raw_parts(addr_of!((*t).r).cast::<PhysRange>(), used(t)) };
    let mark = efi_memory_what.as_ptr();
    let listed = ranges.iter().fold(false, |listed, r| {
        listed | (r.what == mark && r.start <= frame && frame.wrapping_add(reserve::PAGE) <= r.end)
    });
    if !listed {
        return 0;
    }
    // SAFETY: `bi` and `t` validated above; tags readable by the caller's contract.
    let input = unsafe { efi_input(bi, t) };
    i32::from(unsafe { vos_efi_firmware_frame_v1(&input, frame) } == 1)
}

/// The text of an EFI exclusion error code (`efi_memory_report.error`); "unknown" for anything out of range.
#[unsafe(no_mangle)]
pub extern "C" fn efi_memory_strerror(error: u32) -> *const u8 {
    const WHY: [&[u8]; 14] = [
        b"unknown\0",
        b"UEFI without an EFI memory map\0",
        b"EFI boot services still active\0",
        b"duplicate EFI memory map\0",
        b"EFI map truncated\0",
        b"bad EFI descriptor size\0",
        b"bad EFI descriptor version\0",
        b"bad EFI descriptor count\0",
        b"unaligned EFI descriptor\0",
        b"EFI descriptor overflows\0",
        b"bad memory map tag\0",
        b"bad window\0",
        b"no reservation capacity\0",
        b"reservation refused\0",
    ];
    WHY.get(error as usize).unwrap_or(&WHY[0]).as_ptr()
}
