//! B1: the boot-time physical reservation logic of `arch/x86_64/bootinfo.c` (`boot_reserve`) and `arch/x86_64/resv.c`, ported
//! without any change of behavior. Pure: no unsafe code, no allocation, no panic path, no I/O. The checks run in the C's order,
//! because the order decides which message wins and which entries a failing call has already added to the table (entries added
//! before an error are kept, and the kernel prints them before it panics).
//!
//! The table itself lives in the C caller's storage (`struct resv_table`); this code only decides what to add and reaches the
//! table through [`Sink`], so the unsafe access stays in `arch/x86_64/rust/boot_resv.rs`.

pub const PAGE: u64 = 0x1000;
/// `BOOT_MAP_LIMIT` of memlayout.h: the boot page tables map physical [0, 1 GiB).
pub const BOOT_MAP_LIMIT: u64 = 0x4000_0000;
/// `RESV_MAX` of resv.h.
pub const RESV_MAX: usize = 80;
pub const RESV_OK: i32 = 0;
pub const RESV_BAD_RANGE: i32 = 1;
pub const RESV_FULL: i32 = 2;

const LOW_MEM_END: u64 = 0x10_0000; // physical 0-1 MiB is always reserved
const PAGE0_END: u64 = 0x1000; // physical page 0: never a loader object
const LEGACY_HOLE: u64 = 0xA_0000; // [0xA0000, 1 MiB): video memory and ROMs, never RAM
const MB2_MMAP_AVAILABLE: u32 = 1;
/// `sizeof(struct mb2_tag_mmap)` and `sizeof(struct mb2_mmap_entry)`.
pub const MMAP_HEADER: usize = 16;
const ENTRY_MIN: usize = 24;

/// The error strings, byte for byte as in C, each NUL-terminated so a pointer to the first byte is a C string.
pub const E_NO_MAP: &[u8] = b"no memory map\0";
pub const E_FULL: &[u8] = b"reservation table full\0";
const E_KERNEL_BELOW: &[u8] = b"kernel image below 1 MiB\0";
const E_KERNEL_RAM: &[u8] = b"kernel image not in usable RAM\0";
const E_INFO_PAGE0: &[u8] = b"multiboot2 info in physical page 0\0";
const E_INFO_HOLE: &[u8] = b"multiboot2 info in the legacy hole (0xA0000-1 MiB)\0";
const E_INFO_KERNEL: &[u8] = b"multiboot2 info shares a page with the kernel image\0";
const E_INFO_RAM: &[u8] = b"multiboot2 info not in usable RAM\0";
const E_MOD_INFO: &[u8] = b"module overlaps the multiboot2 info\0";
const E_MODS: &[u8] = b"modules overlap\0";
const E_MOD_PAGE0: &[u8] = b"module in physical page 0\0";
const E_MOD_HOLE: &[u8] = b"module in the legacy hole (0xA0000-1 MiB)\0";
const E_MOD_KERNEL: &[u8] = b"module overlaps the kernel image\0";
const E_MOD_RAM: &[u8] = b"module not in usable RAM\0";

/// What each reservation is for (`what` of `struct phys_range`; the kernel prints it, so each is a NUL-terminated string).
const WHAT_LOW: &[u8] = b"low 1 MiB\0";
const WHAT_KERNEL: &[u8] = b"kernel image\0";
const WHAT_INFO: &[u8] = b"multiboot2 info\0";
const WHAT_MODULE: &[u8] = b"module\0";

/// `struct phys_range` of resv.h: a half-open, page-aligned range and the (opaque here) pointer that says why it is reserved.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct PhysRange {
    pub start: u64,
    pub end: u64,
    pub what: *const u8,
}

/// `resv_add`'s decision: the page-rounded range and where it goes, or the error code. The range is checked before the table is
/// looked at, so a bad range wins over a full table, as in the C.
pub fn plan_add(count: usize, start: u64, end: u64) -> Result<(u64, u64), i32> {
    if start >= end || end > u64::MAX - (PAGE - 1) {
        return Err(RESV_BAD_RANGE);
    }
    if count >= RESV_MAX {
        return Err(RESV_FULL);
    }
    Ok((start & !(PAGE - 1), (end + (PAGE - 1)) & !(PAGE - 1))) // end <= u64::MAX - (PAGE - 1): no overflow
}

/// `resv_overlaps`: some reserved page intersects [start, end). `ranges` is the used part of the table.
pub fn overlaps(ranges: &[PhysRange], start: u64, end: u64) -> bool {
    start < end && ranges.iter().any(|r| start < r.end && r.start < end)
}

/// Where a reservation is entered. `what` is a NUL-terminated static string; the result is `RESV_OK`, `RESV_BAD_RANGE` or `RESV_FULL`.
pub trait Sink {
    fn add(&mut self, start: u64, end: u64, what: &'static [u8]) -> i32;
}

/// The loader modules `mb2_parse` recorded, as [start, end) ranges, read one at a time (no copy of the list is made).
pub trait Modules {
    fn len(&self) -> usize;
    fn get(&self, i: usize) -> (u64, u64);
}

/// The validated memory map: the whole `struct mb2_tag_mmap` tag, so every entry is read with bounds.
pub struct Map<'a> {
    tag: &'a [u8],
    entry_size: usize,
    entries: u32,
}

struct Entry {
    base: u64,
    length: u64,
    kind: u32,
}

fn le32(b: &[u8]) -> u32 {
    let mut w = [0u8; 4];
    w.copy_from_slice(b.get(..4).unwrap_or(&[0; 4]));
    u32::from_le_bytes(w)
}

fn le64(b: &[u8]) -> u64 {
    let mut w = [0u8; 8];
    w.copy_from_slice(b.get(..8).unwrap_or(&[0; 8]));
    u64::from_le_bytes(w)
}

impl<'a> Map<'a> {
    /// `tag` is the tag, `entry_size` and `entries` the values `mb2_parse` validated. None unless every entry lies inside `tag`
    /// (the C trusts `mb2_parse`; this does not need to).
    pub fn new(tag: &'a [u8], entry_size: usize, entries: u32) -> Option<Map<'a>> {
        let used = (entries as usize).checked_mul(entry_size)?.checked_add(MMAP_HEADER)?;
        (entry_size >= ENTRY_MIN && used <= tag.len()).then_some(Map { tag, entry_size, entries })
    }

    fn entry(&self, i: u32) -> Option<Entry> {
        let at = MMAP_HEADER + (i as usize) * self.entry_size; // bounded by `new`
        let e = self.tag.get(at..at + ENTRY_MIN)?;
        Some(Entry { base: le64(e), length: le64(e.get(8..)?), kind: le32(e.get(16..)?) })
    }

    fn each(&self) -> impl Iterator<Item = Entry> + '_ {
        (0..self.entries).filter_map(move |i| self.entry(i))
    }
}

fn page_down(x: u64) -> u64 {
    x & !(PAGE - 1)
}

fn page_up(x: u64) -> u64 {
    // callers keep x <= BOOT_MAP_LIMIT; the C wraps the same way
    x.wrapping_add(PAGE - 1) & !(PAGE - 1)
}

fn overlap(a0: u64, a1: u64, b0: u64, b1: u64) -> bool {
    a0 < b1 && b0 < a1
}

/// [start, end) lies wholly inside one available entry (entries that overflow are ignored).
fn in_usable_ram(map: &Map<'_>, start: u64, end: u64) -> bool {
    map.each().any(|e| {
        let top = e.base.wrapping_add(e.length);
        e.kind == MB2_MMAP_AVAILABLE && e.length != 0 && top >= e.base && start >= e.base && end <= top
    })
}

/// [start, end) intersects an entry that is NOT available. A non-available entry whose end overflows extends to 2^64.
fn hits_unavailable(map: &Map<'_>, start: u64, end: u64) -> bool {
    map.each().any(|e| {
        if e.kind == MB2_MMAP_AVAILABLE || e.length == 0 {
            return false;
        }
        let mut top = e.base.wrapping_add(e.length);
        if top < e.base {
            top = u64::MAX;
        }
        start < top && e.base < end
    })
}

/// Add a range that must be real, conflict-free RAM below 1 GiB.
fn reserve_ram<S: Sink>(map: &Map<'_>, sink: &mut S, start: u64, end: u64, what: &'static [u8], bad: &'static [u8])
    -> Result<(), &'static [u8]> {
    if start >= end || end > BOOT_MAP_LIMIT {
        return Err(bad);
    }
    if !in_usable_ram(map, page_down(start), page_up(end)) || hits_unavailable(map, page_down(start), page_up(end)) {
        return Err(bad);
    }
    match sink.add(start, end, what) {
        RESV_OK => Ok(()),
        RESV_FULL => Err(E_FULL),
        _ => Err(bad),
    }
}

/// The messages that tell a loader object (the info block or a module) apart: its kind decides each one.
struct Messages {
    what: &'static [u8],
    page0: &'static [u8],
    hole: &'static [u8],
    kernel: &'static [u8],
    bad: &'static [u8],
}

const INFO: Messages = Messages { what: WHAT_INFO, page0: E_INFO_PAGE0, hole: E_INFO_HOLE, kernel: E_INFO_KERNEL, bad: E_INFO_RAM };
const MODULE: Messages = Messages { what: WHAT_MODULE, page0: E_MOD_PAGE0, hole: E_MOD_HOLE, kernel: E_MOD_KERNEL, bad: E_MOD_RAM };

/// U1: placement rules for a loader object, wherever the loader put it (GRUB on UEFI places them below 1 MiB, on BIOS above the
/// kernel). Non-empty, ends at or below 1 GiB, never touches page 0 or the legacy hole, shares no page with the kernel image,
/// and must then pass `reserve_ram`.
fn place_object<S: Sink>(map: &Map<'_>, sink: &mut S, kernel: (u64, u64), start: u64, end: u64, m: &Messages)
    -> Result<(), &'static [u8]> {
    if start >= end || end > BOOT_MAP_LIMIT {
        return Err(m.bad);
    }
    if page_down(start) < PAGE0_END {
        return Err(m.page0);
    }
    if overlap(page_down(start), page_up(end), LEGACY_HOLE, LOW_MEM_END) {
        return Err(m.hole);
    }
    if overlap(page_down(start), page_up(end), page_down(kernel.0), page_up(kernel.1)) {
        return Err(m.kernel);
    }
    reserve_ram(map, sink, start, end, m.what, m.bad)
}

/// `boot_reserve`: add the boot reservations to the table behind `sink`, in the C's order. `info` and `modules` are the
/// [start, end) ranges `mb2_parse` recorded. Entries added before an error stay in the table.
pub fn reserve<S: Sink, M: Modules>(map: &Map<'_>, kernel: (u64, u64), info: (u64, u64), modules: &M, sink: &mut S)
    -> Result<(), &'static [u8]> {
    if sink.add(0, LOW_MEM_END, WHAT_LOW) != RESV_OK {
        return Err(E_FULL);
    }
    if kernel.0 < LOW_MEM_END {
        return Err(E_KERNEL_BELOW);
    }
    reserve_ram(map, sink, kernel.0, kernel.1, WHAT_KERNEL, E_KERNEL_RAM)?;
    place_object(map, sink, kernel, info.0, info.1, &INFO)?;
    for i in 0..modules.len() {
        let m = modules.get(i);
        if overlap(m.0, m.1, info.0, info.1) {
            return Err(E_MOD_INFO);
        }
        if (0..i).any(|j| {
            let p = modules.get(j);
            overlap(m.0, m.1, p.0, p.1)
        }) {
            return Err(E_MODS);
        }
        place_object(map, sink, kernel, m.0, m.1, &MODULE)?;
    }
    Ok(())
}
