//! USB.2-C3a.2 early UEFI firmware memory exclusion (portable, safe, no_std).
//!
//! GRUB's Multiboot2 memory map (tag 6) reports EFI BootServicesCode/Data as
//! available RAM: after ExitBootServices the OS may reuse it, although firmware
//! drivers left DMA engines pointing into it (xHCI rings, AHCI FIS areas). This
//! module decides, from the loader's tag 6 and EFI memory map (tag 17), which
//! parts of the tag-6 available RAM must never become allocatable.
//!
//! The only output is a list of ranges to REMOVE from the available RAM; the
//! platform applies them through a clear-only path, so the EFI map can restrict
//! but never expand what is allocatable. Every byte is loader input: all reads
//! are bounds-checked, nothing panics, nothing allocates.

/// Frame size; EFI descriptors are in units of 4 KiB pages.
pub const PAGE: u64 = 4096;
/// Bounds on loader input (OVMF supplies 121 descriptors and 21 entries).
pub const DESCRIPTOR_LIMIT: usize = 512;
pub const ENTRY_LIMIT: usize = 512;
/// EFI_MEMORY_DESCRIPTOR attribute bits that matter here.
pub const WB: u64 = 1 << 3;
pub const SPECIFIC_PURPOSE: u64 = 1 << 18;
pub const RUNTIME: u64 = 1 << 63;

const HEADER: usize = 16;
const EFI_MEMORY_MAP_TAG: u64 = 17;
const MEMORY_MAP_TAG: u64 = 6;
const DESCRIPTOR_MIN: usize = 40;
const DESCRIPTOR_MAX: usize = 1024;
const DESCRIPTOR_VERSION: u64 = 1;
const ENTRY_MIN: usize = 24;
const AVAILABLE: u64 = 1;
const LOADER_CODE: u32 = 1;
const LOADER_DATA: u32 = 2;
const CONVENTIONAL: u32 = 7;

/// Why the firmware memory could not be classified. Every error fails closed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// UEFI firmware, but no EFI memory map.
    Missing = 1,
    /// The loader did not exit boot services (Multiboot2 tag 18).
    BootServicesActive = 2,
    /// More than one EFI memory map.
    Duplicate = 3,
    /// Tag type/size inconsistent with the bytes supplied.
    Truncated = 4,
    DescriptorSize = 5,
    Version = 6,
    /// No descriptors, too many, or a partial descriptor.
    Count = 7,
    Unaligned = 8,
    Overflow = 9,
    /// The tag-6 memory map is malformed.
    MemoryMap = 10,
    /// The tracked window is empty or unaligned.
    Window = 11,
    /// No room for even one exclusion range.
    Capacity = 12,
    /// The platform refused an exclusion range.
    Sink = 13,
}

impl Error {
    pub fn code(self) -> u32 {
        self as u32
    }
}

/// What the loader handed over, as seen by the platform's tag walk.
#[derive(Clone, Copy)]
pub struct Evidence {
    /// An EFI system-table tag (11 or 12) was present.
    pub efi_firmware: bool,
    /// How many EFI memory-map tags (17) were present.
    pub efi_map_tags: u32,
    /// Tag 18 (EFI boot services not terminated) was present.
    pub boot_services_active: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Plan {
    /// Legacy BIOS: no EFI memory types exist; the e820 map alone protects firmware memory.
    NotApplicable,
    /// Apply the EFI map.
    Apply,
}

/// Missing or contradictory EFI information never counts as "nothing to exclude".
pub fn plan(e: &Evidence) -> Result<Plan, Error> {
    if e.boot_services_active {
        return Err(Error::BootServicesActive);
    }
    match e.efi_map_tags {
        0 if e.efi_firmware => Err(Error::Missing),
        0 => Ok(Plan::NotApplicable),
        1 => Ok(Plan::Apply),
        _ => Err(Error::Duplicate),
    }
}

// One out-of-line copy: every field read stays bounds-checked without being
// unrolled at each call site.
#[inline(never)]
fn le(bytes: &[u8], at: usize, n: usize) -> Option<u64> {
    let field = bytes.get(at..at.checked_add(n)?)?;
    Some(field.iter().rev().fold(0, |v, &b| (v << 8) | u64::from(b)))
}

/// One validated EFI memory descriptor: [start, end) with end > start.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Descriptor {
    pub kind: u32,
    pub start: u64,
    pub end: u64,
    pub attribute: u64,
}

impl Descriptor {
    /// Only memory the OS may own outright: loader or conventional RAM that is
    /// write-back capable and neither runtime nor specific-purpose memory.
    pub fn allowed(&self) -> bool {
        matches!(self.kind, LOADER_CODE | LOADER_DATA | CONVENTIONAL)
            && self.attribute & WB != 0
            && self.attribute & (RUNTIME | SPECIFIC_PURPOSE) == 0
    }
}

/// A validated EFI memory map (tag 17). Constructed only by `parse`.
#[derive(Clone, Copy)]
pub struct EfiMap<'a> {
    bytes: &'a [u8],
    size: usize,
    count: usize,
}

impl<'a> EfiMap<'a> {
    /// `tag` is the whole tag, header included, exactly `size` bytes long.
    pub fn parse(tag: &'a [u8]) -> Result<Self, Error> {
        let kind = le(tag, 0, 4).ok_or(Error::Truncated)?;
        let size = le(tag, 4, 4).ok_or(Error::Truncated)? as usize;
        if kind != EFI_MEMORY_MAP_TAG || size < HEADER || size != tag.len() {
            return Err(Error::Truncated);
        }
        let ds = le(tag, 8, 4).ok_or(Error::Truncated)? as usize;
        if !(DESCRIPTOR_MIN..=DESCRIPTOR_MAX).contains(&ds) || ds % 8 != 0 {
            return Err(Error::DescriptorSize);
        }
        if le(tag, 12, 4) != Some(DESCRIPTOR_VERSION) {
            return Err(Error::Version);
        }
        let body = size - HEADER;
        let count = body / ds;
        if body % ds != 0 || count == 0 || count > DESCRIPTOR_LIMIT {
            return Err(Error::Count);
        }
        let map = EfiMap { bytes: tag, size: ds, count };
        for i in 0..count {
            map.raw(i)?;
        }
        Ok(map)
    }

    pub fn len(&self) -> usize {
        self.count
    }

    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    /// Validated descriptor `i`, or None for a zero-page descriptor (covers nothing).
    pub fn descriptor(&self, i: usize) -> Option<Descriptor> {
        self.raw(i).ok().flatten()
    }

    fn raw(&self, i: usize) -> Result<Option<Descriptor>, Error> {
        let at = i.checked_mul(self.size).and_then(|o| o.checked_add(HEADER)).ok_or(Error::Truncated)?;
        let kind = le(self.bytes, at, 4).ok_or(Error::Truncated)? as u32;
        let start = le(self.bytes, at + 8, 8).ok_or(Error::Truncated)?;
        let pages = le(self.bytes, at + 24, 8).ok_or(Error::Truncated)?;
        let attribute = le(self.bytes, at + 32, 8).ok_or(Error::Truncated)?;
        if start % PAGE != 0 {
            return Err(Error::Unaligned);
        }
        let length = pages.checked_mul(PAGE).ok_or(Error::Overflow)?;
        let end = start.checked_add(length).ok_or(Error::Overflow)?;
        Ok(if pages == 0 { None } else { Some(Descriptor { kind, start, end, attribute }) })
    }
}

/// The loader's tag-6 memory map. Constructed only by `parse`.
#[derive(Clone, Copy)]
pub struct MemoryMap<'a> {
    bytes: &'a [u8],
    size: usize,
    count: usize,
}

impl<'a> MemoryMap<'a> {
    pub fn parse(tag: &'a [u8]) -> Result<Self, Error> {
        let kind = le(tag, 0, 4).ok_or(Error::MemoryMap)?;
        let size = le(tag, 4, 4).ok_or(Error::MemoryMap)? as usize;
        let es = le(tag, 8, 4).ok_or(Error::MemoryMap)? as usize;
        if kind != MEMORY_MAP_TAG || size <= HEADER || size != tag.len() || es < ENTRY_MIN || es % 8 != 0
            || le(tag, 12, 4) != Some(0) || (size - HEADER) % es != 0 || (size - HEADER) / es > ENTRY_LIMIT
        {
            return Err(Error::MemoryMap);
        }
        Ok(MemoryMap { bytes: tag, size: es, count: (size - HEADER) / es })
    }

    /// Available RAM entry `i` as [start, end); None if not RAM, empty or overflowing
    /// (the PMM never frees an overflowing entry either).
    fn available(&self, i: usize) -> Option<(u64, u64)> {
        let (base, end, kind) = self.entry(i)?;
        (kind == AVAILABLE && base != end).then_some((base, end))
    }

    /// Entry `i` as (base, end, type); an overflowing end saturates to 2^64 - 1.
    fn entry(&self, i: usize) -> Option<(u64, u64, u64)> {
        let at = HEADER + i.checked_mul(self.size)?;
        let base = le(self.bytes, at, 8)?;
        let length = le(self.bytes, at + 8, 8)?;
        let kind = le(self.bytes, at + 16, 4)?;
        match base.checked_add(length) {
            Some(end) => Some((base, end, kind)),
            None if kind == AVAILABLE => None,
            None => Some((base, u64::MAX, kind)),
        }
    }

    /// Whether any non-available entry touches [start, end).
    fn unavailable_overlaps(&self, start: u64, end: u64) -> bool {
        (0..self.count).any(|i| {
            matches!(self.entry(i), Some((b, e, k)) if k != AVAILABLE && b < e && b < end && start < e)
        })
    }
}

/// What was removed.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Summary {
    pub ranges: u32,
    /// Frames covered by the emitted ranges, rounded outward.
    pub frames: u64,
    /// Ranges were merged across a gap because the platform table was full.
    pub coalesced: bool,
}

struct Sweep<'s> {
    capacity: usize,
    emitted: usize,
    pending: Option<(u64, u64)>,
    last_frame_end: u64,
    summary: Summary,
    sink: &'s mut dyn FnMut(u64, u64) -> bool,
}

impl Sweep<'_> {
    fn push(&mut self, s: u64, e: u64) -> Result<(), Error> {
        self.pending = match self.pending {
            None if self.capacity == 0 => return Err(Error::Capacity),
            None => Some((s, e)),
            Some((ps, pe)) if pe == s => Some((ps, e)),
            // Only the pending slot is left: widen it over the gap. More is
            // excluded, never less.
            Some((ps, _)) if self.emitted + 1 >= self.capacity => {
                self.summary.coalesced = true;
                Some((ps, e))
            }
            Some((ps, pe)) => {
                self.emit(ps, pe)?;
                Some((s, e))
            }
        };
        Ok(())
    }

    fn emit(&mut self, s: u64, e: u64) -> Result<(), Error> {
        if !(self.sink)(s, e) {
            return Err(Error::Sink);
        }
        self.emitted += 1;
        let first = (s / PAGE).max(self.last_frame_end);
        let last = e.div_ceil(PAGE);
        self.summary.frames += last.saturating_sub(first);
        self.last_frame_end = last;
        self.summary.ranges += 1;
        Ok(())
    }
}

/// Whether `p` must be excluded, and the next range boundary after `p` (at
/// most `hi`); the answer is constant on [p, next).
#[inline(never)]
fn probe(efi: &EfiMap<'_>, ram: &MemoryMap<'_>, p: u64, hi: u64) -> (bool, u64) {
    let mut next = hi;
    let mut edge = |s: u64, e: u64| {
        for b in [s, e] {
            if b > p && b < next {
                next = b;
            }
        }
        s <= p && p < e
    };
    let (mut ram_here, mut ok, mut bad) = (false, false, false);
    for i in 0..ram.count {
        if let Some((s, e)) = ram.available(i) {
            ram_here |= edge(s, e);
        }
    }
    for i in 0..efi.count {
        if let Some(d) = efi.descriptor(i) {
            if edge(d.start, d.end) {
                if d.allowed() {
                    ok = true;
                } else {
                    bad = true;
                }
            }
        }
    }
    (ram_here && (bad || !ok), next)
}

/// USB.2-C3a.3: whether the whole frame at `frame` is excluded firmware RAM:
/// inside the page-aligned window [lo, hi), touching no non-available entry,
/// and excluded with no range boundary inside it. `probe` treats every tag-6
/// RAM and EFI descriptor edge as a boundary, so "excluded at `frame` and the
/// next boundary at or past its end" means the whole frame is tag-6 RAM with
/// one classification. Such a frame is RAM the kernel's alias maps and the
/// allocator never hands out; inspecting it never makes it releasable.
pub fn firmware_frame(efi: &EfiMap<'_>, ram: &MemoryMap<'_>, frame: u64, lo: u64, hi: u64) -> bool {
    let Some(end) = frame.checked_add(PAGE) else { return false };
    if frame % PAGE != 0 || lo % PAGE != 0 || hi % PAGE != 0 || frame < lo || end > hi {
        return false;
    }
    if ram.unavailable_overlaps(frame, end) {
        return false;
    }
    let (excluded, next) = probe(efi, ram, frame, end);
    excluded && next >= end
}

/// Emit, in ascending order, every part of tag-6 available RAM inside the
/// page-aligned window [lo, hi) that is not covered by an allowed EFI
/// descriptor or is covered by any other EFI descriptor (overlaps and
/// contradictions resolve toward exclusion). At most `capacity` ranges are
/// emitted; beyond that the last range is widened.
pub fn exclusions(
    efi: &EfiMap<'_>,
    ram: &MemoryMap<'_>,
    lo: u64,
    hi: u64,
    capacity: usize,
    sink: &mut dyn FnMut(u64, u64) -> bool,
) -> Result<Summary, Error> {
    if lo >= hi || lo % PAGE != 0 || hi % PAGE != 0 {
        return Err(Error::Window);
    }
    let mut sweep = Sweep { capacity, emitted: 0, pending: None, last_frame_end: 0, summary: Summary::default(), sink };
    let mut cursor = lo;
    while cursor < hi {
        let (excluded, n) = probe(efi, ram, cursor, hi);
        if excluded {
            sweep.push(cursor, n)?;
        }
        cursor = n;
    }
    if let Some((s, e)) = sweep.pending.take() {
        sweep.emit(s, e)?;
    }
    Ok(sweep.summary)
}
